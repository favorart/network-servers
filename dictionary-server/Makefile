# dictionary-server (Linux only): make [all|test|clean|print-bin] [SAN=1] [RUN="setarch -R"]
BIN      := dictionary-server
BUILD    := build$(if $(SAN),-san)

SRCS     := $(wildcard src/*.c)
LIB_SRCS := $(filter-out src/srv_main.c,$(SRCS))
HDRS     := $(wildcard src/*.h)
TESTS    := $(BUILD)/test_unit $(BUILD)/test_integration
TEST_DICT := tests/dict_small.txt

CFLAGS   ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wpedantic -Werror -pthread
LDLIBS   += -levent -lpthread
# Prefix for running the test binaries, e.g. RUN="setarch -R" for ASan under WSL.
RUN      ?=

ifdef SAN
SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
override CFLAGS  += $(SAN_FLAGS)
override LDFLAGS += $(SAN_FLAGS)
endif

all: $(BUILD)/$(BIN)

$(BUILD)/$(BIN): $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD)/test_unit: tests/test_unit.c $(LIB_SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Isrc -DTEST_DICT='"$(TEST_DICT)"' $(CFLAGS) $< $(LIB_SRCS) -o $@ \
		$(LDFLAGS) -lcmocka $(LDLIBS)

$(BUILD)/test_integration: tests/test_integration.c
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -DSERVER_BIN='"$(BUILD)/$(BIN)"' -DTEST_DICT='"$(TEST_DICT)"' $(CFLAGS) \
		$< -o $@ $(LDFLAGS) -lcmocka -lpthread

test: $(BUILD)/$(BIN) $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; $(RUN) ./$$t || exit 1; done

clean:
	rm -rf build build-san

print-bin:
	@echo $(BUILD)/$(BIN)

.PHONY: all test clean print-bin
