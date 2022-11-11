#include "userprog/syscall.h"
#include <stdio.h>
#include <float.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"

static void syscall_handler(struct intr_frame*);
static void copy_in(void*, const void*, size_t);

bool retval;

/* Serializes file system operations. */
static struct lock fs_lock;

/* Pointer to current thread global lock */
struct lock* process_thread_lock;

void syscall_init(void) {
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&fs_lock);
}

/* System call handler. */
static void syscall_handler(struct intr_frame* f) {
  typedef int syscall_function(int, int, int);
  process_thread_lock = &thread_current()->pcb->process_thread_lock;

  /* A system call. */
  struct syscall {
    size_t arg_cnt;         /* Number of arguments. */
    syscall_function* func; /* Implementation. */
  };

  /* Table of system calls. */
  static const struct syscall syscall_table[] = {
      {0, (syscall_function*)sys_halt},
      {1, (syscall_function*)sys_exit},
      {1, (syscall_function*)sys_exec},
      {1, (syscall_function*)sys_wait},
      {2, (syscall_function*)sys_create},
      {1, (syscall_function*)sys_remove},
      {1, (syscall_function*)sys_open},
      {1, (syscall_function*)sys_filesize},
      {3, (syscall_function*)sys_read},
      {3, (syscall_function*)sys_write},
      {2, (syscall_function*)sys_seek},
      {1, (syscall_function*)sys_tell},
      {1, (syscall_function*)sys_close},
      {1, (syscall_function*)sys_practice},
      {1, (syscall_function*)sys_compute_e},
      {3, (syscall_function*)sys_pt_create},    /* Creates a new thread */
      {0, (syscall_function*)sys_pt_exit},      /* Exits the current thread */
      {1, (syscall_function*)sys_pt_join},      /* Waits for thread to finish */
      {1, (syscall_function*)sys_lock_init},    /* Initializes a lock */
      {1, (syscall_function*)sys_lock_acquire}, /* Acquires a lock */
      {1, (syscall_function*)sys_lock_release}, /* Releases a lock */
      {2, (syscall_function*)sys_sema_init},    /* Initializes a semaphore */
      {1, (syscall_function*)sys_sema_down},    /* Downs a semaphore */
      {1, (syscall_function*)sys_sema_up},      /* Ups a semaphore */
      {0, (syscall_function*)sys_get_tid},      /* Gets TID of the current thread */
  };

  const struct syscall* sc;
  unsigned call_nr;
  int args[3];

  /* Get the system call. */
  copy_in(&call_nr, f->esp, sizeof call_nr);
  if (call_nr >= sizeof syscall_table / sizeof *syscall_table)
    process_exit();
  sc = syscall_table + call_nr;

  if (sc->func == NULL)
    process_exit();

  /* Get the system call arguments. */
  ASSERT(sc->arg_cnt <= sizeof args / sizeof *args);
  memset(args, 0, sizeof args);
  copy_in(args, (uint32_t*)f->esp + 1, sizeof *args * sc->arg_cnt);

  /* Execute the system call,
     and set the return value. */
  f->eax = sc->func(args[0], args[1], args[2]);
}

/* Closes a file safely */
void safe_file_close(struct file* file) {
  lock_acquire(&fs_lock);
  file_close(file);
  lock_release(&fs_lock);
}

/* Returns true if UADDR is a valid, mapped user address,
   false otherwise. */
static bool verify_user(const void* uaddr) {
  return (uaddr < PHYS_BASE && pagedir_get_page(thread_current()->pcb->pagedir, uaddr) != NULL);
}

/* Copies a byte from user address USRC to kernel address DST.
   USRC must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static inline bool get_user(uint8_t* dst, const uint8_t* usrc) {
  int eax;
  asm("movl $1f, %%eax; movb %2, %%al; movb %%al, %0; 1:" : "=m"(*dst), "=&a"(eax) : "m"(*usrc));
  return eax != 0;
}

/* Writes BYTE to user address UDST.
   UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static inline bool put_user(uint8_t* udst, uint8_t byte) {
  int eax;
  asm("movl $1f, %%eax; movb %b2, %0; 1:" : "=m"(*udst), "=&a"(eax) : "q"(byte));
  return eax != 0;
}

/* Copies SIZE bytes from user address USRC to kernel address
   DST.
   Call process_exit() if any of the user accesses are invalid. */
