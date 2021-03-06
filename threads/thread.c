#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "list.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

static struct list sleep_list;												// SJ, 기존 busy waiting 방식은 sleep_list가 없고 ready_list에 넣는 방식이다. 그러면 계속 ready_list에 접근하여 깨어나야 하는 쓰레드가 있는지 확인하게 된다.
																			// SJ, 이러한 방식은 계속해서 확인해야 하기에 CPU가 낭비될 수 있다. 따라서 sleep_list에 쓰레드를 잠재우고, 나중에 깨우는 방식으로 하면 CPU 낭비를 없앨 수 있다.

list_less_func *less;														// SJ, list_insert_ordered를 위함

static int64_t next_tick_to_awake;											// SJ, 현재 sleep_list, 즉 대기 중인 '쓰레드'들 중의 wake_tick 변수 중 가장 작은 값을 저장하게 된다.

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);
	list_init (&sleep_list); 												// SJ, 리스트가 NULL인지 아닌지 확인하고 초기화시킨다. 
																			// head의 이전 노드를, tail의 다음 노드를 NULL로 하고(더미 노드 설치), head와 tail을 이어준다.

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

void donate_priority(void) {													// SJ, 다른 쓰레드가 락을 쥐고 있어서, 그 락을 원하는 현재 쓰레드가 락을 얻지 못할 때, 락을 쥐고 있는 쓰레드에게 우선순위를 기부한다. 이 때 락을 쥐고 있는 쓰레드의 
	int nested_depth = 0;
	struct thread* current_thread = thread_current();
	struct thread* search_thread = current_thread;
	
	while (nested_depth != 8) {													// SJ, 핀토스의 nested_depth는 8로 제한되어 있다고 한다.
		nested_depth--;
		
		if (search_thread->wait_on_lock == NULL) {								// SJ, 탐색하고 있는 쓰레드가 더 이상 기다리고 있는 자원이 없다면 우선순위를 기부하는 것이 멈추게 된다.
			break;
		}
		
		struct thread* next_thread = search_thread->wait_on_lock->holder;		// SJ, 현재 쓰레드의 wait_on_lock 설정은 lock_acquire에서 이미 해주었다.
		next_thread->priority = search_thread->priority;
		search_thread = next_thread;
	}
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

void test_max_priority(void) {										// SJ, 새로운 쓰레드가 생겨서 CPU를 뺏어와야 하거나, 현재 CPU의 우선순위가 바뀌었을 때, ready_list의(이미 우선순위가 높은 것이 앞에 오도록 정렬되어 있다) 가장 앞 쓰레드와 비교하여, 조건 만족 시 yield한다.
	struct thread *current_thread = thread_current();
	struct thread *ready_list_front = list_entry(list_begin(&ready_list), struct thread, elem);
	
	if (current_thread->priority < ready_list_front->priority) {	// SJ, 현재 쓰레드가 ready_list에서 가장 우선순위가 높은 맨 앞 쓰레드보다 우선순위가 작다면
		thread_yield();												// SJ, CPU에서 러닝 중인 것을 ready_list로 내리고, ready_list의 맨 앞 쓰레드를 CPU에 올린다.
	}
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) {							// SJ, 현재 CPU를 점유하고 있는 쓰레드의 우선 순위가 바뀐다면 검사해서 yield할지 말지 판단해야한다.
	thread_current()->priority = new_priority;						// SJ, 현재 CPU의 우선순위가, ready_list 맨 앞의 쓰레드보다 낮아졌다면 yield가 되어야 할 것이다.
	thread_current()->init_priority = new_priority;						
	
	refresh_priority();												// ??					
	test_max_priority();
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;
	enum intr_level old_level;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);						// SJ, 처음 쓰레드의 상태는 Block이다. init_thread 들어가면 block으로 초기화된다.
	tid = t->tid = allocate_tid ();

	t->fd_table = palloc_get_multiple(PAL_ZERO, FDT_PAGES); // SJ, 파일 구조체를 가르키는 주소값을 가진 포인터(8byte)를 담을 파일 테이블을 위해 페이지를 할당받는다.
															// SJ, 이 때 FDT_PAGES는 3이니까 3개의 페이지를 할당받지 않을까? 그리고 주석 설명처럼 연속해서 사용할 수 있는 페이지를 할당받나보다.
															// SJ, PAL_ZERO라서 페이지를 0으로 초기화한다.
	if (t->fd_table == NULL) {								// SJ, file descriptor table를 할당받는 데에 실패하면 에러를 반환한다. 할당받을 수 있는 페이지 수가 적으면 NULL 포인터를 반환한다.
		return TID_ERROR;
	}
	
	// SJ, fd_table[fd] = 우리가 찾고자 하는 파일
	t->fd = 2;												// SJ, 0은 stdin, 1은 stdout이 이미 할당되어있다고 보아야 한다.
	t->fd_table[0] = 1;										// SJ, Dummy Value
	t->fd_table[1] = 2;										// SJ, Dummy Value
	
	list_push_back(&thread_current()->child_list, &t->child_elem); // SJ, 현재 쓰레드는 부모 쓰레드이다. 부모 쓰레드의 자식 리스트에, 지금 만들고 있는 쓰레드(자식 쓰레드)를 추가한다.

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	// struct thread *current_thread = thread_current();		// SJ, CPU가 비어있더라도, 즉 thread_current()가 NULL로 반환되더라도, else문을 만나, 새로운 쓰레드는 ready_list에 들어갔다가 yield가 된다.

	// old_level = intr_disable ();							// SJ, 현재 인터럽트 값 저장
	// list_insert_ordered(&ready_list, &t->elem, cmp_priority, 0);
	// t->status = THREAD_READY;
	thread_unblock(t);
	if (!list_empty(&ready_list)) {							// SJ, ready_list가 비어있지 않다면, ready_list에 넣고 yield할지 말지 판단한다.
		// list_insert_ordered(&ready_list, &t->elem, cmp_priority, 0);
		test_max_priority();
		
	} else {												// SJ, ready_list가 비어있다면, ready_list에 넣고 yield하여 CPU에 올라가도록 한다.
		// list_insert_ordered(&ready_list, &t->elem, cmp_priority, 0);
		thread_yield();
	}
	
	// thread_unblock(t);		
	// test_max_priority();									// SJ, ready_list에서 우선순위가 바뀌었을 수도 있으니(CPU에 담긴 것보다 ready_list의 맨 앞 쓰레드의 우선순위가 더 높을 수 있으니(방금 생성된 쓰레드)) 우선순위 비교해서 yield 시켜줘야 한다.
	// intr_set_level (old_level);								// SJ, 인터럽트 원복
	
	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) {														
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;													// SJ, 현재 쓰레드의 상태를 Block으로 바꾼다.
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	// list_push_back (&ready_list, &t->elem); 							// SJ, block&sleep, busy&waiting만 했을 때이다.
	list_insert_ordered(&ready_list, &t->elem, cmp_priority, 0); 		// SJ, priority(우선 순위)를 고려해줘야 하므로 ready_list가 계속 내림차순으로 정렬되어있게 삽입해야 한다.
	t->status = THREAD_READY;											// SJ, BLOCK임을 확인하고 READY로 바꿔준다.
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

bool
cmp_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct thread *t1 = list_entry(a, struct thread, elem);
	struct thread *t2 = list_entry(b, struct thread, elem);
	return t1->priority > t2->priority;															// SJ, list_insert_ordered에서 less(elem, e, aux)를 보면, elem이 우리가 넣고자 하는 쓰레드이고, e가 list에 담긴 쓰레드이다.
}																								// SJ, 만약 리스트 우선순위가 [9 3 1]이고(내림차순으로 정렬되어 있을 것이다) 여기에 7을 넣으려고 한다면 e가 3일 때 t1->priority > t2->priority가 참이 되어 break된다.
																								// SJ, 그러면 list_insert로 인해 3의 앞으로 7이 찢어서 들어가게 된다.
