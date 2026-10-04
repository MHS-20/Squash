# make            build libsquash.a and the squash CLI
# make test       unit, round-trip, robustness and interop tests
# make SAN=1 test the same under AddressSanitizer + UndefinedBehaviorSanitizer

CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Wshadow -Isrc

ifeq ($(SAN),1)
BUILD   := build-san
CFLAGS  += -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address,undefined
else
BUILD   := build
endif

LDLIBS  += -lm

LIB_SRC := $(filter-out src/cli.c,$(wildcard src/*.c))
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/obj/%.o)
LIB     := $(BUILD)/libsquash.a
CLI     := $(if $(wildcard src/cli.c),$(BUILD)/squash)
TESTS   := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))

.PHONY: all test clean
all: $(LIB) $(CLI)

$(BUILD)/obj/%.o: src/%.c $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(CLI): $(BUILD)/obj/cli.o $(LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_%: tests/test_%.c tests/test.h $(LIB)
	$(CC) $(CFLAGS) -Itests $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

test: $(TESTS) $(CLI)
	@fail=0; for t in $(TESTS); do $$t || fail=1; done; \
	sh tests/interop.sh $(CLI) || fail=1; \
	exit $$fail

clean:
	rm -rf build build-san
