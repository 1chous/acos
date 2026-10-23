CC     = cc
CFLAGS = -std=c17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -O2 -pthread

SRC = src/config.c src/main.c
HDR = src/elevator.h

all: build/elevator-sem

build/elevator-sem: $(SRC) src/sync_sem.c $(HDR) | build
	$(CC) $(CFLAGS) -o $@ $(SRC) src/sync_sem.c

build:
	mkdir -p build

clean:
	rm -rf build *.log

.PHONY: all clean
