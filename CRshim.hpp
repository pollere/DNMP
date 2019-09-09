/*
 * CRshim.hpp: Command-Reply DNMP shim
 *
 * Copyright (c) 2019,  Pollere Inc.
 *
 * This file is part of the Distributed Network Measurement Protocol (DNMP)
 * proof-of-concept (PoC), a project primarily funded under NIST 70NANB18H186.
 * See AUTHORS.md for complete list of syncps authors and contributors.
 *
 * DNMP is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * DNMP is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this source, e.g., COPYING.md file. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * CRshim provides an API between DNMP and the syncps protocol. It is both
 * DNMP and syncps aware.  A client app (usually) creates one CRshim.
 * A nod should create one CRshim for each topic to which it subscribes,
 * e.g., nod/local, nod/all and nod/id.
 *
 * This file contains the methods implementing the DNMP command/reply shim.
 * It is a self-contained, 'header-only' library.
 */

#include <utility>
#include <unistd.h>
#include "syncps/syncps.hpp"

using namespace syncps;
using namespace ndn;

struct Reply : public Publication {
    /**
     * @brief Name-based accessors for Reply name components.
     *
     * These use the schema string table to map a component's name to
     * it's index in that publication.
     */
    const name::Component& operator[](std::string_view s) const
    {
        return getName()[m_n2i.at(s)];
    }

    /*
     * return the time difference (in seconds) between timepoint 'tp'
     * and the timepoint in component 'idx' this pub's name
     */
    double timeDelta(int indx, time::system_clock::time_point tp =
                                time::system_clock::now()) const {

        return boost::chrono::duration_cast<boost::chrono::duration<double>>
                (tp - getName()[indx].toTimestamp()).count();
    }
    double timeDelta(std::string_view s, time::system_clock::time_point tp =
                                time::system_clock::now()) const {
        return timeDelta(m_n2i.at(s), tp);
    }

    /*
     * return the time difference (in seconds) between 
     * the timepoints in components 'l' and 'f' this pub's name
     */
    double timeDelta(int l, int f) const {
        auto name = getName();
        return boost::chrono::duration_cast<boost::chrono::duration<double>>
                (name[l].toTimestamp() - name[f].toTimestamp()).count();
    }
    double timeDelta(std::string_view sl, std::string_view sf) const {
        return timeDelta(m_n2i.at(sl), m_n2i.at(sf));
    }
  private:
    // XXX temporary placeholder component name to index map. Will be
    // replaced by schema-based version for release.
    static inline const std::unordered_map<std::string_view, int8_t> m_n2i {
        { "rTimestamp", -1 },
        { "rSrcId", -2 },
        { "cTimestamp", -3 },
        { "pArgs", -4 },
        { "pType", -5 },
        { "origin", -7 },
        { "Id", -8 },
        { "role", -9 },
    };
};

class CRshim;

using rpHndlr = std::function<void(const Reply&, CRshim&)>;
using cmHndlr = std::function<void(Name&&, CRshim&)>;
using Timer = ndn::scheduler::ScopedEventId;
using TimerCb = std::function<void()>;

#define LOG(x)

class CRshim
{
   public:
    CRshim(Face& face, const std::string& target) :
        m_face(face),
        m_sync(m_face, targetToPrefix(target)),
        m_topic{topicName(target)}
    {}
    CRshim(const std::string& target) :
        CRshim(*new Face(), target) {}
    CRshim(const CRshim& s1, const std::string& target) :
        CRshim(s1.m_face, target) {}

    void run() { m_face.processEvents(); }
    const auto prefix() { return m_topic; }

    /* command/reply client methods */

    /*
     * build a command for the probeType (passed in as a string)
     * with the optional probeArgs (passed in as a string)
     * Creates an NDN name according to DNMP spec for command
    */
    Publication buildCmd(const std::string& s, const std::string& a = "")
    {
        Name cmd(prefix());
        cmd.append(Name::Component(s)).append(Name::Component(a)).appendTimestamp();
        return Publication(cmd);
    }

