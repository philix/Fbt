/**
 * @file fbt_syscall.c
 * Implementation of special system call handlers.
 *
 * Copyright (c) 2011 ETH Zurich
 * @author Mathias Payer <mathias.payer@nebelwelt.net>
 *
 * $Date: 2012-01-18 16:44:48 +0100 (Wed, 18 Jan 2012) $
 * $LastChangedDate: 2012-01-18 16:44:48 +0100 (Wed, 18 Jan 2012) $
 * $LastChangedBy: kravinae $
 * $Revision: 1189 $
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA  02110-1301, USA.
 */
#define _GNU_SOURCE

#include "fbt_syscall.h"

#include <assert.h>
#include <stddef.h>
#include <ucontext.h>
#include <linux/sched.h>

#include "fbt_code_cache.h"
#include "fbt_datatypes.h"
#include "fbt_debug.h"
#include "fbt_mem_mgmt.h"
#include "fbt_translate.h"
#include "libfastbt.h"
#include "generic/fbt_libc.h"
#include "generic/fbt_llio.h"

/*
      sycall code    |          Remark
---------------------+--------------------------------------------
SYS_exit               ensure that we close BT
SYS_execve             redirected to auth_execve
SYS_ptrace             deny
SYS_signal             installs a new signal handler (deprecated)
SYS_sigaction          installs a new signal handler
SYS_mmap               redirected to auth_mmap
SYS_munmap             TODO
SYS_fstat              old fstat syscall, used by fbt_dso.c
SYS_stat64             use new fstat syscall
SYS_fstat64            use new fstat syscall
SYS_sigreturn          we should never see this syscall
SYS_clone              initializes a new thread
SYS_mprotect           redirect to auth_mprotect
SYS_rt_sigreturn       we should never see this syscall
SYS_rt_sigaction       install a new signal handler
SYS_rt_sigprocmask     change the list of currently blocked signals
SYS_getcwd             get current wd
SYS_mmap2              redirected to auth_mmap2
SYS_gettid             get thread identification (Linux-specific)
SYS_set_thread_area    set_thread_area
SYS_get_thread_area    get_thread_area
SYS_exit_group         ensure that we close BT
SYS_sys_setaltroot     deny
SYS_unused1            deny
SYS_unused2            deny
SYS_unused3            deny
*/

#if defined(AUTHORIZE_SYSCALLS)
/*
 * System call authorization functions must ensure a couple of things:
 * - First of all they may not change or write any of their arguments
 * - Second they must verify that the syscall_nr is correct (even if they assume
 *   that they are only called from one syscall number)
 *
 * These functions are called by asm-magic and include all possible
 * parameters. If you change any parameters then they will be propagated back
 * and the changed parameters will be used for the system call if it is allowed.
 */

/**
 * Prints all arguments of any registered system call (think about ptrace)
 * @return Allows the current system call
 */
static enum syscall_auth_response debug_syscall(struct thread_local_data *tld,
                                                ulong_t syscall_nr, ulong_t arg1,
                                                ulong_t arg2, ulong_t arg3,
                                                ulong_t arg4, ulong_t arg5,
                                                ulong_t *arg6,
                                                ulong_t is_sysenter,
                                                ulong_t *retval);

/**
 * Denies the current system call.
 * @return Denies the current system call and returns -1
 */
static enum syscall_auth_response deny_syscall(struct thread_local_data *tld,
                                               ulong_t syscall_nr, ulong_t arg1,
                                               ulong_t arg2, ulong_t arg3,
                                               ulong_t arg4, ulong_t arg5,
                                               ulong_t *arg6,
                                               ulong_t is_sysenter,
                                               ulong_t *retval);

/**
 * Allows the current system call.
 * @return Allows the current system call and passes control to the kernel.
 */
static enum syscall_auth_response allow_syscall(struct thread_local_data *tld,
                                                ulong_t syscall_nr, ulong_t arg1,
                                                ulong_t arg2, ulong_t arg3,
                                                ulong_t arg4, ulong_t arg5,
                                                ulong_t *arg6,
                                                ulong_t is_sysenter,
                                                ulong_t *retval);

/**
 * Catches an execve system call and checks the parameters. If the parameters
 * still contain the LD_PRELOAD of our BT then the system call is allowed.
 * @return Allows the system call if parameters are OK.
 */
static enum syscall_auth_response auth_execve(struct thread_local_data *tld,
                                              ulong_t syscall_nr, ulong_t arg1,
                                              ulong_t arg2, ulong_t arg3,
                                              ulong_t arg4, ulong_t arg5,
                                              ulong_t *arg6,
                                              ulong_t is_sysenter,
                                              ulong_t *retval);

