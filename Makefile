# Builds every server (Linux only): make [all|clean|dist]
SERVERS := cache-server chat-server dictionary-server http-server proxy-server subscribe-server

# Version from the latest tag (0.0.5, 0.0.5-3-gabc1234); CI passes the next tag.
VERSION ?= $(shell git describe --tags --match 'v[0-9]*' 2>/dev/null | sed 's/^v//')
ifeq ($(VERSION),)
VERSION := 0.0.0
endif
export CPPFLAGS += -DNS_VERSION=\"$(VERSION)\"

all:
	for s in $(SERVERS); do $(MAKE) -C $$s all || exit 1; done

clean:
	for s in $(SERVERS); do $(MAKE) -C $$s clean || exit 1; done
	rm -f network-servers-*.tar.gz

# Release archive with all the binaries: network-servers-<version>-linux-<arch>.tar.gz
DIST := network-servers-$(VERSION)-linux-$(shell uname -m)
dist: all
	mkdir $(DIST)
	for s in $(SERVERS); do for b in $$($(MAKE) -s -C $$s print-bin); do cp $$s/$$b $(DIST)/; done; done
	tar -czf $(DIST).tar.gz $(DIST)
	rm -r $(DIST)

.PHONY: all clean dist
