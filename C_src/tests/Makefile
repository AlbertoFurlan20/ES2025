# Host-compiled unit tests for the dependency-free helpers.
# These do NOT use the RTEMS toolchain - they run on the development machine.

CXX      ?= c++
CXXFLAGS  = -std=c++17 -Wall -Wextra -Werror -O1 -g -pthread -I../inc

# One folder per module, mirroring src/ and inc/.
BINS = telemetry/test_fmt bmp_app/test_snapshot

.PHONY: all run clean

all: $(BINS)

telemetry/test_fmt: telemetry/test_fmt.cpp ../inc/telemetry/fmt.h
	$(CXX) $(CXXFLAGS) $< -o $@

bmp_app/test_snapshot: bmp_app/test_snapshot.cpp ../inc/bmp_app/snapshot.h
	$(CXX) $(CXXFLAGS) $< -o $@

run: $(BINS)
	@for b in $(BINS); do ./$$b || exit 1; done

clean:
	rm -rf $(BINS) $(addsuffix .dSYM,$(BINS))
