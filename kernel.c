#include <stdarg.h>
#include "drivers/serial.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "process/syscall.h"
#include "process/process.h"
#include "memory/address_space.h"
#include "cpu/usermode.h"
#include "fs/vfs.h"
#include "drivers/ata.h"
#include "storage/block_device.h"
#include "fs/fat16.h"

#define STACK_SIZE 8192

extern void *kmalloc(unsigned n);
extern void kfree(void *p);

/* Build (32-bit freestanding): see the Makefile. Flags used:
 *   gcc -m32 -ffreestanding -fno-builtin -fno-pic -fno-pie -fno-stack-protector \
 *       -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns \
 *       -mgeneral-regs-only -O2 -Wall -c kernel.c
 *
 * Link with entry.o first so _start lands at 0x10000.
 * Run QEMU with  -serial file:serial.log  (or -serial stdio) to see serial output.
 */

/* ---------- port I/O ---------- */
static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned char inb(unsigned short port) {
    unsigned char v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void io_wait(void) { outb(0x80, 0); }

/* ---------- interrupt-flag save/restore (poor man's lock) ---------- */
static inline unsigned irq_save(void) {
    unsigned f;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(f) : : "memory");
    return f;
}

static inline void irq_restore(unsigned f) {
    __asm__ volatile("push %0; popf" : : "r"(f) : "memory", "cc");
}

/* ---------- memory helpers (no libc!) ----------
 * GCC at -O2 can turn these loops back into calls to memset/memcpy (infinite
 * recursion). The build flag -fno-tree-loop-distribute-patterns is the reliable
 * fix; the attribute is a belt-and-braces fallback. */
#define NO_LOOP_IDIOM __attribute__((optimize("no-tree-loop-distribute-patterns")))

NO_LOOP_IDIOM void *memset(void *d, int v, unsigned n) {
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)v;
    return d;
}

NO_LOOP_IDIOM void *memcpy(void *d, const void *s, unsigned n) {
    unsigned char *dp = d;
    const unsigned char *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}


/* ---------- VGA text terminal ---------- */
#define VGA ((volatile unsigned short *)0xB8000)
#define COLS 80
#define ROWS 25
#define TOP  1                          /* row 0 is the status bar (clock) */

static int row = TOP, col = 0;
static unsigned char color = 0x0F;      /* white on black */

static void update_cursor(void) {
    unsigned short pos = row * COLS + col;
    outb(0x3D4, 14); outb(0x3D5, pos >> 8);
    outb(0x3D4, 15); outb(0x3D5, pos & 0xFF);
}

/* Scrolls rows TOP..ROWS-1 only, so the status bar is left alone */
static void scroll(void) {
    for (int i = TOP * COLS; i < (ROWS - 1) * COLS; i++)
        VGA[i] = VGA[i + COLS];
    for (int i = (ROWS - 1) * COLS; i < ROWS * COLS; i++)
        VGA[i] = (color << 8) | ' ';
    row = ROWS - 1;
}

static void clear(void) {
    unsigned f = irq_save();
    for (int i = 0; i < ROWS * COLS; i++)
        VGA[i] = (color << 8) | ' ';
    row = TOP;
    col = 0;
    update_cursor();
    irq_restore(f);
}

/* No locking and no cursor update: callers handle both.
 * Everything printed is mirrored to the serial port. */
static void kputc_raw(char c) {
    if (c == '\n') serial_putchar('\r');
    serial_putchar(c);

    if (c == '\n')      { col = 0; row++; }
    else if (c == '\r') { col = 0; }
    else if (c == '\b') {
        if (col > 0) col--;
        else if (row > TOP) { row--; col = COLS - 1; }
    } else {
        VGA[row * COLS + col] = (color << 8) | (unsigned char)c;
        if (++col == COLS) { col = 0; row++; }
    }
    if (row >= ROWS) scroll();
}

static void puts_raw(const char *s) {
    if (!s) s = "(null)";
    while (*s) kputc_raw(*s++);
}

static void uint_raw(unsigned v, unsigned base) {
    char buf[32];
    int i = 0;
    if (v == 0) buf[i++] = '0';
    while (v) { buf[i++] = "0123456789abcdef"[v % base]; v /= base; }
    while (i) kputc_raw(buf[--i]);
}

/* Safe to call from any task: no task switch can happen mid-character */
static void kputc(char c) {
    unsigned f = irq_save();
    kputc_raw(c);
    update_cursor();
    irq_restore(f);
}

static void kputs(const char *s) {
    unsigned f = irq_save();
    puts_raw(s);
    update_cursor();
    irq_restore(f);
}

void kprintf(const char *fmt, ...) {
    unsigned fl = irq_save();               /* keep a whole line together */
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { kputc_raw(*fmt); continue; }
        switch (*++fmt) {
        case 's': puts_raw(va_arg(ap, const char *)); break;
        case 'c': kputc_raw((char)va_arg(ap, int)); break;
        case 'x': puts_raw("0x"); uint_raw(va_arg(ap, unsigned), 16); break;
        case 'u': uint_raw(va_arg(ap, unsigned), 10); break;
        case 'd': {
            int v = va_arg(ap, int);
            unsigned u = (unsigned)v;
            if (v < 0) { kputc_raw('-'); u = 0u - u; }   /* INT_MIN safe */
            uint_raw(u, 10);
            break;
        }
        case '%': kputc_raw('%'); break;
        case 0:   fmt--; break;             /* trailing '%' must not run past the NUL */
        default:  kputc_raw('%'); kputc_raw(*fmt); break;
        }
    }
    va_end(ap);
    update_cursor();
    irq_restore(fl);
}

static void panic(const char *msg) {
    color = 0x4F;
    kprintf("\nPANIC: %s\n", msg);
    for (;;) __asm__ volatile("cli; hlt");
}

