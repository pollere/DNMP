/*
 * Copyright (c) 2019,  Pollere Inc.
 *
 * This file is part of syncps (NDN sync for pubsub).
 * See AUTHORS.md for complete list of syncps authors and contributors.
 *
 * syncps is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * syncps is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * syncps, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 **/

#ifndef SYNCPS_SYNCPS_HPP
#define SYNCPS_SYNCPS_HPP

#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <unordered_map>

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/validator-null.hpp>
#include <ndn-cxx/util/logger.hpp>
#include <ndn-cxx/util/random.hpp>
#include <ndn-cxx/util/scheduler.hpp>
#include <ndn-cxx/util/time.hpp>

#include "syncps/iblt.hpp"

namespace syncps
{
NDN_LOG_INIT(syncps.SyncPubsub);

using Name = ndn::Name;         // type of a name
using Publication = ndn::Data;  // type of a publication
using SigningInfo = ndn::security::SigningInfo; // how to sign publication
using ScopedEventId = ndn::scheduler::ScopedEventId; // scheduler events

namespace tlv
{
    enum { syncpsContent = 129 }; // tlv for block of publications
} // namespace tlv

constexpr int maxPubSize = 1300;    // max payload in Data (approximate)

using namespace ndn::literals::time_literals;
constexpr ndn::time::milliseconds maxPubLifetime = 1_s;
constexpr ndn::time::milliseconds maxClockSkew = 1_s;

/**
 * @brief app callback when new publications arrive
 */
using UpdateCb = std::function<void(const Publication&)>;

/**
 * @brief app callback to test if publication is expired
 */
using IsExpiredCb = std::function<bool(const Publication&)>;
/**
 * @brief app callback to filter peer publication requests
 */
using PubPtr = std::shared_ptr<const Publication>;
using VPubPtr = std::vector<PubPtr>;
using FilterPubsCb = std::function<VPubPtr(VPubPtr&,VPubPtr&)>;

/**
 * @brief sync a lifetime-bounded set of publications among
 *        an arbitrary set of nodes.
 *
 * Application should call 'publish' to add a new publication to the
 * set and register an UpdateCallback that is called whenever new
 * publications from others are received. Publications are automatically
 * deleted (without notice) at the end their lifetime.
 *
 * Publications are named, signed objects (ndn::Data). The last component of
 * their name is a version number (local ms clock) that is used to bound the
 * pub lifetime. This component is added by 'publish' before the publication
 * is signed so it is protected against replay attacks. App publications
 * are signed by pubCertificate and external publications are verified by
 * pubValidator on arrival. (XXX not yet)
 */

class SyncPubsub
{
  public:
    class Error : public std::runtime_error
    {
      public:
        using std::runtime_error::runtime_error;
    };
    /**
     * @brief constructor
     *
     * Registers syncPrefix in NFD and sends a sync interest
     *
     * @param face application's face
     * @param syncPrefix The ndn name prefix for sync interest/data
     * @param syncInterestLifetime lifetime of the sync interest
     * @param expectedNumEntries expected entries in IBF
     */
    SyncPubsub(ndn::Face& face, Name syncPrefix,
        IsExpiredCb isExpired, FilterPubsCb filterPubs,
        ndn::time::milliseconds syncInterestLifetime = 4_s,
        size_t expectedNumEntries = 85)  // = 128/1.5 (see detail/iblt.hpp)
        : m_face(face),
          m_syncPrefix(std::move(syncPrefix)),
          m_expectedNumEntries(expectedNumEntries),
          m_validator(ndn::security::v2::getAcceptAllValidator()), //XXX
          m_scheduler(m_face.getIoService()),
          m_iblt(expectedNumEntries),
          m_signingInfo(ndn::security::SigningInfo::SIGNER_TYPE_SHA256),
          m_isExpired{std::move(isExpired)}, m_filterPubs{std::move(filterPubs)},
          m_syncInterestLifetime(syncInterestLifetime),
          m_registeredPrefix(m_face.setInterestFilter(
              ndn::InterestFilter(m_syncPrefix).allowLoopback(false),
              [this](auto f, auto i) { onSyncInterest(f, i); },
              [this](auto/*n*/) { m_registering = false; sendSyncInterest(); },
              [this](auto n, auto s) { onRegisterFailed(n, s); },
              m_signingInfo))
    { }

