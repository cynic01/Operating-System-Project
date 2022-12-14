#include "userprog/process.h"
#include <debug.h>
#include <float.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static thread_func start_process NO_RETURN;
static thread_func start_pthread NO_RETURN;
static bool load(const char* cmd_line, void (**eip)(void), void** esp);
bool setup_thread(void (**eip)(void), void** esp, void* aux);

/* Data structure shared between process_execute() in the
   invoking thread and start_process() in the newly invoked
   thread. */
struct exec_info {
  const char* file_name;           /* Program to load. */
  struct semaphore load_done;      /* "Up"ed when loading complete. */
  struct wait_status* wait_status; /* Child process. */
  bool success;                    /* Program successfully loaded? */
};

/* Initializes user programs in the system by ensuring the main
   thread has a minimal PCB so that it can execute and wait for
   the first user process. Any additions to the PCB should be also
   initialized here if main needs those members */
void userprog_init(void) {
  struct thread* t = thread_current();
  bool success;

  /* Allocate process control block
     It is imoprtant that this is a call to calloc and not malloc,
     so that t->pcb->pagedir is guaranteed to be NULL (the kernel's
     page directory) when t->pcb is assigned, because a timer interrupt
     can come at any time and activate our pagedir */
  t->pcb = calloc(sizeof(struct process), 1);
  success = t->pcb != NULL;

  /* Main only needs a list of children */
  if (success)
    list_init(&t->pcb->children);

  /* Kill the kernel if we did not succeed */
  ASSERT(success);
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   process id, or TID_ERROR if the thread cannot be created. */
pid_t process_execute(const char* file_name) {
  struct exec_info exec;
  char thread_name[16];
  char* save_ptr;
  tid_t tid;

  /* Initialize exec_info. */
  exec.file_name = file_name;
  sema_init(&exec.load_done, 0);

  /* Create a new thread to execute FILE_NAME. */
  strlcpy(thread_name, file_name, sizeof thread_name);
  strtok_r(thread_name, " ", &save_ptr);
  tid = thread_create(thread_name, PRI_DEFAULT, start_process, &exec);
  if (tid != TID_ERROR) {
    sema_down(&exec.load_done);
    if (exec.success)
      list_push_back((struct list*)&thread_current()->pcb->children, &exec.wait_status->elem);
    else
      tid = TID_ERROR;
  }

  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void start_process(void* exec_) {
  struct thread* t = thread_current();

  struct exec_info* exec = exec_;
  struct intr_frame if_;
  uint32_t fpu_curr[27];
  bool success, pcb_success, ws_success;
  user_thread_entry_t* user_thread_entry;

  /* Allocate process control block */
  struct process* new_pcb = malloc(sizeof(struct process));
  success = pcb_success = new_pcb != NULL;

  /* Initialize process control block */
  if (success) {
    // Ensure that timer_interrupt() -> schedule() -> process_activate()
    // does not try to activate our uninitialized pagedir
    new_pcb->pagedir = NULL;
    t->pcb = new_pcb;

    // Continue initializing the PCB as normal
    list_init(&t->pcb->children);
    list_init(&t->pcb->fds);
    t->pcb->next_handle = 2;
    t->pcb->main_thread = t;
    strlcpy(t->pcb->process_name, t->name, sizeof t->name);

    /* Initialize global per-process thread lock */
    lock_init(&t->pcb->process_thread_lock);

    /* Initialize join status list and create join status for main thread */
    list_init(&t->pcb->join_statuses);
    struct join_status* join_status = calloc(1, sizeof(struct join_status));
    if (join_status == NULL) {
      success = false;
    } else {
      t->join_status = join_status;
      join_status->tid = t->tid;
      join_status->waited_on = false;
      sema_init(&t->join_status->sema, 0);
      list_push_front(&t->pcb->join_statuses, &join_status->elem);
    }

    /* Initialize threads list and add main thread to head */
    list_init(&t->pcb->user_thread_list.lst);
    user_thread_entry = calloc(1, sizeof(user_thread_entry_t));
    if (user_thread_entry == NULL) {
      success = false;
    } else {
      user_thread_entry->thread = t;
      user_thread_entry->tid = t->tid;
      user_thread_entry->waited_on = false;
      user_thread_entry->completed = false;
      user_thread_entry->initialized = true;
      list_push_front(&t->pcb->user_thread_list.lst, &user_thread_entry->elem);
    }

    /* Set user thread counter */
    t->pcb->user_thread_counter = 1;

    /* Init upage offset bitmap */

    t->pcb->offsets[0] = true; // 0 is unusable
    t->pcb->offsets[1] = true; // 1 is unusable
    for (int i = 2; i < 256; i++) {
      t->pcb->offsets[i] = false;
    }

    if (t->pcb->locks == NULL || t->pcb->semaphores == NULL) {
      success = false;
    }
  }

  /* Allocate wait_status. */
  if (success) {
    exec->wait_status = t->pcb->wait_status = malloc(sizeof *exec->wait_status);
    success = ws_success = exec->wait_status != NULL;
  }

  /* Initialize wait_status. */
  if (success) {
    lock_init(&exec->wait_status->lock);
    exec->wait_status->ref_cnt = 2;
    exec->wait_status->pid = t->tid;
    exec->wait_status->exit_code = -1;
    sema_init(&exec->wait_status->dead, 0);
  }

  /* Initialize interrupt frame and load executable. */
  if (success) {
    memset(&if_, 0, sizeof if_);
    fpu_save_init(&if_.fpu, &fpu_curr);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    success = load(exec->file_name, &if_.eip, &if_.esp);
  }

  /* Handle failure with succesful PCB malloc. Must free the PCB */
  if (!success && pcb_success) {
    // Avoid race where PCB is freed before t->pcb is set to NULL
    // If this happens, then an unfortuantely timed timer interrupt
    // can try to activate the pagedir, but it is now freed memory
    struct process* pcb_to_free = t->pcb;
    t->pcb = NULL;
    free(pcb_to_free);
  }

  /* Handle failure with successful wait_status malloc */
  if (!success && ws_success)
    free(exec->wait_status);

  /* Notify parent thread and clean up. */
  exec->success = success;
  sema_up(&exec->load_done);
  if (!success)
    thread_exit();

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Releases one reference to CS and, if it is now unreferenced,
   frees it. */
static void release_child(struct wait_status* cs) {
  int new_ref_cnt;

  lock_acquire(&cs->lock);
  new_ref_cnt = --cs->ref_cnt;
  lock_release(&cs->lock);

  if (new_ref_cnt == 0)
    free(cs);
}

static void release_thread(struct join_status* cs) {
  int new_ref_cnt;

  lock_acquire(&cs->lock);
  new_ref_cnt = --cs->ref_cnt;
  lock_release(&cs->lock);

  if (new_ref_cnt == 0)
    free(cs);
}

/* Waits for process with PID child_pid to die and returns its exit status.
   If it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If child_pid is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting. */
int process_wait(pid_t child_pid) {
  struct thread* cur = thread_current();
  struct list_elem* e;

  for (e = list_begin(&cur->pcb->children); e != list_end(&cur->pcb->children); e = list_next(e)) {
    struct wait_status* cs = list_entry(e, struct wait_status, elem);
    if (cs->pid == child_pid) {
      int exit_code;
      list_remove(e);
      sema_down(&cs->dead);
      exit_code = cs->exit_code;
      release_child(cs);
      return exit_code;
    }
  }
  return -1;
}

/* Free the current process's resources. */
void process_exit(void) {
  struct thread* cur = thread_current();
  struct list_elem *e, *next;
  uint32_t* pd;

  /* If this thread does not have a PCB, don't worry */
  if (cur->pcb == NULL) {
    thread_exit();
    NOT_REACHED();
  }

  /* Close executable (and allow writes). */
  safe_file_close(cur->pcb->bin_file);

  /* Free entries of children list. */
  for (e = list_begin(&cur->pcb->children); e != list_end(&cur->pcb->children); e = next) {
    struct wait_status* cs = list_entry(e, struct wait_status, elem);
    next = list_remove(e);
    release_child(cs);
  }

  /* Free entries of join_statuses list. */
  for (e = list_begin(&cur->pcb->join_statuses); e != list_end(&cur->pcb->join_statuses);
       e = next) {
    struct join_status* js = list_entry(e, struct join_status, elem);
    next = list_remove(e);
    free(js);
  }

  /* Free entries of user threads list. */
  for (e = list_begin(&cur->pcb->user_thread_list); e != list_end(&cur->pcb->user_thread_list);
       e = next) {
    user_thread_entry_t* user_thread = list_entry(e, user_thread_entry_t, elem);
    next = list_remove(e);
    free(user_thread);
  }

  /* Close all currently open file descriptors */
  while (!list_empty(&cur->pcb->fds)) {
    e = list_begin(&cur->pcb->fds);
    struct file_descriptor* fd = list_entry(e, struct file_descriptor, elem);
    sys_close(fd->handle);
  }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pcb->pagedir;
  if (pd != NULL) {
    /* Correct ordering here is crucial.  We must set
         cur->pcb->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
    cur->pcb->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }

  /* Notify parent that we're dead, as the last thing we do. */
  if (cur->pcb->wait_status != NULL) {
    struct wait_status* cs = cur->pcb->wait_status;
    printf("%s: exit(%d)\n", cur->pcb->process_name, cs->exit_code);
    sema_up(&cs->dead);
    release_child(cs);
  }

  /* Free the PCB of this process and kill this thread
     Avoid race where PCB is freed before t->pcb is set to NULL
     If this happens, then an unfortuantely timed timer interrupt
     can try to activate the pagedir, but it is now freed memory */
  struct process* pcb_to_free = cur->pcb;
  cur->pcb = NULL;
  free(pcb_to_free);
  thread_exit();
}

/* Sets up the CPU for running user code in the current
   thread. This function is called on every context switch. */
void process_activate(void) {
  struct thread* t = thread_current();

  /* Activate thread's page tables. */
  if (t->pcb != NULL && t->pcb->pagedir != NULL)
    pagedir_activate(t->pcb->pagedir);
  else
    pagedir_activate(NULL);

  /* Set thread's kernel stack for use in processing interrupts.
     This does nothing if this is not a user process. */
  tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(const char* cmd_line, void** esp);
static bool validate_segment(const struct Elf32_Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char* cmd_line, void (**eip)(void), void** esp) {
  struct thread* t = thread_current();
  char file_name[NAME_MAX + 2];
  struct Elf32_Ehdr ehdr;
  struct file* file = NULL;
  off_t file_ofs;
  bool success = false;
  char* cp;
  int i;

  /* Allocate and activate page directory. */
  t->pcb->pagedir = pagedir_create();
  if (t->pcb->pagedir == NULL)
    goto done;
  process_activate();

  /* Extract file_name from command line. */
  while (*cmd_line == ' ')
    cmd_line++;
  strlcpy(file_name, cmd_line, sizeof file_name);
  cp = strchr(file_name, ' ');
  if (cp != NULL)
    *cp = '\0';

  /* Open executable file. */
  t->pcb->bin_file = file = filesys_open(file_name);
  if (file == NULL) {
    printf("load: %s: open failed\n", file_name);
    goto done;
  }
  file_deny_write(file);

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 ||
      ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", file_name);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
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
        if (validate_segment(&phdr, file)) {
          bool writable = (phdr.p_flags & PF_W) != 0;
          uint32_t file_page = phdr.p_offset & ~PGMASK;
          uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
          uint32_t page_offset = phdr.p_vaddr & PGMASK;
          uint32_t read_bytes, zero_bytes;
          if (phdr.p_filesz > 0) {
            /* Normal segment.
                     Read initial part from disk and zero the rest. */
            read_bytes = page_offset + phdr.p_filesz;
            zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
          } else {
            /* Entirely zero.
                     Don't read anything from disk. */
            read_bytes = 0;
            zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
          }
          if (!load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable))
            goto done;
        } else
          goto done;
        break;
    }
  }

  /* Set up stack. */
  if (!setup_stack(cmd_line, esp))
    goto done;

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  return success;
}

/* load() helpers. */

static bool install_page(void* upage, void* kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr* phdr, struct file* file) {
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void*)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
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

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t* kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable)) {
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Reverse the order of the ARGC pointers to char in ARGV. */
static void reverse(int argc, char** argv) {
  for (; argc > 1; argc -= 2, argv++) {
    char* tmp = argv[0];
    argv[0] = argv[argc - 1];
    argv[argc - 1] = tmp;
  }
}

/* Pushes the SIZE bytes in BUF onto the stack in KPAGE, whose
   page-relative stack pointer is *OFS, and then adjusts *OFS
   appropriately.  The bytes pushed are rounded to a 32-bit
   boundary.

   If successful, returns a pointer to the newly pushed object.
   On failure, returns a null pointer. */
static void* push(uint8_t* kpage, size_t* ofs, const void* buf, size_t size) {
  size_t padsize = ROUND_UP(size, sizeof(uint32_t));
  if (*ofs < padsize)
    return NULL;

  *ofs -= padsize;
  memcpy(kpage + *ofs + (padsize - size), buf, size);
  return kpage + *ofs + (padsize - size);
}

/* Sets up command line arguments in KPAGE, which will be mapped
   to UPAGE in user space.  The command line arguments are taken
   from CMD_LINE, separated by spaces.  Sets *ESP to the initial
   stack pointer for the process. */
static bool init_cmd_line(uint8_t* kpage, uint8_t* upage, const char* cmd_line, void** esp) {
  size_t ofs = PGSIZE;
  char* const null = NULL;
  char* cmd_line_copy;
  char *karg, *saveptr;
  int argc;
  char** argv;
  static void* arguments[MAX_ARGS];

  /* Push command line string. */
  cmd_line_copy = push(kpage, &ofs, cmd_line, strlen(cmd_line) + 1);
  if (cmd_line_copy == NULL)
    return false;

  /* Parse command line into arguments */
  argc = 0;
  for (karg = strtok_r(cmd_line_copy, " ", &saveptr); karg != NULL;
       karg = strtok_r(NULL, " ", &saveptr)) {
    arguments[argc++] = upage + (karg - (char*)kpage);
  }

  // Insert padding to ensure the stack pointer will ultimately be 16-byte-aligned
  size_t alignment_adjustment =
      ((PGSIZE - ofs) + (argc + 1) * sizeof(char*) + sizeof(char**) + sizeof(int)) % 16;
  ofs -= 16 - alignment_adjustment;

  // Push sentinel null for argv[argc]
  if (push(kpage, &ofs, &null, sizeof null) == NULL)
    return false;

  // Push command line arguments
  for (int i = 0; i < argc; i++) {
    if (push(kpage, &ofs, arguments + i, sizeof(void**)) == NULL)
      return false;
  }

  /* Reverse the order of the command line arguments. */
  argv = (char**)(upage + ofs);
  reverse(argc, (char**)(kpage + ofs));

  /* Push argv, argc, "return address". */
  if (push(kpage, &ofs, &argv, sizeof argv) == NULL ||
      push(kpage, &ofs, &argc, sizeof argc) == NULL ||
      push(kpage, &ofs, &null, sizeof null) == NULL)
    return false;

  /* Set initial stack pointer. */
  *esp = upage + ofs;
  return true;
}

/* Create a minimal stack for T by mapping a page at the
   top of user virtual memory.  Fills in the page using CMD_LINE
   and sets *ESP to the stack pointer. */
static bool setup_stack(const char* cmd_line, void** esp) {
  uint8_t* kpage;
  bool success = false;

  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL) {
    uint8_t* upage = ((uint8_t*)PHYS_BASE) - PGSIZE;
    if (install_page(upage, kpage, true)) {
      success = init_cmd_line(kpage, upage, cmd_line, esp);
      thread_current()->kpage = kpage;
      thread_current()->upage = upage;
    } else
      palloc_free_page(kpage);
  }

  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable) {
  struct thread* t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pcb->pagedir, upage) == NULL &&
          pagedir_set_page(t->pcb->pagedir, upage, kpage, writable));
}

