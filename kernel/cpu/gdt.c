
#include "cpu/gdt.h"

/* Provided by kernel.c for now. */
extern void *memset(void *ptr, int value, unsigned int size);

#define BOOT_STACK_TOP 0x90000u

struct gdt_entry {
    unsigned short limit_lo;
    unsigned short base_lo;
    unsigned char  base_mid;
    unsigned char  access;
    unsigned char  gran;
    unsigned char  base_hi;
} __attribute__((packed));

struct gdt_ptr {
    unsigned short limit;
    unsigned int base;
} __attribute__((packed));

struct tss {
    unsigned int prev_tss;
    unsigned int esp0;
    unsigned int ss0;
    unsigned int esp1;
    unsigned int ss1;
    unsigned int esp2;
    unsigned int ss2;
    unsigned int cr3;
    unsigned int eip;
    unsigned int eflags;
    unsigned int eax, ecx, edx, ebx;
    unsigned int esp, ebp, esi, edi;
    unsigned int es, cs, ss, ds, fs, gs;
    unsigned int ldt;
    unsigned short trap;
    unsigned short iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct gdt_ptr gdtp;
static struct tss tss;

static void gdt_set(int index, unsigned int base,
                    unsigned int limit, unsigned char access,
                    unsigned char gran) {
    gdt[index].base_lo = base & 0xFFFF;
    gdt[index].base_mid = (base >> 16) & 0xFF;
    gdt[index].base_hi = (base >> 24) & 0xFF;
    gdt[index].limit_lo = limit & 0xFFFF;
    gdt[index].gran = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[index].access = access;
}

void tss_set_stack(unsigned int top) {
    tss.esp0 = top;
    tss.ss0 = SEL_KDATA;
}

void gdt_init(void) {
    memset(&tss, 0, sizeof(tss));
    tss.ss0 = SEL_KDATA;
    tss.esp0 = BOOT_STACK_TOP;
    tss.iomap_base = sizeof(tss);

    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xC0);
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xC0);
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xC0);
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xC0);
    gdt_set(5, (unsigned int)&tss, sizeof(tss) - 1, 0x89, 0x00);

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base = (unsigned int)gdt;

    __asm__ volatile(
        "lgdt (%0)\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "ljmp $0x08, $1f\n"
        "1:"
        : : "r"(&gdtp) : "eax", "memory");

    __asm__ volatile("ltr %%ax" : : "a"((unsigned short)SEL_TSS));
}
