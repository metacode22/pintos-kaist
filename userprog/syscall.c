#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "include/filesys/filesys.h"
#include "threads/init.h"
#include "include/lib/stdio.h"
#include "stdbool.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void check_address (void *addr);
void halt (void);
void exit (int status);
tid_t fork (const char *thread_name, struct intr_frame *f);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int write (int fd, const void *buffer, unsigned size);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
check_address (void *uaddr) {							// SJ, 유저 프로그램이 시스템 콜을 요청할 때 요청한 포인터 인자가 NULL이거나, 커널 공간을 가르키는 포인터이거나, 가상 메모리에 맵핑되어 있지 않다면 프로세스를 종료시킨다.
	struct thread *current_thread = thread_current();
	
	if (uaddr == NULL || is_kernel_vaddr(uaddr) || pml4_get_page(current_thread->pml4, uaddr) == NULL) {
		exit(-1);
	}
}

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {				// SJ, 시스템 콜이 호출되면 시스템 콜 핸들러가 이 시스템 콜을 어떻게 다뤄야 할지 중재한다.
	// TODO: Your implementation goes here.
	// int syscall_number = f->R.rax;							// SJ, 사용자 프로그램이 어떤 시스템 콜을 요청한 것인지 확인해야 한다.
	
	switch(f->R.rax) {
		case SYS_HALT:
			halt();
			break;
		case SYS_EXIT:
			exit(f->R.rdi);
			break;
		case SYS_CREATE:
			create(f->R.rdi, f->R.rsi);
			break;
		case SYS_REMOVE:
			remove(f->R.rdi);
			break;
		case SYS_WRITE:
			f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		default:
			exit(-1);
			break;
	}
	
	// printf ("system call!\n");
	// thread_exit ();
}

void 
halt (void) {
	power_off();											// SJ, 핀토스 종료
}
          
void
exit (int status) {
    struct thread *cur = thread_current();
    cur->exit_status = status;                         // 종료시 상태를 확인, 정상종료면 state = 0
    printf("%s: exit(%d)\n", thread_name(), status); // 종료 메시지 출력
    thread_exit();   
}

bool
create (const char *file, unsigned initial_size) {
	check_address(file);
	return filesys_create(file, initial_size);				// SJ, file 생성 성공 시 true를 반환한다.
}

bool
remove (const char *file) {
	check_address(file);
	return filesys_remove(file);							// SJ, file 제거 성공 시 true를 반환한다.
}

int
write (int fd, const void *buffer, unsigned size) {
	if (fd == STDOUT_FILENO) {
		putbuf(buffer, size);
		return size;
	}
}

