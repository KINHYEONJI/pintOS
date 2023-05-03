#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
// ******************************LINE ADDED****************************** //
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "threads/palloc.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
void check_address(void *addr);

// Project 2-2-1: User Programs - System Call - Basics
void halt(void);
void exit(int status);
bool create(const char *file, unsigned initial_size);
bool remove(const char *file);
tid_t fork(const char *thread_name, struct intr_frame *f);
int exec(char *file_name);
int wait(tid_t pid);
int open(const char *file);
int filesize(int fd);
int read(int fd, void *buffer, unsigned size);
int write(int fd, void *buffer, unsigned size);
void seek(int fd, unsigned position);
unsigned tell(int fd);
void close(int fd);

// Project 2-2-2 : User Programs - System Call - File Descriptor
static struct file *find_file_by_fd(int fd);
int add_file_to_fdt(struct file *file);
void remove_file_from_fdt(int fd);

const int STDIN = 1;
const int STDOUT = 2;
// *************************ADDED LINE ENDS HERE************************* //

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

	/* System call 추가 */
	lock_init(&filesys_lock);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED)
{
	// TODO: Your implementation goes here.
	// ******************************LINE ADDED****************************** //
	// Project 2-2-1: User Programs - System Call - Basics
	// Defined as enum @ include/lib/syscall-nr.h
	int sys_number = f->R.rax; // 유저 스택에 저장되어 있는 시스템 콜 번호를 sys_number 변수에 저장
	// switch 문 통해 해당되는 시스템 콜 호출
	// REFERENCE in include/lib/syscall-nr.h
	switch (sys_number)
	{
	case SYS_HALT: // Halt the operating system.
		halt();
		break;
	case SYS_EXIT: // Terminate this process.
		exit(f->R.rdi);
		break;
	case SYS_FORK: // Clone current process. (자식 프로세스 생성)
		f->R.rax = fork(f->R.rdi, f);
		break;
	case SYS_EXEC: // Switch current process.
		if (exec(f->R.rdi) == -1)
		{
			exit(-1);
		}
		break;
	case SYS_WAIT: // Wait for a child process to die. (자식 프로세스 종료 대기)
		f->R.rax = wait(f->R.rdi);
		break;
	case SYS_CREATE: // Create a file.
		f->R.rax = create(f->R.rdi, f->R.rsi);
		break;
	case SYS_REMOVE: // Delete a file.
		f->R.rax = remove(f->R.rdi);
		break;
	case SYS_OPEN: // Open a file.
		f->R.rax = open(f->R.rdi);
		break;
	case SYS_FILESIZE: // Obtain a file's size.
		f->R.rax = filesize(f->R.rdi);
		break;
	case SYS_READ: // Read from a file.
		f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_WRITE: // Write to a file.
		f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_SEEK: // Change position in a file.
		/*seek(f -> R.rdi, f -> R.rdx); <<<<<<<<<<< 이새끼가 범인이에요*/
		seek(f->R.rdi, f->R.rsi);
		break;
	case SYS_TELL: // Report current position in a file.
		f->R.rax = tell(f->R.rdi);
		break;
	case SYS_CLOSE: // Close a file.
		close(f->R.rdi);
		break;
	default:
		/*printf ("system call!\n");
		thread_exit ();*/
		exit(-1);
		break;
	}
	// *************************ADDED LINE ENDS HERE************************* //

	// default 분기에서 처리해줌
	/*printf ("system call!\n");
	thread_exit ();*/
}

/* Project 2_2 User memory access 추가 ---------------------------------------------- */
/* 	1. 사용자가 잘못된 포인터 → Null Pointer이거나
	2. 커널 메모리에 대한 포인터 → Kernel VM을 가리키거나 ( = KERN_BASE보다 큰 값일 때)
	3. 그 영역들 중 하나에 부분적으로 블록을 제공한다면
	(a block partially in one of those regions) -> 가상메모리주소 블럭을 페이지테이블에 준다?
		→ mapping 되지 않은 VM을 가리킨다면(할당되지 않은 vm주소에 접근하면)
	⇒ 프로세스를 종료해야 한다. `(exit(-1)`  */
