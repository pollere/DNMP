/*
 * probes.hpp: proof-of-concept DNMP probes
 *
 * Copyright (C) 2019 Pollere, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <https://www.gnu.org/licenses/>.
 *  You may contact Pollere, Inc at info@pollere.net.
 *
 *  The DNMP proof-of-concept is not intended as production code.
 *  More information on DNMP is available from info@pollere.net
 */

/*      Probes 
 *
 * Probes that can be used by a PoC DNMP NOD. Must follow format of accepting a string
 * that can be parsed for any required arguments and returning results in a string
 * that the DNMP syncer publisher will convert to Data.
 * Each probe has a DNMP probeType name and may need some way to indicate that.
 * Probes need to check arguments to ensure makes sense.
 */

//Pinger Client just uses timestamps from Publication name so do nothing
static std::string echoProbe(const std::string& args) {
    if(!args.empty())
        LOG("echoProbe: nonempty argument is ignored");
    return std::string("");
}

#include <iostream>
#include <stdlib.h>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/data.hpp>
#include <ndn-cxx/util/scheduler.hpp>
#include <ndn-cxx/mgmt/nfd/forwarder-status.hpp>
#include <ndn-cxx/mgmt/nfd/rib-entry.hpp>
#include <ndn-cxx/encoding/tlv-nfd.hpp>


/*
 * Object used by all the Probes that use the local NFD's Management Protocol interface.
 * Takes care of sending the Interest and gets the Management Data set.
 */

namespace ndn {
   class nfdManagementQ : noncopyable
   {
    public:
    void run(std::string s)
    {
        Interest interest(Name(s.c_str()));
        interest.setInterestLifetime(2_s); // 2 seconds
        interest.setCanBePrefix(true);
        interest.setMustBeFresh(true);

        m_face.expressInterest(interest,
                           bind(&nfdManagementQ::onData, this,  _1, _2),
                           bind(&nfdManagementQ::onNack, this, _1, _2),
                           bind(&nfdManagementQ::onTimeout, this, _1));

        LOG("nfdManagementQ entity sending Interest:");
        LOG(interest);

        // processEvents will block until the requested data received or timeout occurs
        m_face.processEvents();
    }
    const Data& dataVal()
    {
        return mgmtData;
    }
    private:
    void onData(const Interest& interest, const Data& data)
    {
      LOG("nfdManagementQ entity received Data: ");
      LOG(data);
      mgmtData = data;
    }
    void onNack(const Interest& interest, const lp::Nack& nack)
    {
        LOG("received Nack with reason: ");
        LOG(nack.getReason());
        LOG("for Interest: ");
        LOG(interest);
    }
    void onTimeout(const Interest& interest)
    {
        LOG("nfdManagmentQ entity received Timeout for Interest");
    }
    private:
        Face m_face;
        Data mgmtData;
   };
} // namespace ndn

/*
 * The NFD General Status Probe uses the NFD management interface.
 * The GSmetrics set is used to select particular metrics rather than
 * returning all.
 */

static const std::set<std::string> GSmetrics = {
    "NfdVersion",
    "StartTimestamp",
    "CurrentTimestamp",
    "Uptime",
    "NameTreeEntries",
    "FibEntries",
    "PitEntries",
    "MeasurementsEntries",
    "CsEntries",
    "Interests",
    "Data",
    "Nacks",
    "SatisfiedInterests",
    "UnsatisfiedInterests",
    "all"
};

