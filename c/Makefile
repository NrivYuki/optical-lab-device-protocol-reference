# 機器の側の参照の実装(C)。PC(WSL の gcc)でテストと模擬機器を作る。
MONO    = ../third_party/monocypher/monocypher-4.0.3/src
CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -O2 -pthread -Isrc -I$(MONO) -Itests
LIB     = src/oldp_sha256.c src/oldp_cobs.c src/oldp_x25519.c src/oldp_noise.c src/oldp_device.c src/oldp_job.c $(MONO)/monocypher.c

all: test_vectors test_device sim_device

# tests/vectors.h と src/oldp_labels.h は Windows の node で作る(tools/gen-c-vectors.js・tools/gen-c-labels.js)

test_vectors: tests/test_vectors.c tests/vectors.h $(LIB) src/*.h
	$(CC) $(CFLAGS) -o $@ tests/test_vectors.c $(LIB)

test_device: tests/test_device.c $(LIB) src/*.h
	$(CC) $(CFLAGS) -o $@ tests/test_device.c $(LIB)

sim_device: sim/sim_device.c $(LIB) src/*.h
	$(CC) $(CFLAGS) -o $@ sim/sim_device.c $(LIB)

test: test_vectors test_device
	./test_vectors
	./test_device

clean:
	rm -f test_vectors test_device sim_device

.PHONY: all test clean