#ifdef SYS_mmap
/**
 * Checks the parameters of an mmap and ensures that the region does not overlap
 * with any BT region. It also checks if new code is marked executable.
 * @return Allows the system call if parameters are OK.
 */
static enum syscall_auth_response auth_mmap(struct thread_local_data *tld,
                                            ulong_t syscall_nr, ulong_t arg1,
                                            ulong_t arg2, ulong_t arg3,
                                            ulong_t arg4, ulong_t arg5,
                                            ulong_t *arg6,
                                            ulong_t is_sysenter,
                                            ulong_t *retval);
#endif  // SYS_mmap

#ifdef SYS_mmap2
/**
 * Checks the parameters of an mmap2 and ensures that the region does not overlap
 * with any BT region. It also checks if new code is marked executable.
 * @return Allows the system call if parameters are OK.
 */
static enum syscall_auth_response auth_mmap2(struct thread_local_data *tld,
                                             ulong_t syscall_nr, ulong_t arg1,
                                             ulong_t arg2, ulong_t arg3,
                                             ulong_t arg4, ulong_t arg5,
                                             ulong_t *arg6,
                                             ulong_t is_sysenter,
                                             ulong_t *retval);
#endif  // SYS_mmap2

/**
 * Checks the parameters of an mprotect and ensures that the application does
 * not have access to any BT region. It also checks if new code is marked
 * executable.
 * @return Allows the system call if parameters are OK.
 */
static enum syscall_auth_response auth_mprotect(struct thread_local_data *tld,
                                                ulong_t syscall_nr, ulong_t arg1,
                                                ulong_t arg2, ulong_t arg3,
                                                ulong_t arg4, ulong_t arg5,
                                                ulong_t *arg6,
                                                ulong_t is_sysenter,
                                                ulong_t *retval);

#if defined(HANDLE_SIGNALS)
/**
 * Checks the parameters of a signal system call and verifies that the signal
 * handler will be instrumented with a trampoline if the signal is delivered.
 * @return Allows the system call if parameters are OK.
 */
static enum syscall_auth_response auth_signal(struct thread_local_data *tld,
                                              ulong_t syscall_nr, ulong_t arg1,
                                              ulong_t arg2, ulong_t arg3,
                                              ulong_t arg4, ulong_t arg5,
                                              ulong_t *arg6,
                                              ulong_t is_sysenter,
                                              ulong_t *retval);

/**
 * Initialization function for the thread-local signal handlers.
 * @param tld Pointer to thread local data. */
static void init_signal_handlers(struct thread_local_data *tld);

#if defined(SLEEP_ON_FAIL)
/**
 * Special handler for illegal signals that prints a warning message and goes
 * into a spin loop.
 */
static void failhandler();
#endif  /* SLEEP_ON_FAIL */
#endif  /* HANDLE_SIGNALS */

#if defined(HANDLE_THREADS)
static enum syscall_auth_response auth_clone(struct thread_local_data *tld,
                                             ulong_t syscall_nr, ulong_t arg1,
                                             ulong_t arg2, ulong_t arg3,
                                             ulong_t arg4, ulong_t arg5,
                                             ulong_t *arg6,
                                             ulong_t is_sysenter,
                                             ulong_t *retval);

static enum syscall_auth_response auth_exit(struct thread_local_data *tld,
                                            ulong_t syscall_nr, ulong_t arg1,
                                            ulong_t arg2, ulong_t arg3,
                                            ulong_t arg4, ulong_t arg5,
                                            ulong_t *arg6,
                                            ulong_t is_sysenter,
                                            ulong_t *retval);
#endif  /* HANDLE_THREADS */

#if defined(HANDLE_SIGNALS)

void fbt_bootstrap_thread(struct thread_local_data *tld) {
#if defined(SHARED_DATA)
  PRINT_DEBUG("Adding new thread to thread list...\n");
  /* Add thread to our list */
  fbt_mutex_lock(&tld->shared_data->threads_mutex);

  fbt_gettid(tld->tid);

  struct thread_entry *te = fbt_smalloc(tld, sizeof(struct thread_entry));
  te->next = tld->shared_data->threads;
  te->tld = tld;
  tld->shared_data->threads = te;

  fbt_mutex_unlock(&tld->shared_data->threads_mutex);
  PRINT_DEBUG("Done.\n");
#endif /* SHARED_DATA */
}

void internal_sighandler(int signal __attribute__((unused)),
												 fbt_siginfo_t *siginfo __attribute__((unused)),
												 void *ucontext __attribute__((unused))) {
  //struct thread_local_data *tld = (*(struct thread_local_data **)&siginfo->value);
}