/* ---------- tiny string helpers ---------- */
static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static unsigned parse_uint(const char *s) {
    unsigned v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

/* Must match isr_common in entry.asm: pusha, then int_no/err_code, then CPU frame */
struct regs {
    unsigned int edi, esi, ebp, esp, ebx, edx, ecx, eax;
    unsigned int int_no, err_code;
    unsigned int eip, cs, eflags;
};

static const char *exc_names[32] = {
    "Divide by zero", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor overrun", "Invalid TSS",
    "Segment not present", "Stack fault", "General protection fault",
    "Page fault", "Reserved", "x87 FPU error", "Alignment check",
    "Machine check", "SIMD FPU error", "Virtualization", "Control protection",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Security", "Reserved"
};

/* ---------- PIC ---------- */
static void pic_remap(void) {
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();    /* IRQ0-7  -> int 32-39 */
    outb(0xA1, 0x28); io_wait();    /* IRQ8-15 -> int 40-47 */
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();

    outb(0x21, 0xFC);               /* unmask IRQ0 (timer) and IRQ1 (keyboard) */
    outb(0xA1, 0xFF);
}

static void pic_eoi(int int_no) {
    if (int_no >= 40) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

/* Detect spurious IRQ7 / IRQ15 (they must not get a normal EOI) */
static int pic_is_spurious(int int_no) {
    unsigned short port = (int_no == 47) ? 0xA0 : 0x20;
    outb(port, 0x0B);               /* OCW3: read In-Service Register */
    return !(inb(port) & 0x80);
}

/* ---------- PIT timer ---------- */
#define TIMER_HZ 100

static volatile unsigned ticks = 0;

static void timer_init(void) {
    unsigned divisor = 1193182 / TIMER_HZ;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}


/* Where the CPU lands when a ring-3 task enters the kernel (interrupt/syscall).
 * schedule() updates this on every task switch. */

// static void tss_set_stack(unsigned top) { tss.esp0 = top; }

/* =====================================================================
 *  PHYSICAL MEMORY MANAGER: one bit per 4 KB frame (1 = used, 0 = free)
 * ===================================================================== */
#define PAGE 4096

/* Everything the allocator hands out must be identity-mapped by vmm_init. */
#define MAX_FRAMES 65536u           /* 256 MB */

struct e820 {
    unsigned long long base, len;
    unsigned type, acpi;
} __attribute__((packed));          /* 24 bytes: stage2 stores 24-byte entries */

#define E820_COUNT (*(volatile unsigned *)0x1000)
#define E820_MAP   ((struct e820 *)0x1010)

/* Place the bitmap after the kernel image if the linker provides _end. */
extern char _end[] __attribute__((weak));

static unsigned char *bitmap;
static unsigned bitmap_addr;
static unsigned total_frames;
static unsigned ram_frames;
static unsigned free_frames;
static unsigned bitmap_bytes;
static unsigned search_hint;
static unsigned reserved_frames;    /* frames below this are never freed */

static inline void bit_set(unsigned f) { bitmap[f >> 3] |=  (1 << (f & 7)); }
static inline void bit_clr(unsigned f) { bitmap[f >> 3] &= ~(1 << (f & 7)); }
static inline int  bit_get(unsigned f) { return bitmap[f >> 3] & (1 << (f & 7)); }

static void pmm_init(void) {
    unsigned n = E820_COUNT;
    if (n == 0 || n > 32)
        panic("no BIOS memory map (did you update stage2.asm?)");

    bitmap_addr = 0x100000;
    if ((unsigned)_end > bitmap_addr)
        bitmap_addr = ((unsigned)_end + PAGE - 1) & ~(PAGE - 1);
    bitmap = (unsigned char *)bitmap_addr;

    unsigned long long top = 0;
    for (unsigned i = 0; i < n; i++) {
        struct e820 *e = &E820_MAP[i];
        if (e->type != 1 || e->len == 0 || e->base >= 0x100000000ULL) continue;
        unsigned long long end = e->base + e->len;
        if (end > 0x100000000ULL) end = 0x100000000ULL;
        if (end > top) top = end;
    }
    /* never track frames above what vmm_init identity-maps */
    if ((top >> 12) > MAX_FRAMES) top = (unsigned long long)MAX_FRAMES << 12;
    total_frames = (unsigned)(top >> 12);
    if (total_frames == 0)
        panic("memory map has no usable RAM");
    bitmap_bytes = (total_frames + 7) / 8;

    memset(bitmap, 0xFF, bitmap_bytes);
    for (unsigned i = 0; i < n; i++) {
        struct e820 *e = &E820_MAP[i];
        if (e->type != 1 || e->len == 0 || e->base >= 0x100000000ULL) continue;
        unsigned long long end = e->base + e->len;
        if (end > 0x100000000ULL) end = 0x100000000ULL;
        unsigned long long first64 = (e->base + PAGE - 1) >> 12;
        unsigned long long last64  = end >> 12;
        if (last64 > total_frames) last64 = total_frames;
        for (unsigned long long f = first64; f < last64; f++) {
            bit_clr((unsigned)f);
            ram_frames++;
        }
    }

    /* Reserve everything from 0 up to the end of the bitmap
     * (low memory, BIOS data, E820 table, kernel, bitmap). */
    unsigned reserved = (bitmap_addr + bitmap_bytes + PAGE - 1) / PAGE;
    reserved_frames = reserved;
    for (unsigned f = 0; f < reserved && f < total_frames; f++)
        bit_set(f);

    for (unsigned f = 0; f < total_frames; f++)
        if (!bit_get(f)) free_frames++;

    if (free_frames == 0)
        panic("no free physical memory");
}

unsigned pmm_alloc(void) {
    if (!total_frames) return 0;
    for (unsigned i = 0; i < total_frames; i++) {
        unsigned f = (search_hint + i) % total_frames;
        if (!bit_get(f)) {
            bit_set(f);
            free_frames--;
            search_hint = f + 1;
            return f << 12;
        }
    }
    return 0;
}

void pmm_free(unsigned addr) {
    unsigned f = addr >> 12;
    if (f < reserved_frames || f >= total_frames) return;   /* never free kernel/bitmap/low RAM */
    if (bit_get(f)) {
        bit_clr(f);
        free_frames++;
        if (f < search_hint) search_hint = f;
    }
}

/* =====================================================================
 *  PAGING
 * ===================================================================== */
#define PTE_P 0x1
#define PTE_W 0x2
#define PTE_U 0x4

static unsigned *page_dir;
static int paging_on = 0;

static void vmm_map(unsigned virt, unsigned phys, unsigned flags) {
    unsigned di = virt >> 22;
    unsigned ti = (virt >> 12) & 0x3FF;

    if (!(page_dir[di] & PTE_P)) {
        unsigned pt = pmm_alloc();

        if (!pt)
            panic("out of memory for a page table");

        memset((void *)pt, 0, PAGE);

        page_dir[di] = pt | PTE_P | PTE_W;
    }

    /*
     * User access requires the U/S bit in BOTH
     * the page directory and page table entries.
     */
    if (flags & PTE_U)
        page_dir[di] |= PTE_U;

    unsigned *pt = (unsigned *)(page_dir[di] & 0xFFFFF000);

    pt[ti] = (phys & 0xFFFFF000) | flags | PTE_P;

    if (paging_on)
        __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

static void vmm_init(void) {
    page_dir = (unsigned *)pmm_alloc();
    if (!page_dir) panic("no memory for page directory");
    memset(page_dir, 0, PAGE);

    /* total_frames <= MAX_FRAMES, so every frame pmm_alloc can return is mapped */
    for (unsigned f = 1; f < total_frames; f++)
        vmm_map(f << 12, f << 12, PTE_W);

    __asm__ volatile("mov %0, %%cr3" : : "r"(page_dir));
    unsigned cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    paging_on = 1;
}

/* =====================================================================
 *  ADDRESS SPACE MANAGEMENT
 * ===================================================================== */

/*
 * Every process receives its own page directory.
 * Kernel page tables are initially shared.
 * A page table is copied when a process needs a private user mapping.
 */

unsigned paging_kernel_directory(void) {
    return (unsigned)page_dir;
}

unsigned paging_active_directory(void) {
    unsigned directory;

    __asm__ volatile(
        "mov %%cr3, %0"
        : "=r"(directory)
    );

    return directory & 0xFFFFF000;
}

/* Switch the active page directory by loading CR3. */
void paging_switch_directory(unsigned directory) {
    if (!directory)
        return;

    unsigned current_cr3;

    __asm__ volatile(
        "mov %%cr3, %0"
        : "=r"(current_cr3)
    );

    if ((current_cr3 & 0xFFFFF000) == (directory & 0xFFFFF000))
        return;

    __asm__ volatile(
        "mov %0, %%cr3"
        :
        : "r"(directory)
        : "memory"
    );
}

/* Create a page directory containing the kernel's existing mappings. */
int paging_create_directory(unsigned *directory) {
    if (!directory || !page_dir)
        return 0;

    unsigned frame = pmm_alloc();

    if (!frame)
        return 0;

    unsigned *new_directory = (unsigned *)frame;

    memset(new_directory, 0, PAGE);
    memcpy(new_directory, page_dir, PAGE);

    *directory = frame;

    return 1;
}

/*
 * Destroy an address space.
 *
 * Page tables identical to the kernel's are shared and must not be freed.
 * Private page tables belong to this address space and are freed.
 */
void paging_destroy_directory(unsigned directory) {
    if (!directory)
        return;

    unsigned *target = (unsigned *)directory;

    if (target == page_dir)
        return;

    unsigned current_cr3;

    __asm__ volatile(
        "mov %%cr3, %0"
        : "=r"(current_cr3)
    );

    if ((current_cr3 & 0xFFFFF000) == directory)
        paging_switch_directory((unsigned)page_dir);

    for (unsigned i = 0; i < 1024; i++) {
        unsigned entry = target[i];

        if (!(entry & PTE_P))
            continue;

        /* Shared kernel page table: leave it alone. */
        if (entry == page_dir[i])
            continue;

        unsigned table = entry & 0xFFFFF000;

        pmm_free(table);
    }

    pmm_free(directory);
}

/*
 * Map a user page.
 *
 * If the page table is shared with the kernel, make a private copy first.
 * Existing supervisor mappings can be replaced, but existing user mappings
 * cannot be silently overwritten.
 */
int paging_map_user(
    unsigned directory,
    unsigned virtual_address,
    unsigned physical_address,
    unsigned flags
) {
    if (!directory || !physical_address)
        return 0;

    if ((virtual_address & 0xFFF) ||
        (physical_address & 0xFFF))
        return 0;

    unsigned *dir = (unsigned *)directory;

    if (dir == page_dir)
        return 0;

    unsigned di = virtual_address >> 22;
    unsigned ti = (virtual_address >> 12) & 0x3FF;

    unsigned *table;

    if (!(dir[di] & PTE_P)) {
        /* No existing page table. Create a private one. */
        unsigned frame = pmm_alloc();

        if (!frame)
            return 0;

        table = (unsigned *)frame;
        memset(table, 0, PAGE);

        dir[di] = frame | PTE_P | PTE_W | PTE_U;
    }
    else if (dir[di] == page_dir[di]) {
        /* The page table is shared with the kernel. Clone it. */
        unsigned frame = pmm_alloc();

        if (!frame)
            return 0;

        table = (unsigned *)frame;

        unsigned *kernel_table =
            (unsigned *)(page_dir[di] & 0xFFFFF000);

        memcpy(table, kernel_table, PAGE);

        dir[di] = frame | PTE_P | PTE_W | PTE_U;
    }
    else {
        /* Already private to this address space. */
        table = (unsigned *)(dir[di] & 0xFFFFF000);

        dir[di] |= PTE_U;
    }

    /* Never overwrite an existing user mapping. */
    if (table[ti] & PTE_U)
        return 0;

    table[ti] =
        (physical_address & 0xFFFFF000) |
        PTE_P |
        PTE_U |
        (flags & PTE_W);

    /* Flush this address if the target directory is currently active. */
    unsigned current_cr3;

    __asm__ volatile(
        "mov %%cr3, %0"
        : "=r"(current_cr3)
    );

    if ((current_cr3 & 0xFFFFF000) == directory) {
        __asm__ volatile(
            "invlpg (%0)"
            :
            : "r"(virtual_address)
            : "memory"
        );
    }

    return 1;
}

/* Remove a user mapping without freeing its physical frame. */
int paging_unmap_user(
    unsigned directory,
    unsigned virtual_address
) {
    if (!directory || (virtual_address & 0xFFF))
        return 0;

    unsigned *dir = (unsigned *)directory;

    unsigned di = virtual_address >> 22;
    unsigned ti = (virtual_address >> 12) & 0x3FF;

    if (!(dir[di] & PTE_P) || !(dir[di] & PTE_U))
        return 0;

    /* Never modify a shared kernel page table. */
    if (dir[di] == page_dir[di])
        return 0;

    unsigned *table =
        (unsigned *)(dir[di] & 0xFFFFF000);

    if (!(table[ti] & PTE_U))
        return 0;

    table[ti] = 0;

    unsigned current_cr3;

    __asm__ volatile(
        "mov %%cr3, %0"
        : "=r"(current_cr3)
    );

    if ((current_cr3 & 0xFFFFF000) == directory) {
        __asm__ volatile(
            "invlpg (%0)"
            :
            : "r"(virtual_address)
            : "memory"
        );
    }

    return 1;
}

/* Translate a user virtual address into its physical address. */
unsigned paging_translate_user(
    unsigned directory,
    unsigned virtual_address
) {
    if (!directory)
        return 0;

    unsigned *dir = (unsigned *)directory;

    unsigned di = virtual_address >> 22;
    unsigned ti = (virtual_address >> 12) & 0x3FF;

    if (!(dir[di] & PTE_P) || !(dir[di] & PTE_U))
        return 0;

    unsigned *table =
        (unsigned *)(dir[di] & 0xFFFFF000);

    unsigned entry = table[ti];

    if (!(entry & PTE_P) || !(entry & PTE_U))
        return 0;

    return (entry & 0xFFFFF000) |
           (virtual_address & 0xFFF);
}

#define USER_CODE  0x00400000
#define USER_STACK 0xBFFFF000

unsigned user_mode_saved_esp = 0;
unsigned user_mode_saved_eflags = 0;

static unsigned active_user_pid = 0;

extern void paging_switch_directory(unsigned directory);

static void user_mode_test(void);

static void user_mode_test(void) {
    unsigned code_page = pmm_alloc();
    unsigned stack_page = pmm_alloc();

    if (!code_page || !stack_page)
        panic("could not allocate user pages");

    address_space space;

    if (!address_space_create(&space))
        panic("could not create process address space");

    static const unsigned char user_program[] = {
        /* SYS_WRITE(message) */
        0xB8, 0x01, 0x00, 0x00, 0x00,
        0xBB, 0x00, 0x01, 0x40, 0x00,
        0xCD, 0x80,

        /* SYS_EXIT(42) */
        0xB8, 0x00, 0x00, 0x00, 0x00,
        0xBB, 0x2A, 0x00, 0x00, 0x00,
        0xCD, 0x80,

        /* Should never reach this */
        0xEB, 0xFE
    };

    static const char message[] = "Hello from isolated Ring 3!\n";

    memset((void *)code_page, 0, PAGE);
    memset((void *)stack_page, 0, PAGE);

    memcpy((void *)code_page, user_program, sizeof(user_program));
    memcpy((void *)(code_page + 0x100), message, sizeof(message));

    if (!address_space_map_user(
            &space, USER_CODE, code_page, PTE_W)) {
        panic("could not map user code");
    }

    if (!address_space_map_user(
            &space, USER_STACK, stack_page, PTE_W)) {
        panic("could not map user stack");
    }

    int pid = process_create(
        "ring3-demo",
        USER_CODE,
        USER_STACK + PAGE,
        space.directory
    );

    if (pid < 0)
        panic("process table is full");

    active_user_pid = (unsigned)pid;

    process_set_state((unsigned)pid, PROCESS_RUNNING);

    kprintf("Created process %u\n", (unsigned)pid);
    kprintf("Kernel directory: %x\n",
            address_space_kernel_directory());
    kprintf("Process directory: %x\n", space.directory);

    address_space_activate(&space);

    void *kernel_stack = kmalloc(STACK_SIZE);

    if (!kernel_stack)
        panic("could not allocate user kernel stack");

    unsigned kernel_stack_top =
        (unsigned)kernel_stack + STACK_SIZE;

    tss_set_stack(kernel_stack_top);

    /* Disable timer preemption while the isolated user program runs. */
    outb(0x21, inb(0x21) | 0x01);

    kprintf("Entering isolated Ring 3...\n");

    enter_user_mode(
        USER_CODE,
        USER_STACK + PAGE,
        kernel_stack_top
    );

    kprintf("Process %u exited.\n", active_user_pid);

    address_space_destroy(&space);

    pmm_free(code_page);
    pmm_free(stack_page);

    kfree(kernel_stack);

    active_user_pid = 0;

    /* Re-enable the timer interrupt. */
    outb(0x21, inb(0x21) & ~0x01);
}

/* =====================================================================
 *  HEAP (interrupt-safe: task switches can't happen mid-allocation)
 * ===================================================================== */
#define HEAP_START 0xD0000000u

struct block {
    unsigned size;
    unsigned free;
    struct block *next;
    unsigned pad;
};

static struct block *heap_head = 0, *heap_tail = 0;
static unsigned heap_end = HEAP_START;

/* Returns the number of bytes added (non-zero = progress). */
static unsigned heap_grow(unsigned bytes) {
    bytes = (bytes + PAGE - 1) & ~(PAGE - 1);
    unsigned start = heap_end;

    for (unsigned off = 0; off < bytes; off += PAGE) {
        unsigned frame = pmm_alloc();
        if (!frame) break;
        vmm_map(heap_end, frame, PTE_W);
        heap_end += PAGE;
    }

    unsigned got = heap_end - start;
    if (!got) return 0;

    if (heap_tail && heap_tail->free) {
        heap_tail->size += got;
    } else {
        struct block *b = (struct block *)start;
        b->size = got - sizeof(struct block);
        b->free = 1;
        b->next = 0;
        if (heap_tail) heap_tail->next = b; else heap_head = b;
        heap_tail = b;
    }
    return got;
}

static void *kmalloc_raw(unsigned n) {
    if (!n) return 0;
    n = (n + 15) & ~15u;

    for (;;) {
        for (struct block *b = heap_head; b; b = b->next) {
            if (!b->free || b->size < n) continue;

            if (b->size >= n + sizeof(struct block) + 16) {
                struct block *nb = (struct block *)((char *)(b + 1) + n);
                nb->size = b->size - n - sizeof(struct block);
                nb->free = 1;
                nb->next = b->next;
                b->next = nb;
                b->size = n;
                if (heap_tail == b) heap_tail = nb;
            }
            b->free = 0;
            return b + 1;
        }
        if (!heap_grow(n + sizeof(struct block)))
            return 0;
    }
}

static void kfree_raw(void *p) {
    if (!p) return;
    ((struct block *)p - 1)->free = 1;

    for (struct block *c = heap_head; c && c->next; ) {
        if (c->free && c->next->free) {
            if (c->next == heap_tail) heap_tail = c;
            c->size += sizeof(struct block) + c->next->size;
            c->next = c->next->next;
        } else {
            c = c->next;
        }
    }
}

void *kmalloc(unsigned n) {
    unsigned f = irq_save();
    void *p = kmalloc_raw(n);
    irq_restore(f);
    return p;
}

void kfree(void *p) {
    unsigned f = irq_save();
    kfree_raw(p);
    irq_restore(f);
}

/* =====================================================================
 *  TASKS: preemptive round-robin scheduler with sleeping / blocking
 * ===================================================================== */
#define MAX_TASKS  16
#define BOOT_STACK_TOP 0x90000u

enum { T_FREE = 0, T_READY, T_DEAD, T_SLEEPING, T_BLOCKED };

struct task {
    int state;
    unsigned esp;
    void *stack;
    void (*fn)(void *);
    void *arg;
    unsigned wake;
    char name[16];
};

static struct task tasks[MAX_TASKS];
static struct task *current = 0;

extern void switch_context(unsigned *save_old_esp, unsigned new_esp);

/* Every new task begins here (switch_context "returns" into this) */
static void task_start(void) __attribute__((noreturn));
static void task_start(void) {
    __asm__ volatile("sti");        /* we arrive from inside the timer interrupt */
    current->fn(current->arg);
    current->state = T_DEAD;        /* function returned: task is finished */
    /* sti before hlt: a finished task can never halt with interrupts off */
    for (;;) __asm__ volatile("sti; hlt" : : : "memory");
}

static int task_create(const char *name, void (*fn)(void *), void *arg) {
    unsigned f = irq_save();

    int id = -1;
    for (int i = 1; i < MAX_TASKS; i++)
        if (tasks[i].state == T_FREE) { id = i; break; }
    if (id < 0) { irq_restore(f); return -1; }

    struct task *t = &tasks[id];
    t->stack = kmalloc(STACK_SIZE);
    if (!t->stack) { irq_restore(f); return -1; }

    t->fn = fn;
    t->arg = arg;
    t->wake = 0;
    int i = 0;
    while (name[i] && i < 15) { t->name[i] = name[i]; i++; }
    t->name[i] = 0;

    /* Build the initial stack so switch_context can "resume" this task */
    unsigned *sp = (unsigned *)((char *)t->stack + STACK_SIZE);
    *--sp = 0;                      /* fake return address for task_start */
    *--sp = (unsigned)task_start;   /* where switch_context's ret goes */
    *--sp = 0;                      /* ebp */
    *--sp = 0;                      /* ebx */
    *--sp = 0;                      /* esi */
    *--sp = 0;                      /* edi */
    t->esp = (unsigned)sp;

    t->state = T_READY;
    irq_restore(f);
    return id;
}

/* Called from the timer interrupt (interrupts are off).
 * Must NOT call kfree/kmalloc here: those call irq_restore() which would
 * re-enable interrupts mid-ISR. Stacks are freed via kfree_raw() directly. */

static void schedule(void) {
    if (!current) return;

    /* Wake sleeping tasks. */
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == T_SLEEPING &&
            (int)(ticks - tasks[i].wake) >= 0) {
            tasks[i].state = T_READY;
        }
    }

    /* Reclaim finished tasks that are not currently running. */
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == T_DEAD && &tasks[i] != current) {
            kfree_raw(tasks[i].stack);
            tasks[i].stack = 0;
            tasks[i].state = T_FREE;
        }
    }

    int cur = (int)(current - tasks);

    for (int i = 1; i <= MAX_TASKS; i++) {
        int n = (cur + i) % MAX_TASKS;

        if (tasks[n].state != T_READY)
            continue;

        if (&tasks[n] == current)
            return;

        struct task *prev = current;
        current = &tasks[n];

        unsigned kernel_stack_top;

        if (current->stack) {
            kernel_stack_top =
                (unsigned)current->stack + STACK_SIZE;
        } else {
            kernel_stack_top = BOOT_STACK_TOP;
        }

        tss_set_stack(kernel_stack_top);

        switch_context(&prev->esp, current->esp);
        return;
    }
}

