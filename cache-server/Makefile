# cache-server (Linux only): make [all|test|clean|print-bin] [SAN=1] [RUN='setarch -R']
BIN     := cache-server
BUILD   := build$(if $(SAN),-san)

SRCS     := $(wildcard src/*.c)
OBJS     := $(SRCS:src/%.c=$(BUILD)/%.o)
LIB_OBJS := $(filter-out $(BUILD)/cache_main.o,$(OBJS))

CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wpedantic -Werror
override CPPFLAGS += -MMD -MP
ifneq ($(SAN),)
SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
override CFLAGS += $(SANFLAGS)
override LDFLAGS += $(SANFLAGS)
endif
LDLIBS += -levent

# Prefix for running test binaries, e.g. RUN='setarch -R' for ASan under WSL.
RUN ?=

all: $(BUILD)/$(BIN)

$(BUILD)/$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $@

TESTS     := test_hash test_handling test_config test_integration
TEST_BINS := $(TESTS:%=$(BUILD)/%)

$(BUILD)/tests/%.o: tests/%.c | $(BUILD)
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) -c $< -o $@

# The integration test runs the server binary built next to it.
$(BUILD)/tests/test_integration.o: override CPPFLAGS += -DCACHE_SERVER_BIN='"$(abspath $(BUILD)/$(BIN))"'

$(BUILD)/test_%: $(BUILD)/tests/test_%.o $(LIB_OBJS)
	$(CC) $(LDFLAGS) $^ -o $@ -lcmocka $(LDLIBS)

test: all $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do echo "== $$t"; $(RUN) ./$$t; done

clean:
	rm -rf build build-san

print-bin:
	@echo $(BUILD)/$(BIN)

-include $(OBJS:.o=.d) $(TESTS:%=$(BUILD)/tests/%.d)

# keep the test objects between runs
.SECONDARY:

.PHONY: all test clean print-bin