    /*
     * subscribe to a topic for the expected reply, then publish the command
    */
    CRshim& issueCmd(const std::string& ptype, const std::string& pargs,
        const rpHndlr& rh)
    { 
        auto cmd(buildCmd(ptype, pargs));
        m_sync.subscribeTo(expectedReply(cmd),
                           [this,rh](auto r){ rh((const Reply&)r,*this); });
        m_sync.publish(cmd);
        return *this;
    }

    void doCommand(const std::string& ptype, const std::string& pargs, const rpHndlr& rh)
    {
        issueCmd(ptype, pargs, rh);
        run();
    }

    /* command/reply NOD methods */

    /*
     * Subscribe to command topic and wait for an incoming publication
     */
    CRshim& waitForCmd(const cmHndlr& ch)
    {
        m_sync.subscribeTo(prefix().getSubName(0, prefix().size() - 3),
                           [this, ch](auto c) { ch(expectedReply(c), *this); });
        return *this;
    }

    void sendReply(Name& n, std::string&& rv)
    {
        // append nod id & timestamp to reply name then publish reply
        n.append(Name::Component(myPID())).appendTimestamp();
        Publication reply(n);
        reply.setContent((const uint8_t*)(rv.data()), rv.size());
        m_sync.publish(reply);
    }

    /*
     * Create an array of shims (all using the same face) from an
     * argument list of target names.
     */
    template <typename ... T>
    static auto shims(T...target) {
        Face& f = *new Face();
        return std::array<CRshim,sizeof...(T)> {CRshim(f, target)...};
    }

    /* Common methods */

    /*
     * Construct the 'reply' topic Name expected for a particular 'command'.
     * Used for both NODs and Clients so doesn't add replySrcID or timestamp
     */
    Name expectedReply(const Publication& pub)
    {
        size_t n = prefix().size() - 4;
        const auto& cmd = pub.getName();
        Name reply(cmd.getPrefix(n));
        reply.append("reply").append(cmd.getSubName(n + 1));
        return reply;
    }

    static std::string myPID()
    {
        return addHostname("_", "pid" + std::to_string(getpid()));
    }

    Timer schedule(ndn::time::nanoseconds d, const TimerCb& cb) {
        return m_sync.schedule(d, cb);
    }

  protected:
    // -- temporary pre-schemaLib place holders --
    // these will be replaced with trust schema library routines
    // in the next version.
    /*
     * Construct the NDN name prefix to use for syncps communication
     * with application-level target 't'. In the POC this is a fixed
     * mapping with 'local' mapping to "/localhost/dnmp" and anything
     * else mapping to "/localnet/dnmp".
     * (In the trust-schema-based shim, this mapping is described
     * in a site-specific trust schema.)
     */
    static std::string targetToPrefix(const std::string& t)
    {
        return t == "local"? "/localhost/dnmp" : "/localnet/dnmp";
    }
    static std::string addHostname(const char* sep, std::string id)
    {
        id += sep;
        char h[256];
        if (gethostname(h, sizeof(h) - 1) == 0) {
            h[sizeof(h) - 1] = 0;
            id += h;
        } else {
            id += "??";
        }
        return id;
    }
    static std::string myID()
    {
        return addHostname("/", "uid" + std::to_string(getuid()));
    }
    /*
     * construct the full topic name prefix for this session  given
     * the application-level target 'target'. For the POC this name
     * has a fixed format. (In the trust-schema-based shim, it is
     * determined by a validator/constructor schema.)
     */
    static std::string topicName(const std::string& target)
    {
        std::string p("myHouse/dnmp/nod/");
        p += target;
        p += "/command/";
        p += myID();
        p += "/probe";
        return p;
    }
    // -- end of place holders --

    Face& m_face;
    SyncPubsub m_sync;
    Name m_topic;     // full name of the topic
};
