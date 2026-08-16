# MC micro programmer -- C implementation.  See ../spec.md.
#
#   make          build mcprog
#   make test     run the conformance suite against testdata/
#   make smoke    drive the TUI through a pty
#   make check    all of the above plus a Windows cross-compile
#
# C99, no dependencies.  The codeplug and protocol layers are free of UI and of I/O so they test
# headlessly; ncurses arrives with the TUI in M3.

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -Wshadow -Wconversion -Iinclude
LDFLAGS ?=

# serial_win32.c self-excludes with #ifdef _WIN32, so it can sit in SRC on every platform.
SRC   := src/codeplug.c src/model.c src/dump.c src/protocol.c src/replay.c \
         src/fakeradio.c src/serial_posix.c src/serial_win32.c
OBJ   := $(SRC:src/%.c=build/%.o)
ROOT  ?= .

LIBUTIL ?= $(shell uname -s | grep -q Linux && echo -lutil)
CURSES ?= $(shell pkg-config --libs ncursesw 2>/dev/null || echo -lncurses)

all: build/mcprog

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	@mkdir -p build

build/mcprog: src/main.c src/ui/tui.c $(OBJ) | build
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(CURSES)

build/test_vectors: tests/test_vectors.c $(OBJ) | build
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

build/test_protocol: tests/test_protocol.c $(OBJ) | build
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

build/test_serial: tests/test_serial.c $(OBJ) | build
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LIBUTIL)

test: build/test_vectors build/test_protocol build/test_serial
	@./build/test_vectors $(ROOT)
	@./build/test_protocol $(ROOT)
	@./build/test_serial $(ROOT)

# Drives the real ncurses binary through a pty; the M3 criteria are behavioural.
smoke: build/mcprog
	@python3 tests/tui_smoke.py

# Everything: unit + conformance + pty smoke + the Windows cross-compile.
check: all test smoke win-check

# Cross-compile check for the Windows transport.  Needs mingw-w64; skipped silently without it.
MINGW ?= x86_64-w64-mingw32-gcc
win-check:
	@command -v $(MINGW) >/dev/null || { echo "  (mingw-w64 not installed, skipping)"; exit 0; }; \
	mkdir -p build/win; rc=0; \
	for f in $(SRC) src/main.c; do \
	  $(MINGW) -std=c99 -Wall -Wextra -Wshadow -Wconversion -Iinclude -c $$f \
	     -o build/win/`basename $$f .c`.o || rc=1; \
	done; \
	[ $$rc = 0 ] && echo "  windows cross-compile OK (portable core + Win32 transport)"; \
	echo "  note: src/ui/tui.c needs ncurses, so it is built under MSYS2 rather than here"; \
	exit $$rc

clean:
	rm -rf build

.PHONY: all test smoke check clean win-check