void check_address(void *addr)
{
	struct thread *curr = thread_current();
	// 체크 조건
	// user virtual address 인지 (is_user_vaddr) = 커널 VM이 아닌지
	// 주소가 NULL 은 아닌지
	// 유저 주소 영역내를 가르키지만 아직 할당되지 않았는지 (pml4_get_page)
	if (!is_user_vaddr(addr) || addr == NULL || pml4_get_page(curr->pml4, addr) == NULL)
	{
		exit(-1);
	}
}

// Project 2-2-1: User Programs - System Call - Basics
// SYS_HALT : Halt the operating system 함수 구현
void halt(void)
{
	power_off();
}

// SYS_EXIT : Terminate this process 함수 구현
void exit(int status)
{
	struct thread *curr = thread_current();
	curr->exit_status = status;
	// TODO : 스레드 구조체 exit_status 플래그 변수 만들기 (종료 상태 0) -> DONE
	printf("%s: exit(%d)\n", thread_name(), status);
	thread_exit(); //@thread.c
}

// SYS_CREATE : Create a file 함수 구현. 성공시 TRUE 반환, 실패시 FALSE 반환
bool create(const char *file, unsigned initial_size)
{
	check_address(file);
	if (filesys_create(file, initial_size))
	{
		return true;
	}
	else
	{
		return false;
	}
	/*return filesys_create(file, initial_size);*/
}

// SYS_REMOVE : Delete a file 함수 구현. 성공시 TRUE 반환, 실패시 FALSE 반환
bool remove(const char *file)
{
	check_address(file);
	return filesys_remove(file);
	/*if (filesys_remove(file)) {
			return true;
	} else {
			return false;
	}*/
}

// ******************************LINE ADDED****************************** //
// System Call : fork(), exec(), wait()

// SYS_FORK : Clone current process. (자식 프로세스 생성)
tid_t fork(const char *thread_name, struct intr_frame *f)
{
	// check_address(thread_name);
	return process_fork(thread_name, f);
}

// SYS_WAIT : Wait for a child process to die. (자식 프로세스 종료 대기)
int wait(tid_t pid)
{
	return process_wait(pid);
}

// SYS_EXEC : Switch current process.
tid_t exec(char *file_name)
{ // 현재 프로세스를 command line에서 지정된 인수를 전달하여 이름이 지정된 실행 파일로 변경
	check_address(file_name);
	int size = strlen(file_name) + 1;
	char *fn_copy = palloc_get_page(PAL_ZERO);
	if (fn_copy == NULL)
	{
		exit(-1);
	}
	strlcpy(fn_copy, file_name, size);

	if (process_exec(fn_copy) == -1)
	{
		return -1;
	}
	NOT_REACHED();
	return 0;
}
// *************************ADDED LINE ENDS HERE************************* //

// ******************************LINE ADDED****************************** //
// Project 2-2-2 : User Programs - System Call - File Descriptor
static struct file *find_file_by_fd(int fd)
{
	if (fd < 0 || fd >= FDCOUNT_LIMIT)
	{
		return NULL;
	}

	struct thread *curr = thread_current();

	return curr->fd_table[fd];
}

// 파일 구조체 File Descriptor 테이블에 추가
// 현재 스레드의 fd_idx(File Descriptor table index 끝값)
int add_file_to_fdt(struct file *file)
{
	struct thread *curr = thread_current();
	struct file **fdt = curr->fd_table;

	// Find open spot from the front
	while (curr->fd_idx < FDCOUNT_LIMIT && fdt[curr->fd_idx])
	{
		curr->fd_idx++;
	}

	// error - fd table full
	if (curr->fd_idx >= FDCOUNT_LIMIT)
		return -1;

	fdt[curr->fd_idx] = file;
	return curr->fd_idx;
}

