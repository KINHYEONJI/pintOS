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

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
	 nonnegative integer along with two atomic operators for
	 manipulating it:

	 - down or "P": wait for the value to become positive, then
	 decrement it.

	 - up or "V": increment the value (and wake up one waiting
	 thread, if any). */
void sema_init(struct semaphore *sema, unsigned value)
{
	ASSERT(sema != NULL);

	sema->value = value;
	list_init(&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
	 to become positive and then atomically decrements it.

	 This function may sleep, so it must not be called within an
	 interrupt handler.  This function may be called with
	 interrupts disabled, but if it sleeps then the next scheduled
	 thread will probably turn interrupts back on. This is
	 sema_down function. */
void sema_down(struct semaphore *sema)
{
	enum intr_level old_level;

	ASSERT(sema != NULL);
	ASSERT(!intr_context());

	old_level = intr_disable();
	while (sema->value == 0)
	{
		list_insert_ordered(&sema->waiters, &thread_current()->elem, &cmp_priority, NULL);
		thread_block();
	}
	sema->value--;
	intr_set_level(old_level);
}

/* Down or "P" operation on a semaphore, but only if the
	 semaphore is not already 0.  Returns true if the semaphore is
	 decremented, false otherwise.

	 This function may be called from an interrupt handler. */
bool sema_try_down(struct semaphore *sema)
{
	enum intr_level old_level;
	bool success;

	ASSERT(sema != NULL);

	old_level = intr_disable();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level(old_level);

	return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
	 and wakes up one thread of those waiting for SEMA, if any.

	 This function may be called from an interrupt handler. */
void sema_up(struct semaphore *sema)
{
	enum intr_level old_level;

	ASSERT(sema != NULL);

	old_level = intr_disable();
	if (!list_empty(&sema->waiters))
	{
		list_sort(&sema->waiters, &cmp_priority, NULL);
		thread_unblock(list_entry(list_pop_front(&sema->waiters), struct thread, elem));
	}
	sema->value++;

	test_max_priority();

	intr_set_level(old_level);
}

static void sema_test_helper(void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
	 between a pair of threads.  Insert calls to printf() to see
	 what's going on. */
void sema_self_test(void)
{
	struct semaphore sema[2];
	int i;

	printf("Testing semaphores...");
	sema_init(&sema[0], 0);
	sema_init(&sema[1], 0);
	thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up(&sema[0]);
		sema_down(&sema[1]);
	}
	printf("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper(void *sema_)
{
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down(&sema[0]);
		sema_up(&sema[1]);
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
void lock_init(struct lock *lock)
{
	ASSERT(lock != NULL);

	lock->holder = NULL;
	sema_init(&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
	 necessary.  The lock must not already be held by the current
	 thread.

	 This function may sleep, so it must not be called within an
	 interrupt handler.  This function may be called with
	 interrupts disabled, but interrupts will be turned back on if
	 we need to sleep. */
void lock_acquire(struct lock *lock)
{
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(!lock_held_by_current_thread(lock)); // 현재 스레드가 잠금을 이미 보유하고 있지 않은지 확인

	/* 해당lock 의 holder가 존재한다면 아래작업을 수행한다. */
	/* 현재스레드의 wait_on_lock변수에 획득하기를 기다리는 lock의 주소를 저장*/
	/* multiple donation 을 고려하기 위해 이전상태의 우선순위를 기억,
	donation 을 받은 스레드의 thread 구조체를 list로 관리한다. */
	/* priority donation 수행하기 위해 donate_priority() 함수 호출*/
	sema_down(&lock->semaphore);		 // 잠금을 사용할 수 없는 경우 잠금과 연결된 세마포어에서 sema_down() 함수를 호출하여 현재 스레드를 차단한다.
	lock->holder = thread_current(); // 세마포어가 신호를 받고 잠금이 획득되면 함수는 현재 스레드를 가리키도록 잠금의 'holder' 필드를 업데이트
	thread_current()->wait_on_lock = NULL;

	/* lock을 획득 한 후 lock holder 를 갱신한다.*/
	ASSERT(!lock_held_by_current_thread(lock));

	sema_down(&lock->semaphore);
	lock->holder = thread_current();
}

/* Tries to acquires LOCK and returns true if successful or false
	 on failure.  The lock must not already be held by the current
	 thread.

	 This function will not sleep, so it may be called within an
	 interrupt handler. */
bool lock_try_acquire(struct lock *lock)
{
	bool success;

	ASSERT(lock != NULL);
	ASSERT(!lock_held_by_current_thread(lock));

	success = sema_try_down(&lock->semaphore);
	if (success)
		lock->holder = thread_current();
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
	 This is lock_release function.

	 An interrupt handler cannot acquire a lock, so it does not
	 make sense to try to release a lock within an interrupt
	 handler. */
void lock_release(struct lock *lock)
{
	ASSERT(lock != NULL);
	ASSERT(lock_held_by_current_thread(lock));

	lock->holder = NULL;

	sema_up(&lock->semaphore); // 잠금의 'semaphore' 멤버에서 'sema_up()'을 호출하여 잠금이 사용 가능하고 이 잠금을 기다리고 있는 모든 스레드가 잠금을 획득하려고 시도할 수 있음을 알립니다.

	sema_up(&lock->semaphore);
}

/* Returns true if the current thread holds LOCK, false
	 otherwise.  (Note that testing whether some other thread holds
	 a lock would be racy.) */
bool lock_held_by_current_thread(const struct lock *lock)
{
	ASSERT(lock != NULL);

	return lock->holder == thread_current();
}

/* One semaphore in a list. */
struct semaphore_elem
{
	struct list_elem elem;			/* List element. */
	struct semaphore semaphore; /* This semaphore. */
};

/* Initializes condition variable COND.  A condition variable
	 allows one piece of code to signal a condition and cooperating
	 code to receive the signal and act upon it. */
void cond_init(struct condition *cond)
{
	ASSERT(cond != NULL);

	list_init(&cond->waiters);
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
void cond_wait(struct condition *cond, struct lock *lock)
{
	struct semaphore_elem waiter;

	ASSERT(cond != NULL);
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(lock_held_by_current_thread(lock));

	sema_init(&waiter.semaphore, 0);																						// waiter라는 새로운 semaphore_elem 구조체와 연결된 세마포어를 초기화합니다. 이 세마포어는 조건 변수가 신호를 받을 때까지 스레드를 차단하는 데 사용됩니다.
	list_insert_ordered(&cond->waiters, &waiter.elem, &cmp_sem_priority, NULL); // 'cmp_sem_priority' 비교기 함수를 사용하여 우선순위 방식으로 'cond->waiters' 목록에 삽입합니다. 'waiter' 세마포어는 값 0으로 초기화됩니다. 즉, 이 세마포어에서 'sema_down'을 호출하는 모든 스레드는 신호를 받을 때까지 차단됩니다.
	lock_release(lock);																													// 조건 변수와 관련된 잠금을 해제하여 다른 스레드가 잠금을 획득하고 공유 데이터를 수정할 수 있도록 합니다.
	sema_down(&waiter.semaphore);																								// waiter와 관련된 세마포어를 대기하여 현재 스레드를 차단합니다. 이 호출은 스레드가 cond_signal 또는 cond_broadcast로 신호를 받을 때만 반환됩니다.
	lock_acquire(lock);																													// 이 함수 시작 시 이전에 해제된 잠금을 다시 획득하여 스레드가 잠금을 유지하는 동안 실행을 계속할 수 있도록 합니다. 이렇게 하면 스레드가 잠금을 보유하지 않는 동안 공유 데이터가 수정되지 않습니다.
	sema_init(&waiter.semaphore, 0);
	list_insert_ordered(&cond->waiters, &waiter.elem, &cmp_sem_priority, NULL);
	lock_release(lock);
	sema_down(&waiter.semaphore);
	lock_acquire(lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
	 this function signals one of them to wake up from its wait.
	 LOCK must be held before calling this function.

	 An interrupt handler cannot acquire a lock, so it does not
	 make sense to try to signal a condition variable within an
	 interrupt handler. */
void cond_signal(struct condition *cond, struct lock *lock UNUSED)
{
	ASSERT(cond != NULL);
	ASSERT(lock != NULL);
	ASSERT(!intr_context());
	ASSERT(lock_held_by_current_thread(lock));

	if (!list_empty(&cond->waiters))
	{
		list_sort(&cond->waiters, &cmp_sem_priority, NULL);
		sema_up(&list_entry(list_pop_front(&cond->waiters), struct semaphore_elem, elem)->semaphore);
	}
}

/* Wakes up all threads, if any, waiting on COND (protected by
	 LOCK).  LOCK must be held before calling this function.

	 An interrupt handler cannot acquire a lock, so it does not
	 make sense to try to signal a condition variable within an
	 interrupt handler. */
void cond_broadcast(struct condition *cond, struct lock *lock)
{
	ASSERT(cond != NULL);
	ASSERT(lock != NULL);

	while (!list_empty(&cond->waiters))
		cond_signal(cond, lock);
}

bool cmp_sem_priority(const struct list_elem *a, const struct list_elem *b, void *aux)
{
	struct semaphore_elem *sema_a = list_entry(a, struct semaphore_elem, elem);
	struct semaphore_elem *sema_b = list_entry(b, struct semaphore_elem, elem);
	struct thread *thread_a = list_entry(list_begin(&sema_a->semaphore.waiters), struct thread, elem);
	struct thread *thread_b = list_entry(list_begin(&sema_b->semaphore.waiters), struct thread, elem);
	return thread_a->priority > thread_b->priority;
}