static void copy_in(void* dst_, const void* usrc_, size_t size) {
  uint8_t* dst = dst_;
  const uint8_t* usrc = usrc_;

  for (; size > 0; size--, dst++, usrc++)
    if (usrc >= (uint8_t*)PHYS_BASE || !get_user(dst, usrc))
      process_exit();
}

/* Creates a copy of user string US in kernel memory
   and returns it as a page that must be freed with
   palloc_free_page().
   Truncates the string at PGSIZE bytes in size.
   Call process_exit() if any of the user accesses are invalid. */
static char* copy_in_string(const char* us) {
  char* ks;
  size_t length;

  ks = palloc_get_page(0);
  if (ks == NULL)
    process_exit();

  for (length = 0; length < PGSIZE; length++) {
    if (us >= (char*)PHYS_BASE || !get_user(ks + length, us++)) {
      palloc_free_page(ks);
      process_exit();
    }

    if (ks[length] == '\0')
      return ks;
  }
  ks[PGSIZE - 1] = '\0';
  return ks;
}

/* Halt system call. */
int sys_halt(void) { shutdown_power_off(); }

/* Exit system call. */
int sys_exit(int exit_code) {
  struct thread* cur = thread_current();
  cur->pcb->wait_status->exit_code = exit_code;

  if (cur != cur->pcb->main_thread) {
    cur->pcb->exiting = true;
    pthread_exit();
  } else {
    pthread_exit_main();
  }
  NOT_REACHED();
}

/* Exec system call. */
int sys_exec(const char* ufile) {
  pid_t tid;
  char* kfile = copy_in_string(ufile);

  lock_acquire(&fs_lock);
  tid = process_execute(kfile);
  lock_release(&fs_lock);

  palloc_free_page(kfile);

  return tid;
}

/* Wait system call. */
int sys_wait(pid_t child) { return process_wait(child); }

/* Create system call. */
int sys_create(const char* ufile, unsigned initial_size) {
  char* kfile = copy_in_string(ufile);
  bool ok;

  lock_acquire(&fs_lock);
  ok = filesys_create(kfile, initial_size);
  lock_release(&fs_lock);

  palloc_free_page(kfile);

  return ok;
}

/* Remove system call. */
int sys_remove(const char* ufile) {
  char* kfile = copy_in_string(ufile);
  bool ok;

  lock_acquire(&fs_lock);
  ok = filesys_remove(kfile);
  lock_release(&fs_lock);

  palloc_free_page(kfile);

  return ok;
}

/* Open system call. */
int sys_open(const char* ufile) {
  char* kfile = copy_in_string(ufile);
  struct file_descriptor* fd;
  int handle = -1;

  fd = malloc(sizeof *fd);
  if (fd != NULL) {
    lock_acquire(&fs_lock);
    fd->file = filesys_open(kfile);
    if (fd->file != NULL) {
      struct thread* cur = thread_current();
      handle = fd->handle = cur->pcb->next_handle++;
      list_push_front(&cur->pcb->fds, &fd->elem);
    } else
      free(fd);
    lock_release(&fs_lock);
  }

  palloc_free_page(kfile);
  return handle;
}

/* Returns the file descriptor associated with the given handle.
   Terminates the process if HANDLE is not associated with an
   open file. */
static struct file_descriptor* lookup_fd(int handle) {
  struct thread* cur = thread_current();
  struct list_elem* e;

  for (e = list_begin(&cur->pcb->fds); e != list_end(&cur->pcb->fds); e = list_next(e)) {
    struct file_descriptor* fd;
    fd = list_entry(e, struct file_descriptor, elem);
    if (fd->handle == handle)
      return fd;
  }

  process_exit();
  NOT_REACHED();
}

/* Filesize system call. */
int sys_filesize(int handle) {
  struct file_descriptor* fd = lookup_fd(handle);
  int size;

  lock_acquire(&fs_lock);
  size = file_length(fd->file);
  lock_release(&fs_lock);

  return size;
}

