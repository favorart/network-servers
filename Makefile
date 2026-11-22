# Builds and tests every server (Linux only):
#   make [all|test|clean|dist|bench|format|format-check] [SAN=1 for ASan + UBSan]
SERVERS := cache-server chat-server dictionary-server http-server proxy-server subscribe-server

# Version from the latest tag (0.0.5, 0.0.5-3-gabc1234); CI passes the next tag.
VERSION ?= $(shell git describe --tags --match 'v[0-9]*' 2>/dev/null | sed 's/^v//')
ifeq ($(VERSION),)
VERSION := 0.0.0
endif
export CPPFLAGS += -DNS_VERSION=\"$(VERSION)\"

all:
	for s in $(SERVERS); do $(MAKE) -C $$s all || exit 1; done

test:
	for s in $(SERVERS); do $(MAKE) -C $$s test || exit 1; done

clean:
	for s in $(SERVERS); do $(MAKE) -C $$s clean || exit 1; done
	rm -f network-servers-*.tar.gz
	rm -rf bench/build

# Release archive with all the binaries: network-servers-<version>-linux-<arch>.tar.gz
DIST := network-servers-$(VERSION)-linux-$(shell uname -m)
dist: all
	mkdir $(DIST)
	for s in $(SERVERS); do for b in $$($(MAKE) -s -C $$s print-bin); do cp $$s/$$b $(DIST)/; done; done
	for s in $(SERVERS); do if [ -d $$s/data ]; then cp $$s/data/* $(DIST)/; fi; done
	tar -czf $(DIST).tar.gz $(DIST)
	rm -r $(DIST)

# Benchmark: every server on a free port, BENCH_SECONDS per scenario, one Markdown table on
# stdout (a copy in BENCH_OUT if set). Always the optimized build without sanitizers.
BENCH_SECONDS ?= 3
BENCH_OUT ?=
bench/build/ns-bench: bench/ns-bench.c
	mkdir -p $(@D)
	$(CC) -O2 -g -std=gnu11 -Wall -Wextra -Wpedantic -Werror -pthread $< -o $@

bench: bench/build/ns-bench
	$(MAKE) all SAN=
	./bench/build/ns-bench -t $(BENCH_SECONDS) $(if $(BENCH_OUT),-o $(BENCH_OUT))

# Formatting: clang-format 18 (CI pins 18.1.8: pipx install clang-format==18.1.8)
CLANG_FORMAT ?= clang-format
FORMAT_SRCS := $(shell git ls-files '*.c' '*.h' '*.cpp')
format:
	$(CLANG_FORMAT) -i $(FORMAT_SRCS)

format-check:
	$(CLANG_FORMAT) --dry-run -Werror $(FORMAT_SRCS)

.PHONY: all test clean dist bench format format-check