/* Voluntary scheduler entry point.
 * May be called from a syscall with interrupts already disabled. */
void task_yield(void) {
    unsigned flags = irq_save();
    schedule();
    irq_restore(flags);
}
/* Sleeps without burning CPU: the task leaves the run queue until its wake
 * tick, then schedule() flips it back to READY. Saves/restores the caller's
 * interrupt flag, so it never returns with IF changed. */
static void sleep_ms(unsigned ms) {
    if (!current) return;
    unsigned per  = 1000 / TIMER_HZ;
    unsigned need = ms / per + (ms % per != 0);
    if (!need) return;
    unsigned f = irq_save();
    current->wake  = ticks + need;
    current->state = T_SLEEPING;
    while (current->state != T_READY)
        __asm__ volatile("sti; hlt; cli" : : : "memory");
    irq_restore(f);
}

/* ---------- demo tasks ---------- */
static void clock_task(void *arg) {
    (void)arg;
    for (;;) {
        unsigned s = ticks / TIMER_HZ;
        char digits[12]; int d = 0;
        do { digits[d++] = '0' + s % 10; s /= 10; } while (s);

        char out[16]; int n = 0;
        out[n++] = 'u'; out[n++] = 'p'; out[n++] = ' ';
        while (d) out[n++] = digits[--d];
        out[n++] = 's';
        while (n < 16) out[n++] = ' ';

        for (int k = 0; k < 16; k++)            /* white on blue, row 0, top-right */
            VGA[COLS - 16 + k] = 0x1F00 | (unsigned char)out[k];
        sleep_ms(1000);
    }
}