void sighandler(int signal __attribute__((unused)),
								fbt_siginfo_t *siginfo __attribute__((unused)),
								void *ucontext __attribute__((unused))) {
  /* TODO:
     - check signal number
     - check if in translated code or not (add trampoline)
     - relocate to signal handler
     - or if in translated code: store signal and return
  */
  fllwrite(STDOUT_FILENO, "Signal received.\n");
  while (1);
}

static void init_signal_handlers(struct thread_local_data *tld) {
  long i, retval;
  for (i = 0; i < MAX_NR_SIGNALS; ++i) {
    fbt_sigaction(i, 0x0, &(tld->signals[i]), retval);
  }

#if defined(SLEEP_ON_FAIL)
  tld->signals[4].sigaction = &failhandler; /* SIGILL */
  fbt_sigaction(4, sighandler, 0x0, retval,
                "Could not install SIGILL handler (fbt_syscall.c)\n");
  tld->signals[7].sigaction = &failhandler; /* SIGBUS */
  fbt_sigaction(7, sighandler, 0x0, retval,
                "Could not install SIGBUS handler (fbt_syscall.c)\n");
  tld->signals[11].sigaction = &failhandler; /* SIGSEGV */
  fbt_sigaction(11, sighandler, 0x0, retval,
                "Could not install SIGSEGV handler (fbt_syscall.c)\n");
#endif  /* SLEEP_ON_FAIL */
}

#if defined(SLEEP_ON_FAIL)
static void failhandler() {
  fllwrite(STDOUT_FILENO, "Something bad happened. Attach a debugger NOW.\n");
  while (1);
}
#endif  /* SLEEP_ON_FAIL */

static enum syscall_auth_response auth_signal(struct thread_local_data *tld,
                                              ulong_t syscall_nr, ulong_t arg1,
                                              ulong_t arg2, ulong_t arg3,
                                              ulong_t arg4, ulong_t arg5,
                                              ulong_t *arg6,
                                              ulong_t is_sysenter,
                                              ulong_t *retval) {
#ifdef SYS_signal
  if (syscall_nr != SYS_signal) {
#endif
    if (syscall_nr != SYS_sigaction && syscall_nr != SYS_rt_sigaction) {
      fbt_suicide_str("Invalid system call number in signal auth (fbt_syscall.c).");
    }
#ifdef SYS_signal
  }
#endif

#ifdef SYS_signal
  /* arg1: signal number
     arg2: { const struct sigaction *act | sighandler_t }
   */
  if (syscall_nr == SYS_signal) {
    void *oldfunction = tld->signals[arg1].sigaction;
    tld->signals[arg1].mask = 0x0;
    tld->signals[arg1].flags = 0x0;
    tld->signals[arg1].restorer = 0x0;
    tld->signals[arg1].sigaction = (void*)arg2;
    if ((void*)arg2 == SIG_IGN || (void*)arg2 == SIG_DFL) {
      fbt_signal(arg1, arg2, *retval);
    } else {
      fbt_signal(arg1, &sighandler, *retval);
    }
    /* if we got an error in the signal syscall then we return that error.
       otherwise we return the old value of the signal handler */
    if (*retval < (ulong_t)(-(128 + 1))) {
      *retval = (ulong_t)oldfunction;
    }
    return SYSCALL_AUTH_FAKE;
  }
#endif

  if (syscall_nr == SYS_sigaction || syscall_nr == SYS_rt_sigaction) {
    *retval = 0x0;
    /* store the _old_ target for this signal */
    if (arg3 != 0x0) {
      struct fbt_sigaction *sigaction = (struct fbt_sigaction*)arg3;
      sigaction->sigaction = tld->signals[arg1].sigaction;
      sigaction->mask = tld->signals[arg1].mask;
      sigaction->flags = tld->signals[arg1].flags;
      sigaction->restorer = tld->signals[arg1].restorer;
    }
    /* interpret the _new_ sigaction struct */
    if (arg2 != 0x0) {
      struct fbt_sigaction *sigaction = (struct fbt_sigaction*)arg2;
      #if defined(DEBUG)
      PRINT_DEBUG("sigaction: %p (%d) %p\n", arg2, arg1, sigaction->sigaction);
      #endif
      tld->signals[arg1].mask = sigaction->mask;
      tld->signals[arg1].flags = sigaction->flags;
      tld->signals[arg1].restorer = sigaction->restorer;
      tld->signals[arg1].sigaction = sigaction->sigaction;

      if (syscall_nr == SYS_sigaction) {
        fbt_sigaction(arg1, &(tld->signals[arg1]), 0x0, *retval);
      } else {
        fbt_rt_sigaction(arg1, &(tld->signals[arg1]), 0x0, *retval);
      }
    }
    return SYSCALL_AUTH_FAKE;
  }

  return deny_syscall(tld, syscall_nr, arg1, arg2, arg3, arg4, arg5, arg6,
                      is_sysenter, retval);
}
#endif  /* HANDLE_SIGNALS */