/* Read system call. */
int sys_read(int handle, void* udst_, unsigned size) {
  uint8_t* udst = udst_;
  struct file_descriptor* fd;
  int bytes_read = 0;

  /* Handle keyboard reads. */
  if (handle == STDIN_FILENO) {
    for (bytes_read = 0; (size_t)bytes_read < size; bytes_read++)
      if (udst >= (uint8_t*)PHYS_BASE || !put_user(udst++, input_getc()))
        process_exit();
    return bytes_read;
  }

  /* Handle all other reads. */
  fd = lookup_fd(handle);
  lock_acquire(&fs_lock);
  while (size > 0) {
    /* How much to read into this page? */
    size_t page_left = PGSIZE - pg_ofs(udst);
    size_t read_amt = size < page_left ? size : page_left;
    off_t retval;

    /* Check that touching this page is okay. */
    if (!verify_user(udst)) {
      lock_release(&fs_lock);
      process_exit();
    }

    /* Read from file into page. */
    retval = file_read(fd->file, udst, read_amt);
    if (retval < 0) {
      if (bytes_read == 0)
        bytes_read = -1;
      break;
    }
    bytes_read += retval;

    /* If it was a short read we're done. */
    if (retval != (off_t)read_amt)
      break;

    /* Advance. */
    udst += retval;
    size -= retval;
  }
  lock_release(&fs_lock);

  return bytes_read;
}

/* Write system call. */
int sys_write(int handle, void* usrc_, unsigned size) {
  uint8_t* usrc = usrc_;
  struct file_descriptor* fd = NULL;
  int bytes_written = 0;

  /* Lookup up file descriptor. */
  if (handle != STDOUT_FILENO)
    fd = lookup_fd(handle);

  lock_acquire(&fs_lock);
  while (size > 0) {
    /* How much bytes to write to this page? */
    size_t page_left = PGSIZE - pg_ofs(usrc);
    size_t write_amt = size < page_left ? size : page_left;
    off_t retval;

    /* Check that we can touch this user page. */
    if (!verify_user(usrc)) {
      lock_release(&fs_lock);
      process_exit();
    }

    /* Do the write. */
    if (handle == STDOUT_FILENO) {
      putbuf(usrc, write_amt);
      retval = write_amt;
    } else
      retval = file_write(fd->file, usrc, write_amt);
    if (retval < 0) {
      if (bytes_written == 0)
        bytes_written = -1;
      break;
    }
    bytes_written += retval;

    /* If it was a short write we're done. */
    if (retval != (off_t)write_amt)
      break;

    /* Advance. */
    usrc += retval;
    size -= retval;
  }
  lock_release(&fs_lock);

  return bytes_written;
}

/* Seek system call. */
int sys_seek(int handle, unsigned position) {
  struct file_descriptor* fd = lookup_fd(handle);

  lock_acquire(&fs_lock);
  if ((off_t)position >= 0)
    file_seek(fd->file, position);
  lock_release(&fs_lock);

  return 0;
}

/* Tell system call. */
int sys_tell(int handle) {
  struct file_descriptor* fd = lookup_fd(handle);
  unsigned position;

  lock_acquire(&fs_lock);
  position = file_tell(fd->file);
  lock_release(&fs_lock);

  return position;
}

/* Close system call. */
int sys_close(int handle) {
  struct file_descriptor* fd = lookup_fd(handle);
  safe_file_close(fd->file);
  list_remove(&fd->elem);
  free(fd);
  return 0;
}

/* Practice system call. */
int sys_practice(int input) { return input + 1; }

/* Compute e and return a float cast to an int */
int sys_compute_e(int n) { return sys_sum_to_e(n); }

/************* PROJECT 2 *************/

tid_t sys_pt_create(stub_fun sfun, pthread_fun tfun, void* arg) {
  return pthread_execute(sfun, tfun, arg);
}

void sys_pt_exit(void) { pthread_exit(); }

tid_t sys_pt_join(tid_t tid) { return pthread_join(tid); }