/* Returns true if t is the main thread of the process p */
bool is_main_thread(struct thread* t, struct process* p) { return p->main_thread == t; }

/* Gets the PID of a process */
pid_t get_pid(struct process* p) { return (pid_t)p->main_thread->tid; }

/* Creates a new stack for the thread and sets up its arguments.
   Stores the thread's entry point into *EIP and its initial stack
   pointer into *ESP. Handles all cleanup if unsuccessful. Returns
   true if successful, false otherwise.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. You may find it necessary to change the
   function signature. */
bool setup_thread(void (**eip)(void), void** esp, void* aux) {
  size_t ofs = PGSIZE - 12;
  uint8_t* kpage;
  uint8_t* upage;
  bool success = false;
  char* const null = NULL;
  thread_create_args_t* args = (thread_create_args_t*)aux;

  /* Set eip to stub function */
  *eip = (void*)args->sfun;

  /* Setup the stack and eip */
  kpage = palloc_get_page(PAL_USER | PAL_ZERO);

  if (kpage != NULL) {
    int offset = get_lowest_offset(args->pcb);
    upage = ((uint8_t*)PHYS_BASE) - offset * PGSIZE;

    /* store pages for destroying later, will get pushed to thread list */
    args->kpage = kpage;
    args->upage = upage;
    args->offset = offset;

    success = install_page(upage, kpage, true);
    if (success) {
      // ofs = PHYS_BASE - (offset - 1) * PGSIZE;
      *esp = PHYS_BASE - (offset - 1) * PGSIZE;

      /* Push function and args onto the stack */
      if (push(kpage, &ofs, &args->arg, sizeof args->arg) == NULL ||
          push(kpage, &ofs, &args->tfun, sizeof args->tfun) == NULL ||
          push(kpage, &ofs, &null, sizeof null) == NULL)
        return false;

      /* set the stack pointer */
      *esp = upage + ofs;

    } else
      palloc_free_page(kpage);
  }
  return success;
}

