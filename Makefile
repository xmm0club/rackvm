CC ?= cc
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
CFLAGS ?= -O2 -g
CPPFLAGS += -Iinclude
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -Wstrict-prototypes -fstack-protector-strong
LDFLAGS += -Wl,-z,relro,-z,now
LDLIBS += -pthread

SOURCES := src/main.c src/config.c src/util.c src/serial.c src/vm.c src/devirt.c
OBJECTS := $(SOURCES:src/%.c=build/%.o)
TARGET := build/rackvm

.PHONY: all clean check install uninstall demo-initramfs

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

build/%.o: src/%.c include/rackvm.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

check: $(TARGET)
	./tests/check.sh ./$(TARGET)

demo-initramfs:
	./tools/mkinitramfs.sh examples/assets/initramfs

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)/man1
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/rackvm
	install -m 0644 docs/rackvm.1 $(DESTDIR)$(MANDIR)/man1/rackvm.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/rackvm $(DESTDIR)$(MANDIR)/man1/rackvm.1

clean:
	rm -rf build

-include $(OBJECTS:.o=.d)
