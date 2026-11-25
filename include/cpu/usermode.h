#ifndef CPU_USERMODE_H
#define CPU_USERMODE_H

void enter_user_mode(
    unsigned entry,
    unsigned user_stack,
    unsigned kernel_stack_top
);

void leave_user_mode(void) __attribute__((noreturn));

#endif
