# cache-server (Linux only): make [all|test|clean|print-bin] [SAN=1] [RUN='setarch -R']
BIN     := cache-server
BUILD   := build$(if $(SAN),-san)

SRCS     := $(wildcard src/*.c)
MAIN_SRC := src/cache_main.c
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

test: all

clean:
	rm -rf build build-san

print-bin:
	@echo $(BUILD)/$(BIN)

-include $(OBJS:.o=.d)

.PHONY: all test clean print-bin
