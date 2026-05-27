CC      := gcc
CFLAGS  := -Wall -Wextra -Wno-implicit-fallthrough \
           -std=c11 -O2 -D_GNU_SOURCE
PKGCF   := $(shell pkg-config --cflags ncurses 2>/dev/null | \
             sed 's/-D_XOPEN_SOURCE=[0-9]*//g')
CFLAGS  += $(PKGCF)
LDFLAGS :=
LIBS    := $(shell pkg-config --libs ncurses 2>/dev/null || echo -lncurses) \
           -lpanel -lutil

TARGET  := ncwm
SRCDIR  := src
SRCS    := $(SRCDIR)/main.c \
           $(SRCDIR)/vt100.c \
           $(SRCDIR)/window.c \
           $(SRCDIR)/wm.c
OBJS    := $(SRCS:.c=.o)
HEADER  := $(SRCDIR)/ncwm.h

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

$(SRCDIR)/%.o: $(SRCDIR)/%.c $(HEADER)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/$(TARGET)
