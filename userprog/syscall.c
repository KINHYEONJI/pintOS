#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

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
void syscall_handler(struct intr_frame *f)
{
	// TODO: Your implementation goes here.
	char *fn_copy;

	/*
	 x86-64 규약은 함수가 리턴하는 값을 rax 레지스터에 배치하는 것
	 값을 반환하는 시스템 콜은 intr_frame 구조체의 rax 멤버 수정으로 가능
	 */
	switch (f->R.rax)
	{ // rax is the system call number
	case SYS_HALT:
		halt(); // pintos를 종료시키는 시스템 콜
		break;
	case SYS_EXIT:
		exit(f->R.rdi); // 현재 프로세스를 종료시키는 시스템 콜
		break;
	case SYS_FORK:
		f->R.rax = fork(f->R.rdi, f);
		break;
	case SYS_EXEC:
		if (exec(f->R.rdi) == -1)
		{
			exit(-1);
		}
		break;
	case SYS_WAIT:
		f->R.rax = process_wait(f->R.rdi); // <-- process_wait 함수 호출
		break;
	case SYS_CREATE:
		f->R.rax = create(f->R.rdi, f->R.rsi);
		break;
	case SYS_REMOVE:
		f->R.rax = remove(f->R.rdi);
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
	case SYS_WRITE:
		f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
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

/* 주소 값이 유저영역에서 사용하는 주소 값인지 확인 하는 함수
Pintos에서는 시스템 콜이 접근할 수 있는 주소를 0x8048000~0xc0000000으로 제한함
유저영역을 벗어난 영역일 경우 프로세스 종료(exit(-1)) */
void check_address(void *addr)
{
	/* 포인터가 가리키는 주소가 유저영역의 주소인지 확인*/
	/* 잘못된 접근일 경우 프로세스 종료*/
	struct thread *cur_t = thread_current();
	if (addr == NULL || !(is_user_vaddr(addr)) || pml4_get_page(cur_t->pml4, addr) == NULL)
	{
		exit(-1);
	}
}

/* 유저 스택에 있는 인자들을 커널에 저장하는 함수
스택 포인터(esp)에 count(인자의개수) 만큼의 데이터를 arg에 저장*/
void get_argument(void *esp, int *arg, int count)
{
	/* 유저스택에 저장된 인자값들을 커널로 저장*/
	/* 인자가 저장된 위치가 유저영역인지 확인*/
}

// PintOS를 종료하는 함수
void halt(void)
{
	/* shutdown_power_off()를 사용하여 pintos 종료*/
	power_off(); // init.c의 power_off 활용
}

// 현재 프로세스를 종료시키는 System Casll
void exit(int status)
{
	/* 실행중인 스레드 구조체를 가져옴*/
	struct thread *t = thread_current();
	t->status = status;
	/* 프로세스 종료 메시지 출력,
	출력 양식: “프로세스이름: exit(종료상태)” */
	print("%s: exit(%d)", t, status);
	/* 스레드 종료*/
	thread_exit();
}

// 현재의 프로세스가 cmd_line에서 이름이 주어지는 실행가능한 프로세스로 변경..
int exec (const char *file) {
	char *f = file;
	char *fn_copy = pml4_get_page();
	if (fn_copy == NULL) {
		return -1;
	}
	strcpy(fn_copy, file, strlen(file)+1);
	if (process_exec(fn_copy) == -1) {
		return -1;
	}
	process_exit();
	// 파일의 주소값 체크하기
	// 파일의 사이즈
	// 복사본 초기화
	// 복사본 정상인지 체크
	// 복사본에 file 복사
	// 복사본 실행했는데 정상적이지 않으면 -1 반환
}

// 파일을 생성하는 콜
bool create(const char *file, unsigned initial_size)
{
	// 포인터가 가리키는 주소가 유저영역의 주소인지 확인
	// 파일 이름 & 크기에 해당하는 파일 생성
}

// 파일 삭제 시스템콜
bool remove(const char *file)
{
	// 포인터가 가리키는 주소가 유저영역의 주소인지 확인
	// 파일 이름에 해당하는 파일을 제거
}

// 자식 프로세스를 복제하고 실행시키는 시스템 콜
tid_t fork (const char *thread_name){
//현재 프로세스의 복제본인 새로운 프로세스를, thread_name으로 갖도록 생성해주는 함수.
//%RBX, %RSP, %RBP, %R12~%R15 등등의 calle-saved register는 clone해줘야 한다.
//자식 프로세스의 pid를 반환해야한다.
//자식프로세스의 return 값은 0이어야 한다.
//자식은 파일 디스크립터와 virtual memory space를 포함한 중복된 자원을 갖도록 해야한다.
//부모 프로세스의 fork는, 자식 프로세스가 성공적으로 clone 되었는지 확인하기 전까지는 절때 return 해선 안된다.
//만약 자식 프로세스가 자원을 복사하는데 실패한면, 부모 프로세스의 fork() 호출에서 TID_ERROR를 return해야 한다.
//제공된 뼈대는, pml4_for_each()를 활용해서 user memory space 전체와 그에 대응하는 pagetable 구조체를 복사한다. 다만 pml4_for_each에 주어지는 함수에 대한 비어있는 부분들은 추가적으로 구현해주어야 한다.
	struct thread *curr = thread_current();
	return process_fork(thread_name, &curr->parent_if);
}