#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include <list.h>
#include <stdint.h>
#include "threads/synch.h"

// At most 8MB can be allocated to the stack
// These defines will be used in Project 2: Multithreading
#define MAX_STACK_PAGES (1 << 11)
#define MAX_ARGS 1024
#define MAX_THREADS 127

/* PIDs and TIDs are the same type. PID should be
   the TID of the main thread of the process */
typedef tid_t pid_t;

/* Thread functions (Project 2: Multithreading) */
typedef void (*pthread_fun)(void*);
typedef void (*stub_fun)(pthread_fun, void*);

/* User thread related declarations */
typedef struct user_thread_entry {
  struct thread* thread; /* Pointer to related thread */
  // status or other meta-data
  tid_t tid;
  bool waited_on;
  bool completed;
  bool initialized;
  uint8_t* kpage;
  uint8_t* upage;
  //struct join_status* join_status;
  struct list_elem elem;
} user_thread_entry_t;

typedef struct user_thread_list {
  struct list lst;
  struct lock lock;
} user_thread_list_t;

/* Used to hold locks and their owner thread's tid */
typedef struct thread_lock {
  struct lock lock;
  tid_t tid;
  bool initialized;
} thread_lock_t;

/* Used to hold semaphores and their metadata */
typedef struct thread_sema {
  struct semaphore sema;
  bool initialized;
} thread_sema_t;

/* Struct for passing args in thread_create */
typedef struct thread_create_args {
  stub_fun sfun;
  pthread_fun tfun;
  const void* arg;
  struct process* pcb;
  int thread_count_id;
  struct semaphore load_done;
  bool success;
  uint8_t* kpage;
  uint8_t* upage;
  int offset;
  struct join_status* join_status;
} thread_create_args_t;

/* The process control block for a given process. Since
   there can be multiple threads per process, we need a separate
   PCB from the TCB. All TCBs in a process will have a pointer
   to the PCB, and the PCB will have a pointer to the main thread
   of the process, which is `special`. */
struct process {
  /* Owned by process.c. */
  struct wait_status* wait_status; /* This process's completion status. */
  struct list children;            /* Completion status of children. */
  struct list join_statuses;
  uint32_t* pagedir;          /* Page directory. */
  char process_name[16];      /* Name of the main thread */
  struct file* bin_file;      /* Executable. */
  struct thread* main_thread; /* Pointer to main thread */

  /* Owned by syscall.c. */
  struct list fds; /* List of file descriptors. */
  int next_handle; /* Next handle value. */

  /* Global lock for user threads */
  struct lock process_thread_lock;

  /* Process owned list of threads */
  user_thread_list_t user_thread_list;

  /* Initialized threads counter for thread_naming */
  int user_thread_counter;

  //set this to true if a thread calls process_exit
  bool exiting;

  /* Holds all locks and semaphores for a given process */
  thread_lock_t locks[256];
  thread_sema_t semaphores[256];

  /* Bitmap for dynamically tracking freed pages by offset */
  bool offsets[256];
};

/* Tracks the completion of a process.
   Reference held by both the parent, in its `children' list,
   and by the child, in its `wait_status' pointer. */
struct wait_status {
  struct list_elem elem; /* `children' list element. */
  struct lock lock;      /* Protects ref_cnt. */
  int ref_cnt;           /* 2=child and parent both alive,
                                           1=either child or parent alive,
                                           0=child and parent both dead. */
  pid_t pid;             /* Child process id. */
  int exit_code;         /* Child exit code, if dead. */
  struct semaphore dead; /* 1=child alive, 0=child dead. */
};

/* A file descriptor, for binding a file handle to a file. */
struct file_descriptor {
  struct list_elem elem; /* List element. */
  struct file* file;     /* File. */
  int handle;            /* File handle. */
};

void userprog_init(void);

pid_t process_execute(const char* file_name);
int process_wait(pid_t);
void process_exit(void);
void process_activate(void);

bool is_main_thread(struct thread*, struct process*);
pid_t get_pid(struct process*);

tid_t pthread_execute(stub_fun sfun, pthread_fun tfun, void* arg);
tid_t pthread_join(tid_t);
void pthread_exit(void);
void pthread_exit_main(void);
user_thread_entry_t* create_thread_entry(tid_t tid);
user_thread_entry_t* get_thread_entry(tid_t tid);
void destroy_thread_entry(user_thread_entry_t* thread_entry);
int get_lowest_offset(struct process* pcb);

#endif /* userprog/process.h */
