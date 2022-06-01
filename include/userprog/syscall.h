#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include <stdbool.h>
#include <include/threads/thread.h>
#include <include/threads/synch.h>

struct lock filesys_lock;

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void check_address (void *addr);
void halt (void);
void exit (int status);
tid_t fork (const char *thread_name, struct intr_frame *f);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);

#endif /* userprog/syscall.h */
