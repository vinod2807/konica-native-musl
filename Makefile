# konica-native — pure libc, no dependencies. Builds with gcc or clang, glibc or musl.
CC      ?= cc
# -ffp-contract=off keeps single-precision arithmetic bit-exact (no FMA fusion);
# the CMM LUT generator reproduces the vendor's SSE float pipeline exactly.
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11 -ffp-contract=off -D_POSIX_C_SOURCE=200809L
SRC      = src/main.c src/raster.c src/encode.c src/jbig.c src/halftone.c \
           src/acmm.c src/lutgen.c

# Tests that need a captured corpus (see docs/HANDOFF.md): point CORPUS at one.
CORPUS  ?= corpus
# Differential fuzzer: how many cases and which seed to start from.
FUZZ_CASES ?= 50
FUZZ_SEED  ?= 2000

all: konica-native

konica-native: $(SRC) src/raster.h src/encode.h src/jbig.h src/halftone.h \
               src/acmm.h src/lutgen.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

# Reader cross-check needs libcups headers (apt install libcups2-dev / apk add cups-dev)
tests/libcups_dump: tests/libcups_dump.c
	$(CC) -O2 -Wall -Wno-deprecated-declarations -o $@ $< -lcups

check-reader: konica-native tests/libcups_dump
	sh tests/check-reader.sh

# Self-containment check: regenerate the CMM table and compare with the vendor's
tools/lutgen-dump: tools/lutgen-dump.c src/lutgen.c src/lutgen.h
	$(CC) $(CFLAGS) -Isrc -o $@ tools/lutgen-dump.c src/lutgen.c -lm

check-lutgen: tools/lutgen-dump
	sh tests/check-lutgen.sh

# Geometry keying: points -> PageSizeName -> pixels, and the forced 600 dpi.
check-geometry: konica-native
	sh tests/check-geometry.sh

# PJL MEDIATYPE golden table (no vendor filter needed).
check-mediatype: konica-native
	sh tests/check-mediatype.sh

# Runtime-data hardening: a broken install must fail cleanly with nothing on stdout.
check-errors: konica-native
	sh tests/check-errors.sh

# Byte-exact differential test over a captured corpus (needs the vendor filter).
check-difftest: konica-native
	sh tests/difftest.sh $(CORPUS)

# Same raster twice, once with MALLOC_PERTURB_: catches reads of uninitialised memory.
check-determinism: konica-native
	sh tests/check-determinism.sh $(CORPUS)

# Real end-to-end documents through pdftopdf/pdftoraster (needs the vendor filter).
check-real: konica-native
	sh tests/real-docs.sh

# Randomised differential fuzzing against the vendor (needs the vendor filter).
check-fuzz: konica-native
	sh tests/fuzz-difftest.sh $(FUZZ_CASES) $(FUZZ_SEED)

# Everything that needs neither the vendor filter nor a corpus.
check: check-reader check-lutgen check-geometry check-mediatype check-errors

# Sanitiser build over the vendor-free tests.  Separate binary so a plain `make` is untouched.
# verify_asan_link_order=0: some hosts preload a library via /etc/ld.so.preload, which makes
# libasan non-first and aborts every run with "ASan runtime does not come first".
check-asan: tests/libcups_dump
	$(CC) $(CFLAGS) -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o konica-native-asan $(SRC) -lm
	ASAN_OPTIONS=verify_asan_link_order=0 NATIVE=$(CURDIR)/konica-native-asan sh tests/check-geometry.sh
	ASAN_OPTIONS=verify_asan_link_order=0 NATIVE=$(CURDIR)/konica-native-asan sh tests/check-mediatype.sh
	ASAN_OPTIONS=verify_asan_link_order=0 NATIVE=$(CURDIR)/konica-native-asan sh tests/check-errors.sh
	ASAN_OPTIONS=verify_asan_link_order=0 NATIVE=$(CURDIR)/konica-native-asan sh tests/check-reader.sh

clean:
	rm -f konica-native konica-native-asan tests/libcups_dump tools/lutgen-dump

.PHONY: all check check-reader check-lutgen check-geometry check-mediatype \
        check-errors check-difftest check-determinism check-real check-fuzz \
        check-asan clean
