/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "list.h"

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */

/* One semaphore in a list. */
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
};

// SJ, thread 찾아서 우선순위 비교. 우선순위는 쓰레드가 가지고 있다. 결국엔 쓰레드를 찾아서 우선순위를 비교해야한다.
bool
cmp_sem_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {			// SJ, a, b 인자로는 semaphore_elem에 포함된, semaphore_elem을 이어주고 있는 list_elem이 들어온다.
	struct semaphore_elem *semaphore_elem_a = list_entry(a, struct semaphore_elem, elem);			// SJ, list_entry를 통해 이 list_elem를 멤버로 하는 semaphore_elem을 반환 받는다.
	struct semaphore_elem *semaphore_elem_b = list_entry(b, struct semaphore_elem, elem);
	
	struct list_elem *waiter_list_a = &(semaphore_elem_a->semaphore.waiters);						// SJ, semaphore_elem 구성 요소인 semaphore(즉 진짜 세마포어)의 waiters를 반환 받는다. waiters에 thread_elem이 이어져있다.
	struct list_elem *waiter_list_b = &(semaphore_elem_b->semaphore.waiters);						// SJ, list 구조체는 포인터가 없다. list_elem에 포인터(*prev, *next)가 있기 때문에 이를 통해 list_begin에서 첫 번째 thread_elem을 찾을 수 있다.
	
	struct thread *thread_a = list_entry(list_begin(waiter_list_a), struct thread, elem);
	struct thread *thread_b = list_entry(list_begin(waiter_list_b), struct thread, elem);
	
	return thread_a->priority > thread_b->priority;
}
   
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

void
cond_wait (struct condition *cond, struct lock *lock) {												// SJ, 
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	// list_push_back (&cond->waiters, &waiter.elem);
	list_insert_ordered(&cond->waiters, &waiter.elem, cmp_sem_priority, 0);							// SJ, 그 semaphore_elem의 쓰레드 우선 순위를 따져서 semaphore_elem을 cond의 waiters에 넣어준다.
	lock_release (lock);
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */
void
sema_down (struct semaphore *sema) {																// SJ, P 연산. 
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	while (sema->value == 0) {																		// SJ, Busy waiting 방식은 아니다. 바로 ready_list로 들어가지 않는다. 그리고, signal로 인해 ready_list로 갔더라도, value가 0일 수도 있기 때문에(중간에 새로운 우선순위가 더 높은, 같은 세마포어를 얻으려는 쓰레드가 생성되어 ready_list 맨 앞으로 갈 수도 있기 때문이다) 또 다시 waiters로 들어가버릴 수도 있다. 따라서 while문을 사용해야 한다.
		list_insert_ordered(&sema->waiters, &thread_current ()->elem, cmp_priority, 0);				// SJ, 공유 메모리로 들어가려는 쓰레드가 CPU에 들어가고 나서 lock이 걸려있다면, waiter로 바로 간다. 
		// list_sort(&sema->waiters, cmp_priority, 0);
		thread_block ();																			// SJ, 그리고 그 쓰레드의 상태를 BLOCK 만들고, CPU가 놀면 안되므로 다음 ready_list의 쓰레드를 CPU로 올린다.
	}
	sema->value--;																					// SJ, 락을 얻고 공유 메모리로 들어간다. value는 공유 변수이다.
	intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (!list_empty (&sema->waiters)) {																// SJ, 리스트가 비어있지 않으면 waiters에 있는 것 1개를 ready_list로 올려야 한다.(signal) 리스트가 비어있으면 waiters에서 올릴 것도 없으니 else문에서 무언가 따로 처리해줄 필요가 없다.
		list_sort(&sema->waiters, cmp_priority, 0);													// SJ, 우선순위가 중간에 바뀔 수 있기 때문에 (테스트 케이스에 존재할 수 있다. 혹은 사용자가 우선순위를 맘대로 바꿀 수도 있으니까) 우선순위를 정렬해줘야 한다.
		thread_unblock (list_entry (list_pop_front (&sema->waiters),								// SJ, 혹은 lock의 세마포어의 대기자, 즉 lock을 쥐기 위해 기다리는 대기자들의 우선순위를 재정렬해준다. https://sunset-asparagus-5c5.notion.site/Step-3-Priority-Donation-16f26df9f0284a39b3a29849e85eae12 sema_up 수정 부분 참고.
					struct thread, elem));
	}
	sema->value++;
	
	test_max_priority();																			// SJ, 공유 데이터로 접근 중인 쓰레드의 임계 영역이 끝나면, ready_list의 우선순위와 따져줘서 CPU 제어권을 바꿔줘야 할 수도 있다.
	intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock) {									
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));

	struct thread* current_thread = thread_current();

	if (lock->holder != NULL) {																				// SJ, 현재 쓰레드가 원하는 락을 쥐고 있는 쓰레드가 있다면, 그 쓰레드에게 기부하고 그 쓰레드의 donations로 자신이 들어가야 한다.
		current_thread->wait_on_lock = lock;																// SJ, 현재 쓰레드가 어떤 락을 기다리고 있는지 기억하게 만든다.
		list_insert_ordered(&lock->holder->donations, &current_thread->donation_elem, cmp_priority, 0);		// SJ, 락을 쥐고 있는 쓰레드의 donation 리스트에 자신을 추가한다.
		donate_priority();																					// SJ, 현재 쓰레드가 원하는 자원을 빨리 얻어내기 위해, 그 자원을 쓰고 있는 쓰레드들 + 그 쓰레드가 원하는 자원을 쓰고 있는 쓰레드들 등 타고타고 들어가서 우선순위를 기부한다.
	}
		
	sema_down (&lock->semaphore);																			// SJ, 현재 쓰레드가 원하는 락을 쥐고 있는 쓰레드가 없다면, 즉 lock->holder == NULL이라면 sema_down을 통해 그 락(공유될 수 있는 메모리 영역)으로 접근하게 된다.
	current_thread->wait_on_lock = NULL;																	// SJ, 현재 쓰레드가 원하는 락을 얻고 공유된 메모리로 들어가게 된다면, 그 쓰레드는 원하는 락(기다리는 락)이 없어지게 된다.
																											// SJ, 위의 sema_donw을 통해 세마포어에 들어가고 난 후이다. 따라서 현재 쓰레드가 원하는 락을 모두 얻었기 떄문에 세마포어에 들어갔다는 것이다.
																											// SJ, 따라서 현재 쓰레드가 기다리는 락은 NULL이 된다.
																											
	lock->holder = current_thread;																			// SJ, 현재 쓰레드가 얻은 락의 주인은 현재 쓰레드가 된다.
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

