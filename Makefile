# Builds and tests every server (Linux only):
#   make [all|test|clean|dist|format|format-check] [SAN=1 for ASan + UBSan]
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

# Release archive with all the binaries: network-servers-<version>-linux-<arch>.tar.gz
DIST := network-servers-$(VERSION)-linux-$(shell uname -m)
dist: all
	mkdir $(DIST)
	for s in $(SERVERS); do for b in $$($(MAKE) -s -C $$s print-bin); do cp $$s/$$b $(DIST)/; done; done
	for s in $(SERVERS); do if [ -d $$s/data ]; then cp $$s/data/* $(DIST)/; fi; done
	tar -czf $(DIST).tar.gz $(DIST)
	rm -r $(DIST)

# Formatting: clang-format 18 (CI pins 18.1.8: pipx install clang-format==18.1.8)
CLANG_FORMAT ?= clang-format
FORMAT_SRCS := $(shell git ls-files '*.c' '*.h' '*.cpp')
format:
	$(CLANG_FORMAT) -i $(FORMAT_SRCS)

format-check:
	$(CLANG_FORMAT) --dry-run -Werror $(FORMAT_SRCS)

.PHONY: all test clean dist format format-check