static void worker(void *arg) {
    unsigned id = (unsigned)arg;
    for (int i = 1; i <= 5; i++) {
        kprintf("[worker %u] step %d\n", id, i);
        sleep_ms(1000);
    }
}

static void hog(void *arg) {
    (void)arg;
    volatile unsigned n = 0;
    while (1) {
        __asm__ volatile("" ::: "memory");
    }                 /* never yields, but gets preempted anyway */
}

/* ---------- Keyboard: scancode decoder + type-ahead ring buffer ---------- */
static const char sc_lower[58] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' '
};
static const char sc_upper[58] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' '
};

#define KB_BUF 64

static int shift = 0, caps = 0, skip = 0;   /* skip = extra bytes of a multi-byte key to swallow */
static char kb_buf[KB_BUF];
static volatile unsigned kb_head = 0, kb_tail = 0;
static struct task *volatile kb_waiter = 0; /* task blocked in kb_getc, if any */

static void kb_push(char c) {
    unsigned next = (kb_head + 1) % KB_BUF;
    if (next == kb_tail) return;            /* buffer full: drop */
    kb_buf[kb_head] = c;
    kb_head = next;
}

/* Keys typed while a command runs are queued instead of dropped; Caps Lock
 * works; 0xE0 keys (1 more byte) and the Pause key 0xE1 (5 more bytes) are
 * swallowed whole instead of being misread as normal scancodes.
 * Wakes the shell if it is blocked waiting for a key. */
