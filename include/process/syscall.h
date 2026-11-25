#ifndef SYSCALL_H
#define SYSCALL_H

#define SYS_EXIT  0
#define SYS_WRITE 1
#define SYS_YIELD 2

int syscall_handler(
    unsigned int number,
    unsigned int arg1,
    unsigned int arg2
);

#endif
