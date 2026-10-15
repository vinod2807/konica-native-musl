# konica-native — pure libc, no dependencies. Builds with gcc or clang, glibc or musl.
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11 -D_POSIX_C_SOURCE=200809L
SRC      = src/main.c src/raster.c src/encode.c src/jbig.c src/halftone.c src/acmm.c

all: konica-native

konica-native: $(SRC) src/raster.h src/encode.h src/jbig.h src/halftone.h src/acmm.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

# Reader cross-check needs libcups headers (apt install libcups2-dev / apk add cups-dev)
tests/libcups_dump: tests/libcups_dump.c
	$(CC) -O2 -Wall -Wno-deprecated-declarations -o $@ $< -lcups

check-reader: konica-native tests/libcups_dump
	sh tests/check-reader.sh

clean:
	rm -f konica-native tests/libcups_dump

.PHONY: all check-reader clean
