
CC = gcc
LD = ld
NASM = nasm

CFLAGS = -m32 -ffreestanding -fno-builtin -fno-pic -fno-pie \
         -fno-stack-protector -fno-asynchronous-unwind-tables \
         -fno-tree-loop-distribute-patterns -mgeneral-regs-only \
         -O2 -Wall -Iinclude


CXX = g++

CXXFLAGS = -m32 -ffreestanding -fno-builtin -fno-pic -fno-pie \
         -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
         -fno-stack-protector -mgeneral-regs-only \
         -O2 -Wall -Iinclude
LDFLAGS = -m elf_i386 --oformat binary
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

kernel/%.o: kernel/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

serial.o: kernel/drivers/serial.c include/drivers/serial.h
	$(CC) $(CFLAGS) -c $< -o $@

kernel.bin: entry.o kernel.o serial.o kernel/cpu/gdt.o kernel/cpu/idt.o kernel/process/syscall.o linker.ld
	$(LD) $(LDFLAGS) -T linker.ld -o kernel.bin entry.o kernel.o serial.o kernel/cpu/gdt.o kernel/cpu/idt.o kernel/process/syscall.o

kernel/cpu/gdt.o: kernel/cpu/gdt.c include/cpu/gdt.h
	$(CC) $(CFLAGS) -c kernel/cpu/gdt.c -o kernel/cpu/gdt.o

kernel/cpu/idt.o: kernel/cpu/idt.c include/cpu/idt.h
	$(CC) $(CFLAGS) -c kernel/cpu/idt.c -o $@

kernel/process/syscall.o: kernel/process/syscall.c include/process/syscall.h
	$(CC) $(CFLAGS) -c $< -o $@
os.img: boot.bin stage2.bin kernel.bin
	cat boot.bin stage2.bin kernel.bin > $@
	truncate -s 1M $@

run: os.img
	qemu-system-i386 -drive format=raw,file=os.img

clean:
	rm -f *.bin *.o kernel/cpu/*.o kernel/process/*.o os.img

.PHONY: all run clean
