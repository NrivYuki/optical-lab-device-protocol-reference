# 更新の仕組み(WSL の中で make)。フラッシュの EB0 に入る大きさ(4 KB)に収める。
PREFIX  = $(HOME)/h8300/tools
CC      = $(PREFIX)/bin/h8300-elf-gcc
OBJCOPY = $(PREFIX)/bin/h8300-elf-objcopy
SIZE    = $(PREFIX)/bin/h8300-elf-size
CFLAGS  = -mh -Os -std=c99 -Wall -Wextra -nostdinc -isystem .. -isystem $(shell $(CC) -print-file-name=include) \
          -ffreestanding -fno-builtin -fno-omit-frame-pointer -ffunction-sections -fdata-sections
LDFLAGS = -static -nostdlib -T loader.ld -Wl,--gc-sections
SRCS    = loader.c ../../src/oldp_cobs.c

loader.mot: loader.elf
	$(OBJCOPY) -O srec $< $@

loader.elf: start.s $(SRCS) loader.ld
	mkdir -p asm
	for f in $(SRCS); do $(CC) $(CFLAGS) -S -o asm/$$(basename $$f .c).s $$f || exit 1; done
	python3 ../../../tools/check-frames.py asm/*.s
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.s $(SRCS) -lgcc
	$(SIZE) $@