void remove_with_lock(struct lock *lock) {												
	struct thread* current_thread = thread_current();			// SJ, 현재 락을 쥔 쓰레드가 그 락을 반납한다면(현재 락을 쥔 쓰레드가 CPU에 올라갔을 때 lock_release를 실행하게 된다. 그리고 lock_release를 통해 remove_with_lock이 실행된다. 따라서 현재 CPU의 쓰레드의 donations을 탐색해서 그 락이 반납되므로 해방될 노드들을 찾아 해방(제거)시킨다.)
	struct list_elem* search_thread_elem;
	
	for (search_thread_elem = list_begin(&current_thread->donations); search_thread_elem != list_end(&current_thread->donations); search_thread_elem = list_next(search_thread_elem)) {
		struct thread* search_thread = list_entry(search_thread_elem, struct thread, donation_elem);		// SJ, donation 관련 작업은 donation_elem을 사용해야 한다.
		
		if (search_thread->wait_on_lock == lock) {
			list_remove(&search_thread->donation_elem);		
		}
	}
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) {								
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));

	remove_with_lock(lock);										// SJ, 락을 반납했으니, 해당 락을 반납하는 쓰레드의 donations 리스트에서, 해당 락을 기다리는 쓰레드들을 제거한다.(해방한다.)
																// SJ, 이 쓰레드들은 ready_list에서 계속 락을 못 쥐어서 다시 ready_list로 빠꾸 치고 있다. donations 목록에서 해방되고 락을 쥘 수 있게 되면 자연스럽게 락을 쥘 수 있게 된다. donations 목록에서 제거되지 않더라도 sema_up에서 쥘 수 있게 될 것 같긴 하다. 하지만 우선순위 관리가 되지 않을 것이다. 
																// SJ, refresh_priority에서 이 donation을 사용하기 떄문에 우선순위 새로고침이 원하는 대로 동작하지 않을 것이다.
	refresh_priority();
	
	lock->holder = NULL;
	sema_up (&lock->semaphore);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {									// SJ, 해당 cond에 signal을 날린다. 그러면 그 cond의 waiters의 semaphore_elem의 semaphore의 waiters에서 대기 중인 thread 1개가 꺠어나서 ready_list로 간다.
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters)) {
		list_sort(&cond->waiters, cmp_sem_priority, 0);												// SJ, 대기 중에 cond의 waiters의 semaphore_elem들끼리 우선순위가 변경되었을 가능성이 있다. priority-condvar.c(테스트 케이스)를 보면 우선순위를 갑자기 바꾼다.
		sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore);
	}
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}
