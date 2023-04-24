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
//실행할 준비가 되었지만 실제로 실행되지 않은 프로세스, 즉 THREAD_READY 상태의 프로세스 목록입니다.
static struct list ready_list;

/* Thread_Blocked 상태의 스레드를 관리하기 위한 리스트 자료 구조 추가 */
static struct list sleep_list; // Sleep queue 자료구조를 추가해야 한다. 이미 있는 ready_list와 같은 구조로 만들면 된다.

/* sleep_list 에서 대기중인 스레드들의 wakeup_tick 값 중 최소값을 저장 하기위한 변수 추가 */
static int64_t next_tick_to_awake;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
/* 초기 스레드, init.c:main()을 실행하는 스레드. */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
// allocate_tid()에서 사용하는 잠금입니다.
static struct lock tid_lock;

/* Thread destruction requests */
// Thread 파괴 요청
static struct list destruction_req;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* sleep_list에서 대기중인 스레드들의 wakeup_tick값 중 최소값을 저장 */
static long long next_tick_to_awake; // sleep_list에서 대기중인 스레드들의 wakeup_tick 값 중 최소값을 저장하는 전역 변수 next_tick_to_wake 추가한다.
// next_tict_to_awake는 sleep_list에 있는 스레드들 중에서 가장 빨리 일어나야하는 스레드의 wakeup_tick을 나타낸다.
// 그러니 tick이 증가하면서 tick >= next_tic_to_awake가 되면, 해당 wakeup_tick을 가진 친구를 깨운다.

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
 /* 실행 중인 스레드를 반환합니다. CPU의 스택 포인터 `rsp'를 읽은 다음, 이를 페이지의 시작 부분으로 내림합니다.  구조체 스레드'는 항상 페이지의 시작 부분에 있고 스택 포인터는 중간 어딘가에 있기 때문에, 이것은 큐런트 스레드를 찾습니다. */
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
/* 현재 실행 중인 코드를 스레드로 변환하여 스레딩 시스템을 초기화합니다.  일반적으로는 이 방법이 작동하지 않으며, 이 경우에만 가능한 이유는 loader.S가 스택의 하단을 페이지 경계에 배치하도록 주의를 기울였기 때문입니다. 또한 실행 대기열과 티드 잠금을 초기화합니다. 이 함수를 호출한 후 thread_create()로 스레드를 생성하기 전에 반드시 페이지 할당자를 초기화해야 합니다. 이 함수가 완료될 때까지 thread_current()를 호출하는 것은 안전하지 않습니다. */
void thread_init (void) {
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
	/* Sleep queue 자료구조 초기화 코드 추가 */
	/* sleep_list 를 초기화 */
	list_init (&sleep_list); // list_init 함수로 sleep_init을 초기화
	next_tick_to_awake = INT64_MAX; // next_tick_to_awake는 비교하면서 최솟값을 찾아가야하므로 초기화할때는 정수 최댓값을 넣어준다.

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start (void) {
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
tid_t thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO); // 페이지 할당
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority); // thread 구조체 초기화
	tid = t->tid = allocate_tid (); // tid 할당

	/* Call the kernel_thread if it scheduled.
	 * Note 4rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function; // 스레드ㅏ
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock (t); // 

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF); // intr_off 아무도 인터럽트 못하게 막는다.
	thread_current ()->status = THREAD_BLOCKED; // 상태를 블럭 상태로 만들겠다.
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable (); // 인터럽트 디스에이블 인터럽트 아무도 못오게 하고 나를 어디에? 레디리스트에 집어넣어. 언제가 .. 값을 쓸대가 중요해.. 리스트에 넣고 있으니 막아줘.. 
	ASSERT (t->status == THREAD_BLOCKED);
	list_push_back (&ready_list, &t->elem); // 값을 넣고 있으니까 인터럽트를 막아줘야 함. 우리는 쓰레드 상태 레디임..
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *thread_current (void) {
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
tid_t thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit (void) {
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

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield (void) {
	struct thread *curr = thread_current (); // 현재 실행 되고 있는 thread를 반환
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable (); // 인터럽트를 비활성하고 이전 인터럽트의 상태를 반환
	if (curr != idle_thread)
		list_push_back (&ready_list, &curr->elem); // 주어진 entry를 list의 마지막에 삽입
	do_schedule (THREAD_READY); // schedule() : 컨텍스트 스위치 작업을 수행
	intr_set_level (old_level); // 인자로 전달된 인터럽트 상태로 인터럽트를 설정 하고 이전 인터럽트 상태를 반환
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority (int new_priority) {
	thread_current ()->priority = new_priority;
}

/* Returns the current thread's priority. */
int thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu (void) {
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
	/* 유휴 스레드는 처음에 thread_start()에 의해 준비 목록에 놓입니다.  이 스레드는 처음에 한 번 스케줄링되며, 이 시점에서 idle_thread를 초기화하고, 전달된 세마포어를 "업"하여 thread_start()가 계속될 수 있도록 한 다음 즉시 블록합니다.  그 이후에는 유휴 스레드가 준비 목록에 나타나지 않습니다.  준비 목록이 비어 있을 때 특수한 경우로 next_thread_to_run()에 의해 반환됩니다.*/
static void idle (void *idle_started_ UNUSED) {
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
static void kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void do_iret (struct intr_frame *tf) {
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
thread_launch (struct thread *th) {
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
	thread_current ()->status = status;
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
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock); // 락 걸고
	tid = next_tid++; // 플러스 시키고
	lock_release (&tid_lock); // 락 풀어주고

	return tid;
}

// 실행 중인 스레드를 sleep으로 만든다.
// 기본적인 뼈대는 thread_yield()를 기본으로 만든다.
// ticks는 sleep_list에 넣는 thread가 깨어날 tick을 말한다.
// (ticks = sleep했을 당시의 tick + 사용자가 지정한 tick(몇 tick 뒤에 깨어날지)로 구성된다)
void thread_sleep(int64_t ticks)
{
	// Thread를 blocked 상태로 만들고 sleep queue에 삽입하여 대기
	// devices/timer.c : timer_sleep() 함수에 의해 호출

	/* 현재 스레드가 idle 스레드가 아닐 경우 thread의 상태를 BLOCKED로 바꾸고 깨어나야할 ticks을저장,
	슬립 큐에 삽입하고, awake함수가 실행되어야 할 tick값을 update*/
	/* 현재 스레드를 슬립 큐에 삽입한 후에 스케줄한다. */
	/* 해당 과정중에는 인터럽트를 받아들이지 않는다. */
	struct thread *curr = thread_current(); // 현재 실행 되고 있는 thread를 반환
	enum intr_level old_level;

	ASSERT(!intr_context());

	old_level = intr_disable(); // 인터럽트를 비활성하고 이전 인터럽트의 상태를 반환
	if (curr != idle_thread)
		list_push_back(&sleep_list, &curr->elem); // 주어진 entry를 list의 마지막에 삽입
	do_schedule(THREAD_READY);									// schedule() : 컨텍스트 스위치 작업을 수행
	intr_set_level(old_level);									// 인자로 전달된 인터럽트 상태로 인터럽트를 설정 하고 이전 인터럽트 상태를 반환
}

// sleep_list에 있던 스레드를 깨운다.
// sleep_list를 순회하면서, 스레드의 wakeup_ticks가 ticks보다 작다면 깨울 것이다.
void thread_awake(int64_t ticks)
{
	// wakeup_tick값이 ticks보다 작거나 같은 스레드를 깨움
	// 현재 대기중인 스레드들의 wakeup_tick변수 중 가장 작은 값을 next_tick_to_awake 전역 변수에 저장
	/* sleep list의 모든 entry 를 순회하며 다음과 같은 작업을 수행한다.
	현재 tick이 깨워야 할 tick 보다 크거나 같다면 슬립큐에서 제거하고 unblock 한다.
	작다면 update_next_tick_to_awake() 를 호출한다.
	*/
if (wakeup_tick < ticks) {
	next_tick_to_awake = min(wakeup_tick)
	while sleep_list {
		if curr->tick >= next_tick_to_awake {
			remove(sleep_list[next_tick_to_awake])
		} else {
			update_next_tick_to_awake();
		}
	}
}
}

// sleep_list에서 가장 작은 wakeup_ticks를 갱신한다.
void update_next_tick_to_awake(int64_t ticks)
{
	// next_tick_to_awake 변수를 업데이트
	/* next_tick_to_awake가 깨워야 할 스레드중 가장 작은 tick을 갖도록 업데이트 한다*/
	next_tick_to_awake = ticks;
}

// next_tick_to_awake를 반환하는 함수이다.
int64_t get_next_tick_to_awake(void)
{
	/* next_tick_to_awake을 반환한다. */
	return next_tick_to_awake;
}