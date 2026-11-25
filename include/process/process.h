#ifndef PROCESS_PROCESS_H
#define PROCESS_PROCESS_H

#define PROCESS_NAME_MAX 24

enum process_state {
    PROCESS_FREE = 0,
    PROCESS_CREATED,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_EXITED
};

struct process_info {
    unsigned pid;
    unsigned state;
    unsigned entry;
    unsigned user_stack;
    unsigned page_directory;
    int exit_code;
    char name[PROCESS_NAME_MAX];
};

#ifdef __cplusplus
extern "C" {
#endif

void process_manager_init(void);

int process_create(
    const char *name,
    unsigned entry,
    unsigned user_stack,
    unsigned page_directory
);

int process_set_state(unsigned pid, unsigned state);
int process_exit(unsigned pid, int exit_code);
unsigned process_count(void);
int process_get_info(unsigned index, struct process_info *out);

#ifdef __cplusplus
}
#endif

#endif
