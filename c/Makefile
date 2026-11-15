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

# 暗号の部品を別の実装と比べる口(tools/crosscheck.py)
crosscheck: tests/crosscheck.c $(LIB) src/*.h
	$(CC) $(CFLAGS) -o $@ tests/crosscheck.c $(LIB)

# 乱数で作った入力で叩く(tests/fuzz_device.c)。AddressSanitizer と UndefinedBehaviorSanitizer を付ける
fuzz_device: tests/fuzz_device.c $(LIB) src/*.h
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o $@ tests/fuzz_device.c $(LIB)

# 秘密の値によって処理時間が変わらないかを統計で確かめる(tests/timing.c。x86-64 だけ)
timing: tests/timing.c $(LIB) src/*.h
	$(CC) $(CFLAGS) -o $@ tests/timing.c $(LIB) -lm

test: test_vectors test_device
	./test_vectors
	./test_device

clean:
	rm -f test_vectors test_device sim_device crosscheck fuzz_device timing

.PHONY: all test clean