static std::string nfdGSProbe(const std::string& args) {
    //use NFD management commands
    //get the type of NFDGeneralStatus metric
    auto a = args.empty()? "all"s : args;
    LOG("nfdGSProbe to get metric " + a);

    if(GSmetrics.find(a) == GSmetrics.end()) {
        LOG("no match for metric");
        return std::string("No NFDGeneralStatus entry for " + a);
    }
    ndn::nfdManagementQ fetcher;
    ndn::nfd::ForwarderStatus status;
    try {
        fetcher.run(("/localhost/nfd/status/general"));
        status = ndn::nfd::ForwarderStatus((fetcher.dataVal()).getContent());
        LOG(status);
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    std::stringstream result;
    if(a.compare("Uptime") == 0) {
        result << a <<": " << (status.getCurrentTimestamp() - status.getStartTimestamp());
        return result.str();
    }
    result << status;
    if(a.compare("all") == 0)
        return result.str();
    std::size_t l = result.str().find(a);
    std::size_t e = result.str().find("\n", l+1);
    e -= l;
    return result.str().substr(l, e-1);
}

/*
 * nfdRIBProbe makes use of the RIB Mangement module RIB Dataset
 * https://redmine.named-data.net/projects/nfd/wiki/RibMgmt
 * RIB Dataset at: /localhost/nfd/rib/list and /localhop/nfd/rib/list
*/

/*
 * method to parse ndn::Block elements into a vector of T
 */
#include <vector>

template<typename T>
static std::vector<T>
parseDatasetVector(ndn::Block b, uint32_t t)
{
    std::vector<T> result;

    if(!b.hasWire()) {
        throw "block not in wire format";
        return result;
    }
    b.parse();      //to access elements
    for(auto e=b.elements_begin(); e != b.elements_end(); ++e) {
      if(e->type() != t)
          throw "block element doesn't match passed type";
      result.emplace_back(*e);
    }
    return result;
}

// empty argument string means to get list, otherwise look for match
static std::string nfdRIBProbe(const std::string& args) {
    LOG("nfdRIBProbe for prefix " + args);
    ndn::nfdManagementQ fetcher;
    std::vector<ndn::nfd::RibEntry> dataset;
    std::stringstream result;
    try {
        fetcher.run(("/localhost/nfd/rib/list"));
        dataset = parseDatasetVector<ndn::nfd::RibEntry>
            (std::move((fetcher.dataVal()).getContent()), ndn::tlv::nfd::RibEntry);
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    for(auto&& r:  dataset) {
        LOG(r);
        result << r;
    }
    if(!args.empty()) {
        //search the string for Prefix match(es)
        std::string prefixList;
        std::size_t l=0;
        std::size_t p = result.str().size() - args.size();
        while(l < p)
        {
            l = result.str().find(args, l);
            if(l == std::string::npos)
                break;
            std::size_t e = result.str().find(",",l);
            e -= l;
            prefixList += (result.str().substr(l,e) + " ");
            l += e;
            LOG(prefixList);
        }
        return prefixList;
    }
    return result.str();
}

/*
 * nfdStrategyProbe gets the list of Strategy Choices
 */

#include <ndn-cxx/mgmt/nfd/strategy-choice.hpp>

static std::string nfdStrategyProbe(const std::string& args) {
    //uses NFD management commands
    LOG("nfdRoutePrefixProbe with argument " + args);
    if(!args.empty())
        std::cerr << "nfdStrategyProbe arguments not implemented yet" << std::endl;

    ndn::nfdManagementQ fetcher;
    std::vector<ndn::nfd::StrategyChoice> dataset;
    std::stringstream result;
    try {
        fetcher.run(("/localhost/nfd/strategy-choice/list"));
        dataset = parseDatasetVector<ndn::nfd::StrategyChoice>
            (std::move((fetcher.dataVal()).getContent()), ndn::tlv::nfd::StrategyChoice);
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    for(auto&& r:  dataset) {
        LOG(r);
        result << r;
    }
    return result.str();
}

/*
 * nfdFSProbe gets the Face Status Block
 * Currently "all" is the only argument implemented
 */

// #include <ndn-cxx/mgmt/nfd/face-traits.hpp>
#include <ndn-cxx/mgmt/nfd/face-status.hpp>

static std::string nfdFSProbe(const std::string& args) {
    LOG("nfdFaceStatusProbe with argument " + args);

    ndn::nfdManagementQ fetcher;
    std::vector<ndn::nfd::FaceStatus> dataset;
    std::stringstream result;
    try {
        fetcher.run(("/localhost/nfd/faces/list"));
        dataset = parseDatasetVector<ndn::nfd::FaceStatus>
            (std::move((fetcher.dataVal()).getContent()), ndn::tlv::nfd::FaceStatus);
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
    for(auto&& r:  dataset) {
        LOG(r);
        //if(args.compare("all")) {} for selective argument
        result << r;
    }
    return result.str();
}

/*
 * Probe that uses reply to return location for on-going data.
 * This is really rough, just testing out whether this is possible.
 * The spacing of the General Status queries is a parameter but the
 * probe just does it for a lifetime that is set to 5 times the period
 */

#include <thread>

void periodicReporter(int interval, int lifeTime) {
    int n=0;
    using namespace std::chrono;
    auto stopTime = system_clock::now() + milliseconds(lifeTime);

    while(system_clock::now()  < stopTime) {
        //sleep for interval, then gather data and output
        std::this_thread::sleep_for(milliseconds(interval));
        std::cout << "\nReport number: " << n << std::endl;
        std::cout << nfdGSProbe("");
        n++;
    }
    std::cout << "\nPeriodic reporting ends after " << n << " reports" << std::endl;
}

static std::string periodicProbe(const std::string& args) {
    try {
        int per = std::stoi(args);
        int lt = 5*per; //this should also be part of args in future
        //set up a location for on-going data
        //might involve negotiation with an archive?
        std::thread (periodicReporter, per, lt).detach();
        //return the  the data location
        return("Reports at std::out of nod");
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return(""); //no location returned, should be consdered an error
    }

}

