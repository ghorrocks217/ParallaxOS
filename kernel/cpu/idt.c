#include <stdint.h>
#include "cpu/idt.h"

#define IDT_ENTRIES 256

#define IDT_KERNEL_INTERRUPT 0x8E
#define IDT_USER_INTERRUPT   0xEE

struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

extern uint32_t isr_table[48];
extern void isr128(void);
extern void idt_flush(struct idt_ptr *);

static void idt_set_gate(
    unsigned int n,
    uint32_t handler,
    uint8_t flags
) {
    idt[n].base_low = handler & 0xFFFF;
    idt[n].selector = 0x08;
    idt[n].zero = 0;
    idt[n].flags = flags;
    idt[n].base_high = (handler >> 16) & 0xFFFF;
}

void idt_init(void) {
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, 0, 0);
    }

    for (int i = 0; i < 48; i++) {
        idt_set_gate(i, isr_table[i], IDT_KERNEL_INTERRUPT);
    }

    /* DPL 3: user programs can invoke int 0x80. */
    idt_set_gate(128, (uint32_t)isr128, IDT_USER_INTERRUPT);

    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint32_t)&idt;

    idt_flush(&idtp);
}