static void keyboard_handler(void) {
    unsigned char sc = inb(0x60);

    if (skip)       { skip--; return; }
    if (sc == 0xE0) { skip = 1; return; }
    if (sc == 0xE1) { skip = 5; return; }

    if (sc & 0x80) {
        sc &= 0x7F;
        if (sc == 0x2A || sc == 0x36) shift = 0;
        return;
    }
    if (sc == 0x2A || sc == 0x36) { shift = 1; return; }
    if (sc == 0x3A) { caps ^= 1; return; }
    if (sc >= 58) return;

    char c = shift ? sc_upper[sc] : sc_lower[sc];
    if (caps) {
        if (c >= 'a' && c <= 'z') c -= 32;
        else if (c >= 'A' && c <= 'Z') c += 32;
    }
    if (c) {
        kb_push(c);
        if (kb_waiter) {
            if (kb_waiter->state == T_BLOCKED) kb_waiter->state = T_READY;
            kb_waiter = 0;
        }
    }
}

/* Blocking read of one key: the task leaves the run queue until a key arrives */
static char kb_getc(void) {
    for (;;) {
        unsigned f = irq_save();
        if (kb_head != kb_tail) {
            char c = kb_buf[kb_tail];
            kb_tail = (kb_tail + 1) % KB_BUF;
            irq_restore(f);
            return c;
        }
        kb_waiter = current;
        current->state = T_BLOCKED;
        while (current->state != T_READY)
            __asm__ volatile("sti; hlt; cli" : : : "memory");
        irq_restore(f);
    }
}

#define LINE_MAX 2048
static char line[LINE_MAX];

static void read_line(void) {
    int len = 0;
    for (;;) {
        char c = kb_getc();
        if (c == '\n') {
            kputc('\n');
            line[len] = 0;
            return;
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                unsigned f = irq_save();    /* erase as one unit */
                kputc_raw('\b'); kputc_raw(' '); kputc_raw('\b');
                update_cursor();
                irq_restore(f);
            }
        } else if (c >= ' ' && len < LINE_MAX - 1) {
            line[len++] = c;
            kputc(c);
        }
    }
}

int copy_user_string(char *dst, unsigned src, unsigned capacity) {
    if (!dst || !capacity)
        return 0;

    unsigned directory = paging_active_directory();
    unsigned *dir = (unsigned *)directory;

    for (unsigned i = 0; i < capacity - 1; i++) {
        unsigned addr = src + i;

        if (addr < src)
            return 0;

        unsigned di = addr >> 22;
        unsigned ti = (addr >> 12) & 0x3FF;

        if (!(dir[di] & PTE_P) || !(dir[di] & PTE_U))
            return 0;

        unsigned *table =
            (unsigned *)(dir[di] & 0xFFFFF000);

        unsigned pte = table[ti];

        if (!(pte & PTE_P) || !(pte & PTE_U))
            return 0;

        dst[i] = *(volatile const char *)addr;

        if (dst[i] == '\0')
            return 1;
    }

    dst[capacity - 1] = '\0';
    return 1;
}

/* ---------- interrupt dispatch ---------- */
void isr_handler(struct regs *r) {
    if (r->int_no == 128) {
        int result = syscall_handler(
            r->eax,
            r->ebx,
            r->ecx
        );

        if (result == SYSCALL_TERMINATE) {
            if (!active_user_pid ||
                !process_exit(active_user_pid, (int)r->ebx)) {
                panic("invalid process exit request");
            }

            /* Stop using the process's page directory. */
            paging_switch_directory(address_space_kernel_directory());

            /* Restore the kernel's normal privilege-transition stack. */
            tss_set_stack(0x90000u);

            /*
             * This restores the original kernel stack saved by
             * enter_user_mode(), returning to user_mode_test().
             */
            leave_user_mode();
        }

        r->eax = (unsigned)result;
        return;
    }
    if (r->int_no >= 32) {
        if (r->int_no == 32) {
            ticks++;
            pic_eoi(32);            /* EOI first: we may not come back here for a while */
            schedule();
            return;
        }
        /* spurious IRQ7/IRQ15 get no EOI (IRQ15 still needs one for the master) */
        if ((r->int_no == 39 || r->int_no == 47) && pic_is_spurious(r->int_no)) {
            if (r->int_no == 47) outb(0x20, 0x20);
            return;
        }
        if (r->int_no == 33) keyboard_handler();
        pic_eoi(r->int_no);
        return;
    }

    color = 0x4F;
    kprintf("\n*** EXCEPTION %d: %s ***\n", r->int_no, exc_names[r->int_no]);
    if (r->int_no == 14) {
        unsigned cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("Faulting address: %x  (%s, %s)\n", cr2,
                (r->err_code & 1) ? "protection violation" : "page not present",
                (r->err_code & 2) ? "write" : "read");
    }
    kprintf("EIP=%x  CS=%x  EFLAGS=%x  ERR=%x\n",
            r->eip, r->cs, r->eflags, r->err_code);
    kprintf("EAX=%x EBX=%x ECX=%x EDX=%x\n", r->eax, r->ebx, r->ecx, r->edx);
    for (;;) __asm__ volatile("cli; hlt");
}

/* ---------- filesystem shell support ---------- */

static char current_directory[VFS_PATH_MAX] = "/";

static unsigned fs_strlen(const char *s) {
    unsigned n = 0;
    while (s && s[n]) ++n;
    return n;
}

