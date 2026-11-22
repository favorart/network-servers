# Builds the chat server (epoll) and its console client (select), Linux only.
BIN      := chat-server chat-client
CFLAGS   ?= -O2 -g
CXXFLAGS ?= -O2 -g

all: $(BIN)

chat-server: epoll.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 $< -o $@ $(LDFLAGS)

chat-client: select.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -std=c++11 $< -o $@ $(LDFLAGS)

clean:
	rm -f $(BIN)

print-bin:
	@echo $(BIN)

.PHONY: all clean print-bin
