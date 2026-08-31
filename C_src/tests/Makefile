# Host-compiled unit tests for the dependency-free telemetry helpers.
# These do NOT use the RTEMS toolchain - they run on the development machine.

CXX      ?= c++
CXXFLAGS  = -std=c++17 -Wall -Wextra -Werror -O1 -g -pthread -I../inc

BINS = test_telem_ring test_telem_fmt test_bmp_app_snapshot

.PHONY: all run clean

all: $(BINS)

test_telem_ring: test_telem_ring.cpp ../inc/telem_ring.h
	$(CXX) $(CXXFLAGS) $< -o $@

test_telem_fmt: test_telem_fmt.cpp ../inc/telem_fmt.h
	$(CXX) $(CXXFLAGS) $< -o $@

test_bmp_app_snapshot: test_bmp_app_snapshot.cpp ../inc/bmp_app_snapshot.h
	$(CXX) $(CXXFLAGS) $< -o $@

run: $(BINS)
	@for b in $(BINS); do ./$$b || exit 1; done

clean:
	rm -f $(BINS)