#if defined(HANDLE_THREADS)
static enum syscall_auth_response auth_clone(struct thread_local_data *tld,
                                             ulong_t syscall_nr, ulong_t arg1,
                                             ulong_t arg2, ulong_t arg3,
                                             ulong_t arg4, ulong_t arg5,
                                             ulong_t *arg6 __attribute__((unused)),
                                             ulong_t is_sysenter,
                                             ulong_t *retval) {
  if (syscall_nr != SYS_clone) {
    fbt_suicide_str("Invalid system call number in clone auth (fbt_syscall.c).");
  }

  if (!(arg1 & CLONE_VM)) {
    long local_ret;
#if defined(DEBUG)
    llprintf("Syscall granted (fork through clone)\n");
#endif
#if defined(__i386__)
    __asm__ __volatile__("pushl %%ebx\n"
                         "movl %1, %%ebx\n"
                         "movl %2, %%ecx\n"
                         "movl %3, %%edx\n"
                         "movl %4, %%esi\n"
                         "movl %5, %%edi\n"
                         "movl $0x78, %%eax\n"
                         "int $0x80\n"
                         "popl %%ebx\n"
                         "movl %%eax, %0\n"
                         : "=rmi"(local_ret)
                         : "rmi"(arg1), "rmi"(arg2), "rmi"(arg3), "rmi"(arg4),
                           "rmi"(arg5)
                         : "memory", "cc", "eax", "ecx", "edx", "esi", "edi"
                         );
#elif defined(__arm__)
    // TODO(philix): defined the ARM assembly code for cloning
    fbt_suicide_str(__func__);
    local_ret = 0;
#endif
    *retval = local_ret;
#if defined(DEBUG)
    if (local_ret != 0) {
      llprintf("New process (pid: %d)\n", local_ret);
    }
#endif

    return SYSCALL_AUTH_FAKE;
  }

  /* we start a new thread */
  if ((arg1 & CLONE_VM) && !is_sysenter) {
    long local_ret;
    /* jump over that int 0x80 or sysenter instruction (both are 2bytes long) */
    void *syscall_location = (void*)(((ulong_t)tld->syscall_location)+2);

    /* initialize new BT data structures for the new thread */
    struct thread_local_data *new_threads_tld = fbt_init(NULL);

    #if defined(SHARED_DATA)
    /* Pass on shared data to new thread */
    new_threads_tld->shared_data = tld->shared_data;
    #endif

    fbt_ccache_add_entry(new_threads_tld, (void*)fbt_commit_transaction,
                         (void*)fbt_end_transaction);

#if defined(HIJACKCONTROL)
    fbt_ccache_add_entry(new_threads_tld, (void*)fbt_exit, (void*)fbt_exit);
#endif  /* HIJACKCONTROL */

    /* translate the TU if not already in tcache */
    ulong_t *childsp = (ulong_t*)(arg2 - sizeof(void*));
    struct trampoline *trampo = fbt_create_trampoline(new_threads_tld,
                                                      syscall_location, childsp,
                                                      ORIGIN_CLEAR);
    #if defined(SHARED_DATA)
    /* start thread through bootstrapping trampoline */
    new_threads_tld->ind_target = trampo;
    *childsp = (ulong_t)new_threads_tld->bootstrap_thread_trampoline;
    #else
    /* start translation in child through trampoline */
    *childsp = (ulong_t)trampo;
    #endif /* SHARED_DATA */

    /* start the new thread (execute system call) */
    /* the stack of the child is invalid after this system call,
       so better fix it! */
#if defined(__i386__)
    __asm__ __volatile__("pushl %%ebx\n"
                         "movl %2, %%ecx\n"
                         "leal -4(%%ecx), %%ecx\n"
                         "movl %%ebx, (%%ecx)\n"
                         "movl %1, %%ebx\n"
                         "movl %3, %%edx\n"
                         "movl %4, %%esi\n"
                         "movl %5, %%edi\n"
                         "movl $0x78, %%eax\n"
                         "int $0x80\n"
                         "cmpl $0x0, %%eax\n"
                         "popl %%ebx\n"
                         "jne 1f\n"
                         /* ok, we are the child, let's bail out */
                         "ret\n"
                         "1:\n"
                         "movl %%eax, %0\n"
                         : "=m"(local_ret)
                         : "m"(arg1), "m"(childsp),
                           "m"(arg3), "m"(arg4), "m"(arg5)
                         : "memory", "cc", "eax", "ecx", "edx", "esi", "edi"
                         );
#elif defined(__arm__)
    // TODO(philix): define the ARM code for thread creation
    fbt_suicide_str(__func__);
    local_ret = 0;
#endif

    /* we are the parent thread, let's return the result from the clone syscall */
    *retval = local_ret;
#if defined(DEBUG)
    llprintf("New thread (pid: %d)\n", local_ret);
    llprintf("args = [%x, %x, %x, %x, %x]\n", arg1, arg2, arg3, arg4, arg5);
#endif
    return SYSCALL_AUTH_FAKE;
  }

  fbt_suicide_str("Unhandled combination of arguments for clone.\n");
  return SYSCALL_AUTH_FAKE;

}

