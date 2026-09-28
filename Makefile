CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wshadow -Ivendor/cJSON -Isrc
LDLIBS  ?= -lm

BIN     := playsync2
SRC     := $(wildcard src/*.c) vendor/cJSON/cJSON.c
OBJ     := $(SRC:.c=.o)
DEP     := $(OBJ:.o=.d)

TESTBINS := tests/test_sync tests/test_proto
TESTAUX  := tests/fake_mpv

HDRS    := $(wildcard src/*.h) vendor/cJSON/cJSON.h

.PHONY: all clean test e2e fake

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

tests/test_sync: tests/test_sync.c src/sync.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

tests/test_proto: tests/test_proto.c src/proto.c vendor/cJSON/cJSON.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

tests/fake_mpv: tests/fake_mpv.c src/net.c src/timebase.c vendor/cJSON/cJSON.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

test: $(BIN) $(TESTBINS) $(TESTAUX)
	@echo "== invariants ==";        sh tests/check_invariants.sh
	@echo "== unit: sync ==";        ./tests/test_sync
	@echo "== unit: proto ==";       ./tests/test_proto
	@echo "== server protocol ==";   sh tests/run_protocol.sh
	@echo "== mpv exit ==";          sh tests/run_mpv_exit.sh
	@echo "== redundant seek ==";    sh tests/run_redundant_seek.sh
	@echo "== reconnect ==";         sh tests/run_reconnect.sh
	@echo "== fake-mpv integration =="; sh tests/run_fake_mpv.sh

e2e: $(BIN) $(TESTAUX)
	sh tests/run_e2e.sh

clean:
	rm -f $(OBJ) $(DEP) $(BIN) $(TESTBINS) $(TESTAUX)
	rm -f tests/*.d

-include $(DEP)