/* Convert relative paths into normalized absolute paths. */
static int resolve_path(
    const char *input,
    char *output,
    unsigned capacity
) {
    if (!input || !*input || !output || capacity < 2)
        return 0;

    char combined[VFS_PATH_MAX * 2];

    unsigned pos = 0;

    if (input[0] == '/') {
        while (input[pos] && pos < sizeof(combined) - 1) {
            combined[pos] = input[pos];
            ++pos;
        }
    } else {
        unsigned cwd_len = fs_strlen(current_directory);

        for (unsigned i = 0; i < cwd_len; ++i)
            combined[pos++] = current_directory[i];

        if (pos > 1)
            combined[pos++] = '/';

        unsigned i = 0;

        while (input[i] && pos < sizeof(combined) - 1)
            combined[pos++] = input[i++];

        if (input[i])
            return 0;
    }

    if (input[0] == '/' && input[pos] != '\0')
        return 0;

    combined[pos] = '\0';

    output[0] = '/';
    output[1] = '\0';

    unsigned out_len = 1;
    unsigned i = 1;

    while (combined[i]) {
        while (combined[i] == '/')
            ++i;

        if (!combined[i])
            break;

        unsigned start = i;

        while (combined[i] && combined[i] != '/')
            ++i;

        unsigned length = i - start;

        if (length == 1 && combined[start] == '.')
            continue;

        if (length == 2 &&
            combined[start] == '.' &&
            combined[start + 1] == '.') {
            if (out_len > 1) {
                while (out_len > 1 && output[out_len - 1] != '/')
                    --out_len;
                output[out_len] = '\0';
            }

            continue;
        }

        if (out_len > 1) {
            if (out_len + 1 >= capacity)
                return 0;

            output[out_len++] = '/';
        }

        if (out_len + length >= capacity)
            return 0;

        for (unsigned j = 0; j < length; ++j)
            output[out_len++] = combined[start + j];

        output[out_len] = '\0';
    }

    return 1;
}

static void prompt(void) {
    kprintf("os:%s> ", current_directory);
}

static void cmd_meminfo(void) {
    unsigned f = irq_save();                /* heap list must not change under us */
    unsigned heap_pages = (heap_end - HEAP_START) / PAGE;
    unsigned heap_used = 0, heap_free = 0;
    for (struct block *b = heap_head; b; b = b->next) {
        if (b->free) heap_free += b->size; else heap_used += b->size;
    }
    irq_restore(f);
    kprintf("RAM total : %u KB (%u MB)\n", ram_frames * 4, ram_frames * 4 / 1024);
    kprintf("Frames    : %u free / %u usable\n", free_frames, ram_frames);
    kprintf("Free RAM  : %u KB\n", free_frames * 4);
    kprintf("Heap      : %u pages mapped, %u bytes used, %u bytes free\n",
            heap_pages, heap_used, heap_free);
}

static void cmd_memmap(void) {
    static const char *types[] = { "?", "usable", "reserved", "ACPI reclaim", "ACPI NVS", "bad" };
    unsigned n = E820_COUNT;
    for (unsigned i = 0; i < n; i++) {
        struct e820 *e = &E820_MAP[i];
        unsigned t = e->type < 6 ? e->type : 0;
        kprintf("  %x  len %x  %s\n", (unsigned)e->base, (unsigned)e->len, types[t]);
    }
}

#define VMTEST_ADDRESS 0x00800000

static void cmd_vmtest(void) {
    address_space first = {0};
    address_space second = {0};
    address_space kernel = {0};

    unsigned frame_a = 0;
    unsigned frame_b = 0;
    unsigned irq = irq_save();

    int success = 0;

    kernel.directory = address_space_kernel_directory();

    /* Allocate two separate physical pages. */
    frame_a = pmm_alloc();
    frame_b = pmm_alloc();

    if (!frame_a || !frame_b)
        goto cleanup;

    memset((void *)frame_a, 0, PAGE);
    memset((void *)frame_b, 0, PAGE);

    /* Create two independent page directories. */
    if (!address_space_create(&first))
        goto cleanup;

    if (!address_space_create(&second))
        goto cleanup;

    /* Map the SAME virtual address to DIFFERENT physical pages. */
    if (!address_space_map_user(
            &first, VMTEST_ADDRESS, frame_a, PTE_W))
        goto cleanup;

    if (!address_space_map_user(
            &second, VMTEST_ADDRESS, frame_b, PTE_W))
        goto cleanup;

    volatile unsigned *probe =
        (volatile unsigned *)VMTEST_ADDRESS;

    /* Write through the first address space. */
    address_space_activate(&first);
    *probe = 0x11111111;

    /* Write through the second address space. */
    address_space_activate(&second);
    *probe = 0x22222222;

    /* Verify that the first value was not overwritten. */
    address_space_activate(&first);

    if (*probe != 0x11111111)
        goto cleanup;

    /* Verify the second value independently. */
    address_space_activate(&second);

    if (*probe != 0x22222222)
        goto cleanup;

    success = 1;

cleanup:
    /* Never leave the kernel running on a test page directory. */
    address_space_activate(&kernel);

    address_space_destroy(&first);
    address_space_destroy(&second);

    /* Destroying an address space does not free mapped data frames. */
    if (frame_a) pmm_free(frame_a);
    if (frame_b) pmm_free(frame_b);

    irq_restore(irq);

    if (success)
        kputs("VM test passed: address spaces are isolated.\n");
    else
        kputs("VM test FAILED.\n");
}

static void cmd_memtest(void) {
    void *a = kmalloc(100);
    void *b = kmalloc(5000);
    void *c = kmalloc(32);
    kprintf("a=%x b=%x c=%x\n", (unsigned)a, (unsigned)b, (unsigned)c);
    if (!a || !b || !c) { kputs("allocation failed\n"); return; }

    memset(a, 0xAB, 100);
    memset(b, 0xCD, 5000);

    kfree(b);
    void *d = kmalloc(4000);
    kprintf("freed b, then kmalloc(4000) -> d=%x (%s)\n", (unsigned)d,
            d == b ? "reused b's space" : "different spot");

    kfree(a); kfree(c); kfree(d);
    kputs("freed everything\n");
}

static void cmd_ps(void) {
    static const char *states[] = { "free ", "ready", "dead ", "sleep", "block" };
    kputs("ID  STATE  NAME\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == T_FREE) continue;
        kprintf("%d   %s  %s%s\n", i, states[tasks[i].state], tasks[i].name,
                &tasks[i] == current ? "  (this shell)" : "");
    }
}

static void cmd_procs(void) {
    static const char *states[] = {
        "free", "created", "ready", "running", "blocked", "exited"
    };

    struct process_info info;

    kputs("PID  STATE    ENTRY      USER STACK  NAME\n");

    for (unsigned i = 0; i < 16; ++i) {
        if (!process_get_info(i, &info))
            continue;

        kprintf("%u    %s  %x  %x  %s",
                info.pid,
                states[info.state],
                info.entry,
                info.user_stack,
                info.name);

        if (info.state == PROCESS_EXITED)
            kprintf(" (exit %d)", info.exit_code);

        kputc('\n');
    }
}

static void cmd_ls(const char *argument) {
    char path[VFS_PATH_MAX];

    if (!resolve_path(
            argument && *argument ? argument : ".",
            path,
            sizeof(path))) {
        kputs("invalid path\n");
        return;
    }

    struct vfs_dirent entries[16];

    int count = vfs_list(path, entries, 16);

    if (count < 0) {
        kputs("not a directory\n");
        return;
    }

    unsigned shown = count < 16 ? (unsigned)count : 16;

    for (unsigned i = 0; i < shown; ++i) {
        kprintf(
            "%s %s",
            entries[i].type == VFS_DIRECTORY ? "[DIR] " : "[FILE]",
            entries[i].name
        );

        if (entries[i].type == VFS_FILE)
            kprintf(" (%u bytes)", entries[i].size);

        kputc('\n');
    }

    if (count > 16)
        kprintf("... and %u more entries\n", (unsigned)count - 16);
}

static void cmd_cd(const char *argument) {
    char path[VFS_PATH_MAX];

    if (!resolve_path(
            argument && *argument ? argument : "/",
            path,
            sizeof(path))) {
        kputs("invalid path\n");
        return;
    }

    if (!vfs_is_directory(path)) {
        kputs("directory does not exist\n");
        return;
    }

    unsigned i = 0;

    while (path[i] && i < sizeof(current_directory) - 1) {
        current_directory[i] = path[i];
        ++i;
    }

    current_directory[i] = '\0';
}

