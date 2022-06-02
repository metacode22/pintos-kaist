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
int write (int fd, const void *buffer, unsigned size);
int open (const char *file);
int filesize (int fd);
int read (int fd, void *buffer, unsigned size);
void seek(int fd, unsigned position);
unsigned tell (int fd);
void close (int fd);

int add_file_to_fd_table (struct file *file);
struct file *get_file_from_fd_table (int fd);
void close_file_from_fd_table (int fd);

#endif /* userprog/syscall.h */