    /**
     * @brief handle a new publication from app
     *
     * A publication is published at most once and
     * lives for at most pubLifetime.
     *
     * @param pub the object to publish
     */
    SyncPubsub& publish(Publication&& pub)
    {
        m_keyChain.sign(pub, m_signingInfo); //XXX
        if (isKnown(pub)) {
            NDN_LOG_WARN("republish of '" << pub.getName() << "' ignored");
        } else {
            NDN_LOG_INFO("Publish: " << pub.getName());
            ++m_publications;
            addToActive(std::move(pub), true);
            // new pub may let us respond to pending interest(s).
            if (! m_delivering) {
                sendSyncInterest();
                handleInterests();
            }
        }
        return *this;
    }

    /**
     * @brief subscribe to a subtopic
     *
     * Calls 'cb' on each new publication to 'topic' arriving
     * from some external source.
     *
     * @param  topic the topic
     */
    SyncPubsub& subscribeTo(const Name& topic, UpdateCb&& cb)
    {
        // add to subscription dispatch table. NOTE that an existing
        // subscription to 'topic' will be changed to the new callback.
        m_subscription[topic] = std::move(cb);
        NDN_LOG_INFO("subscribeTo: " << topic);
        return *this;
    }

    /**
     * @brief unsubscribe to a subtopic
     *
     * A subscription to 'topic', if any, is removed.
     *
     * @param  topic the topic
     */
    SyncPubsub& unsubscribe(const Name& topic)
    {
        m_subscription.erase(topic);
        NDN_LOG_INFO("unsubscribe: " << topic);
        return *this;
    }

    /**
     * @brief set Sync Interest lifetime
     *
     * @param t interest lifetime in ms
     */
    SyncPubsub& setSyncInterestLifetime(ndn::time::milliseconds t)
    {
        m_syncInterestLifetime = t;
        return *this;
    }

    /**
     * @brief schedule a callback after some time
     *
     * This lives here to avoid exposing applications to the complicated mess
     * of NDN's relationship to Boost
     *
     * @param after how long to wait (in nanoseconds)
     * @param cb routine to call
     */
    ScopedEventId schedule(ndn::time::nanoseconds after,
                           const std::function<void()>& cb)
    {
        return m_scheduler.schedule(after, cb);
    }

    /**
     * @brief set publication signingInfo
     *
     * All publications are signed when published using this signing info.
     * If no signing info is supplied, a SHA256 signature is used
     * (essentially a high quality checksum without provenance or
     * trust semantics).
     *
     * @param si a valid ndn::Security::SigningInfo 
     */
    SyncPubsub& setSigningInfo(const SigningInfo& si)
    {
        m_signingInfo = si;
        return *this;
    }

    /**
     * @brief set packet validator
     *
     * All arriving Data and/or Interest packets are validated
     * with this validator. If no validator is set, an 'accept
     * all' validator is used.
     *
     * @param validator new packet validator to use
     * XXX can't do this because validator base class is not copyable
    SyncPubsub& setValidator(ndn::security::v2::Validator& validator)
    {
        m_validator = validator;
        return *this;
    }
     */

    const ndn::security::v2::Validator& getValidator() { return m_validator; }

   private:

    /**
     * @brief reexpress our current sync interest so it doesn't time out
     */
    void reExpressSyncInterest()
    {
        // The interest is sent 20ms ahead of when it's due to time out
        // to allow for propagation and precessing delays.
        //
        // note: previously scheduled timer is automatically cancelled.
        auto when = m_syncInterestLifetime - 20_ms;
        m_scheduledSyncInterestId =
            m_scheduler.schedule(when, [this] { sendSyncInterest(); });
    }

