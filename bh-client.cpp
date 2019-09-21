/*
 * bh-client.cpp: prototype black hole DNMP client
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
 *  The DNMP is not intended as production code. 
 *  More information on DNMP is available from info@pollere.net
 */

#include <getopt.h>
#include <functional>
#include <iostream>

/* The CRshim object in CRshim.hpp provides the Command/Reply API from
 * DNMP applications to the PubSub sync protocol, syncps.
 */
#include "CRshim.hpp"

/* Blackhole utility command-line DNMP Client.
 *    bhClient -p prefix_name -w maximum_wait_time_for_reply -t target
 *       where the -w and -t arguments are not required (have defaults)
 * This is not exactly the same as the blackhole utility described in the paper
 * since it is making use of the NFDRIB management interface for now.
 */

// handles command line
static struct option opts[] = {
    {"probe", required_argument, nullptr, 'p'},
    {"target", required_argument, nullptr, 't'},
    {"wait", required_argument, nullptr, 'w'},
    {"debug", no_argument, nullptr, 'd'},
    {"help", no_argument, nullptr, 'h'}
};
static void usage(const char* cname)
{
    std::cerr << "usage: " << cname << " [flags] -p probe_name -t target\n";
}
static void help(const char* cname)
{
    usage(cname);
    std::cerr << " flags:\n"
           "  -p |--prefix name     prefix name\n"
           "  -t |--target name    probe target: local|all|name\n"
           "\n"
           "  -w |--wait time      longest time to wait for a reply (ms)\n"
           "  -d |--debug          enable debugging output\n"
           "  -h |--help           print help then exit\n";
}

static int debug{0};

/*
 * bhFinish is the callback function for the timer
 * and is also called if the target is local and just one NOD
 * so that the client will exit.
*/

static int nReply = 0;     //reply counter
static int nBH = 0;        //blackhole counter
static ndn::time::nanoseconds interval = 3_s;
static std::string target("all");  //default
static Timer timer;

static void bhFinish()
{
    std::cout << "Blackhole Utility finished with " << nReply <<
            " NODs replying and " << nBH << " blackhole(s)" << std::endl;
    exit(0);
}

/*
 * blackholeReply is the callback function whenever this client receives
 *  a reply from a NOD that received the nod/all/command/NFDRIB/prefix
 */
static void blackholeReply(const Reply& r, CRshim& shim)
{
    // Using the reply timestamps to print cli-to-nod & nod-to-cli times
    std::cout << "Reply from NOD " << r["rSrcId"] << " took "
              << to_string(r.timeDelta("rTS", "cTS")) << " secs to, "
              << to_string(r.timeDelta("rTS")) << " from." << std::endl;
    nReply++;
    const auto& c = r.getContent();
    if(!c.empty()) {
        std::cout << "\tHas route to: " <<
            std::string((const char*)(c.value()), c.value_size()) << std::endl;
    } else {
        std::cout << "\tDoes not have a route to prefix" << std::endl;
        nBH++;
    }
    if (target == "all") {
        // wait for more replies
        timer = shim.schedule(interval, []() { bhFinish(); });
        return;
    }
    bhFinish();
}

// main for DNMP BlackHole Client
int main(int argc, char* argv[])
{
    std::string pargs;

    // parse input line, exit if not a good probe directive
    if (argc <= 1) {
        help(argv[0]);
        return 1;
    }
    for (int c;
         (c = getopt_long(argc, argv, "p:t:w:dh", opts, nullptr)) != -1;) {
        switch (c) {
        case 'p':
            pargs = optarg;
            break;
        case 't':
            target = optarg;
            break;
        case 'w':
            interval = (ndn::time::nanoseconds) atoi(optarg);
            break;
        case 'd':
            ++debug;
            break;
        case 'h':
            help(argv[0]);
            exit(0);
        }
    }
    if (optind < argc || pargs.empty()) {
        usage(argv[0]);
        return 1;
    }

    LOG("Blackhole utility for prefix: " + pargs);

    try {
        // make a CRshim with this target
        CRshim s(target);
        timer = s.schedule(interval, []() { bhFinish(); });
        s.doCommand("NFDRIB", pargs, blackholeReply);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
}
