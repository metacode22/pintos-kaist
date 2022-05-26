#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#ifdef VM
#include "vm/vm.h"
#endif


/* States in a thread's life cycle. */
enum thread_status {
	THREAD_RUNNING,     /* Running thread. */
	THREAD_READY,       /* Not running but ready to run. */
	THREAD_BLOCKED,     /* Waiting for an event to trigger. */
	THREAD_DYING        /* About to be destroyed. */
};

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

/* A kernel thread or user process.
 *
 * Each thread structure is stored in its own 4 kB page.  The
 * thread structure itself sits at the very bottom of the page
 * (at offset 0).  The rest of the page is reserved for the
 * thread's kernel stack, which grows downward from the top of
 * the page (at offset 4 kB).  Here's an illustration:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * The upshot of this is twofold:
 *
 *    1. First, `struct thread' must not be allowed to grow too
 *       big.  If it does, then there will not be enough room for
 *       the kernel stack.  Our base `struct thread' is only a
 *       few bytes in size.  It probably should stay well under 1
 *       kB.
 *
 *    2. Second, kernel stacks must not be allowed to grow too
 *       large.  If a stack overflows, it will corrupt the thread
 *       state.  Thus, kernel functions should not allocate large
 *       structures or arrays as non-static local variables.  Use
 *       dynamic allocation with malloc() or palloc_get_page()
 *       instead.
 *
 * The first symptom of either of these problems will probably be
 * an assertion failure in thread_current(), which checks that
 * the `magic' member of the running thread's `struct thread' is
 * set to THREAD_MAGIC.  Stack overflow will normally change this
 * value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
 * the run queue (thread.c), or it can be an element in a
 * semaphore wait list (synch.c).  It can be used these two ways
 * only because they are mutually exclusive: only a thread in the
 * ready state is on the run queue, whereas only a thread in the
 * blocked state is on a semaphore wait list. */
struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. */
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* Priority. */
	int64_t wake_ticks;					// SJ, 8바이트

	/* Shared between thread.c and synch.c. */
	struct list_elem elem;              /* List element. */
	
	int init_priority;					// SJ, 자원을 모두 반납하고 나면 이전의, 본래 가지고 있던 우선순위로 돌아갈 수 있어야 한다. 따라서 본래의 우선순위를 저장하기 위해 새로운 저장 매체를 만든다.
	struct lock *wait_on_lock;			// SJ, 이 쓰레드가 원하는 락을, 다른 쓰레드가 이미 점유하고 있어(이 쓰레드의 우선순위가 낮더라도 뺏을 수 없다) 이 쓰레드는 원하는 락을 얻을 수 없다. 이 기다리는 락을 wait_on_lock에 저장한다. 자원을 쥐고 있는 쓰레드가 그 자원을 반납할 때, 그 쓰레드에 줄 서 있는 donation 노드들의 그 자원을 원하는 쓰레드를 찾아서 제거해주기 위해(즉 판별하기 위해) 존재한다. remove_with_lock에서 사용된다.
	struct list donations;				// SJ, 반대로, 이 쓰레드가 소유한 락을 얻기 위한 쓰레드들은, 자신의 우선순위를 기부하며 이 쓰레드가 빨리 끝나길 바란다. 이 쓰레드를 donations라는 리스트에 저장한다. 이 친구들도 순서가 있다. 우선순위대로 정렬해주어야 한다.
	struct list_elem donation_elem;		// SJ, 기부한 쓰레드들을 저장하기 위한 노드들이다. 즉 이어주기 위해 존재한다.
	
	
#ifdef USERPROG
	/* Owned by userprog/process.c. */
	uint64_t *pml4;                     /* Page map level 4 */
#endif
#ifdef VM
	/* Table for whole virtual memory owned by thread. */
	struct supplemental_page_table spt;
#endif

	/* Owned by thread.c. */
	struct intr_frame tf;               /* Information for switching */
	unsigned magic;                     /* Detects stack overflow. */
};

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

void thread_sleep (int64_t ticks);															// SJ, 쓰레드를 sleep list에 넣는다. 실행 중인 쓰레드를 sleep으로 만든다.
void thread_awake (int64_t ticks);															// SJ, sleep_list에서 깨워야할 쓰레드를 깨운다. 즉, sleep_list에서 ready_list로 넣는다.
void update_next_tick_to_awake (int64_t ticks); 											// SJ, sleep_list에서 최소 wake_tick을 가진 쓰레드의 wake_ticks로 갱신한다. 즉, next_tick_to_awake를 갱신한다. '재울 때', '깨울 때' update를 하면 된다.
int64_t get_next_tick_to_awake (void);														// SJ, next_tick_to_awake 값을 반환한다. 가져온다.

void test_max_priority (void);																// SJ, 새로운 쓰레드가 생겨서 CPU를 뺏어와야 하거나, 현재 CPU의 우선순위가 바뀌었을 때, ready_list의(이미 우선순위가 높은 것이 앞에 오도록 정렬되어 있다) 가장 앞 쓰레드와 비교하여, 조건 만족 시 yield한다.
bool cmp_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED);	// SJ, a와 b는, 쓰레드들을 이어줄 수 있게 하는 노드이다. 즉 쓰레드라고 봐도 무방하다. list_entry를 통해 쓰레드를 뽑아낼 수 있다. 쓰레드 a의 우선순위가 쓰레드 b의 우선순위보다 높다면 true를 반환한다.
#endif /* threads/thread.h */
