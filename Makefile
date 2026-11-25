
CC = gcc
LD = ld
NASM = nasm

CFLAGS = -m32 -ffreestanding -fno-builtin -fno-pic -fno-pie \
         -fno-stack-protector -fno-asynchronous-unwind-tables \
         -fno-tree-loop-distribute-patterns -mgeneral-regs-only \
         -O2 -Wall -Iinclude

LDFLAGS = -m elf_i386 -T linker.ld --oformat binary

# 32 sectors * 512 bytes
KERNEL_MAX = 16384

all: os.img

boot.bin: boot/boot.asm
	nasm -f bin $< -o $@

stage2.bin: boot/stage2.asm
	nasm -f bin $< -o $@

entry.o: entry.asm
	$(NASM) -f elf32 $< -o $@

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c $< -o $@

serial.o: kernel/drivers/serial.c include/drivers/serial.h
	$(CC) $(CFLAGS) -c $< -o $@

kernel.bin: entry.o kernel.o serial.o linker.ld
	$(LD) $(LDFLAGS) -o $@ entry.o kernel.o serial.o
	@test $$(stat -c %s $@) -le $(KERNEL_MAX) || \
	(echo "Kernel exceeds $(KERNEL_MAX) bytes!" && false)
	truncate -s $(KERNEL_MAX) $@

os.img: boot.bin stage2.bin kernel.bin
	cat boot.bin stage2.bin kernel.bin > $@
	truncate -s 1M $@

run: os.img
	qemu-system-i386 -drive format=raw,file=os.img

clean:
	rm -f *.bin *.o os.img

.PHONY: all run clean