    /**
     * @brief Send a sync interest describing our publication set
     *        to our peers.
     *
     * Creates & sends interest of the form: /<sync-prefix>/<own-IBF>
     */
    void sendSyncInterest()
    {
        // if an interest is sent before the initial register is done the reply can't
        // reach us. don't send now since the register callback will do it.
        if (m_registering) {
            return;
        }
        // schedule the next send
        reExpressSyncInterest();

        // Build and ship the interest. Format is
        // /<sync-prefix>/<ourLatestIBF>
        ndn::Name name = m_syncPrefix;
        m_iblt.appendToName(name);

        ndn::Interest syncInterest(name);
        m_currentInterest = ndn::random::generateWord32();
        syncInterest.setNonce(m_currentInterest)
            .setCanBePrefix(true)
            .setMustBeFresh(true)
            .setInterestLifetime(m_syncInterestLifetime);
        m_face.expressInterest(syncInterest,
                [this](auto i, auto d) {
                    m_validator.validate(d,
                        [this, i](auto d) { onValidData(i, d); },
                        [](auto d, auto e) { NDN_LOG_INFO("Invalid: " << e << " Data " << d); }); },
                [](auto i, auto/*n*/) { NDN_LOG_INFO("Nack for " << i); },
                [](auto i) { NDN_LOG_INFO("Timeout for " << i); });
        ++m_interestsSent;
        NDN_LOG_DEBUG("sendSyncInterest " << std::hex
                      << m_currentInterest << "/" << hashIBLT(name));
    }

    /**
     * @brief Send a sync interest sometime soon
     */
    void sendSyncInterestSoon()
    {
        NDN_LOG_DEBUG("sendSyncInterestSoon");
        m_scheduledSyncInterestId =
            m_scheduler.schedule(3_ms, [this]{ sendSyncInterest(); });
    }

    /**
     * @brief callback to Process a new sync interest from NFD
     *
     * Get differences between our IBF and IBF in the sync interest.
     * If we have some things that the other side does not have,
     * reply with a Data packet containing (some of) those things.
     *
     * @param prefixName prefix registration that matched interest
     * @param interest   interest packet
     */
    void onSyncInterest(const ndn::Name& prefixName, const ndn::Interest& interest)
    {
        if (interest.getNonce() == m_currentInterest) {
            // library looped back our interest
            return;
        }
        const ndn::Name& name = interest.getName();
        NDN_LOG_DEBUG("onSyncInterest " << std::hex << interest.getNonce() << "/"
                      << hashIBLT(name));

        if (name.size() - prefixName.size() != 1) {
            NDN_LOG_INFO("invalid sync interest: " << interest);
            return;
        }
        if (!  handleInterest(name)) {
            // couldn't handle interest immediately - remember it until
            // we satisfy it or it times out;
            m_interests[name] = ndn::time::system_clock::now() +
                                    m_syncInterestLifetime;
        }
    }

    void handleInterests()
    {
        NDN_LOG_DEBUG("handleInterests");
        auto now = ndn::time::system_clock::now();
        for (auto i = m_interests.begin(); i != m_interests.end(); ) {
            const auto& [name, expires] = *i;
            if (expires <= now || handleInterest(name)) {
                i = m_interests.erase(i);
            } else {
                ++i;
            }
        }
    }

    bool handleInterest(const ndn::Name& name)
    {
        // 'Peeling' the difference between the peer's iblt & ours gives
        // two sets:
        //   have - (hashes of) items we have that they don't
        //   need - (hashes of) items we need that they have
        IBLT iblt(m_expectedNumEntries);
        try {
            iblt.initialize(name.get(-1));
        } catch (const std::exception& e) {
            NDN_LOG_WARN(e.what());
            return true;
        }
        std::set<uint32_t> have;
        std::set<uint32_t> need;
        (m_iblt - iblt).listEntries(have, need);
        NDN_LOG_DEBUG("handleInterest " << std::hex << hashIBLT(name)
                      << " need " << need.size() << ", have " << have.size());

        // If we have things the other side doesn't, send as many as
        // will fit in one Data. Make two lists of needed, active publications:
        // ones we published and ones published by others.

        VPubPtr pOurs, pOthers;
        for (const auto hash : have) {
            if (auto h = m_hash2pub.find(hash); h != m_hash2pub.end()) {
                // 2^0 bit of p->second is =0 if pub expired; 2^1 bit is 1 if we
                // did publication.
                if (const auto p = m_active.find(h->second); p != m_active.end()
                    && (p->second & 1U) != 0) {
                    ((p->second & 2U) != 0? &pOurs : &pOthers)->push_back(h->second);
                }
            }
        }
        pOurs = m_filterPubs(pOurs, pOthers);
        if (pOurs.empty()) {
            return false;
        }
        ndn::Block pubs(tlv::syncpsContent);
        for (const auto& p : pOurs) {
            NDN_LOG_DEBUG("Send pub " << p->getName());
            pubs.push_back((*(p)).wireEncode());
            if (pubs.size() >= maxPubSize) {
                break;
            }
        }
        pubs.encode();
        sendSyncData(name, pubs);
        return true;
    }

