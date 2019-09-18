/*
 * nodpoc.cpp: proof-of-concept DNMP NOD
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


/*
 * Proof of Concept DNMP NOD.
 */

#include <getopt.h>
#include <unistd.h>
#include <functional>
#include <iostream>
#include <random>
#include <unordered_map>

#include "CRshim.hpp"      //DNMP command-reply shim

/* Probes return a string (that can be converted to a NDN object for Content
 * field) Permissions should be checked before calling probe. DNMP keywords for
 * probes are used here to pair with the appropriate probe function in
 * probeList.
 */

#include "probes.hpp"

using pb_f = std::function<std::string(const std::string&)>;

const static std::unordered_map<std::string, pb_f> probeTable = {
    {"perNFDGS"s, periodicProbe},
    {"NFDStrategy"s, nfdStrategyProbe},
    {"NFDRIB"s, nfdRIBProbe},
    {"NFDGeneralStatus"s, nfdGSProbe},
    {"NFDFaceStatus"s, nfdFSProbe},
    {"Pinger"s, echoProbe}
};

static int debug{};

/*
 * probeDispatch gets function from probeTable and passes the probe arguments.
 * For asynchronous probes, need a way to callback to shim method to publish reply.
 * This could require a different shim and different type of publication.
 * Asynchronous probes can publish a reply with the location of their output.
 */
static void probeDispatch(RName&& r, CRshim& shim)
{
    try {
        shim.sendReply(r, probeTable.at(r.str("pType"))(r.str("pArgs")));
    } catch (const std::exception& e) {
        std::cerr << e.what() << " for: " << r << std::endl;
    }
}

static struct option opts[] = {
    {"debug", no_argument, nullptr, 'd'},
    {"help", no_argument, nullptr, 'h'}
};

static void usage(const char* cname)
{
    std::cerr << "usage: " << cname << " [--debug]\n";
}

/*
 * DNMP NOD
 */

int main(int argc, char* argv[])
{
    //initialization
    for (int c;
         (c = getopt_long(argc, argv, "dh", opts, nullptr)) != -1;) {
        switch (c) {
        case 'd':
            ++debug;
            break;
        case 'h':
            usage(argv[0]);
            exit(0);
        }
    }

    // make a CRshim for each target we respond to (all using the same
    // face so they'll share the same event hander).

    auto shims{CRshim::shims("local", "all", CRshim::myPID())};
    for (auto& s : shims) s.waitForCmd(probeDispatch);

    try {
        shims[0].run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
}
