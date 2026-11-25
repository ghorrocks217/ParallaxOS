#ifndef PROCESS_SYSCALL_H
#define PROCESS_SYSCALL_H

#define SYS_EXIT  0
#define SYS_WRITE 1
#define SYS_YIELD 2

#define SYSCALL_TERMINATE (-2147483647 - 1)

int syscall_handler(
    unsigned int number,
    unsigned int arg1,
    unsigned int arg2
);

#endif
