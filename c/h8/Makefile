# H8/3069F の内蔵フラッシュで動くファームウェア(WSL の中で make)。kz_h8write.ps1 で書き込む oldp-h8.mot を作る。
PREFIX  = $(HOME)/h8300/tools
CC      = $(PREFIX)/bin/h8300-elf-gcc
OBJCOPY = $(PREFIX)/bin/h8300-elf-objcopy
SIZE    = $(PREFIX)/bin/h8300-elf-size
MONO    = ../../third_party/monocypher/monocypher-4.0.3/src
# gcc 16.2.0 の H8/300H 向けの、局所変数を退避したレジスタと同じ場所に置く不具合(tools/check-frames.py で見つける)は、
# フレームポインタをいつも使えば避けられる。フラッシュには余裕があるので、大きくなっても使う。
CFLAGS  = -mh -Os -std=c99 -Wall -Wextra -nostdinc -isystem . -isystem $(shell $(CC) -print-file-name=include) \
          -ffreestanding -fno-builtin -fno-omit-frame-pointer -ffunction-sections -fdata-sections \
          -DOLDP_FRAME_MAX=512 -I. -I../src -I$(MONO)
LDFLAGS = -static -nostdlib -T rom.ld -Wl,--gc-sections
SRCS    = main.c net.c rtl8019.c store.c libc_min.c ctmul.c ../src/oldp_sha256.c ../src/oldp_cobs.c ../src/oldp_x25519.c ../src/oldp_noise.c ../src/oldp_device.c ../src/oldp_job.c $(MONO)/monocypher.c

# 本体だけ(tools/h8-update.ps1 で更新するとき)
oldp-h8.mot: oldp-h8.elf
	$(OBJCOPY) -O srec $< $@

# 更新の仕組みと本体を合わせたもの(DIP スイッチの書き込みの位置で kz_h8write.ps1 で書くとき)
oldp-h8-all.mot: oldp-h8.mot loader/loader.mot
	# 番地の小さい更新の仕組みは S1、本体は S2 で書かれている
	grep -h '^S[12]' loader/loader.mot oldp-h8.mot > all.tmp
	(head -1 oldp-h8.mot; cat all.tmp; echo S804000100FA) > $@
	rm all.tmp

all: oldp-h8.mot oldp-h8-all.mot

oldp-h8.elf: crt0.s store_asm.s $(SRCS) rom.ld devkey.h job.h ../src/*.h
	mkdir -p asm
	for f in $(SRCS); do $(CC) $(CFLAGS) -S -o asm/$$(basename $$f .c).s $$f || exit 1; done
	python3 ../../tools/check-frames.py asm/*.s
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ crt0.s store_asm.s $(SRCS) -lgcc
	$(SIZE) $@

# 試験用の組み立て: 乱数に混ぜる文字の到着の時刻を記録する rngtrace コマンドを加える。製品の基板には書かない
trace: oldp-h8-trace.mot

oldp-h8-trace.mot: oldp-h8-trace.elf
	$(OBJCOPY) -O srec $< $@

oldp-h8-trace.elf: crt0.s store_asm.s $(SRCS) rom.ld devkey.h job.h ../src/*.h
	mkdir -p asm-trace
	for f in $(SRCS); do $(CC) $(CFLAGS) -DOLDP_RNG_TRACE -S -o asm-trace/$$(basename $$f .c).s $$f || exit 1; done
	python3 ../../tools/check-frames.py asm-trace/*.s
	$(CC) $(CFLAGS) -DOLDP_RNG_TRACE $(LDFLAGS) -o $@ crt0.s store_asm.s $(SRCS) -lgcc
	$(SIZE) $@

# 未定義の動作を確かめる組み立て: 未定義の動作の手前で abort を呼び、番地を返して止まる(int が 16 ビットのこの CPU で確かめるため)。
# 製品の基板には書かない
UBFLAGS = -DOLDP_UBSAN -fsanitize=undefined -fsanitize-trap=undefined
ubsan: oldp-h8-ub.mot

oldp-h8-ub.mot: oldp-h8-ub.elf
	$(OBJCOPY) -O srec $< $@

oldp-h8-ub.elf: crt0.s store_asm.s $(SRCS) rom.ld devkey.h job.h ../src/*.h
	mkdir -p asm-ub
	for f in $(SRCS); do $(CC) $(CFLAGS) $(UBFLAGS) -S -o asm-ub/$$(basename $$f .c).s $$f || exit 1; done
	python3 ../../tools/check-frames.py asm-ub/*.s
	# フラッシュを書く手順(store.c)は RAM に写して動かすので、大きくしないよう確かめを入れない
	$(CC) $(CFLAGS) -c -o asm-ub/store.o store.c
	$(CC) $(CFLAGS) $(UBFLAGS) $(LDFLAGS) -o $@ crt0.s store_asm.s asm-ub/store.o $(filter-out store.c,$(SRCS)) -lgcc
	$(SIZE) $@

clean:
	rm -rf oldp-h8.elf oldp-h8.mot asm oldp-h8-trace.elf oldp-h8-trace.mot asm-trace oldp-h8-ub.elf oldp-h8-ub.mot asm-ub

.PHONY: clean trace ubsan
