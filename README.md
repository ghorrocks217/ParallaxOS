# ParallaxOS

A custom 32-bit operating system written in C and x86 Assembly.

## About

ParallaxOS is a hobby operating system built from scratch, with a focus on understanding low-level computing, kernel development, and hardware interaction.

## Current Features

- BIOS bootloader
- 32-bit protected mode
- Global Descriptor Table (GDT)
- Interrupt Descriptor Table (IDT)
- Task State Segment (TSS)
- Physical memory management
- Virtual memory and paging
- Kernel heap allocator
- Preemptive round-robin task scheduler
- PIT timer
- PS/2 keyboard input
- VGA text-mode terminal
- Interactive kernel shell

## Architecture

- Architecture: x86 (32-bit)
- Boot mode: BIOS
- Kernel language: C
- Assembly: NASM
- Emulator: QEMU
- Compiler: GCC

## Building

```bash
make
```

## Running

```bash
make run
```

## Project Goals

- Modular kernel architecture
- Hardware driver support
- Disk I/O
- Filesystem support
- User-mode applications
- System calls
- ELF executable loading
- Graphical environment

## Status

ParallaxOS is an experimental hobby operating system under active development.