void refresh_priority(){
	struct thread* current_thread = thread_current();
	current_thread->priority = current_thread->init_priority;									// SJ, 현재 쓰레드가 락을 반납하면, 현재 쓰레드의 우선순위는 일단 원래 우선순위로 돌아간다. if 문을 충족하지 못한다면 그냥 원래 우선순위로 저장된 그대로 남아있으면 된다.
	
	if (!list_empty(&current_thread->donations)) {
		list_sort(&current_thread->donations, cmp_priority, 0);									// lock_acquire에서 락을 얻지 못한 쓰레드를 donation에 넣어줄 때 이미 list_insert_ordered를 썼기 떄문에 sort를 해주지 않아도 될 것 같지만, 혹시나 몰라서 추가한다. 살제로 해당 코드가 존재하지 않아도 테스트 케이스를 통과한다.
		struct thread* front_thread = list_entry(list_front(&current_thread->donations), struct thread, donation_elem);
		
		if (front_thread->priority > current_thread->priority) {
			current_thread->priority = front_thread->priority;
		}
	}
}
																					
// SJ, 현재 CPU에서 running중인 쓰레드를 ready_list에 넣고, 
// do schedule, 쓰레드의 상태(CPU에 있던 쓰레드)를 ready 상태로 바꾼다.
// schedule, ready_list에서 맨 앞의 쓰레드 상태(ready_list에서 ready였던 쓰레드)를 running으로 바꾸고 ready_list에서 맨 앞의 쓰레드를 뽑아서 CPU에 올린다.

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) {																			// SJ, 4 tick(TIME_SLICE)마다 실행된다. 러닝 상태의 쓰레드가 sleep_list로 가게 된다.
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_insert_ordered(&ready_list, &curr->elem, cmp_priority, 0);							// SJ, CPU가 비어있다면 무시하게 된다. 즉 ready_list에서 맨 앞의 쓰레드를 CPU에 올리는 과정만 한다(do_schedule).
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

void
thread_sleep (int64_t wakeup_time) {															// SJ, wakeup_time : 쓰레드가 언제 깰 지, 즉 '시각'이다.
	struct thread *curr = thread_current();
	enum intr_level old_level;
	
	old_level = intr_disable();																	// SJ, 밑의 과정을 하는 동안 다른 인터럽트가 방해하지 않도록, 인터럽트를 무시하도록 설정한다.
	if (curr != idle_thread) {																	// SJ, 현재 쓰레드가 idle(빈) 쓰레드가 아닐 경우, idle_thread 구조체는 다 비어있다.
		curr->wake_ticks = wakeup_time;															// SJ, sleep_list로 내릴 쓰레드의 wake_ticks, 즉 꺠어날 시간을 현재 인자로 들어온 wakeup_time으로 바꾼다.(언제 그 쓰레드가 깨어나야 되는지 갱신해준다)
		list_push_back(&sleep_list, &curr->elem);												// SJ, sleep_list에 잠들 쓰레드를 넣는다.
		update_next_tick_to_awake(curr->wake_ticks);											// SJ, sleep_list에 새로운 쓰레드가 들어왔으니, 그 쓰레드가 가장 작은 값일 수도 있으므로 nexy_tick_to_awake를 갱신한다.
	}
	
	do_schedule(THREAD_BLOCKED);																// SJ, 위 과정을 통해 sleep_list로 쓰레드가 들어간 다음, 그 쓰레드의 상태를 BLOCK으로 바꾸고, ready_list의 한 쓰레드가 CPU 제어권을 잡도록 한다.
	intr_set_level(old_level);																	// SJ, 인터럽트가 다시 ON 되도록 한다.
}

int64_t
get_next_tick_to_awake(void) {																	// SJ, 지금 현재 sleep_list에서 가장 작은 wake_time을 가진 쓰레드의 wake_time을 가져온다.
	return next_tick_to_awake;
}

void
thread_awake(int64_t ticks) {																	// SJ, sleep_list에서, ticks에 대해 깨어나야 할 쓰레드를 꺠운다.
	struct list_elem *tmp;	
	tmp = list_begin(&sleep_list);
	next_tick_to_awake = INT64_MAX;
	
	while(tmp != list_end(&sleep_list)) {
		struct thread *current_thread = list_entry(tmp, struct thread, elem);
		
		if (current_thread->wake_ticks <= ticks) {												// SJ, 현재 ticks보다 작거나 같다면 깨어나야 한다.
			tmp = list_remove(&current_thread->elem);											// SJ, tmp가 자동으로 다음을 가리키게 된다.
			thread_unblock(current_thread);														// SJ, BLOCK -> READY 해주고, ready_list에 그 쓰레드를 넣는다.
			
		} else {
			tmp = list_next(tmp);																
			update_next_tick_to_awake(current_thread->wake_ticks);
		}
	}
}

void
update_next_tick_to_awake(int64_t wakeup_time) {
	next_tick_to_awake = (next_tick_to_awake > wakeup_time) ? wakeup_time : next_tick_to_awake;  // SJ, 현재 sleep_list에서 가장 작은 wake_ticks의 쓰레드의 wake_ticks보다, sleep_list로 들어올 쓰레드의 wake_ticks가 더 작다면, 
																								 // SJ, next_tick_to_awake값을 현재 들어올 쓰레드의 wakeup_time으로 갱신한다.
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;
	
	t->init_priority = priority;				// SJ, 원래 자신의 우선순위로 돌아오려면 원래 자신의 우선순위를 저장해두어야 한다.
	t->wait_on_lock = NULL;						// SJ, 쓰레드가 기다리는 락은 처음엔 없을 것이다. lock_acquire(lock)을 통해 lock을 얻게 되는데, 이 때 lock을 얻지 못하면 이 쓰레드의 wait_on_lock에 그 lock이 저장된다.
	list_init(&t->donations);					// SJ, donations을 초기화한다.
	
	list_init(&t->child_list);					// SJ, 자식 쓰레드 리스트를 초기화한다.
	
	t->exit_status = 0;
	sema_init(&t->fork_sema, 0);
	sema_init(&t->wait_sema, 0);
	sema_init(&t->free_sema, 0);
	
	t->running_file = NULL;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {											// SJ, CPU 주도권을 쓰레드가 잡게 해준다.
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}


// SJ, thread_sleep에서 현재 running중인 쓰레드를 sleep_list에 넣은 다음
// do_schedule로 그 쓰레드의 상태를 Block으로 바꾸고
// schedule를 통해 CPU에서 run하게 될 다음 쓰레드를, ready_list로부터 뽑아서(next_thread_to_run에 구현되어 있음)
// ready_list로부터 뽑힌 그 쓰레드에게 CPU 주도권을 준다.(schedule의 thread_launch()를 이용하여)

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;										// SJ, 현재 쓰레드의 상태를 status로 바꾼다.
	schedule ();
}

static void
schedule (void) {			
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used bye the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);												// next에게 CPU 제어권을 준다.
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}


