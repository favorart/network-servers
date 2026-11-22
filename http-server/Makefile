# Builds the http-server binary from the sources as they are (Linux only).
BIN      := http-server
SRCS     := $(wildcard *.c)

CFLAGS   ?= -O2 -g
override CFLAGS += -std=gnu11 -fcommon
LDLIBS   += -levent -lpthread

$(BIN): $(SRCS) $(wildcard *.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS) $(LDLIBS)

all: $(BIN)

clean:
	rm -f $(BIN)

print-bin:
	@echo $(BIN)

.PHONY: all clean print-bin
