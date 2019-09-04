# on a mac need to set PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
CCFLAGS = -g -O2 -I. -Wall -std=c++17
CCFLAGS += $(shell pkg-config --cflags libndn-cxx)
LIBS = $(shell pkg-config --libs libndn-cxx) -lboost_iostreams-mt
HDRS = CRshim.hpp syncps/syncps.hpp syncps/iblt.hpp
DEPS = $(HDRS)

all: genericCLI nod bhClient

.PHONY: clean distclean tags

genericCLI: generic-client.cpp $(DEPS)
	$(CXX) $(CCFLAGS) -o $@ $< $(LIBS)

bhClient: bh-client.cpp $(DEPS)
	$(CXX) $(CCFLAGS) -o $@ $< $(LIBS)

nod: nod.cpp probes.hpp $(DEPS)
	$(CXX) $(CCFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f genericCLI bhClient nod

distclean: clean
	rm -rf genericCLI.dSYM bhClient.dSYM nod.dSYM
