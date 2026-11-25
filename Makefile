
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
         -O2 -Wall -Iinclude -fno-threadsafe-statics \
         -fno-asynchronous-unwind-tables

LDFLAGS = -m elf_i386 --oformat binary

# 64 sectors * 512 bytes
KERNEL_MAX = 65536

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

kernel.bin: entry.o kernel.o serial.o kernel/drivers/ata.o kernel/storage/BlockDevice.o kernel/cpu/gdt.o kernel/cpu/idt.o kernel/cpu/usermode.o kernel/process/syscall.o kernel/process/ProcessManager.o kernel/memory/AddressSpace.o kernel/fs/VFS.o kernel/fs/RamFS.o linker.ld
	$(LD) $(LDFLAGS) -T linker.ld -o kernel.bin entry.o kernel.o serial.o kernel/drivers/ata.o kernel/storage/BlockDevice.o kernel/cpu/gdt.o kernel/cpu/idt.o kernel/cpu/usermode.o kernel/process/syscall.o kernel/process/ProcessManager.o kernel/memory/AddressSpace.o kernel/fs/VFS.o kernel/fs/RamFS.o
	@test $$(stat -c%s kernel.bin) -le $(KERNEL_MAX) || (echo "Kernel exceeds $(KERNEL_MAX)-byte bootloader limit"; exit 1)

kernel/cpu/gdt.o: kernel/cpu/gdt.c include/cpu/gdt.h
	$(CC) $(CFLAGS) -c kernel/cpu/gdt.c -o kernel/cpu/gdt.o

kernel/cpu/idt.o: kernel/cpu/idt.c include/cpu/idt.h
	$(CC) $(CFLAGS) -c kernel/cpu/idt.c -o $@

kernel/drivers/ata.o: kernel/drivers/ata.c include/drivers/ata.h
	$(CC) $(CFLAGS) -c $< -o $@

kernel/process/syscall.o: kernel/process/syscall.c include/process/syscall.h
	$(CC) $(CFLAGS) -c $< -o $@

kernel/process/ProcessManager.o: kernel/process/ProcessManager.cpp include/process/process.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

kernel/memory/AddressSpace.o: kernel/memory/AddressSpace.cpp include/memory/address_space.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

kernel/cpu/usermode.o: kernel/cpu/usermode.asm
	nasm -f elf32 $< -o $@

kernel/storage/BlockDevice.o: kernel/storage/BlockDevice.cpp include/storage/block_device.h include/drivers/ata.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

kernel/fs/VFS.o: kernel/fs/VFS.cpp include/fs/vfs.h include/fs/ramfs.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

kernel/fs/RamFS.o: kernel/fs/RamFS.cpp include/fs/ramfs.h include/fs/vfs.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

os.img: boot.bin stage2.bin kernel.bin
	cat boot.bin stage2.bin kernel.bin > $@
	truncate -s 1M $@

data.img:
	truncate -s 16M data.img

run: os.img data.img
	qemu-system-i386 \
		-drive file=os.img,format=raw,if=ide,index=0 \
		-drive file=data.img,format=raw,if=ide,index=1

clean:
	rm -f *.bin *.o kernel/cpu/*.o kernel/process/*.o kernel/memory/*.o kernel/fs/*.o kernel/drivers/*.o kernel/storage/*.o os.img

.PHONY: all run clean
