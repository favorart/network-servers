# dictionary-server (Linux only): make [all|test|clean|print-bin] [SAN=1]
BIN      := dictionary-server
BUILD    := build$(if $(SAN),-san)

SRCS     := $(wildcard src/*.c)
LIB_SRCS := $(filter-out src/srv_main.c,$(SRCS))
HDRS     := $(wildcard src/*.h)

CFLAGS   ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wpedantic -Werror -pthread
LDLIBS   += -levent -lpthread

ifdef SAN
SAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
override CFLAGS  += $(SAN_FLAGS)
override LDFLAGS += $(SAN_FLAGS)
endif

all: $(BUILD)/$(BIN)

$(BUILD)/$(BIN): $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS) $(LDLIBS)

test: all

clean:
	rm -rf build build-san

print-bin:
	@echo $(BUILD)/$(BIN)

.PHONY: all test clean print-bin
