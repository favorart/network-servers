# subscribe-server (Linux only). Targets: all, test, clean, print-bin.
# SAN=1 builds with ASan+UBSan into build-san/. RUN is a prefix for test binaries
# (e.g. RUN='setarch -R' where ASan needs ASLR off).
BIN      := subscribe-server
BUILD    := build$(if $(SAN),-san)

SRCS     := $(wildcard src/*.cpp)
MAIN_SRC := src/subs_main.cpp
LIB_OBJS := $(patsubst src/%.cpp,$(BUILD)/obj/%.o,$(filter-out $(MAIN_SRC),$(SRCS)))
MAIN_OBJ := $(patsubst src/%.cpp,$(BUILD)/obj/%.o,$(MAIN_SRC))

SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

CXXFLAGS ?= -O2 -g
override CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -Werror -MMD -MP
ifneq ($(SAN),)
override CXXFLAGS += $(SANFLAGS)
override LDFLAGS  += $(SANFLAGS)
endif
LDLIBS   += -levent

RUN ?=

all: $(BUILD)/$(BIN)

$(BUILD)/$(BIN): $(MAIN_OBJ) $(LIB_OBJS)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/obj/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

test: all

clean:
	rm -rf build build-san

print-bin:
	@echo $(BUILD)/$(BIN)

-include $(wildcard $(BUILD)/obj/*.d)

.PHONY: all test clean print-bin