static enum syscall_auth_response auth_exit(struct thread_local_data *tld,
                                            ulong_t syscall_nr, ulong_t arg1,
                                            ulong_t arg2 __attribute__((unused)),
                                            ulong_t arg3 __attribute__((unused)),
                                            ulong_t arg4 __attribute__((unused)),
                                            ulong_t arg5 __attribute__((unused)),
                                            ulong_t *arg6 __attribute__((unused)),
                                            ulong_t is_sysenter
                                            __attribute__((unused)),
                                            ulong_t *retval
                                            __attribute__((unused))) {
  if ((syscall_nr != SYS_exit) && (syscall_nr != SYS_exit_group)) {
    fbt_suicide_str("Invalid system call number in exit auth (fbt_syscall.c).");
  }

  /* we are shutting down this thread -> clean up BT */
#if defined(DEBUG)
  llprintf(
    "thread/process exit (%p, retval: %d) %s\n",
    tld,
    arg1,
    (syscall_nr == SYS_exit ? "exit" : "exit_group")
  );
#endif

#if defined(SHARED_DATA)
  /* Make sure our list of threads reflects thread termination */
  fbt_mutex_lock(&tld->shared_data->threads_mutex);

  struct thread_entry *te = tld->shared_data->threads;
  struct thread_entry *prev = NULL;
  while (te != NULL && te->tld != tld) {
    prev = te;
    te = te->next;
  }

  if (te->tld == tld) {
    if (prev == NULL) {
      tld->shared_data->threads = te->next;
    } else {
      prev->next = te->next;
    }
  } else {
    PRINT_DEBUG("*** WARNING *** Couldn't find current thread in tld->shared_data->threads.\n");
  }
  fbt_mutex_unlock(&tld->shared_data->threads_mutex);
#endif

  /* we are in the context of the BT, but we might want to print some
     statistics... (otherwise end_transaction would not be needed)  */
  fbt_end_transaction();
  fbt_exit(tld);

  /* fbt_exit unmaps all memory except the last and final pages for the tld.
     we need this storage because we are currently running on this stack.
     So we need a careful trick to get rid of that last memory. We therefore
     call munmap directly in an assembler sequence. After the munmap call the
     stack is no longer valid, so we need to keep all data that we need after
     that syscall in registers. */
  /* this system call will never return, so don't bother about a clean stack */
#if defined(__i386__)
  if (syscall_nr == SYS_exit) {
    __asm__ __volatile__("movl %0, %%eax\n"
                         "movl %1, %%ebx\n"
                         "movl %2, %%ecx\n"
                         "int $0x80\n"
                         "movl %3, %%eax\n"
                         "movl %4, %%ebx\n"
                         "int $0x80\n"
                         "hlt\n"
                         : /* no return value */
                         : "i"(SYS_munmap), "m"(tld->chunk->ptr),
                         "m"(tld->chunk->size), "i"(SYS_exit), "r"(arg1)
                         : "memory", "eax", "ecx");
  } else {
    __asm__ __volatile__("movl %0, %%eax\n"
                         "movl %1, %%ebx\n"
                         "movl %2, %%ecx\n"
                         "int $0x80\n"
                         "movl %3, %%eax\n"
                         "movl %4, %%ebx\n"
                         "int $0x80\n"
                         "hlt\n"
                         : /* no return value */
                         : "i"(SYS_munmap), "m"(tld->chunk->ptr),
                         "m"(tld->chunk->size), "i"(SYS_exit_group), "r"(arg1)
                         : "memory", "eax", "ecx");
  }
#elif defined(__arm__)
  // TODO(philix): free the last bit of memory on the ARM translator
  fbt_suicide_str(__func__);
#endif

  fbt_suicide_str("Failed to exit thread/process (fbt_syscall.c)\n");
  return SYSCALL_AUTH_FAKE;
}
#endif  /* HANDLE_THREADS */