    /**
     * @brief Send a sync data packet responding to a sync interest.
     *
     * Send a packet containing one or more publications that are known
     * to be in our active set but not in the interest sender's set.
     *
     * @param name  is the name from the sync interest we're responding to
     *              (data packet's base name)
     * @param pubs  is the list of publications (data packet's payload)
     */
    void sendSyncData(const ndn::Name& name, const ndn::Block& pubs)
    {
        NDN_LOG_DEBUG("sendSyncData: " << name);
        auto data = std::make_shared<ndn::Data>();
        data->setName(name).setContent(pubs).setFreshnessPeriod(maxPubLifetime / 2);
        m_keyChain.sign(*data, m_signingInfo);
        m_face.put(*data);
    }

    /**
     * @brief Process sync data after successful validation
     *
     * Add each item in Data content that we don't have to
     * our list of active publications then notify the
     * application about the updates.
     *
     * @param interest interest for which we got the data
     * @param data     sync data content
     */
    void onValidData(const ndn::Interest& interest, const ndn::Data& data)
    {
        NDN_LOG_DEBUG("onValidData: " << std::hex << interest.getNonce() << "/"
                       << hashIBLT(interest.getName())
                       << " " << data.getName());

        const ndn::Block& pubs(data.getContent().blockFromValue());
        if (pubs.type() != tlv::syncpsContent) {
            NDN_LOG_WARN("Sync Data with wrong content type " <<
                         pubs.type() << " ignored.");
            return;
        }

        // if publications result from handling this data we don't want to
        // respond to a peer's interest until we've handled all of them.
        m_delivering = true;
        auto initpubs = m_publications;

        pubs.parse();
        for (const auto& e : pubs.elements()) {
            if (e.type() != ndn::tlv::Data) {
                NDN_LOG_WARN("Sync Data with wrong Publication type " <<
                             e.type() << " ignored.");
                continue;
            }
            //XXX validate pub against schema here
            Publication pub(e);
            if (m_isExpired(pub) || isKnown(pub)) {
                NDN_LOG_DEBUG("ignore expired or known " << pub.getName());
                continue;
            }
            // we don't already have this publication so deliver it
            // to the longest match subscription.
            // XXX lower_bound goes one too far when doing longest
            // prefix match. It would be faster to stick a marker on
            // the end of subscription entries so this wouldn't happen.
            // Also, it would be faster to do the comparison on the
            // wire-format names (excluding the leading length value)
            // rather than default of component-by-component.
            const auto& p = addToActive(std::move(pub));
            const auto& nm = p->getName();
            auto sub = m_subscription.lower_bound(nm);
            if ((sub != m_subscription.end() && sub->first.isPrefixOf(nm)) ||
                (sub != m_subscription.begin() && (--sub)->first.isPrefixOf(nm))) {
                NDN_LOG_DEBUG("deliver " << nm << " to " << sub->first);
                sub->second(*p);
            } else {
                NDN_LOG_DEBUG("no sub for  " << nm);
            }
        }

        // We've delivered all the publications in the Data.
        // If this is our currently active sync interest, send an
        // interest to replace the one consumed by the Data.
        // If deliveries resulted in new publications, try to satisfy
        // pending peer interests.
        m_delivering = false;
        if (interest.getNonce() == m_currentInterest) {
            sendSyncInterest();
        }
        if (initpubs != m_publications) {
            handleInterests();
        }
    }

    /**
     * @brief Methods to manage the active publication set.
     */

