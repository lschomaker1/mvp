CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

TARGET := mvp
SRC := src/mvp.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)
