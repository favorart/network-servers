# proxy-server: TCP balancing proxy on libevent (Linux only).
# make [all|test|clean|print-bin] [SAN=1] [RUN="setarch -R"]
BIN      := proxy-server
BUILD    := build$(if $(SAN),-san)
SRCS     := $(wildcard src/*.c)
LIB_SRCS := $(filter-out src/proxy_main.c,$(SRCS))
HDRS     := $(wildcard src/*.h)
TESTS    := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))
# Prefix for running the test binaries, e.g. RUN="setarch -R" for ASan under WSL.
RUN      ?=

CFLAGS   ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wpedantic -Werror
ifdef SAN
SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
override CFLAGS  += $(SAN_FLAGS)
override LDFLAGS += $(SAN_FLAGS)
endif
LDLIBS   += -levent

all: $(BUILD)/$(BIN)

$(BUILD):
	mkdir -p $@

$(BUILD)/$(BIN): $(SRCS) $(HDRS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS) $(LDLIBS)

# Every test links all sources except main(); PROXY_BIN is the server for integration tests.
$(BUILD)/test_%: tests/test_%.c $(LIB_SRCS) $(HDRS) $(BUILD)/$(BIN) | $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc -DPROXY_BIN=\"$(BUILD)/$(BIN)\" $(CFLAGS) -pthread $< $(LIB_SRCS) \
		-o $@ $(LDFLAGS) $(LDLIBS) -lcmocka

test: all $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $(RUN) ./$$t || exit 1; done

clean:
	rm -rf build build-san

print-bin:
	@echo $(BUILD)/$(BIN)

.PHONY: all test clean print-bin
