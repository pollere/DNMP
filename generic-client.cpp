/*
 * generic-client.cpp: DNMP generic client, a command-line client
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
 *  DNMP is not intended as production code. 
 *  More information on DNMP is available from info@pollere.net
 */

/*
 * Generic Client for DNMP
 */

#include <getopt.h>
#include <charconv>
#include <functional>
#include <iostream>
#include <chrono>

/*
 * The CRshim object in CRshim.hpp provides the Command/Reply API from
 * DNMP applications to the PubSub sync protocol.
 */
#include "CRshim.hpp"

/* 
 * This is for command-line:
 *   generic-client -p probe_name -a probe_arguments -t target
 *                  -c count -i interval 
 *-w maximum_wait_time_for_reply
 */

// handles command line
static struct option opts[] = {
    {"probe", required_argument, nullptr, 'p'},
    {"arguments", required_argument, nullptr, 'a'},
    {"target", required_argument, nullptr, 't'},
    {"interval", required_argument, nullptr, 'i'},
    {"count", required_argument, nullptr, 'c'},
    {"debug", no_argument, nullptr, 'd'},
    {"help", no_argument, nullptr, 'h'}
};
static void usage(const char* cname)
{
    std::cerr << "usage: " << cname << " [flags] -p probe_name\n";
}
static void help(const char* cname)
{
    usage(cname);
    std::cerr << " flags:\n"
           "  -p |--probe name     name of probe\n"
           "\n"
           "  -a |--arguments args optional probe arguments\n"
           "  -t |--target name    probe target: local|all|name\n"
           "\n"
           "  -c |--count          number of requests to send\n"
           "  -i |--interval       time between requests (sec)\n"
           "  -d |--debug          enable debugging output\n"
           "  -h |--help           print help then exit\n";
}


static int debug = 0;
static int count = 1;
static ndn::time::nanoseconds interval = 1_s;
static std::string target("local");
static std::string ptype;
static std::string pargs;
static Timer timer;

/*
 * doFinish is the callback function for the timer
 * and is also called if the target is local and just one NOD
 * so that the client will exit.
 */
void doFinish()
{
    exit(0);
}

/* processReply contains the code for how this client will process
 * the reply from the NOD probe. This is the callback function for
 * reception of a reply from a nod.
 */
void processReply(const Reply& pub, CRshim& shim)
{
    const auto& c = pub.getContent();
    if (c.value_size() > 0) {
        std::cout << std::string((const char*)(c.value()), c.value_size()) << "\n";
    }

    // Using the reply timestamps to print cli-to-nod & nod-to-cli times
    std::cout << "Reply from " << pub["rSrcId"] << ": timing (in sec.): "
              << "to NOD=" + to_string(pub.timeDelta("rTimestamp", "cTimestamp"))
              << "  from NOD=" + to_string(pub.timeDelta("rTimestamp")) << std::endl;

    if (--count > 0) {
        // wait then launch another command
        timer = shim.schedule(interval, [&shim]() {
            shim.issueCmd(ptype, pargs, processReply);
        });
        return;
    }
    if (target == "all") {
        // wait for more replies
        timer = shim.schedule(interval, []() { doFinish(); });
        return;
    }
    doFinish();
}

/*
 * Main for a Generic Client that passes the probe type and arguments
 * and prints the reply.
 * Other probes can be modeled on this template but may do formating
 * or other operations on the reply.
 */
int main(int argc, char* argv[])
{
    // parse input line, exit if not a good probe directive
    if (argc <= 1) {
        help(argv[0]);
        return 1;
    }
    for (int c;
         (c = getopt_long(argc, argv, "p:a:t:c:i:dh", opts, nullptr)) != -1;) {
        switch (c) {
        case 'p':
            ptype = optarg;
            break;
        case 'a':
            pargs = optarg;
            break;
        case 't':
            target = optarg;
            break;
        case 'c':
            int rint;
            if (auto [p,ec] = std::from_chars(optarg, optarg+strlen(optarg), rint);
                ec == std::errc() && rint >= 1 && rint <= 10000) {
                count = rint;
            }
            break;
        case 'i':
            double rdbl;
            rdbl = std::stod(optarg);
            if (rdbl >= 0.01) {
                interval = boost::chrono::nanoseconds((int)(rdbl * 1e9));
            }
            break;
        case 'd':
            ++debug;
            break;
        case 'h':
            help(argv[0]);
            exit(0);
        }
    }
    if (optind < argc || ptype.empty() || target.empty()) {
        usage(argv[0]);
        return 1;
    }

    try {
        // make a CRshim with this target
        CRshim s(target);
        // builds and publishes command and waits for reply
        s.doCommand(ptype, pargs, processReply);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
}