static void cmd_mkdir(const char *argument) {
    char path[VFS_PATH_MAX];

    if (!resolve_path(argument, path, sizeof(path))) {
        kputs("invalid path\n");
        return;
    }

    if (!vfs_mkdir(path))
        kputs("could not create directory\n");
    else
        kputs("directory created\n");
}

static void cmd_touch(const char *argument) {
    char path[VFS_PATH_MAX];

    if (!resolve_path(argument, path, sizeof(path))) {
        kputs("invalid path\n");
        return;
    }

    if (vfs_exists(path)) {
        kputs("already exists\n");
        return;
    }

    if (!vfs_create(path))
        kputs("could not create file\n");
    else
        kputs("file created\n");
}

static void cmd_cat(const char *argument) {
    char path[VFS_PATH_MAX];
    char buffer[VFS_FILE_MAX];

    if (!resolve_path(argument, path, sizeof(path))) {
        kputs("invalid path\n");
        return;
    }

    int size = vfs_read(path, buffer, sizeof(buffer));

    if (size < 0) {
        kputs("could not read file\n");
        return;
    }

    for (int i = 0; i < size; ++i)
        kputc(buffer[i]);

    if (!size || buffer[size - 1] != '\n')
        kputc('\n');
}

static void cmd_write(char *argument) {
    while (*argument == ' ')
        ++argument;

    if (!*argument) {
        kputs("usage: write <file> <text>\n");
        return;
    }

    char *path_argument = argument;

    while (*argument && *argument != ' ')
        ++argument;

    if (!*argument) {
        kputs("usage: write <file> <text>\n");
        return;
    }

    *argument++ = '\0';

    while (*argument == ' ')
        ++argument;

    char path[VFS_PATH_MAX];

    if (!resolve_path(path_argument, path, sizeof(path))) {
        kputs("invalid path\n");
        return;
    }

    if (!vfs_exists(path)) {
        if (!vfs_create(path)) {
            kputs("could not create file\n");
            return;
        }
    }

    if (vfs_is_directory(path)) {
        kputs("cannot write to a directory\n");
        return;
    }

    unsigned size = fs_strlen(argument);

    if (!vfs_write(path, argument, size))
        kputs("write failed\n");
    else
        kputs("file written\n");
}

static int path_is_same_or_parent(
    const char *candidate,
    const char *directory
) {
    unsigned i = 0;

    while (candidate[i] && directory[i]) {
        char a = candidate[i];
        char b = directory[i];

        if (a >= 'a' && a <= 'z')
            a -= 32;

        if (b >= 'a' && b <= 'z')
            b -= 32;

        if (a != b)
            return 0;

        ++i;
    }

    /*
     * Candidate must be a complete path component,
     * not merely a matching string prefix.
     */
    if (candidate[i] == '\0')
        return 1;

    return 0;
}

static void cmd_rm(const char *argument) {
    char path[VFS_PATH_MAX];

    if (!resolve_path(argument, path, sizeof(path))) {
        kputs("invalid path\n");
        return;
    }

    if (path_is_same_or_parent(path, current_directory)) {
        kputs("cannot remove your current directory or one of its parents\n");
        return;
    }

    if (!vfs_remove(path))
        kputs("could not remove entry (directory may not be empty)\n");
    else
        kputs("removed\n");
}

static void cmd_disktest(void) {
    block_device disk;

    /* QEMU data.img is the primary IDE slave: device index 1. */
    if (!block_device_get(1, &disk)) {
        kputs("Data disk not found.\n");
        return;
    }

    if (disk.sector_count <= 1) {
        kputs("Data disk is too small.\n");
        return;
    }

    unsigned char original[ATA_SECTOR_SIZE];
    unsigned char pattern[ATA_SECTOR_SIZE];
    unsigned char result[ATA_SECTOR_SIZE];

    if (!disk.read(disk.id, 1, 1, original)) {
        kputs("Could not read test sector.\n");
        return;
    }

    for (unsigned i = 0; i < ATA_SECTOR_SIZE; ++i)
        pattern[i] = (unsigned char)((i * 37 + 123) & 0xFF);

    if (!disk.write(disk.id, 1, 1, pattern)) {
        kputs("Test write failed.\n");
        return;
    }

    int read_ok = disk.read(disk.id, 1, 1, result);
    int matches = read_ok;

    if (matches) {
        for (unsigned i = 0; i < ATA_SECTOR_SIZE; ++i) {
            if (result[i] != pattern[i]) {
                matches = 0;
                break;
            }
        }
    }

    int restored = disk.write(disk.id, 1, 1, original);

    if (!restored) {
        kputs("WARNING: Could not restore original sector!\n");
        return;
    }

    if (matches)
        kputs("Disk test passed: sector write/read/compare/restore succeeded.\n");
    else
        kputs("Disk test failed: read-back data did not match.\n");
}

static void cmd_fatinfo(void) {
    struct fat16_info info;

    if (!fat16_get_info(&info)) {
        kputs("No FAT16 filesystem mounted.\n");
        return;
    }

    unsigned long long bytes =
        (unsigned long long)info.total_sectors * 512ULL;

    kputs("FAT16 filesystem:\n");

    kprintf("  Device:              IDE %u\n", info.device);
    kprintf("  Partition start:     LBA %u\n",
            info.partition_start_sector);
    kprintf("  Size:                %u MB\n",
            (unsigned)(bytes / (1024ULL * 1024ULL)));
    kprintf("  Total sectors:       %u\n", info.total_sectors);
    kprintf("  Sectors per cluster: %u\n", info.sectors_per_cluster);
    kprintf("  Reserved sectors:    %u\n", info.reserved_sectors);
    kprintf("  FAT copies:          %u\n", info.fat_count);
    kprintf("  Sectors per FAT:     %u\n", info.sectors_per_fat);
    kprintf("  Root entries:        %u\n", info.root_entries);
    kprintf("  Root directory size: %u sectors\n",
            info.root_directory_sectors);
    kprintf("  Data start:          LBA %u\n", info.data_start_sector);
    kprintf("  Data clusters:       %u\n", info.cluster_count);
}

static void cmd_fatls(const char *argument) {
    const char *path =
        argument && *argument ? argument : "/";

    struct vfs_dirent entries[16];

    int count = fat16_list(path, entries, 16);

    if (count < 0) {
        kputs("Could not list FAT16 directory.\n");
        return;
    }

    for (unsigned i = 0; i < (unsigned)count && i < 16; ++i) {
        kprintf(
            "%s %s",
            entries[i].type == VFS_DIRECTORY
                ? "[DIR] " : "[FILE]",
            entries[i].name
        );

        if (entries[i].type == VFS_FILE)
            kprintf(" (%u bytes)", entries[i].size);

        kputc('\n');
    }

    if (count > 16)
        kprintf("... and %u more entries\n", (unsigned)count - 16);
}

static void cmd_fatcat(const char *path) {
    if (!path || !*path) {
        kputs("usage: fatcat /FILE.TXT\n");
        return;
    }

    /*
     * Static buffer avoids putting a large file buffer on the
     * shell's kernel stack. Files larger than this are rejected.
     */
    static char buffer[4096];

    int size = fat16_read(path, buffer, sizeof(buffer));

    if (size < 0) {
        kputs("Could not read FAT16 file (maximum 4096 bytes).\n");
        return;
    }

    for (int i = 0; i < size; ++i)
        kputc(buffer[i]);

    if (!size || buffer[size - 1] != '\n')
        kputc('\n');
}