static enum syscall_auth_response __attribute__((unused))
debug_syscall(struct thread_local_data *tld __attribute__((unused)),
              ulong_t syscall_nr __attribute__((unused)),
							ulong_t arg1 __attribute__((unused)),
							ulong_t arg2 __attribute__((unused)),
							ulong_t arg3 __attribute__((unused)),
              ulong_t arg4 __attribute__((unused)),
							ulong_t arg5 __attribute__((unused)),
							ulong_t *arg6 __attribute__((unused)), ulong_t is_sysenter,
              ulong_t *retval __attribute__((unused))) {
  if (is_sysenter) {
    #if defined(DEBUG)
    llprintf("Syscall: %d (arguments: 0x%x 0x%x 0x%x 0x%x 0x%x, ebp: %p, from "
           "sysenter)\n", syscall_nr, arg1, arg2, arg3, arg4, arg5, arg6);
    #endif
  } else {
    #if defined(DEBUG)
    llprintf("Syscall: %d (arguments: 0x%x 0x%x 0x%x 0x%x 0x%x, ebp: %p, from "
           "int)\n", syscall_nr, arg1, arg2, arg3, arg4, arg5, arg6);
    #endif
  }
  return SYSCALL_AUTH_GRANTED;
}

static enum syscall_auth_response
deny_syscall(struct thread_local_data *tld __attribute__((unused)),
             ulong_t syscall_nr, ulong_t arg1, ulong_t arg2, ulong_t arg3,
             ulong_t arg4, ulong_t arg5, ulong_t *arg6, ulong_t is_sysenter,
             ulong_t *retval) {
  if (is_sysenter) {
    llprintf("Syscall: %d (arguments: 0x%x 0x%x 0x%x 0x%x 0x%x, ebp: %p, from "
           "sysenter)\n", syscall_nr, arg1, arg2, arg3, arg4, arg5, arg6);
  } else {
    llprintf("Syscall: %d (arguments: 0x%x 0x%x 0x%x 0x%x 0x%x, ebp: %p, from "
           "int)\n", syscall_nr, arg1, arg2, arg3, arg4, arg5, arg6);
  }
  fbt_suicide_str("This system call is illegal (fbt_syscall.c).\n");
  *retval = -1;
  return SYSCALL_AUTH_FAKE;
}

static enum syscall_auth_response
allow_syscall(struct thread_local_data *tld __attribute__((unused)),
              ulong_t syscall_nr __attribute__((unused)),
              ulong_t arg1 __attribute__((unused)),
              ulong_t arg2 __attribute__((unused)),
              ulong_t arg3 __attribute__((unused)),
              ulong_t arg4 __attribute__((unused)),
              ulong_t arg5 __attribute__((unused)),
              ulong_t *arg6 __attribute__((unused)),
              ulong_t is_sysenter __attribute__((unused)),
              ulong_t *retval __attribute__((unused))) {
  return SYSCALL_AUTH_GRANTED;
}

static enum syscall_auth_response
auth_execve(struct thread_local_data *tld __attribute__((unused)),
            ulong_t syscall_nr, ulong_t arg1,
            ulong_t arg2 __attribute__((unused)), ulong_t arg3,
            ulong_t arg4 __attribute__((unused)),
            ulong_t arg5  __attribute__((unused)),
            ulong_t *arg6  __attribute__((unused)),
            ulong_t is_sysenter __attribute__((unused)),
            ulong_t *retval) {
  if (syscall_nr != SYS_execve) {
    fbt_suicide_str("Invalid system call number in execve (fbt_syscall.c).");
  }
  /* TODO: for spech benchmarks, TODO: remove again! */
  return SYSCALL_AUTH_GRANTED;
  char **envp = (char**)arg3;
  int result = SYSCALL_AUTH_FAKE;
  int i = 0;
  char *env_var;
  for (i = 0; *envp != NULL; ++i, ++envp) {
    env_var = *envp;
    if (fbt_strncmp(env_var, "LD_PRELOAD=", 11) == 0) {
      /* there is an LD_PRELOAD in the environment variables array */
      if (fbt_strncmp(env_var+11, LIBNAME, LIBLEN) == 0) {
        /* LD_PRELOAD has not been modified */
        result = SYSCALL_AUTH_GRANTED;
      } else {
        result = SYSCALL_AUTH_FAKE;
      }
    }
  }
  if (result == SYSCALL_AUTH_FAKE) {
    *retval = -1;
    llprintf("command %s not executed due to modified LD_PRELOAD\n", arg1);
    PRINT_DEBUG("command %s not executed due to modified LD_PRELOAD\n", arg1);
  } else {
    PRINT_DEBUG("command %s executed with LD_PRELOAD=%s\n", arg1, (env_var+11));
  }
  return result;
}