void remove_file_from_fdt(int fd)
{
	struct thread *cur = thread_current();
	if (fd < 0 || fd >= FDCOUNT_LIMIT)
	{
		return;
	}
	cur->fd_table[fd] = NULL;
}

/* open(file) -> filesys_open(file) -> file_open(inode) -> file open 함수 실행 -> filesys_open
 * 을 통해서 file open 함수에 inode를 넣고 실행하여 file을 반환받음
 * inode는 우리가 입력한 파일 이름을 컴퓨터가 알고 있는 파일이름으로 바꾸는 과정
 * file_obj = file이 되고, 이를 현재 스레드 파일 디스크립터 테이블에 추가하여 관리할 수 있게함*/
int open(const char *file)
{
	check_address(file);
	lock_acquire(&filesys_lock);

	struct file *file_obj = filesys_open(file);

	if (file_obj == NULL)
	{
		return -1;
	}

	int fd = add_file_to_fdt(file_obj);

	/* if fd full?*/
	if (fd == -1)
	{
		file_close(file_obj);
	}

	lock_release(&filesys_lock);
	return fd;
}

int filesize(int fd)
{
	struct file *file_obj = find_file_by_fd(fd);

	// 파일이 열려있지 않을 경우 예외 처리
	if (file_obj == NULL)
	{
		return -1;
	}
	return file_length(file_obj);
}

/*열린 파일의 데이터를 읽는 시스템 콜
- 파일에 동시 접근이 일어날 수 있으므로 Lock 사용
- 파일 디스크립터를 이용하여 파일 객체 검색
- 파일 디스크립터가 0일 경우 키보드에 입력을 버퍼에 저장 후, 버퍼의 저장한 크기를 리턴 (input_getc() 이용)
- 파일 디스크립터가 0이 아닐 경우 파일의 데이터를 크기만큼 저장 후 읽은 바이트 수를 리턴*/
int read(int fd, void *buffer, unsigned size)
{

	check_address(buffer);
	check_address(buffer + size - 1);
	int read_count; // 글자수 카운트 용(for문 사용하기 위해)

	struct file *file_obj = find_file_by_fd(fd);
	unsigned char *buf = buffer;

	if (file_obj == NULL)
	{
		return -1;
	}

	if (file_obj == STDIN)
	{ // STDIN
		char key;
		for (int read_count = 0; read_count < size; read_count++)
		{
			key = input_getc();
			*buf++ = key;
			if (key == '\0')
			{
				break;
			}
		}
	}
	else if (file_obj == STDOUT)
	{ // STDOUT
		return -1;
	}
	else
	{
		lock_acquire(&filesys_lock);
		read_count = file_read(file_obj, buffer, size);
		lock_release(&filesys_lock);
	}

	return read_count;
}

int write(int fd, void *buffer, unsigned size)
{
	check_address(buffer);
	int read_count;
	struct file *file_obj = find_file_by_fd(fd);

	if (file_obj == NULL)
	{
		return -1;
	}

	if (file_obj == STDOUT)
	{												// STDOUT
		putbuf(buffer, size); // fd값이 1일 때, 버퍼에 저장된 데이터를 화면에 출력(putbuf()이용)
		read_count = size;
	}
	else if (file_obj == STDIN)
	{ // STDIN
		return -1;
	}
	else
	{
		lock_acquire(&filesys_lock);
		read_count = file_write(file_obj, buffer, size);
		lock_release(&filesys_lock);
	}
	return read_count;
}

void seek(int fd, unsigned position)
{
	struct file *file_obj = find_file_by_fd(fd);
	if (fd < 2)
	{
		return;
	}
	file_seek(file_obj, position);
}

unsigned tell(int fd)
{
	struct file *file_obj = find_file_by_fd(fd);
	if (fd < 2)
	{
		return;
	}
	return file_tell(file_obj);
}

void close(int fd)
{
	if (fd <= 1)
		return;
	struct file *file_obj = find_file_by_fd(fd);

	if (file_obj == NULL)
	{
		return;
	}
	remove_file_from_fdt(fd);
}
// *************************ADDED LINE ENDS HERE************************* //
