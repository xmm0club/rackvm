CC ?= cc
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
CFLAGS ?= -O2 -g
CPPFLAGS += -Iinclude
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -Wstrict-prototypes -fstack-protector-strong
LDFLAGS += -Wl,-z,relro,-z,now
LDLIBS += -pthread

SOURCES := src/main.c src/config.c src/util.c src/serial.c src/virtio_blk.c src/sandbox.c src/vm.c src/devirt.c
OBJECTS := $(SOURCES:src/%.c=build/%.o)
TARGET := build/rackvm
SANDBOX_TEST := build/sandbox-test

.PHONY: all clean check install uninstall demo-initramfs

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

build/%.o: src/%.c include/rackvm.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

check: $(TARGET) $(SANDBOX_TEST)
	./tests/check.sh ./$(TARGET)
	./$(SANDBOX_TEST)

$(SANDBOX_TEST): tests/sandbox.c build/sandbox.o include/rackvm.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/sandbox.c build/sandbox.o

demo-initramfs:
	./tools/mkinitramfs.sh examples/assets/initramfs

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/rackvm

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/rackvm

clean:
	rm -rf build

-include $(OBJECTS:.o=.d)
