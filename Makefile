# make            build libsquash.a and the squash CLI
# make test       unit, round-trip, robustness and interop tests
# make SAN=1 test the same under AddressSanitizer + UndefinedBehaviorSanitizer
# make bench      ratio and speed of every codec and the system tools
# make fuzz       libFuzzer over every decoder (clang)

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

LIB_SRC := $(filter-out src/cli.c src/bench.c,$(wildcard src/*.c))
LIB_OBJ := $(LIB_SRC:src/%.c=$(BUILD)/obj/%.o)
LIB     := $(BUILD)/libsquash.a
CLI     := $(if $(wildcard src/cli.c),$(BUILD)/squash)
TESTS   := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))

.PHONY: all test bench fuzz clean
all: $(LIB) $(CLI)

$(BUILD)/obj/%.o: src/%.c $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(CLI): $(BUILD)/obj/cli.o $(BUILD)/obj/bench.o $(LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/test_%: tests/test_%.c tests/test.h $(LIB)
	$(CC) $(CFLAGS) -Itests $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

test: $(TESTS) $(CLI)
	@fail=0; for t in $(TESTS); do $$t || fail=1; done; \
	sh tests/interop.sh $(CLI) || fail=1; \
	exit $$fail

# make bench [BENCH_FILES="silesia/*"]: generated data when no files given.
bench: $(CLI)
	$(CLI) bench $(BENCH_FILES)

# make fuzz [FUZZ_TIME=60]: libFuzzer over all decoders (needs clang).
FUZZ_TIME ?= 60
fuzz: $(CLI)
	@mkdir -p build-fuzz/corpus
	sh fuzz/seed.sh $(CLI) build-fuzz/corpus
	clang -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g -Isrc '-DSQ_BUF_MAX=((size_t)1<<26)' \
	    -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
	    fuzz/fuzz_decode.c $(LIB_SRC) -lm -o build-fuzz/fuzz_decode
	build-fuzz/fuzz_decode -max_total_time=$(FUZZ_TIME) -max_len=65536 build-fuzz/corpus

clean:
	rm -rf build build-san build-fuzz
