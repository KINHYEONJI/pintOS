#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

static void
process_init (void) {
	struct thread *current = thread_current ();
}

tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy(fn_copy, file_name, PGSIZE);

	char *next_ptr;
	strtok_r(file_name, " ", &next_ptr);

	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

tid_t process_fork(const char *name, struct intr_frame *if_ UNUSED)
{

	struct thread *curr = thread_current();
	memcpy(&curr->parent_if, if_, sizeof(struct intr_frame));

	tid_t pid = thread_create(name, PRI_DEFAULT, __do_fork, curr);

	if (pid == TID_ERROR)
	{
		return TID_ERROR;
	}

	struct thread *child = get_child(pid);

	sema_down(&child->fork_sema);

	if (child->exit_status == -1)
		return TID_ERROR;

	return pid;
}

#ifndef VM

static bool duplicate_pte(uint64_t *pte, void *va, void *aux)
{
	struct thread *current = thread_current();
	struct thread *parent = (struct thread *)aux;
	void *parent_page;
	void *newpage;
	bool writable;
	if (is_kernel_vaddr(va))
	{
		return true;
	}
	parent_page = pml4_get_page(parent->pml4, va);
	if (parent_page == NULL)
	{
		return false;
	}
	newpage = palloc_get_page(PAL_USER | PAL_ZERO);
	if (newpage == NULL)
	{
		return false;
	}

	memcpy(newpage, parent_page, PGSIZE); // parent_page는 가상주소이고, 이것을 newpage에 복사 _ SIZE는 4KB(할당해준 공간도 4KB)
	writable = is_writable(pte);					// pte가 읽고/쓰기가 가능한지 확인

	if (!pml4_set_page(current->pml4, va, newpage, writable))
	{
		return false;
	}
	return true;
}
#endif

