# http-server (Linux only): make [all|test|clean|print-bin] [SAN=1]
BIN      := http-server
BUILD    := build$(if $(SAN),-san)

SRCS     := $(wildcard src/*.c)
MAIN_SRC := src/http_server_main.c
LIB_SRCS := $(filter-out $(MAIN_SRC),$(SRCS))
OBJS     := $(SRCS:src/%.c=$(BUILD)/obj/%.o)
LIB_OBJS := $(LIB_SRCS:src/%.c=$(BUILD)/obj/%.o)

CFLAGS   ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wpedantic -Werror
ifneq ($(SAN),)
SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
override CFLAGS  += $(SANFLAGS)
override LDFLAGS += $(SANFLAGS)
endif
LDLIBS   += -levent

all: $(BUILD)/$(BIN)

$(BUILD)/$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/obj/%.o: src/%.c | $(BUILD)/obj
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj:
	mkdir -p $@

test: all

clean:
	rm -rf build build-san

print-bin:
	@echo $(BUILD)/$(BIN)

-include $(OBJS:.o=.d)

.PHONY: all test clean print-bin