#if defined(SYS_mmap)
static enum syscall_auth_response
auth_mmap(struct thread_local_data *tld __attribute__((unused)),
          ulong_t syscall_nr, ulong_t arg1, ulong_t arg2,
          ulong_t arg3 __attribute__((unused)),
          ulong_t arg4 __attribute__((unused)),
          ulong_t arg5 __attribute__((unused)),
          ulong_t *arg6 __attribute__((unused)),
          ulong_t is_sysenter __attribute__((unused)),
          ulong_t *retval __attribute__((unused))) {
  if (syscall_nr != SYS_mmap) {
    fbt_suicide_str("Invalid system call number in mmap (fbt_syscall.c).");
  }
#if defined(SECU_ALLOW_RUNTIME_ALLOC)
  /* TODO: secu allow runtime code alloc */
  if ((arg3 & PROT_EXEC) && (arg4 & MAP_ANONYMOUS)) {
    /* runtime allocation of executable memory without file backing */
    fbt_mmap((void*) arg1, arg2, arg3, arg4 & (~MAP_FIXED), arg5, arg6, retval);
    fbt_memprotect_add_valid((void*) retval, arg2);
    return SYSCALL_AUTH_FAKE;
  }
#endif  /* SECU_ALLOW_RUNTIME_ALLOC */

  /* TODO: add check for regions of elf files */

  /* ensure we don't remap memory structures of the BT */
  struct mem_info *mem_info = tld->chunk;
  void *startptr = (void*)arg1;
  ulong_t size = arg2;
  if (startptr != NULL) {
    while (mem_info != NULL) {
      if (OVERLAPPING_REGIONS(startptr, size, mem_info->ptr, mem_info->size)) {
        PRINT_DEBUG("Application got access to internal data and tries to " \
                    "mmap  our memory. Access rejected. Address: %p, length: " \
                    "%d\nMem_info: %p, length: %d\n", (void*)arg1, arg2,
                    mem_info->ptr, mem_info->size);
        fbt_suicide_str("Application tried to mmap internal BT data! "  \
                        "(fbt_syscall.c)\n");
      }
      mem_info = mem_info->next;
    }
  }
  return SYSCALL_AUTH_GRANTED;
}
#endif  // SYS_mmap

#if defined(SYS_mmap2)
static enum syscall_auth_response
auth_mmap2(struct thread_local_data *tld __attribute__((unused)),
           ulong_t syscall_nr, ulong_t arg1, ulong_t arg2,
           ulong_t arg3 __attribute__((unused)),
           ulong_t arg4 __attribute__((unused)),
           ulong_t arg5 __attribute__((unused)),
           ulong_t *arg6 __attribute__((unused)),
           ulong_t is_sysenter __attribute__((unused)),
           ulong_t *retval __attribute__((unused))) {
  if (syscall_nr != SYS_mmap2) {
    fbt_suicide_str("Invalid system call number in mmap (fbt_syscall.c).");
  }
#if defined(SECU_ALLOW_RUNTIME_ALLOC)
  /* TODO: secu allow runtime code alloc */
  if ((arg3 & PROT_EXEC) && (arg4 & MAP_ANONYMOUS)) {
    /* runtime allocation of executable memory without file backing */
    fbt_mmap2((void*) arg1, arg2, arg3, arg4 & (~MAP_FIXED), arg5, arg6, retval);
    fbt_memprotect_add_valid((void*) retval, arg2);
    return SYSCALL_AUTH_FAKE;
  }
#endif  /* SECU_ALLOW_RUNTIME_ALLOC */

  /* TODO: add check for regions of elf files */
  // TODO(philix): extract the common code from auth_mmap and auth_mmap2;

  /* ensure we don't remap memory structures of the BT */
  struct mem_info *mem_info = tld->chunk;
  void *startptr = (void*)arg1;
  ulong_t size = arg2;
  if (startptr != NULL) {
    while (mem_info != NULL) {
      if (OVERLAPPING_REGIONS(startptr, size, mem_info->ptr, mem_info->size)) {
        PRINT_DEBUG("Application got access to internal data and tries to " \
                    "mmap  our memory. Access rejected. Address: %p, length: " \
                    "%d\nMem_info: %p, length: %d\n", (void*)arg1, arg2,
                    mem_info->ptr, mem_info->size);
        fbt_suicide_str("Application tried to mmap internal BT data! "  \
                        "(fbt_syscall.c)\n");
      }
      mem_info = mem_info->next;
    }
  }
  return SYSCALL_AUTH_GRANTED;
}
#endif  // SYS_mmap2

