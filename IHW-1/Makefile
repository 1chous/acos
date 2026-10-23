CC     = cc
CFLAGS = -std=c17 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -O2

SRC = $(wildcard src/*.c)
OBJ = $(SRC:src/%.c=build/%.o)
BIN = build/elevator

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

# rebuild everything when a header changes, the project is too small
# for proper dependency tracking
$(OBJ): $(wildcard src/*.h)

build:
	mkdir -p build

clean:
	rm -rf build *.log

.PHONY: all clean
