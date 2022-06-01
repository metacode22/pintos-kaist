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

int add_file_to_fd_table (struct file *file);
struct file *get_file_from_fd_table (int fd);
void close_file_from_fd_table (int fd);

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
check_address (void *uaddr) {											// SJ, 유저 프로그램이 시스템 콜을 요청할 때 요청한 포인터 인자가 NULL이거나, 커널 공간을 가르키는 포인터이거나, 가상 메모리에 맵핑되어 있지 않다면 프로세스를 종료시킨다.
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
			
	lock_init(&filesys_lock);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {							// SJ, 시스템 콜이 호출되면 시스템 콜 핸들러가 이 시스템 콜을 어떻게 다뤄야 할지 중재한다.
	// TODO: Your implementation goes here.
	// int syscall_number = f->R.rax;									// SJ, 사용자 프로그램이 어떤 시스템 콜을 요청한 것인지 확인해야 한다.
	
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
		case SYS_OPEN:
			f->R.rax = open(f->R.rdi);
			break;
		case SYS_FILESIZE:
			f->R.rax = filesize(f->R.rdi);
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
	power_off();														// SJ, 핀토스 종료
}
          
void
exit (int status) {
    struct thread *current_thread = thread_current();
    current_thread->exit_status = status;                         		// SJ, 종료시 상태를 확인, 정상종료면 state = 0
    printf("%s: exit(%d)\n", current_thread->name, status); 			// SJ, 종료 메시지 출력
    thread_exit();   
}

bool
create (const char *file, unsigned initial_size) {
	check_address(file);
	
	lock_acquire(&filesys_lock);
	bool result = filesys_create(file, initial_size);
	lock_release(&filesys_lock);
	
	return result;														// SJ, file 생성 성공 시 true를 반환한다.
}

bool
remove (const char *file) {
	check_address(file);
	
	lock_acquire(&filesys_lock);
	bool result = filesys_remove(file);
	lock_release(&filesys_lock);
	
	return result;														// SJ, file 제거 성공 시 true를 반환한다.
}

int
write (int fd, const void *buffer, unsigned size) {
	if (fd == STDOUT_FILENO) {
		putbuf(buffer, size);											// SJ, console에 대한 lock(console_lock)을 얻고 작업을 마친 후 lock을 해제한다.
		return size;													// SJ, console에 대한 작업도 겹치면 안되기 때문에 lock을 걸어준다.
	}
}

int 
open (const char *file) {												// SJ, 디렉토리를 열어서? 디스크에서? 해당하는 파일을 찾아서, 그 파일만큼 메모리를 할당받고(filesys_open 안의 file_open에서 calloc) 파일 테이블에서 빈 fd에(add_file_to_fd_table) open한 파일을 배정시킨다.
	check_address(file);
	
	lock_acquire(&filesys_lock);
	struct file *file_object = filesys_open(file);			
	
	if (file_object == NULL) {
		return -1;
	}
	
	int fd = add_file_to_fd_table(file_object);							// SJ, 해당 프로세스의 fd_table에서 빈 fd를 찾고 file을 배정시킨다.
	
	if (fd == -1) {
		file_close(file_object);										// SJ, inode close하고 file이 할당 받은 메모리를 해제한다.
	}
	lock_release(&filesys_lock);
	
	return fd;															// SJ, 실패했으면 -1을 반환할 것이다.
}

int	
filesize (int fd) {														// SJ, 파일의 사이즈를 반환한다. off_t가 int32_t니까 filesize가 1이면 4바이트일 것 같다.
	struct file *file = get_file_from_fd_table(fd);
	
	if (file == NULL) {
		return -1;
	}
	
	off_t file_size = file_length(file);
	return file_size;
}

// SJ, file descriptor table 관련 helper functions
int 
add_file_to_fd_table(struct file *file) {
	struct thread *current_thread = thread_current();
	struct file **fd_table = current_thread->fd_table;
	
	while (current_thread < FDT_COUNT_LIMIT && fd_table[current_thread->fd]) {		// SJ, file descriptor table에 담을 수 있는 총 갯수인 3 * 2^9개보다 작을 동안, 그리고 파일 테이블에서 해당 파일 디스크립터가 이미 배정되어있다면 +1하면서 새로 배정할 곳을 찾아야 할 것이다.
		current_thread->fd++;														// SJ, 만약 2, 3, 4가 배정되어 있었는데 3이 빠진다면, 새로운 파일을 추가할 때 while문의 2번째 조건으로 인해 while문을 빠져나오고 3에 배정할 것이다.
	}
	
	if (current_thread->fd >= FDT_COUNT_LIMIT) {									// SJ, 테이블이 꽉차 있으면 -1을 반환한다.
		return -1;																	
	}
	
	fd_table[current_thread->fd] = file;											// SJ, fd_table[current_thread->fd] 이것도 포인터라고 보면 된다.						
	return current_thread->fd;
}

struct file *get_file_from_fd_table (int fd) {
	struct thread *current_thread = thread_current();
	struct file **fd_table = current_thread->fd_table;
	
	if (fd < 0 || fd >= FDT_COUNT_LIMIT) {											// SJ, 사용자 프로그램이 잘못된 fd를 요청하면 NULL을 반환한다.
		return NULL;
		
	} else {
		return fd_table[fd];														// SJ, struct file을 가르키는 포인터를 반환한다.
	} 
}

void close_file_from_fd_table (int fd) {											// SJ, 지금 할당을 해제시키는 것이 아니라, process_exit()할 때 모든 파일들을 할당해제 한다.
	struct thread *current_thread = thread_current();
	struct file **fd_table = current_thread->fd_table;
	
	if (fd < 0 || fd >= FDT_COUNT_LIMIT) {
		return;
		
	} else {
		fd_table[fd] = NULL;
	}																	
}