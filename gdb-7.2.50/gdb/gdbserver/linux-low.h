/* Internal interfaces for the GNU/Linux specific target code for gdbserver.
   Copyright (C) 2002, 2004, 2005, 2007, 2008, 2009, 2010
   Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#ifdef HAVE_THREAD_DB_H
#include <thread_db.h>
#endif

#include "gdb_proc_service.h"

#ifdef HAVE_LINUX_REGSETS
typedef void (*regset_fill_func) (struct regcache *, void *);
typedef void (*regset_store_func) (struct regcache *, const void *);
enum regset_type {
  GENERAL_REGS,
  FP_REGS,
  EXTENDED_REGS,
};

struct regset_info
{
  int get_request, set_request;
  /* If NT_TYPE isn't 0, it will be passed to ptrace as the 3rd
     argument and the 4th argument should be "const struct iovec *".  */
  int nt_type;
  int size;
  enum regset_type type;
  regset_fill_func fill_function;
  regset_store_func store_function;
};
extern struct regset_info target_regsets[];
#endif

struct siginfo;

struct process_info_private
{
  /* Arch-specific additions.  */
  struct arch_process_info *arch_private;

  /* libthread_db-specific additions.  Not NULL if this process has loaded
     thread_db, and it is active.  */
  struct thread_db *thread_db;
};

struct lwp_info;

struct linux_target_ops
{
  /* Architecture-specific setup.  */
  void (*arch_setup) (void);

  int num_regs;
  int *regmap;
  int (*cannot_fetch_register) (int);

  /* Returns 0 if we can store the register, 1 if we can not
     store the register, and 2 if failure to store the register
     is acceptable.  */
  int (*cannot_store_register) (int);
  CORE_ADDR (*get_pc) (struct regcache *regcache);
  void (*set_pc) (struct regcache *regcache, CORE_ADDR newpc);
  const unsigned char *breakpoint;
  int breakpoint_len;
  CORE_ADDR (*breakpoint_reinsert_addr) (void);

  int decr_pc_after_break;
  int (*breakpoint_at) (CORE_ADDR pc);

  /* Breakpoint and watchpoint related functions.  See target.h for
     comments.  */
  int (*insert_point) (char type, CORE_ADDR addr, int len);
  int (*remove_point) (char type, CORE_ADDR addr, int len);
  int (*stopped_by_watchpoint) (void);
  CORE_ADDR (*stopped_data_address) (void);

  /* Hooks to reformat register data for PEEKUSR/POKEUSR (in particular
     for registers smaller than an xfer unit).  */
  void (*collect_ptrace_register) (struct regcache *regcache,
				   int regno, char *buf);
  void (*supply_ptrace_register) (struct regcache *regcache,
				  int regno, const char *buf);

  /* Hook to convert from target format to ptrace format and back.
     Returns true if any conversion was done; false otherwise.
     If DIRECTION is 1, then copy from INF to NATIVE.
     If DIRECTION is 0, copy from NATIVE to INF.  */
  int (*siginfo_fixup) (struct siginfo *native, void *inf, int direction);

  /* Hook to call when a new process is created or attached to.
     If extra per-process architecture-specific data is needed,
     allocate it here.  */
  struct arch_process_info * (*new_process) (void);

  /* Hook to call when a new thread is detected.
     If extra per-thread architecture-specific data is needed,
     allocate it here.  */
  struct arch_lwp_info * (*new_thread) (void);

  /* Hook to call prior to resuming a thread.  */
  void (*prepare_to_resume) (struct lwp_info *);

  /* Hook to support target specific qSupported.  */
  void (*process_qsupported) (const char *);

  /* Returns true if the low target supports tracepoints.  */
  int (*supports_tracepoints) (void);

  /* Fill ADDRP with the thread area address of LWPID.  Returns 0 on
     success, -1 on failure.  */
  int (*get_thread_area) (int lwpid, CORE_ADDR *addrp);

  /* Install a fast tracepoint jump pad.  See target.h for
     comments.  */
  int (*install_fast_tracepoint_jump_pad) (CORE_ADDR tpoint, CORE_ADDR tpaddr,
					   CORE_ADDR collector,
					   CORE_ADDR lockaddr,
					   ULONGEST orig_size,
					   CORE_ADDR *jump_entry,
					   unsigned char *jjump_pad_insn,
					   ULONGEST *jjump_pad_insn_size,
					   CORE_ADDR *adjusted_insn_addr,
					   CORE_ADDR *adjusted_insn_addr_end);

  /* Return the bytecode operations vector for the current inferior.
     Returns NULL if bytecode compilation is not supported.  */
  struct emit_ops *(*emit_ops) (void);
};

extern struct linux_target_ops the_low_target;

#define ptid_of(proc) ((proc)->head.id)
#define pid_of(proc) ptid_get_pid ((proc)->head.id)
#define lwpid_of(proc) ptid_get_lwp ((proc)->head.id)

