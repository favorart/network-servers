# Builds the chat server (epoll) and its console client (select), Linux only.
# make [all|test|clean|print-bin] [SAN=1] [RUN='setarch -R']
BUILD    := build$(if $(SAN),-san)
SERVER   := $(BUILD)/chat-server
CLIENT   := $(BUILD)/chat-client

CFLAGS   ?= -O2 -g
CXXFLAGS ?= -O2 -g
override CFLAGS   += -std=gnu11 -Wall -Wextra -Wpedantic -Werror
override CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -Werror
override CPPFLAGS += -MMD -MP

ifdef SAN
SANFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
override CFLAGS   += $(SANFLAGS)
override CXXFLAGS += $(SANFLAGS)
endif

# Wrapper for running test binaries, e.g. RUN='setarch -R' for ASan under WSL.
RUN ?=

LIB_SRCS := src/chat_server.c
LIB_OBJS := $(LIB_SRCS:src/%.c=$(BUILD)/%.o)

all: $(SERVER) $(CLIENT)

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(SERVER): $(BUILD)/main.o $(LIB_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(CLIENT): $(BUILD)/chat_client.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

TESTS := $(BUILD)/test_chat $(BUILD)/test_server

$(BUILD)/test_%.o: tests/test_%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/test_chat: $(BUILD)/test_chat.o $(LIB_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lcmocka

$(BUILD)/test_server: $(BUILD)/test_server.o
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) -lcmocka

# The integration test starts the server binary named by CHAT_SERVER.
test: all $(TESTS)
	$(RUN) $(BUILD)/test_chat
	CHAT_SERVER=$(SERVER) $(RUN) $(BUILD)/test_server

clean:
	rm -rf build build-san

print-bin:
	@echo $(SERVER) $(CLIENT)

-include $(wildcard $(BUILD)/*.d)

.PHONY: all test clean print-bin
