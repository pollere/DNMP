# on a mac need to set PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
# the code *requires* C++ 17 or later
CXXFLAGS = -g -O2 -I. -Wall -std=c++17
CXXFLAGS += $(shell pkg-config --cflags libndn-cxx)
LIBS = $(shell pkg-config --libs libndn-cxx)
HDRS = CRshim.hpp syncps/syncps.hpp syncps/iblt.hpp
DEPS = $(HDRS)
BINS = genericCLI nod bhClient
JUNK = 

# OS dependent definitions
ifeq ($(shell uname -s),Darwin)
LIBS += -lboost_iostreams-mt
JUNK += $(addsuffix .dSYM,$(BINS))
else
LIBS += -lboost_iostreams
endif

all: $(BINS)

.PHONY: clean distclean tags

genericCLI: generic-client.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIBS)

bhClient: bh-client.cpp $(DEPS)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIBS)

nod: nod.cpp probes.hpp $(DEPS)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f $(BINS)

distclean: clean
	rm -rf $(JUNK)