    // publications are stored using a shared_ptr so we
    // get to them indirectly via their hash.

    uint32_t hashPub(const Publication& pub) const
    {
        const auto& b = pub.wireEncode();
        return murmurHash3(N_HASHCHECK,
                           std::vector<uint8_t>(b.wire(), b.wire() + b.size()));
    }

    bool isKnown(uint32_t h) const
    {
        //return m_hash2pub.contains(h);
        return m_hash2pub.find(h) != m_hash2pub.end();
    }

    bool isKnown(const Publication& pub) const
    {
        // publications are stored using a shared_ptr so we
        // get to them indirectly via their hash.
        return isKnown(hashPub(pub));
    }

    std::shared_ptr<Publication> addToActive(Publication&& pub, bool localPub = false)
    {
        NDN_LOG_DEBUG("addToActive: " << pub.getName());
        auto hash = hashPub(pub);
        auto p = std::make_shared<Publication>(pub);
        m_active[p] = localPub? 3 : 1;
        m_hash2pub[hash] = p;
        m_iblt.insert(hash);

        // We remove an expired publication from our active set at twice its pub
        // lifetime (the extra time is to prevent replay attacks enabled by clock
        // skew).  An expired publication is never supplied in response to a sync
        // interest so this extra hold time prevents end-of-lifetime spurious
        // exchanges due to clock skew.
        //
        // Expired publications are kept in the iblt for at least the max clock skew
        // interval to prevent a peer with a late clock giving it back to us as soon
        // as we delete it.

        m_scheduler.schedule(maxPubLifetime, [this, p] { m_active[p] &=~ 1U; });
        m_scheduler.schedule(maxPubLifetime + maxClockSkew,
            [this, hash] { m_iblt.erase(hash); sendSyncInterestSoon(); });
        m_scheduler.schedule(maxPubLifetime * 2, [this, p] { removeFromActive(p); });

        return p;
    }

    void removeFromActive(const PubPtr& p)
    {
        NDN_LOG_DEBUG("removeFromActive: " << (*p).getName());
        m_active.erase(p);
        m_hash2pub.erase(hashPub(*p));
    }

    /**
     * @brief Log a message if setting an interest filter fails
     *
     * @param prefix
     * @param msg
     */
    void onRegisterFailed(const ndn::Name& prefix, const std::string& msg) const
    {
        NDN_LOG_ERROR("onRegisterFailed " << prefix << " " << msg);
        BOOST_THROW_EXCEPTION(Error(msg));
    }

    uint32_t hashIBLT(const Name& n) const
    {
        const auto& b = n[-1];
        return murmurHash3(N_HASHCHECK,
                           std::vector<uint8_t>(b.value_begin(), b.value_end()));
    }

  private:
    ndn::Face& m_face;
    ndn::Name m_syncPrefix;
    uint32_t m_expectedNumEntries;
    ndn::security::v2::Validator& m_validator;
    ndn::Scheduler m_scheduler;
    std::map<const Name, ndn::time::system_clock::TimePoint> m_interests{};
    IBLT m_iblt;
    ndn::KeyChain m_keyChain;
    SigningInfo m_signingInfo;
    // currently active published items
    std::unordered_map<std::shared_ptr<const Publication>, uint8_t> m_active{};
    std::unordered_map<uint32_t, std::shared_ptr<const Publication>> m_hash2pub{};
    std::map<const Name, UpdateCb> m_subscription{};
    IsExpiredCb m_isExpired;
    FilterPubsCb m_filterPubs;
    ndn::time::milliseconds m_syncInterestLifetime;
    ndn::scheduler::ScopedEventId m_scheduledSyncInterestId;
    //ndn::ScopedPendingInterestHandle m_interest;
    ndn::ScopedRegisteredPrefixHandle m_registeredPrefix;
    uint32_t m_currentInterest{};   // nonce of current sync interest
    uint32_t m_publications{};      // # local publications
    uint32_t m_interestsSent{};
    bool m_delivering{false};       // currently processing a Data
    bool m_registering{true};
};

}  // namespace syncps

#endif  // SYNCPS_SYNCPS_HPP
