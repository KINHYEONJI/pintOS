#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H
#define MAX_ARGC 64
#define ALIGNMENT 8

#include "threads/thread.h"

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);
void push_argument_to_user_stack(struct intr_frame *if_, char **argv, int argc);

// ******************************LINE ADDED****************************** //
// Project 2-2-1 : User Programs - System Call - Basics
struct thread *get_child(int pid);
// *************************ADDED LINE ENDS HERE************************* //

#endif /* userprog/process.h */
