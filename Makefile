CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Ivendor/cJSON -Isrc
LDLIBS  ?= -lm

BIN     := playsync2
SRC     := $(wildcard src/*.c) vendor/cJSON/cJSON.c
OBJ     := $(SRC:.c=.o)

TESTBINS := tests/test_sync tests/test_proto

.PHONY: all clean test e2e fake

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

tests/test_sync: tests/test_sync.c src/sync.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

tests/test_proto: tests/test_proto.c src/proto.c src/json_mut.c vendor/cJSON/cJSON.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

tests/fake_mpv: tests/fake_mpv.c src/net.c src/timebase.c src/json_mut.c vendor/cJSON/cJSON.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: $(BIN) $(TESTBINS) tests/fake_mpv
	@echo "== invariants ==";        sh tests/check_invariants.sh
	@echo "== unit: sync ==";        ./tests/test_sync
	@echo "== unit: proto ==";       ./tests/test_proto
	@echo "== server protocol ==";   sh tests/run_protocol.sh
	@echo "== mpv exit ==";          sh tests/run_mpv_exit.sh
	@echo "== reconnect ==";         sh tests/run_reconnect.sh
	@echo "== fake-mpv integration =="; sh tests/run_fake_mpv.sh

e2e: $(BIN) tests/fake_mpv
	sh tests/run_e2e.sh

clean:
	rm -f $(OBJ) $(BIN) $(TESTBINS) tests/fake_mpv
	rm -f vendor/cJSON/cJSON.o
