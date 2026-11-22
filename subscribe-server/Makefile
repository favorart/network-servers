# Builds the subscribe-server binary from the sources as they are (Linux only).
BIN      := subscribe-server
SRCS     := $(wildcard *.cpp)

CXXFLAGS ?= -O2 -g
override CXXFLAGS += -std=c++11
LDLIBS   += -levent -lpthread

$(BIN): $(SRCS) $(wildcard *.h)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(SRCS) -o $@ $(LDFLAGS) $(LDLIBS)

all: $(BIN)

clean:
	rm -f $(BIN)

print-bin:
	@echo $(BIN)

.PHONY: all clean print-bin
