CC     = gcc
CFLAGS = -m32 -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
         -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns \
         -mgeneral-regs-only -O2 -Wall

# Must equal KERNEL_SECTORS * 512 in stage2.asm (32 sectors = 16384)
KERNEL_MAX = 16384

all: os.img

boot.bin: boot.asm
	nasm -f bin $< -o $@

stage2.bin: stage2.asm
	nasm -f bin $< -o $@

entry.o: entry.asm
	nasm -f elf32 $< -o $@

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c $< -o $@

kernel.bin: entry.o kernel.o linker.ld
	ld -m elf_i386 -T linker.ld --oformat binary -o $@ entry.o kernel.o
	@test $$(stat -c %s $@) -le $(KERNEL_MAX) || (echo "kernel.bin > $(KERNEL_MAX) bytes: raise KERNEL_SECTORS in stage2.asm AND KERNEL_MAX here" && false)
	truncate -s $(KERNEL_MAX) $@

os.img: boot.bin stage2.bin kernel.bin
	cat boot.bin stage2.bin kernel.bin > $@
	truncate -s 1M $@

run: os.img
	qemu-system-i386 -drive format=raw,file=os.img

clean:
	rm -f *.bin *.o os.img

.PHONY: all run clean