#define get_lwp(inf) ((struct lwp_info *)(inf))
#define get_thread_lwp(thr) (get_lwp (inferior_target_data (thr)))
#define get_lwp_thread(proc) ((struct thread_info *)			\
			      find_inferior_id (&all_threads,		\
						get_lwp (proc)->head.id))

struct lwp_info
{
  struct inferior_list_entry head;

  /* If this flag is set, the next SIGSTOP will be ignored (the
     process will be immediately resumed).  This means that either we
     sent the SIGSTOP to it ourselves and got some other pending event
     (so the SIGSTOP is still pending), or that we stopped the
     inferior implicitly via PTRACE_ATTACH and have not waited for it
     yet.  */
  int stop_expected;

  /* If this flag is set, we've sent this LWP a SIGINT.  If it is set
     to -1, the SIGINT will be ignored (the process will be
     immediately resumed).  If it is set to 1, it will be reported to
     GDB.  */
  int sigint_expected;

  /* When this is true, we shall not try to resume this thread, even
     if last_resume_kind isn't resume_stop.  */
  int suspended;

  /* If this flag is set, the lwp is known to be stopped right now (stop
     event already received in a wait()).  */
  int stopped;

  /* If this flag is set, the lwp is known to be dead already (exit
     event already received in a wait(), and is cached in
     status_pending).  */
  int dead;

  /* When stopped is set, the last wait status recorded for this lwp.  */
  int last_status;

  /* When stopped is set, this is where the lwp stopped, with
     decr_pc_after_break already accounted for.  */
  CORE_ADDR stop_pc;

  /* If this flag is set, STATUS_PENDING is a waitstatus that has not yet
     been reported.  */
  int status_pending_p;
  int status_pending;

  /* STOPPED_BY_WATCHPOINT is non-zero if this LWP stopped with a data
     watchpoint trap.  */
  int stopped_by_watchpoint;

  /* On architectures where it is possible to know the data address of
     a triggered watchpoint, STOPPED_DATA_ADDRESS is non-zero, and
     contains such data address.  Only valid if STOPPED_BY_WATCHPOINT
     is true.  */
  CORE_ADDR stopped_data_address;

  /* If this is non-zero, it is a breakpoint to be reinserted at our next
     stop (SIGTRAP stops only).  */
  CORE_ADDR bp_reinsert;

  /* If this flag is set, the last continue operation at the ptrace
     level on this process was a single-step.  */
  int stepping;

  /* If this flag is set, we need to set the event request flags the
     next time we see this LWP stop.  */
  int must_set_ptrace_flags;

  /* If this is non-zero, it points to a chain of signals which need to
     be delivered to this process.  */
  struct pending_signals *pending_signals;

  /* A link used when resuming.  It is initialized from the resume request,
     and then processed and cleared in linux_resume_one_lwp.  */
  struct thread_resume *resume;

  /* True if it is known that this lwp is presently collecting a fast
     tracepoint (it is in the jump pad or in some code that will
     return to the jump pad.  Normally, we won't care about this, but
     we will if a signal arrives to this lwp while it is
     collecting.  */
  int collecting_fast_tracepoint;

  /* If this is non-zero, it points to a chain of signals which need
     to be reported to GDB.  These were deferred because the thread
     was doing a fast tracepoint collect when they arrived.  */
  struct pending_signals *pending_signals_to_report;

  /* When collecting_fast_tracepoint is first found to be 1, we insert
     a exit-jump-pad-quickly breakpoint.  This is it.  */
  struct breakpoint *exit_jump_pad_bkpt;

  /* True if the LWP was seen stop at an internal breakpoint and needs
     stepping over later when it is resumed.  */
  int need_step_over;

  int thread_known;
#ifdef HAVE_THREAD_DB_H
  /* The thread handle, used for e.g. TLS access.  Only valid if
     THREAD_KNOWN is set.  */
  td_thrhandle_t th;
#endif

  /* Arch-specific additions.  */
  struct arch_lwp_info *arch_private;
};

extern struct inferior_list all_lwps;

char *linux_child_pid_to_exec_file (int pid);
int elf_64_file_p (const char *file);

void linux_attach_lwp (unsigned long pid);
struct lwp_info *find_lwp_pid (ptid_t ptid);
void linux_stop_lwp (struct lwp_info *lwp);

/* From thread-db.c  */
int thread_db_init (int use_events);
void thread_db_detach (struct process_info *);
void thread_db_mourn (struct process_info *);
int thread_db_handle_monitor_command (char *);
int thread_db_get_tls_address (struct thread_info *thread, CORE_ADDR offset,
			       CORE_ADDR load_module, CORE_ADDR *address);
int thread_db_look_up_one_symbol (const char *name, CORE_ADDR *addrp);
