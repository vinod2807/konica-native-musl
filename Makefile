# konica-native — pure libc, no dependencies. Builds with gcc or clang, glibc or musl.
CC      ?= cc
# -ffp-contract=off keeps single-precision arithmetic bit-exact (no FMA fusion);
# the CMM LUT generator reproduces the vendor's SSE float pipeline exactly.
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11 -ffp-contract=off -D_POSIX_C_SOURCE=200809L
SRC      = src/main.c src/raster.c src/encode.c src/jbig.c src/halftone.c \
           src/acmm.c src/lutgen.c

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

check: check-reader check-lutgen

clean:
	rm -f konica-native tests/libcups_dump tools/lutgen-dump

.PHONY: all check check-reader check-lutgen clean
