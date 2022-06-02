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
#include "include/filesys/file.h"

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
			f->R.rax = create(f->R.rdi, f->R.rsi);
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
		case SYS_READ:
			f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_SEEK:
			seek(f->R.rdi, f->R.rsi);
			break;
		case SYS_TELL:
			f->R.rax = tell(f->R.rdi);
			break;
		case SYS_CLOSE:
			close(f->R.rdi);
			break;
		default:
			exit(-1);
			break;
	}
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
	
	return filesys_create(file, initial_size);														// SJ, file 생성 성공 시 true를 반환한다.
}

bool
remove (const char *file) {
	check_address(file);
	
	return filesys_remove(file);														// SJ, file 제거 성공 시 true를 반환한다.
}

int 
open (const char *file) {												// SJ, 디렉토리를 열어서? 디스크에서? 해당하는 파일을 찾아서, 그 파일만큼 메모리를 할당받고(filesys_open 안의 file_open에서 calloc) 파일 테이블에서 빈 fd에(add_file_to_fd_table) open한 파일을 배정시킨다.
	if (file == NULL) {
		return -1;
	}
	
	check_address(file);
	struct file *file_object = filesys_open(file);	
			
	if (file_object == NULL) {
		return -1;
	}
	
	int fd = add_file_to_fd_table(file_object);							// SJ, 해당 프로세스의 fd_table에서 빈 fd를 찾고 file을 배정시킨다. 프로세스(쓰레드)는 이 파일을 이용할 수 있게 된다.
	
	if (fd == -1) {
		lock_acquire(&filesys_lock);
		file_close(file_object);										// SJ, inode close하고 file이 할당 받은 메모리를 해제한다.
		lock_release(&filesys_lock);
	}
	
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

int
read (int fd, void *buffer, unsigned size) {		// SJ, fd로부터 size만큼 읽어서 buffer에 담아라. fd가 0이면 키보드 버퍼로부터 size만큼 읽어서 buffer에 담아라.
	check_address(buffer);							// SJ, read할 첫 부분을 체크(커널 공간이면 바로 빠꾸)
	// check_address(buffer + size - 1);				// SJ, 맨 끝 읽으려는 부분이 커널 공간일 수도 있기 때문에 맨 끝도 검사해준다. 맨 처음도 쳐주기 때문에 -1을 해준다.
	
	int read_count;
	struct file *file = get_file_from_fd_table(fd);
	
	if (file == NULL || file <= 0 || fd == STDOUT_FILENO || fd < 0) {
		return -1;
	}
	
	if (fd == STDIN_FILENO) {						// SJ, 키보드 입력을 통해 저장되어 있는 버퍼로부터 읽어온다.
		unsigned char *buf = buffer;				// SJ, 주소값에는 음수가 없다. 그리고 문자열에 접근할 때는 unsigned를 사용한다. 그냥 buffer를 그대로 쓰면, 나중에 buffer++하면서 주소값이 변할 수 있다. 즉 원본 buffer 주소를 나중에 쓸 수도 있는데, 원하는 처음 주소가 아닐 수도 있다.
		char key;									// SJ, 한 글자 한 글자
		
		lock_acquire(&filesys_lock);
		for (read_count = 0; read_count < size; read_count++) {			
			key = input_getc();						// SJ, 불릴 때마다 하나씩 옮겨가면서 읽어온다.
			*buf++ = key;							// SJ, buf가 가르키는 곳에 key라는 문자열을 넣는다.
			
			if (key == '\0') {						// SJ, 마지막 문자를 만나면 넣는 것을 멈춘다.
				break;
			}
		}
		lock_release(&filesys_lock);
	}
	
	else {
		lock_acquire(&filesys_lock);
		read_count = file_read(file, buffer, size);
		lock_release(&filesys_lock);
	}
	
	return read_count;
}

int
write (int fd, const void *buffer, unsigned size) {						// SJ, buffer에서 size만큼 복사해서 fd에 작성해라. fd가 1이면, buffer에서 size만큼 복사해서 콘솔(모니터)에 넣어라.
	check_address(buffer);
	check_address(buffer + size - 1);									// SJ, 쓰려는 공간이 커널 공간일 수도 있기 때문에 체크해준다.
	unsigned result;
	
	struct file *file = get_file_from_fd_table(fd);
	
	if (file == NULL || fd <= STDIN_FILENO || file <= 1) {
		result = -1;
	}
	
	else if (fd == STDOUT_FILENO) {										// SJ, 버퍼에 저장된 값을 화면에 출력해준다.
		lock_acquire(&filesys_lock);
		putbuf(buffer, size);			
		lock_release(&filesys_lock);								
		result = size;
	}
	
	else {
		lock_acquire(&filesys_lock);
		int write_count = file_write(file, buffer, size);
		lock_release(&filesys_lock);
		result = write_count;
	}
	
	return result;
}

void 
seek(int fd, unsigned position) {
	struct file *file = get_file_from_fd_table(fd);
	
	if (file == NULL || fd <= 1) {
		return;
	}
	
	file_seek(file, position);							// SJ, file의 pos가 position으로 이동한다.
}

unsigned
tell (int fd) {
	struct file *file = get_file_from_fd_table(fd);
	
	if (file == NULL || fd <= 1) {
		return;
	}
	
	return file_tell(file);									// SJ, file의 position을 반환한다.
}

void
close (int fd) {
	if (fd <= 1) {
		return;
	}
	
	struct file *file = get_file_from_fd_table(fd);
	
	if (file == NULL) {
		return;
	}
	
	close_file_from_fd_table(fd);						// SJ, 파일 테이블에서 닫는 것, 없애는 것, 해당 프로세스에서 해당 파일을 관리안하겠다.
	file_close(file);									// SJ, 
}

// SJ, file descriptor table 관련 helper functions
int 
add_file_to_fd_table (struct file *file) {
	// struct thread *curr = thread_current();
	// struct file **fdt = curr->fd_table;

	// while (curr->fd < 10 && fdt[curr->fd]) {
	// 	curr->fd++;
	// }

	// if (curr->fd >= 10) {
	// 	return -1;
	// }

	// fdt[curr->fd] = file;
	// return curr->fd;
	
	struct thread *current_thread = thread_current();
	struct file **fd_table = current_thread->fd_table;
	
	while (current_thread->fd < FDT_COUNT_LIMIT && fd_table[current_thread->fd]) {		// SJ, file descriptor table에 담을 수 있는 총 갯수인 3 * 2^9개보다 작을 동안, 그리고 파일 테이블에서 해당 파일 디스크립터가 이미 배정되어있다면 +1하면서 새로 배정할 곳을 찾아야 할 것이다.
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