bool sys_lock_init(lock_t* lock) {
  /* Note: 'return false;' doesn't work, need a temp bool to work */
  if (lock == NULL) {
    retval = false;
    return retval;
  }

  // thread_lock_t* thread_lock = &thread_current()->pcb->locks[(uint8_t)*lock];
  struct thread* t = thread_current();
  for (uint8_t i = 0; i < 256; i++) {
    if (!t->pcb->locks[i].initialized) {
      lock_acquire(process_thread_lock);
      t->pcb->locks[i].initialized = true;
      t->pcb->locks[i].tid = t->tid;
      lock_init(&t->pcb->locks[i].lock);
      *lock = (lock_t)i;
      lock_release(process_thread_lock);
      retval = true;
      return retval;
    }
  }
  retval = false;
  return retval;

  // this might be wrong, look into it tmr
  // if (thread_lock->tid != 0) {
  //   retval = false;
  //   return retval;
  // } else {
  //   lock_acquire(process_thread_lock);
  //   lock_init(&thread_lock->lock);
  //   lock_release(process_thread_lock);
  //   retval = true;
  //   return retval;
  // }
}

bool sys_lock_acquire(lock_t* lock) {
  if (lock == NULL) {
    retval = false;
    return retval;
  }

  thread_lock_t* thread_lock = &thread_current()->pcb->locks[(uint8_t)*lock];
  if (thread_lock->initialized != true) {
    retval = false;
    return retval;
  } else if (lock_held_by_current_thread(&thread_lock->lock)) {
    retval = false;
    return retval;
  } else {
    lock_acquire(process_thread_lock);
    lock_acquire(&thread_lock->lock);
    thread_lock->tid = thread_current()->tid;
    lock_release(process_thread_lock);
    retval = true;
    return retval;
  }
}

bool sys_lock_release(lock_t* lock) {
  if (lock == NULL) {
    retval = false;
    return retval;
  }

  thread_lock_t* thread_lock = &thread_current()->pcb->locks[(uint8_t)*lock];
  if (thread_lock->tid != thread_current()->tid || thread_lock->initialized != true) {
    retval = false;
    return retval;
  } else {
    lock_acquire(process_thread_lock);
    lock_release(&thread_lock->lock);
    thread_lock->tid = 0;
    lock_release(process_thread_lock);
    retval = true;
    return retval;
  }
}

bool sys_sema_init(sema_t* sema, int val) {
  if (sema == NULL || val < 0) {
    retval = false;
    return retval;
  }

  struct thread* t = thread_current();
  for (uint8_t i = 0; i < 256; i++) {
    if (!t->pcb->semaphores[i].initialized) {
      lock_acquire(process_thread_lock);
      t->pcb->semaphores[i].initialized = true;
      // t->pcb->semaphores[i].tid = t->tid;
      sema_init(&t->pcb->semaphores[i].sema, val);
      *sema = (sema_t)i;
      lock_release(process_thread_lock);
      retval = true;
      return retval;
    }
  }
  retval = false;
  return retval;

  // thread_sema_t* thread_sema = &thread_current()->pcb->semaphores[(uint8_t)*sema];
  // if (thread_sema->initialized) {
  //   retval = false;
  //   return retval;
  // } else {
  //   lock_acquire(process_thread_lock);
  //   sema_init(&thread_sema->sema, val);
  //   thread_sema->initialized = true;
  //   lock_release(process_thread_lock);
  //   retval = true;
  //   return retval;
  // }
}

bool sys_sema_down(sema_t* sema) {
  if (sema == NULL) {
    retval = false;
    return retval;
  }

  thread_sema_t* thread_sema = &thread_current()->pcb->semaphores[(uint8_t)*sema];
  if (thread_sema->initialized != true) {
    retval = false;
    return retval;
  } else {
    sema_down(&thread_sema->sema);
    retval = true;
    return retval;
  }
}

bool sys_sema_up(sema_t* sema) {
  if (sema == NULL) {
    retval = false;
    return retval;
  }

  thread_sema_t* thread_sema = &thread_current()->pcb->semaphores[(uint8_t)*sema];
  if (thread_sema->initialized != true) {
    retval = false;
    return retval;
  } else {
    sema_up(&thread_sema->sema);
    retval = true;
    return retval;
  }
}

tid_t sys_get_tid(void) { return thread_current()->tid; }