static void cmd_disks(void) {
    kputs("ATA devices:\n");

    unsigned count = 0;

    for (unsigned i = 0; i < ATA_DEVICE_COUNT; ++i) {
        ata_device_info dev;

        if (!ata_get_device(i, &dev))
            continue;

        unsigned long long size =
            (unsigned long long)dev.sectors * 512ULL;

        kprintf("  IDE %u: %s\n", i, dev.model);
        kprintf("    Sectors: %u\n", dev.sectors);
        kprintf("    Size: %u MB\n",
                (unsigned)(size / (1024ULL * 1024ULL)));

        ++count;
    }

    if (!count)
        kputs("  No ATA disks detected.\n");

    kprintf("Total devices: %u\n", count);
}

static void run_command(char *input) {
    while (*input == ' ') input++;
    if (!*input) return;

    char *cmd = input;
    char *args = input;
    while (*args && *args != ' ') args++;
    if (*args) { *args++ = 0; while (*args == ' ') args++; }

    if (streq(cmd, "help")) {
        kputs("Commands:\n"
              "  help            show this list\n"
              "  clear           clear the screen\n"
              "  echo <text>     print text\n"
              "  uptime          time since boot\n"
              "  sleep <secs>    wait for N seconds\n"
              "  meminfo         RAM and heap usage\n"
              "  memmap          BIOS memory map\n"
              "  memtest         exercise kmalloc/kfree\n"
              "  ps              list tasks\n"
              "  spawn           start a background worker (prints 5 lines)\n"
              "  hog             start a task that spins forever\n"
              "  kill <id>       stop a task\n"
              "  pagefault [null] deliberately trigger a page fault\n"
              "  gdtinfo         show segment registers and task register\n"
              "  reboot          restart the machine\n"
              "  usermode        run a Ring 3 test program\n"
              "  procs           list user processes\n"
              "  vmtest          test virtual address-space isolation\n"
              "  ls [path]       list directory contents\n"
              "  cd [path]       change directory\n"
              "  pwd             print current directory\n"
              "  mkdir <path>    create directory\n"
              "  touch <file>    create empty file\n"
              "  cat <file>      display file contents\n"
              "  write <file> <text>  write text to file\n"
              "  rm <path>       remove file or empty directory\n"
              "  disks           list detected ATA disks\n"
              "  disktest        test disk sector read/write\n"
              "  fatinfo         show mounted FAT16 filesystem information\n"
              "  fatls [path]    list a FAT16 directory\n"
              "  fatcat <path>   read a FAT16 file\n");
    } else if (streq(cmd, "clear")) {
        clear();
    } else if (streq(cmd, "echo")) {
        kprintf("%s\n", args);
    } else if (streq(cmd, "uptime")) {
        unsigned t = ticks;
        kprintf("up %u s (%u ticks)\n", t / TIMER_HZ, t);
    } else if (streq(cmd, "sleep")) {
        unsigned secs = parse_uint(args);
        kprintf("sleeping %u s...\n", secs);
        for (unsigned s = 0; s < secs; s++)     /* no secs*1000 overflow */
            sleep_ms(1000);
        kputs("done\n");
    } else if (streq(cmd, "meminfo")) {
        cmd_meminfo();
    } else if (streq(cmd, "memmap")) {
        cmd_memmap();
    } else if (streq(cmd, "memtest")) {
        cmd_memtest();
    } else if (streq(cmd, "ps")) {
        cmd_ps();
    } else if (streq(cmd, "spawn")) {
        static unsigned next_worker = 1;
        int id = task_create("worker", worker, (void *)next_worker++);
        if (id < 0) kputs("could not create task\n");
        else kprintf("started task %d\n", id);
    } else if (streq(cmd, "hog")) {
        int id = task_create("hog", hog, 0);
        if (id < 0) kputs("could not create task\n");
        else kprintf("started task %d (spins forever; 'kill %d' to stop it)\n", id, id);
    } else if (streq(cmd, "kill")) {
        unsigned id = parse_uint(args);
        if (id == 0 || id >= MAX_TASKS || &tasks[id] == current ||
            tasks[id].state == T_FREE || tasks[id].state == T_DEAD)
            kputs("no such task (can't kill the shell)\n");
        else {
            tasks[id].state = T_DEAD;       /* schedule() frees its stack */
            kprintf("killed task %u\n", id);
        }
    } else if (streq(cmd, "pagefault")) {
        volatile unsigned *p = streq(args, "null") ? (unsigned *)0
                                                   : (unsigned *)0xDEAD0000;
        *p = 1;
    } else if (streq(cmd, "gdtinfo")) {
        unsigned short cs, ds, ss, tr;

        __asm__ volatile("mov %%cs, %0" : "=r"(cs));
        __asm__ volatile("mov %%ds, %0" : "=r"(ds));
        __asm__ volatile("mov %%ss, %0" : "=r"(ss));
        __asm__ volatile("str %0"       : "=r"(tr));

        kprintf("CS:  %x\n", (unsigned)cs);
        kprintf("DS:  %x\n", (unsigned)ds);
        kprintf("SS:  %x\n", (unsigned)ss);
        kprintf("TR:  %x\n", (unsigned)tr);
    } else if (streq(cmd, "reboot")) {
        outb(0x64, 0xFE);
        for (;;) __asm__ volatile("hlt");
    } else if (streq(cmd, "usermode")) {
        user_mode_test();
    } else if (streq(cmd, "procs")) {
        cmd_procs();
    } else if (streq(cmd, "vmtest")) {
        cmd_vmtest();
    } else if (streq(cmd, "pwd")) {
        kprintf("%s\n", current_directory);
    } else if (streq(cmd, "ls")) {
        cmd_ls(args);
    } else if (streq(cmd, "cd")) {
        cmd_cd(args);
    } else if (streq(cmd, "mkdir")) {
        cmd_mkdir(args);
    } else if (streq(cmd, "touch")) {
        cmd_touch(args);
    } else if (streq(cmd, "cat")) {
        cmd_cat(args);
    } else if (streq(cmd, "write")) {
        cmd_write(args);
    } else if (streq(cmd, "rm")) {
        cmd_rm(args);
    } else if (streq(cmd, "disks")) {
        cmd_disks();
    } else if (streq(cmd, "disktest")) {
        cmd_disktest();
    } else if (streq(cmd, "fatinfo")) {
        cmd_fatinfo();
    } else if (streq(cmd, "fatls")) {
        cmd_fatls(args);
    } else if (streq(cmd, "fatcat")) {
        cmd_fatcat(args);
    } else {
        kprintf("unknown command: %s (try 'help')\n", cmd);
    }
}

/* ---------- kernel main (becomes task 0: the shell) ---------- */
void kmain(void) {
    serial_init();
    ata_init();
    clear();

    kputs("ParallaxOS booted. Type 'help' for commands.\n");

    if (fat16_mount(1))
        kputs("FAT16: mounted IDE device 1.\n");
    else
        kputs("FAT16: no valid FAT16 filesystem on IDE device 1.\n");

    gdt_init();                     /* own the GDT before anything else relies on segments */
    pic_remap();
    idt_init();
    pmm_init();
    vmm_init();
    timer_init();
    process_manager_init();
    vfs_init();

    /* The code we're running right now becomes task 0 */
    tasks[0].state = T_READY;
    tasks[0].name[0] = 's'; tasks[0].name[1] = 'h'; tasks[0].name[2] = 'e';
    tasks[0].name[3] = 'l'; tasks[0].name[4] = 'l'; tasks[0].name[5] = 0;
    current = &tasks[0];

    task_create("clock", clock_task, 0);

    kprintf("Memory: %u MB usable, paging enabled, multitasking on.\n\n",
            ram_frames * 4 / 1024);
    __asm__ volatile("sti");

    prompt();
    for (;;) {
        read_line();
        run_command(line);
        prompt();
    }
}