static enum syscall_auth_response
auth_mprotect(struct thread_local_data *tld, ulong_t syscall_nr,
              ulong_t arg1, ulong_t arg2,
              ulong_t arg3 __attribute__((unused)),
              ulong_t arg4 __attribute__((unused)),
              ulong_t arg5 __attribute__((unused)),
              ulong_t *arg6 __attribute__((unused)),
              ulong_t is_sysenter __attribute__((unused)),
              ulong_t *retval __attribute__((unused))) {
  if (syscall_nr != SYS_mprotect) {
    fbt_suicide_str("Invalid system call number in mprotect (fbt_syscall.c).");
  }

  /* ensure we don't make memory structures of BT executable */
  struct mem_info *mem_info = tld->chunk;

  void *startptr = (void*)arg1;
  ulong_t size = arg2;
  while (mem_info != NULL) {
    if (OVERLAPPING_REGIONS(startptr, size, mem_info->ptr, mem_info->size)) {
      PRINT_DEBUG("Application got access to internal data and tries to mprotect" \
                  " our memory. Access rejected. Address: %p, length: %d\n",
                  (void*)arg1, arg2);
      fbt_suicide_str("Application tried to remap internal BT data! "   \
                      "(fbt_syscall.c)\n");
    }
    mem_info = mem_info->next;
  }

  /* TODO: add check for regions of elf files */

#if defined(SECU_ALLOW_RUNTIME_ALLOC)
  /* TODO: secu allow runtime code alloc */
    if (arg3 & PROT_EXEC) {
        fbt_memprotect_add_valid((void*) arg1, arg2);
        INFO_LLPRINTF("intercepted mprotect call that sets memory executable\n");
    }
#endif  /* SECU_ALLOW_RUNTIME_ALLOC */

    return SYSCALL_AUTH_GRANTED;
}



void fbt_init_syscalls(struct thread_local_data *tld) {
  ulong_t i;
  PRINT_DEBUG("Syscall table: %p %p\n", tld->syscall_table, debug_syscall);
  for (i = 0; i <= NR_syscalls; ++i) {
    /* allow_syscall for production, debug_syscall for testing */
    tld->syscall_table[i] = &allow_syscall;
  }
  for (; i < MAX_SYSCALLS_TABLE; ++i) {
    tld->syscall_table[i] = &deny_syscall;
  }
  /* deny a couple of system calls */
  tld->syscall_table[SYS_ptrace] = &deny_syscall;
  tld->syscall_table[SYS_sigreturn] = &deny_syscall;
  tld->syscall_table[SYS_rt_sigreturn] = &deny_syscall;
#ifdef SYS_unused1
  tld->syscall_table[SYS_unused1] = &deny_syscall;
  tld->syscall_table[SYS_unused2] = &deny_syscall;
  tld->syscall_table[SYS_unused3] = &deny_syscall;
#endif
#ifdef SYS_sys_setaltroot
  tld->syscall_table[SYS_sys_setaltroot] = &deny_syscall;
#endif

  /* special handling for special system calls */
  tld->syscall_table[SYS_execve] = &auth_execve;
#ifdef SYS_mmap
  tld->syscall_table[SYS_mmap] = &auth_mmap;
#endif
#ifdef SYS_mmap2
  tld->syscall_table[SYS_mmap2] = &auth_mmap2;
#endif
  tld->syscall_table[SYS_mprotect] = &auth_mprotect;

#if defined(HANDLE_SIGNALS)
  /* redirect system calls that change the system call handlers to our
     validation functions */
# ifdef SYS_signal
  tld->syscall_table[SYS_signal] = &auth_signal;
#endif
  tld->syscall_table[SYS_sigaction] = &auth_signal;
  tld->syscall_table[SYS_rt_sigaction] = &auth_signal;
  init_signal_handlers(tld);
#endif  /* HANDLE_SIGNALS */
#if defined(HANDLE_THREADS)
  tld->syscall_table[SYS_clone] = &auth_clone;
  tld->syscall_table[SYS_exit] = &auth_exit;
  tld->syscall_table[SYS_exit_group] = &auth_exit;
#endif  /* HANDLE_THREADS */
}

/**
 * overwrite dlclose so that libraries can never be unmapped.
 * If a library gets unmapped and a new library gets mapped to the same place then
 * we might execute stale code that is still in the code cache.
 *
 * TODO: An other option to forbidding the dlclose is to flush the code cache
 * and the hashtable.
 * TODO: handle dlsym, dlvsym, dl_iterate_phdr as well
 **/
__attribute__((visibility("default"))) int dlclose(void *handle __attribute__((unused))) {
  #if defined(DEBUG)
  llprintf("fbt_syscall.c: dlclose intercepted (handle: 0x%p)!\n", handle);
  #endif
  return 0;
}

#endif  /* AUTHORIZE_SYSCALLS */