static void __do_fork(void *aux)
{
	struct intr_frame if_;
	struct thread *parent = (struct thread *)aux;
	struct thread *current = thread_current();
	struct intr_frame *parent_if;
	bool success = true;
	parent_if = &parent->parent_if;

	memcpy(&if_, parent_if, sizeof(struct intr_frame));
	if_.R.rax = 0;

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate(current);
#ifdef VM
	supplemental_page_table_init(&current->spt);
	if (!supplemental_page_table_copy(&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each(parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	if (parent->fd_idx >= FDCOUNT_LIMIT)
		goto error;

	current->fd_table[0] = parent->fd_table[0]; // stdin
	current->fd_table[1] = parent->fd_table[1]; // stdout

	for (int i = 2; i < FDCOUNT_LIMIT; i++)
	{
		struct file *f = parent->fd_table[i];
		if (f == NULL)
		{
			continue;
		}
		current->fd_table[i] = file_duplicate(f);
	}
	current->fd_idx = parent->fd_idx;
	sema_up(&current->fork_sema);

	if (success)
		do_iret(&if_);

error:
	current->exit_status = TID_ERROR;
	sema_up(&current->fork_sema);
	exit(TID_ERROR);
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();

	/* And then load the binary */
	success = load (file_name, &_if);

	/* If load failed, quit. */
	palloc_free_page(file_name);
	
	if (!success)
		return -1;

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}

/* user stack에 인자 삽입 */
void push_argument_to_user_stack(struct intr_frame *if_, char **argv, int argc)
{
	// USER_STACK 위치를 시작점으로 인자들을 user stack에 삽입
	for (int i = argc - 1; i >= 0; i--)
	{
		int arg_length = strlen(argv[i]) + 1;  // NULL 문자(/0) 포함 시켜서 저장 하기 위해서
		if_->rsp -= arg_length;				   // stack pointer 이동
		memcpy(if_->rsp, argv[i], arg_length); // rsp 주소위치에 인자값 복사 (argv[i]가 가리키는 메모리블록에서 arg_length만큼 읽어서, if_->rsp가 가리키는 메모리 블록으로 복사)
		argv[i] = (char *)if_->rsp;			   // 각 index에 해당하는 인자의 시작 주소를 가리키도록 초기화
	}

	// ALIGNMENT(=8byte) 제한 조건에 맞지 않을 경우, padding을 user stack에 삽입해 정렬 조건을 충족시킴
	if (if_->rsp % ALIGNMENT)
	{
		int padding = if_->rsp % ALIGNMENT;
		if_->rsp -= padding;
		memset(if_->rsp, 0, padding);
	}

	// argv[argc+1]의 위치를 sentinel로 user stack에 삽입
	if_->rsp -= ALIGNMENT;
	memset(if_->rsp, 0, ALIGNMENT);

	// 각 인자 문자열의 시작 주솟값을 user stack에 삽입
	for (int i = argc - 1; i >= 0; i--)
	{
		if_->rsp -= ALIGNMENT;
		memcpy(if_->rsp, &argv[i], ALIGNMENT);
	}

	// 반환할 가짜 주소 0을 user stack에 삽입
	if_->rsp -= ALIGNMENT;
	memset(if_->rsp, 0, ALIGNMENT);

	// rsi를 argv[0]의 주소로 초기화, rdi를 argc로 초기화
	if_->R.rsi = if_->rsp + ALIGNMENT;
	if_->R.rdi = argc;
}

int process_wait(tid_t child_tid UNUSED)
{
	struct thread *child = get_child(child_tid);
	if (child == NULL)
	{
		return -1;
	}
	sema_down(&child->wait_sema);
	int exit_status = child->exit_status;
	list_remove(&child->child_elem);
	sema_up(&child->free_sema);

	return exit_status;
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void)
{
	struct thread *curr = thread_current();

	for (int i = 0; i < FDCOUNT_LIMIT; i++)
	{
		close(i);
	}
	palloc_free_multiple(curr->fd_table, FDT_PAGES);
	file_close(curr->running);

	sema_up(&curr->wait_sema);
	sema_down(&curr->free_sema);

	process_cleanup();
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;

	pml4 = curr->pml4;
	if (pml4 != NULL) {
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
						 uint32_t read_bytes, uint32_t zero_bytes,
						 bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	char *token, *next_ptr;
	char *argv[MAX_ARGC];
	int argc = 0;

	token = strtok_r(file_name, " ", &next_ptr);
	
	while (token)
	{
		argv[argc] = token;
		argc++;
		token = strtok_r(NULL, " ", &next_ptr);
	}

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Open executable file. */
	file = filesys_open (file_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	t->running = file;

	file_deny_write(file);

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
		case PT_NULL:
		case PT_NOTE:
		case PT_PHDR:
		case PT_STACK:
		default:
			/* Ignore this segment. */
			break;
		case PT_DYNAMIC:
		case PT_INTERP:
		case PT_SHLIB:
			goto done;
		case PT_LOAD:
				if (validate_segment (&phdr, file)) {
				bool writable = (phdr.p_flags & PF_W) != 0;
				uint64_t file_page = phdr.p_offset & ~PGMASK;
				uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
				uint64_t page_offset = phdr.p_vaddr & PGMASK;
				uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
					/* Normal segment.
					 * Read initial part from disk and zero the rest. */
					read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
					/* Entirely zero.
					 * Don't read anything from disk. */
					read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
				}
					if (!load_segment (file, file_page, (void *) mem_page,
								  read_bytes, zero_bytes, writable))
					goto done;
			}
			else
				goto done;
			break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */

	push_argument_to_user_stack(if_, argv, argc);

	// void hex_dump (uintptr_t ofs, const void *buf_, size_t size, bool ascii) {
	// hex_dump(if_->rsp, if_->rsp, USER_STACK - if_->rsp, true);
	
	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	// file_close (file);
	return success;
}

// ******************************LINE ADDED****************************** //
// Project 2 : User Programs - System Call - fork()
struct thread *get_child(int pid)
{

	struct thread *curr = thread_current();
	struct list *child_list = &curr->child_list;
	struct list_elem *e;
	if (list_empty(child_list))
		return NULL;
	for (e = list_begin(child_list); e != list_end(child_list); e = list_next(e))
	{
		struct thread *t = list_entry(e, struct thread, child_elem);
		if (t->tid == pid)
		{
			return t; // 자식 스레드 반환
		}
	}
	return NULL;
}
// *************************ADDED LINE ENDS HERE************************* //

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page(void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
											writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

	return success;
}
#endif /* VM */

// struct thread *get_child_with_pid(int pid)
// {
// 	struct thread *cur = thread_current();
// 	struct list *child_list = &cur->child_list;

// #ifdef DEBUG_WAIT
// 	printf("\nparent children # : %d\n", list_size(child_list));
// #endif

// 	for (struct list_elem *e = list_begin(child_list); e != list_end(child_list); e = list_next(e))
// 	{
// 		struct thread *t = list_entry(e, struct thread, child_elem);
// 		if (t->tid == pid)
// 			return t;
// 	}
// 	return NULL;
// }