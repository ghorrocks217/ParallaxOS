#include <stdarg.h>
#include "drivers/serial.h"
#include "cpu/gdt.h"
#include "cpu/idt.h"
#include "process/syscall.h"
#include "process/process.h"

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

#define USER_CODE  0x00400000
#define USER_STACK 0xBFFFF000

static void user_mode_test(void) __attribute__((noreturn));

static void user_mode_test(void) {
    unsigned code_page = pmm_alloc();
    unsigned stack_page = pmm_alloc();

    if (!code_page || !stack_page)
        panic("could not allocate user pages");

    /*
     * Machine code:
     *
     * mov eax, 1
     * mov ebx, 0x00400100
     * int 0x80
     * jmp $
     *
     * The string is stored at offset 0x100.
     */
    static const unsigned char user_program[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00,
        0xBB, 0x00, 0x01, 0x40, 0x00,
        0xCD, 0x80,
        0xEB, 0xFE
    };

    static const char message[] = "Hello from Ring 3!\n";

    memset((void *)code_page, 0, PAGE);
    memset((void *)stack_page, 0, PAGE);

    memcpy((void *)code_page, user_program, sizeof(user_program));
    memcpy((void *)(code_page + 0x100), message, sizeof(message));

    vmm_map(USER_CODE, code_page, PTE_W | PTE_U);
    vmm_map(USER_STACK, stack_page, PTE_W | PTE_U);

    int pid = process_create(
        "ring3-demo",
        USER_CODE,
        USER_STACK + PAGE,
        (unsigned)page_dir
    );

    if (pid < 0)
        panic("process table is full");

    process_set_state((unsigned)pid, PROCESS_RUNNING);

    kprintf("Starting process %u in Ring 3...\n", (unsigned)pid);

    kprintf("Entering Ring 3...\n");

    __asm__ volatile(
        "cli\n\t"

        "mov $0x23, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"

        "push $0x23\n\t"
        "push %0\n\t"

        "pushf\n\t"
        "pop %%eax\n\t"
        "or $0x200, %%eax\n\t"
        "push %%eax\n\t"

        "push $0x1B\n\t"
        "push %1\n\t"

        "iret\n\t"

        :
        : "r"(USER_STACK + PAGE),
          "r"(USER_CODE)
        : "eax", "memory"
    );

    __builtin_unreachable();
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
#define STACK_SIZE 8192
#define BOOT_STACK_TOP 0x90000u

enum { T_FREE = 0, T_READY, T_DEAD, T_SLEEPING, T_BLOCKED };

struct task {
    int state;
    unsigned esp;                   /* saved stack pointer while not running */
    void *stack;                    /* kmalloc'd stack (0 for the shell task) */
    void (*fn)(void *);
    void *arg;
    unsigned wake;                  /* tick to wake at (T_SLEEPING) */
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

    /* wake sleepers whose time has come (signed compare: tick wraparound safe) */
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == T_SLEEPING && (int)(ticks - tasks[i].wake) >= 0)
            tasks[i].state = T_READY;

    /* free the stacks of finished tasks (never the one we're running on) */
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
        if (tasks[n].state != T_READY) continue;
        if (&tasks[n] == current) return;       /* nothing else to run */

        struct task *prev = current;
        current = &tasks[n];
        /* ring-3 -> ring-0 entries must land on the new task's kernel stack
         * (the shell task has no kmalloc'd stack: it uses the boot stack) */
        tss_set_stack(current->stack ? (unsigned)current->stack + STACK_SIZE
                                     : BOOT_STACK_TOP);
        switch_context(&prev->esp, current->esp);
        return;
    }
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
    for (;;) n++;                   /* never yields, but gets preempted anyway */
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

#define LINE_MAX 128
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

    for (unsigned i = 0; i < capacity - 1; i++) {
        unsigned addr = src + i;

        // Detect address overflow.
        if (addr < src)
            return 0;

        unsigned di = addr >> 22;
        unsigned ti = (addr >> 12) & 0x3FF;

        // The page directory entry must exist and be user-accessible.
        if (!(page_dir[di] & PTE_P) || !(page_dir[di] & PTE_U))
            return 0;

        unsigned *pt = (unsigned *)(page_dir[di] & 0xFFFFF000);
        unsigned pte = pt[ti];

        // The page table entry must also be user-accessible.
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
        r->eax = (unsigned)syscall_handler(
            r->eax,
            r->ebx,
            r->ecx
        );
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

/* ---------- shell ---------- */
static void prompt(void) { kputs("os> "); }

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
              "  procs           list user processes\n");
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
    } else {
        kprintf("unknown command: %s (try 'help')\n", cmd);
    }
}

/* ---------- kernel main (becomes task 0: the shell) ---------- */
void kmain(void) {
    serial_init();
    clear();
    kputs("MyOS booted. Type 'help' for commands.\n");

    gdt_init();                     /* own the GDT before anything else relies on segments */
    pic_remap();
    idt_init();
    pmm_init();
    vmm_init();
    timer_init();
    process_manager_init();

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