int get_lowest_offset(struct process* pcb) {
  struct lock* process_thread_lock = &pcb->process_thread_lock;
  for (int i = 0; i < 256; i++) {
    if (pcb->offsets[i] == false) {
      /* Synch here for bitmap flip */
      lock_acquire(process_thread_lock);
      pcb->offsets[i] = true;
      lock_release(process_thread_lock);
      return i;
    }
  }
  /* Safety */
  printf("NEED TO INCREASE OFFSET SIZE!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
  NOT_REACHED();
}

/* Starts a new thread with a new user stack running SF, which takes
   TF and ARG as arguments on its user stack. This new thread may be
   scheduled (and may even exit) before pthread_execute () returns.
   Returns the new thread's TID or TID_ERROR if the thread cannot
   be created properly.

   This function will be implemented in Project 2: Multithreading and
   should be similar to process_execute (). For now, it does nothing.
   */
tid_t pthread_execute(stub_fun sfun, pthread_fun tfun, void* arg) {
  struct thread* t = thread_current();
  struct lock* process_thread_lock = &t->pcb->process_thread_lock;
  char new_thread_name[21];
  tid_t new_tid;
  user_thread_entry_t* user_thread_entry;
  thread_create_args_t* args = malloc(sizeof(thread_create_args_t));

  if (args == NULL) {
    return TID_ERROR;
  }

  args->sfun = sfun;
  args->tfun = tfun;
  args->arg = arg;
  args->pcb = t->pcb;
  args->success = false;
  args->kpage = NULL;
  args->upage = NULL;
  args->offset = NULL;
  sema_init(&args->load_done, 0);

  /* Synch here for counter increment */
  lock_acquire(process_thread_lock);
  args->thread_count_id = ++t->pcb->user_thread_counter;
  lock_release(process_thread_lock);

  snprintf(new_thread_name, 20, "%s-%d", t->pcb->main_thread->name, args->thread_count_id);

  new_tid = thread_create((const char*)new_thread_name, PRI_DEFAULT, start_pthread, (void*)args);

  if (new_tid != TID_ERROR) {
    sema_down(&args->load_done);
    if (args->success) {
      lock_acquire(process_thread_lock);
      user_thread_entry = get_thread_entry(new_tid);
      if (!user_thread_entry) {
        create_thread_entry(new_tid);
      }
      lock_release(process_thread_lock); 
    }
  }
  free(args);
  return new_tid;
}

/* A thread function that creates a new user thread and starts it
   running. Responsible for adding itself to the list of threads in
   the PCB.

   This function will be implemented in Project 2: Multithreading and
   should be similar to start_process (). For now, it does nothing. */
static void start_pthread(void* exec_) {
  struct thread* t = thread_current();
  thread_create_args_t* args = (thread_create_args_t*)exec_;
  user_thread_entry_t* user_thread_entry;
  struct intr_frame if_;
  uint32_t fpu_curr[27];
  bool success;

  /* Copy PCB pointer */
  t->pcb = args->pcb;

  /* Init interupt frame */
  memset(&if_, 0, sizeof if_);
  fpu_save_init(&if_.fpu, &fpu_curr);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = setup_thread(&if_.eip, &if_.esp, exec_);

  args->success = success;
  sema_up(&args->load_done);
  if (!success)
    thread_exit();

  process_activate();

  /* Add itself to thread list or update if applicable */
  // Note: lock can only be accessed after pcb init
  struct lock* process_thread_lock = &thread_current()->pcb->process_thread_lock;
  lock_acquire(process_thread_lock);
  user_thread_entry = get_thread_entry(t->tid);
  if (!user_thread_entry) {
    user_thread_entry = create_thread_entry(t->tid);
  }
  user_thread_entry->initialized = true;

  thread_current()->kpage = args->kpage;
  thread_current()->upage = args->upage;
  thread_current()->offset = args->offset;
  user_thread_entry->kpage = args->kpage;
  user_thread_entry->upage = args->upage;

  struct join_status* join_status = calloc(1, sizeof(struct join_status));
  if (join_status != NULL) {
    join_status->tid = t->tid;
    join_status->waited_on = false;
    join_status->ref_cnt = 2;
    lock_init(&join_status->lock);
    sema_init(&join_status->sema, 0);
    list_push_front(&t->pcb->join_statuses, &join_status->elem);
    t->join_status = join_status;
  }

  lock_release(process_thread_lock);


  /* Start the user process by simulating a return from an interrupt */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for thread with TID to die, if that thread was spawned
 *    in the same process and has not been waited on yet. Returns TID on
 *       success and returns TID_ERROR on failure immediately, without
 *          waiting.
 *
 *             This function will be implemented in Project 2: Multithreading. For
 *                now, it does nothing. */
tid_t pthread_join(tid_t tid) {
  struct thread* cur = thread_current();
  struct list_elem* e;

  lock_acquire(&cur->pcb->process_thread_lock);
  for (e = list_begin(&cur->pcb->join_statuses); e != list_end(&cur->pcb->join_statuses);
       e = list_next(e)) {
    struct join_status* join_status = list_entry(e, struct join_status, elem);
    if (join_status->tid == tid && !join_status->waited_on) {
      list_remove(e);
      join_status->waited_on = true;
      lock_release(&cur->pcb->process_thread_lock);
      sema_down(&join_status->sema);
      release_thread(join_status);
      return tid;
    }
  }

  lock_release(&cur->pcb->process_thread_lock);
  return TID_ERROR;
}

/* Free the current thread's resources. Most resources will
 *    be freed on thread_exit(), so all we have to do is deallocate the
 *       thread's userspace stack. Wake any waiters on this thread.
 *
 *          The main thread should not use this function. See
 *             pthread_exit_main() below.
 *
 *                This function will be implemented in Project 2: Multithreading. For
 *                   now, it does nothing. */
void pthread_exit(void) {
  struct thread* t = thread_current();
  struct lock* process_thread_lock = &t->pcb->process_thread_lock;

  if (t == t->pcb->main_thread) {
    pthread_exit_main();
  }

  user_thread_entry_t* thread_entry = get_thread_entry(t->tid);
  list_remove(&thread_entry->elem);
  free(thread_entry);
  
  pagedir_clear_page(t->pcb->pagedir, t->upage);
  palloc_free_page(t->kpage);

  /* Synch here for bitmap flip */
  lock_acquire(process_thread_lock);
  t->pcb->offsets[t->offset] = false;
  lock_release(process_thread_lock);

  /* Notify parent that we're dead, as the last thing we do. */
  if (t->join_status != NULL) {
    struct join_status* join_status = t->join_status;
    sema_up(&join_status->sema);
  }

  /* Exit thread */
  thread_exit();
}

/* Only to be used when the main thread explicitly calls pthread_exit.
   The main thread should wait on all threads in the process to
   terminate properly, before exiting itself. When it exits itself, it
   must terminate the process in addition to all necessary duties in
   pthread_exit.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit_main(void) {
  struct thread* t = thread_current();
  struct list_elem* e;
  struct list_elem* new_e;
  struct list user_thread_list = t->pcb->user_thread_list.lst;
  user_thread_entry_t* thread_entry;

  /* Free thread */
  /*thread_entry = get_thread_entry(t->tid);
  list_remove(&thread_entry->elem);
  free(thread_entry);*/

  /* Notify waiter that we're dead, as the last thing we do. */
  if (t->join_status != NULL) {
    struct join_status* join_status = t->join_status;
    sema_up(&join_status->sema);
  }

  struct join_status* join_status;
  /* Join on all unjoined threads */
  lock_acquire(&t->pcb->process_thread_lock);

  e = list_begin(&t->pcb->join_statuses);
  while (e != list_end(&t->pcb->join_statuses)) {
    new_e = list_next(e);

    join_status = list_entry(e, struct join_status, elem);
    struct join_status* new_join_status = list_entry(new_e, struct join_status, elem);

    if (join_status->tid != thread_current()->tid) {
      lock_release(&t->pcb->process_thread_lock);
      pthread_join(join_status->tid);
      lock_acquire(&t->pcb->process_thread_lock);
    }
    e = new_e;
  }
  lock_release(&t->pcb->process_thread_lock);

  for (int i = 0; i < 256; i++) {
    thread_lock_t this_lock = t->pcb->locks[i]; 
    this_lock.initialized = false; 
    this_lock.tid = 0;
  }
  for (int i = 0; i < 256; i++) {
    thread_sema_t this_sema = t->pcb->semaphores[i]; 
    this_sema.initialized = false; 
  }

  /*while (!list_empty(&user_thread_list)) {
    e = list_pop_front(&user_thread_list);
    thread_entry = list_entry(e, user_thread_entry_t, elem);
    list_remove(e);
    free(thread_entry);
  }*/

  /* finally free the kpage and exit the process */
  pagedir_clear_page(t->pcb->pagedir, t->upage);
  palloc_free_page(t->kpage);

  process_exit();
}

user_thread_entry_t* create_thread_entry(tid_t tid) {
  struct thread* t = thread_current();

  user_thread_entry_t* user_thread_entry = calloc(1, sizeof(user_thread_entry_t));
  if (user_thread_entry == NULL) {
    return NULL;
  }

  if (tid == t->tid) {
    user_thread_entry->thread = t;
  } else {
    user_thread_entry->thread = NULL;
  }

  user_thread_entry->tid = tid;
  user_thread_entry->waited_on = false;
  user_thread_entry->completed = false;
  user_thread_entry->initialized = false;

  list_push_back(&t->pcb->user_thread_list.lst, &user_thread_entry->elem);

  return user_thread_entry;
}

user_thread_entry_t* get_thread_entry(tid_t tid) {
  struct list_elem* e;
  user_thread_entry_t* thread_entry;

  for (e = list_begin(&thread_current()->pcb->user_thread_list.lst);
       e != list_end(&thread_current()->pcb->user_thread_list.lst); e = list_next(e)) {
    thread_entry = list_entry(e, user_thread_entry_t, elem);

    /* Return the current thread matches */
    if (thread_entry->tid == tid) {
      return thread_entry;
    }
  }
  return NULL;
}

void destroy_thread_entry(user_thread_entry_t* thread_entry) { free(thread_entry); }
