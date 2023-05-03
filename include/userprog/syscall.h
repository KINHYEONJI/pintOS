#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init(void);

// ******************************LINE ADDED****************************** //
// Project 2-2-1: User Programs - System Call - Basics
void check_address(void *addr);
// Project 2-2-2 : User Programs - System Call - File Descriptor
struct lock filesys_lock;
// *************************ADDED LINE ENDS HERE************************* //

#endif /* USERPROG_SYSCALL_H */
