CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags libevdev)
LDLIBS += $(shell pkg-config --libs libevdev) -lm

BIN = build/omarchy-dtp-palm-rejectiond
TEST = build/test_classifier

all: $(BIN)

build:
	mkdir -p build

$(BIN): src/palm-rejectiond.c src/classifier.c src/config.c src/classifier.h src/config.h | build
	$(CC) $(CFLAGS) -o $@ src/palm-rejectiond.c src/classifier.c src/config.c $(LDLIBS)

$(TEST): tests/test_classifier.c src/classifier.c src/config.c src/classifier.h src/config.h | build
	$(CC) $(CFLAGS) -o $@ tests/test_classifier.c src/classifier.c src/config.c -lm

test: $(TEST)
	./$(TEST)
	bash tests/cli-test.sh

clean:
	rm -rf build

.PHONY: all test clean
