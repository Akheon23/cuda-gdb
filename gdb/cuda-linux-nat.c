/*
 * NVIDIA CUDA Debugger CUDA-GDB Copyright (C) 2007-2012 NVIDIA Corporation
 * Written by CUDA-GDB team at NVIDIA <cudatools@nvidia.com>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/*Warning: this isn't intended as a standalone compile module! */

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/signal.h>
#include <defs.h>
#include <time.h>
#include <objfiles.h>
#include "block.h"
#include "gdbthread.h"
#include "language.h"
#include "demangle.h"
#include "regcache.h"
#include "arch-utils.h"
#include "cuda-builtins.h"
#include "cuda-commands.h"
#include "cuda-events.h"
#include "cuda-notifications.h"
#include "cuda-options.h"
#include "cuda-tdep.h"
#include "cuda-parser.h"
#include "cuda-state.h"
#include "cuda-utils.h"
#include "gdbthread.h"
#include "valprint.h"
#include "command.h"
#include "gdbcmd.h"
#ifdef __linux__
#include "linux-nat.h"
#endif
#include "inf-child.h"

#define CUDA_NUM_CUDART_FRAME_ENTRIES 3

static struct {
  char *objfile_path;
  bool created;
  struct objfile *objfile;
} cuda_cudart_symbols;

/* Copy of the original host set of target operations. When a CUDA target op
   does not apply because we are dealing with the host code/cpu, use those
   routines instead. */
static struct target_ops host_target_ops;

cuda_exception_t cuda_exception;

/* The whole Linux siginfo structure is presented to the user, but, internally,
   only the si_signo matters. We do not save the siginfo object. Instead we
   save only the signo. Therefore any read/write to any other field of the
   siginfo object will have no effect or will return 0. */
static LONGEST
cuda_nat_xfer_siginfo (struct target_ops *ops, enum target_object object,
                       const char *annex, gdb_byte *readbuf,
                       const gdb_byte *writebuf, ULONGEST offset, LONGEST len)
{
  /* the size of siginfo is not consistent between ptrace and other parts of
     GDB. On 32-bit Linux machines, the layout might be 64 bits. It does not
     matter for CUDA because only signo is used and the rest is set to zero. We
     just allocate 8 extra bytes and bypass the issue. On 64-bit Mac, the
     difference is 24 bytes. Therefore take the max of the 2 values. */
  gdb_byte buf[sizeof (siginfo_t) + 24];
  siginfo_t *siginfo = (siginfo_t *) buf;

  gdb_assert (object == TARGET_OBJECT_SIGNAL_INFO);
  gdb_assert (readbuf || writebuf);

  if (!cuda_focus_is_device ())
    return -1;

  if (offset >= sizeof (buf))
    return -1;

  if (offset + len > sizeof (buf))
    len = sizeof (buf) - offset;

  memset (buf, 0 , sizeof buf);

  if (readbuf)
    {
      siginfo->si_signo = cuda_get_signo ();
      memcpy (readbuf, siginfo + offset, len);
    }
  else
    {
      memcpy (siginfo + offset, writebuf, len);
      cuda_set_signo (siginfo->si_signo);
    }

  return len;
}

static LONGEST
cuda_nat_xfer_partial (struct target_ops *ops,
                       enum target_object object, const char *annex,
                       gdb_byte *readbuf, const gdb_byte *writebuf,
                       ULONGEST offset, LONGEST len)
{
  LONGEST nbytes = 0;
  uint32_t dev, sm, wp, ln;

  /* If focus set on device, call the host routines directly */
  if (!cuda_focus_is_device ())
    {
      nbytes = host_target_ops.to_xfer_partial (ops, object, annex, readbuf,
                                                writebuf, offset, len);
      return nbytes;
    }

  switch (object)
  {
    /* See if this address is in pinned system memory first.  This refers to
       system memory allocations made by the inferior through the CUDA API, and
       not those made by directly using mmap(). */
    case TARGET_OBJECT_MEMORY:

      if ((readbuf  && cuda_api_read_pinned_memory  (offset, readbuf, len)) ||
          (writebuf && cuda_api_write_pinned_memory (offset, writebuf, len)))
        nbytes = len;

      break;

    /* The stack lives in local memory for ABI compilations. */
    case TARGET_OBJECT_STACK_MEMORY:

      cuda_coords_get_current_physical (&dev, &sm, &wp, &ln);
      if (readbuf)
        {
          cuda_api_read_local_memory (dev, sm, wp, ln, offset, readbuf, len);
          nbytes = len;
        }
      else if (writebuf)
        {
          cuda_api_write_local_memory (dev, sm, wp, ln, offset, writebuf, len);
          nbytes = len;
        }
      break;

    /* When stopping on the device, build a simple siginfo object */
    case TARGET_OBJECT_SIGNAL_INFO:

      nbytes = cuda_nat_xfer_siginfo (ops, object, annex, readbuf, writebuf,
                                      offset, len);
      break;
  }

  if (nbytes < len)
    nbytes = host_target_ops.to_xfer_partial (ops, object, annex, readbuf,
                                              writebuf, offset, len);

  return nbytes;
}

static void
cuda_nat_kill (struct target_ops *ops)
{
   /* XXX potential race condition here. we kill the application, and will later kill
      the device when finalizing the API. Should split the process into smaller steps:
      kill the device, then kill the app, then kill the dispatcher thread, then free
      resources in gdb (cuda_cleanup). OR do one initialize/finalize of the API per
      gdb run. */
  cuda_cleanup ();

  host_target_ops.to_kill (ops);
}

/* We don't want to issue a full mourn if we've encountered a cuda exception,
   because the host application has not actually reported that it has
   terminated yet. */
static void
cuda_nat_mourn_inferior (struct target_ops *ops)
{
  if (!cuda_exception.valid)
  {
    cuda_cleanup ();
    clear_solib ();
    host_target_ops.to_mourn_inferior (ops);
  }
}

/* This is a helper function to print a cleanly formatted assertion
   message to the ui output. This message takes the form :
   <FILE_NAME>:<LINE_NUMBER>: <KERNEL_NAME>: Assertion failed at
   block <BLOCKIDX> thread <THREADIX>
*/
static void
cuda_print_assert_message (void)
{
  cuda_coords_t c;
  struct symtab_and_line sa;
  uint64_t phys_addr, virt_addr;
  kernel_t kernel;
  uint64_t start_pc;
  struct symbol *symbol;
  const char *func_name;
  struct symtab_and_line sal;

  if (!cuda_focus_is_device ())
    return;

  cuda_coords_get_current (&c);

  virt_addr = lane_get_virtual_pc (c.dev, c.sm, c.wp, c.ln);
  kernel = warp_get_kernel (c.dev, c.sm, c.wp);
  start_pc = kernel_get_virt_code_base (kernel);
  symbol = find_pc_function ((CORE_ADDR) virt_addr);
  func_name = cuda_find_kernel_name_from_pc (start_pc, false);
  sal = find_pc_line ((CORE_ADDR)virt_addr, 0);

  if (sal.symtab && sal.line)
    {
      char *file = strrchr (sal.symtab->filename, '/');

      if (file)
        ++file;
      else
        file = sal.symtab->filename;
      ui_out_text         (uiout, "\n");
      ui_out_field_string (uiout, "filename"    , file);
      ui_out_text         (uiout, ":");
      ui_out_field_int    (uiout, "line"        , sal.line);
      ui_out_text         (uiout, ": ");
      ui_out_field_string (uiout, "kernel"      , func_name);
      ui_out_text         (uiout, ": ");
      ui_out_text         (uiout, "Assertion failed at block (");
      ui_out_field_int    (uiout, "blockidx.x"  , c.blockIdx.x);
      ui_out_text         (uiout, ",");
      ui_out_field_int    (uiout, "blockidx.y"  , c.blockIdx.y);
      ui_out_text         (uiout, ",");
      ui_out_field_int    (uiout, "blockidx.z"  , c.blockIdx.z);
      ui_out_text         (uiout, "), thread (");
      ui_out_field_int    (uiout, "threadidx.x"  , c.threadIdx.x);
      ui_out_text         (uiout, ",");
      ui_out_field_int    (uiout, "threadidx.y"  , c.threadIdx.y);
      ui_out_text         (uiout, ",");
      ui_out_field_int    (uiout, "threadidx.z"  , c.threadIdx.z);
      ui_out_text         (uiout, ")\n");
    }
  else
    {
      ui_out_text         (uiout, "\n");
      ui_out_field_string (uiout, "kernel"      , func_name);
      ui_out_text         (uiout, ": ");
      ui_out_text         (uiout, "Assertion failed at block (");
      ui_out_field_int    (uiout, "blockidx.x"  , c.blockIdx.x);
      ui_out_text         (uiout, ",");
      ui_out_field_int    (uiout, "blockidx.y"  , c.blockIdx.y);
      ui_out_text         (uiout, ",");
      ui_out_field_int    (uiout, "blockidx.z"  , c.blockIdx.z);
      ui_out_text         (uiout, "), thread (");
      ui_out_field_int    (uiout, "threadidx.x"  , c.threadIdx.x);
      ui_out_text         (uiout, ",");
      ui_out_field_int    (uiout, "threadidx.y"  , c.threadIdx.y);
      ui_out_text         (uiout, ",");
      ui_out_field_int    (uiout, "threadidx.z"  , c.threadIdx.z);
      ui_out_text         (uiout, ")\n");
    }
}

static bool sendAck = false;

/*This discusses how CUDA device exceptions are handled.  This
   includes hardware exceptions that are detected and propagated
   through the Debug API.

   Similarly, we optionally integrate CUDA Memcheck with CUDA GDB
   (set cuda memcheck on), to get memory access checking in CUDA code.

   We adopt the host semantics such that a device exception will
   terminate the host application as well. This is the simplest option
   for now. For this purpose we use the boolean cuda_exception.valid to
   track the propagation of this event.

   This is a 3-step process:

   1. cuda_wait ()

     Device exception detected.  We indicate this process has "stopped"
     (i.e. is not yet terminated) with a signal.  We suspend the
     device, and allow a user to inspect their program for the reason
     why they hit the fault.  cuda_exception.valid is set to true at this
     point.

   2. cuda_resume ()

     The user has already inspected any and all state in the
     application, and decided to resume.  On host systems, you cannot
     resume your app beyond a terminal signal (the app dies).  So,
     since in our case the app doesn't die, we need to enforce this if
     we desire the same behavior.  This is done by seeeing if
     cuda_exception.valid is set to true.

   3. cuda_wait ()

     If cuda_exception.valid is set, then we know we've killed the app due
     to an exception.  We need to indicate the process has been
     "signalled" (i.e. app has terminated) with a signal.  At this point,
     cuda_exception.valid is set back to false.  Process mourning ensues
     and the world is a better place.
*/

extern struct breakpoint *step_resume_breakpoint;

#ifdef __linux__
static void
cuda_clear_pending_sigint (pid_t pid)
{
  int status = 0, options = 0;
  ptrace (PTRACE_CONT, pid, 0, 0); /* Resume the host to consume the pending SIGINT */
  waitpid (pid, &status, options); /* Ensure we return for the right reason */
  gdb_assert (WIFSTOPPED (status) && WSTOPSIG (status) == SIGINT);
}
#endif

static int
cuda_check_pending_sigint (pid_t pid)
{
#ifdef __linux__
  sigset_t pending, blocked, ignored;

  linux_proc_pending_signals (pid, &pending, &blocked, &ignored);
  if (sigismember (&pending, SIGINT))
    {
      cuda_clear_pending_sigint (pid);
      return 1;
    }
#endif

   /* No pending SIGINT */
   return 0;
}

struct {
  bool saved;
  uint32_t print;
  uint32_t stop;
} cuda_sigtrap_info;

static void
cuda_sigtrap_set_silent (void)
{
  enum target_signal sigtrap = target_signal_from_host (SIGTRAP);

  cuda_sigtrap_info.stop = signal_stop_state (sigtrap);
  cuda_sigtrap_info.print = signal_print_state (sigtrap);
  cuda_sigtrap_info.saved = true;

  signal_stop_update (sigtrap, 0);
  signal_print_update (sigtrap, 0);
}

static void
cuda_sigtrap_restore_settings (void)
{
  enum target_signal sigtrap = target_signal_from_host (SIGTRAP);

  if (cuda_sigtrap_info.saved)
    {
      signal_stop_update (sigtrap, cuda_sigtrap_info.stop);
      signal_print_update (sigtrap, cuda_sigtrap_info.print);
      cuda_sigtrap_info.saved = false;
    }
}

/*CUDA_RESUME:

  For the meaning and interaction of ptid and sstep, read gnu-nat.c,
  line 1924.

  The actions of cuda_resume are based on 3 inputs: sstep, resume_all,
  and cuda_focus_is_device(). The actions are summarized in this
  table. 'sstep/resume dev' means single-stepping/resuming the device
  in focus if any, respectively.  'resume other dev' means resume any
  active device that is not in focus.

      device   sstep   resume  | sstep   resume   resume    resume/sstep
      focus             all    |  dev     dev    other dev      host
      ------------------------------------------------------------------
        0        0        0    |   0       0         0           1
        0        0        1    |   0       1         1           1
        0        1        0    |   0       0         0           1
        0        1        1    |   0       1         1           1
        1        0        0    |  n/a     n/a       n/a         n/a(c)
        1        0        1    |   0       1         1           1
        1        1        0    |   1       0         0           0(a)
        1        1        1    |   1       0         0           0(b)

     (a) because we fake single-stepping to GDB by not calling the
     wait() routine, there is no need to resume the host. We used to
     resume the host so that the host could capture any SIGTRAP signal
     sent during single-stepping.

     (b) 'resume_all' should force the host to resume as
     well. However, it is incompatble with the way we fake device
     single-stepping in GDB (no call to cuda/target_wait).

     (c) currently, there is no way to resume a single device, without
     resuming the rest of the world. That would lead to a deadlock.
*/
static void
cuda_nat_resume (struct target_ops *ops, ptid_t ptid, int sstep, enum target_signal ts)
{
  uint32_t dev;
  cuda_coords_t c;
  bool cuda_event_found = false;
  CUDBGEvent event;

  cuda_trace ("cuda_resume: sstep=%d", sstep);

  /* In cuda-gdb we have two types of device exceptions :
     Recoverable : CUDA_EXCEPTION_WARP_ASSERT
     Nonrecoverable : All others (e.g. CUDA_EXCEPTION_LANE_ILLEGAL_ADDRESS)

     The main difference is that a recoverable exception ensures that device
     state is consistent. Therefore, the user can request that the device
     continue execution. Currently, CUDA_EXCEPTION_WARP_ASSERT is the only
     recoverable exception.

     When a device side exception is hit, it sets cuda_exception in cuda_wait.
     In the case of a nonrecoverable exception, the cuda_resume call
     kills the host application and return early. The subsequent cuda_wait
     call cleans up the exception state.
     In the case of a recoverable exception, cuda-gdb must reset the exception
     state here and can then continue executing.
     In the case of CUDA_EXCEPTION_WARP_ASSERT, the handling of the
     exception (i.e. printing the assert message) is done as part of the
     cuda_wait call.
  */
  if (cuda_exception.valid && !cuda_exception.recoverable)
  {
    target_kill ();
    cuda_trace ("cuda_resume: exception found");
    return;
  }

  if (cuda_exception.valid && cuda_exception.recoverable)
    {
      cuda_exception.valid = false;
      cuda_exception.recoverable = false;
      cuda_exception.value = 0;
      cuda_trace ("cuda_resume: recoverable exception found\n");
    }

  /* We have now handled all the CUDA notifications. We are ready to
     handle the next batch when the world resumes. Pending CUDA
     timeout events will be ignored until next time. */
  cuda_notification_mark_consumed ();
  cuda_sigtrap_restore_settings ();

  /* Check if a notification was received while a previous event was being
     serviced. If yes, check the event queue for a pending event, and service
     the event if one is found. */
  if (cuda_notification_aliased_event ())
    {
      cuda_notification_reset_aliased_event ();
      cuda_api_get_next_event (&event);
      cuda_event_found = event.kind != CUDBG_EVENT_INVALID;

      if (cuda_event_found)
        {
          cuda_process_events (&event);
          sendAck = true;
        }
    }

  /* Acknowledge the CUDA debugger API */
  if (sendAck)
    {
      cuda_api_acknowledge_events ();
      sendAck = false;
    }

  cuda_sstep_reset (sstep);

  // sstep the device in focus?
  if (cuda_focus_is_device () && sstep)
    cuda_sstep_execute (inferior_ptid);

  // resume the device in focus?
  if (cuda_focus_is_device () && !sstep)
    device_resume (cuda_current_device ());

  // resume the devices not in focus?
  if (!sstep)
    if (!cuda_notification_pending ())
      for (dev = 0; dev < cuda_system_get_num_devices (); ++dev)
        if (!cuda_focus_is_device () || dev != cuda_current_device ())
          device_resume (dev);

  // resume the host?
  if (!cuda_focus_is_device () ||
      (cuda_focus_is_device () && !sstep))
    host_target_ops.to_resume (ops, ptid, sstep, ts);

  cuda_clock_increment ();
  cuda_trace ("cuda_resume: done");
}

/*CUDA_WAIT:

  The wait function freezes the world, update the cached information
  about the CUDA devices, and cook the wait_status.

  If we hit a SIGTRAP because of a debugger API event, qualify the
  signal as spurious, so that GDB ignores it.

  If we are single-stepping the device, we never resume the host. But
  GDB needs to believe a SIGTRAP has been received. We fake the
  target_wait_status accordingly. If we are stepping instruction
  (stepi, not step), GDB cannot guarantee that there is an actual next
  instruction (unlike stepping a source line). If the kernel dies, we
  have to recognize the case.

  Device exceptions (including memcheck violations) are presented to GDB as
  unique signals (defined in signal.[c,h]).  Everything else is presented
  as a SIGTRAP, spurious in the case of a debugger API event.
 */
static ptid_t
cuda_nat_wait (struct target_ops *ops, ptid_t ptid,
               struct target_waitstatus *ws,
               int target_options)
{
  ptid_t r;
  uint32_t dev;
  bool cuda_event_found = false;
  CUDBGEvent event;
  struct thread_info *tp;

  cuda_trace ("cuda_wait");

  if (cuda_exception.valid)
    {
      ws->kind = TARGET_WAITKIND_SIGNALLED;
      ws->value.sig = cuda_exception.value;
      cuda_exception.valid = false;
      cuda_exception.recoverable = false;
      cuda_trace ("cuda_wait: exception found");
      return inferior_ptid;
    }
  else if (cuda_sstep_is_active ())
    {
      /* Cook the ptid and wait_status if single-stepping a CUDA device. */
      cuda_trace ("cuda_wait: single-stepping");
      r = cuda_sstep_ptid ();

      /* When stepping the device, the host process remains suspended.
         So, if the user issued a Ctrl-C, we wouldn't detect it since
         we never actually check its wait status.  We must explicitly
         check for a pending SIGINT here. */
      if (cuda_check_pending_sigint (PIDGET (r)))
        {
          ws->kind = TARGET_WAITKIND_STOPPED;
          ws->value.sig = TARGET_SIGNAL_INT;
          cuda_set_signo (TARGET_SIGNAL_INT);
        }
      else
        {
          ws->kind = TARGET_WAITKIND_STOPPED;
          ws->value.sig = TARGET_SIGNAL_TRAP;
          cuda_set_signo (TARGET_SIGNAL_TRAP);

          /* If we single stepped the last warp on the device, then the
             launch has completed.  However, we do not see the event for
             kernel termination until we resume the application.  We must
             explicitly handle this here by indicating the kernel has
             terminated and switching to the remaining host thread. */
          if (cuda_sstep_kernel_has_terminated ())
            {
              cuda_system_update_kernels ();
              cuda_coords_invalidate_current ();
              switch_to_thread (r);
              tp = inferior_thread ();
              tp->step_range_end = 1;
              return r;
            }
        }
    }
  else {
    cuda_trace ("cuda_wait: host_wait\n");
    cuda_coords_invalidate_current ();
    r = host_target_ops.to_wait (ops, ptid, ws, target_options);
  }

  cuda_initialize_target ();

  /* Suspend all the CUDA devices. */
  cuda_trace ("cuda_wait: suspend devices");
  for (dev = 0; dev < cuda_system_get_num_devices (); ++dev)
    device_suspend (dev);

  cuda_notification_analyze (r, ws);
  if (cuda_notification_received ())
    {
      /* Check if there is any CUDA event to be processed */
      cuda_api_get_next_event (&event);
      cuda_event_found = event.kind != CUDBG_EVENT_INVALID;
    }

  /* Handle all the CUDA events immediately.  In particular, for
     GPU events that may happen without prior notification (GPU
     grid launches, for example), API events will be packed
     alongside of them, so we need to process the API event first. */
  if (cuda_event_found)
    {
      cuda_process_events (&event);
      sendAck = true;
    }

  /* Update the info about the kernels */
  cuda_system_update_kernels ();

  /* Decide which thread/kernel to switch focus to. */
  if (cuda_exception_hit_p (&cuda_exception))
    {
      cuda_trace ("cuda_wait: stopped because of an exception");
      ws->kind = TARGET_WAITKIND_STOPPED;
      ws->value.sig = cuda_exception.value;
      cuda_set_signo (cuda_exception.value);
      cuda_coords_update_current (false, true);
      if (cuda_exception.value == TARGET_SIGNAL_CUDA_WARP_ASSERT)
        cuda_print_assert_message ();
    }
  else if (cuda_sstep_is_active ())
    {
      cuda_trace ("cuda_wait: stopped because we are single-stepping");
      cuda_coords_update_current (false, false);
    }
  else if (cuda_breakpoint_hit_p (cuda_clock ()))
    {
      cuda_trace ("cuda_wait: stopped because of a breakpoint");
      cuda_set_signo (TARGET_SIGNAL_TRAP);
      cuda_coords_update_current (true, false);
    }
  else if (cuda_system_is_broken (cuda_clock ()))
    {
      cuda_trace ("cuda_wait: stopped because there are broken warps (induced trap?)");
      cuda_coords_update_current (false, false);
    }
  else if (cuda_event_found)
    {
      cuda_trace ("cuda_wait: stopped because of a CUDA event");
      cuda_sigtrap_set_silent ();
      cuda_coords_update_current (false, false);
    }
  else if (ws->value.sig == TARGET_SIGNAL_INT)
    {
      /* CTRL-C was hit. Preferably switch focus to a device if possible */
      cuda_trace ("cuda_wait: stopped because a SIGINT was received.");
      cuda_set_signo (TARGET_SIGNAL_INT);
      cuda_coords_update_current (false, false);
    }
  else if (cuda_notification_received ())
    {
      /* No reason found when actual reason was consumed in a previous iteration (timeout,...) */
      cuda_trace ("cuda_wait: stopped for no visible CUDA reason.");
      cuda_set_signo (TARGET_SIGNAL_TRAP); /* Dummy signal. We stopped after all. */
      cuda_coords_invalidate_current ();
    }
  else
    {
      cuda_trace ("cuda_wait: stopped for a non-CUDA reason.");
      cuda_set_signo (TARGET_SIGNAL_TRAP);
      cuda_coords_invalidate_current ();
    }

  /* Switch focus and update related data */
  cuda_update_convenience_variables ();
  if (cuda_focus_is_device ())
    /* Must be last, once focus and elf images have been updated */
    switch_to_cuda_thread (NULL);

  cuda_trace ("cuda_wait: done");
  return r;
}

static void
cuda_nat_fetch_registers (struct target_ops *ops,
                          struct regcache *regcache,
                          int regno)
{
  uint64_t val;
  cuda_coords_t c;
  struct gdbarch *gdbarch = get_regcache_arch (regcache);
  uint32_t pc_regnum = gdbarch_pc_regnum (gdbarch);
  int num_regs = gdbarch_num_regs (gdbarch);

  /* delegate to the host routines when not on the device */
  if (!cuda_focus_is_device ())
    {
      host_target_ops.to_fetch_registers (ops, regcache, regno);
      return;
    }

  cuda_coords_get_current (&c);

  /* if all the registers are wanted, then we need the host registers and the
     device PC */
  if (regno == -1)
    {
      host_target_ops.to_fetch_registers (ops, regcache, regno);
      val = lane_get_virtual_pc (c.dev, c.sm, c.wp, c.ln);
      regcache_raw_supply (regcache, pc_regnum, &val);
      return;
    }

  /* get the PC */
  if (regno == pc_regnum )
    {
      val = lane_get_virtual_pc (c.dev, c.sm, c.wp, c.ln);
      regcache_raw_supply (regcache, pc_regnum, &val);
      return;
    }

  /* raw register */
  val = lane_get_register (c.dev, c.sm, c.wp, c.ln, regno);
  regcache_raw_supply (regcache, regno, &val);
}

static void
cuda_nat_store_registers (struct target_ops *ops,
                          struct regcache *regcache,
                          int regno)
{
  uint64_t val;
  cuda_coords_t c;
  struct gdbarch *gdbarch = get_regcache_arch (regcache);
  uint32_t pc_regnum = gdbarch_pc_regnum (gdbarch);
  int num_regs = gdbarch_num_regs (gdbarch);

  gdb_assert (regno >= 0 && regno < num_regs);

  if (!cuda_focus_is_device ())
    {
      host_target_ops.to_store_registers (ops, regcache, regno);
      return;
    }

  if (regno == pc_regnum)
    error (_("The PC of CUDA thread is not writable"));

  cuda_coords_get_current (&c);
  regcache_raw_collect (regcache, regno, &val);
  cuda_api_write_register (c.dev, c.sm, c.wp, c.ln, regno, val);
}

static int
cuda_nat_insert_breakpoint (struct gdbarch *gdbarch, struct bp_target_info *bp_tgt)
{
  uint32_t dev;
  kernels_t kernels;
  bool is_cuda_addr;
  bool inserted;

  cuda_api_is_device_code_address (bp_tgt->placed_address, &is_cuda_addr);

  if (is_cuda_addr)
    {
      /* Insert the breakpoint on whatever device accepts it (valid address). */
      inserted = false;
      for (dev = 0; dev < cuda_system_get_num_devices (); ++dev)
        {
          if (!device_is_any_context_present (dev))
            continue;

          /* If we haven't received a launch notification, don't bother
             inserting breakpoints.  Let the caller think this operation
             was successful, as this is not an error. */
          kernels = device_get_kernels (dev);
          if (kernels_get_num_kernels (kernels) == 0)
            {
              inserted = true;
              continue;
            }

          inserted |= cuda_api_set_breakpoint (dev, bp_tgt->placed_address);
        }
      return !inserted;
    }
  else
    return host_target_ops.to_insert_breakpoint (gdbarch, bp_tgt);
}

static int
cuda_nat_remove_breakpoint (struct gdbarch *gdbarch, struct bp_target_info *bp_tgt)
{
  uint32_t dev;
  kernels_t kernels;
  CORE_ADDR cuda_addr;
  bool is_cuda_addr;
  bool removed;

  cuda_api_is_device_code_address (bp_tgt->placed_address, &is_cuda_addr);

  if (is_cuda_addr)
    {
      /* Removed the breakpoint on whatever device accepts it (valid address). */
      removed = false;
      for (dev = 0; dev < cuda_system_get_num_devices (); ++dev)
        {
          if (!device_is_any_context_present (dev))
            continue;

          /* If we haven't received a launch notification, don't bother
             removing breakpoints.  Let the caller think this operation
             was successful, as this is not an error. */
          kernels = device_get_kernels (dev);
          if (kernels_get_num_kernels (kernels) == 0)
            {
              removed = true;
              continue;
            }

          removed |= cuda_api_unset_breakpoint (dev, bp_tgt->placed_address);
        }
      return !removed;
    }
  else
    return host_target_ops.to_remove_breakpoint (gdbarch, bp_tgt);
}

static struct gdbarch *
cuda_nat_thread_architecture (struct target_ops *ops, ptid_t ptid)
{
  if (cuda_focus_is_device ())
    return cuda_get_gdbarch ();
  else
    return target_gdbarch;
}

/*Comprehensive container for everything that may need to be restored
   when switching focus temporarily. Not all the fields are used at
   the present time. */
static struct {
  bool valid;
  ptid_t ptid;
  cuda_coords_t coords;
} previous_focus;

void
cuda_save_focus (void)
{
  previous_focus.valid = true;
  previous_focus.ptid = inferior_ptid;
  previous_focus.coords = CUDA_INVALID_COORDS;
  cuda_coords_get_current (&previous_focus.coords);
}

void
cuda_restore_focus (void)
{
  gdb_assert (previous_focus.valid);
  if (previous_focus.coords.valid)
    switch_to_cuda_thread  (&previous_focus.coords);
  else if (TIDGET (previous_focus.ptid))
    switch_to_thread (previous_focus.ptid);
  previous_focus.valid = false;
}

void
switch_to_cuda_thread (cuda_coords_t *coords)
{
  cuda_coords_t c;

  gdb_assert (coords || cuda_focus_is_device ());

  if (coords)
    cuda_coords_set_current (coords);
  cuda_coords_get_current (&c);

  cuda_update_elf_images ();
  cuda_update_cudart_symbols ();
  reinit_frame_cache ();
  registers_changed ();
  stop_pc = lane_get_virtual_pc (c.dev, c.sm, c.wp, c.ln);
}

void
cuda_update_cudart_symbols (void)
{
  int fd;
  struct stat s;
  char tmp_sym_file[CUDA_GDB_TMP_BUF_SIZE];

  /* If not done yet, create a CUDA runtime symbols file */
  if (!cuda_cudart_symbols.created)
    {
      snprintf (tmp_sym_file, sizeof (tmp_sym_file),
                "%s/builtins.XXXXXX", cuda_gdb_session_get_dir ());

      if (!(fd = mkstemp (tmp_sym_file)))
        error (_("Failed to create the cudart symbol file."));

      if (!write (fd, cuda_builtins, sizeof (cuda_builtins)))
        error (_("Failed to write the cudart symbole file."));

      close (fd);

      if (stat (tmp_sym_file, &s))
        error (_("Failed to stat the cudart symbol file."));

      if (s.st_size != sizeof (cuda_builtins))
        error (_("The cudart symbol file size is incorrect."));

      cuda_cudart_symbols.created = true;
      cuda_cudart_symbols.objfile_path = xmalloc (strlen (tmp_sym_file) + 1);
      strncpy (cuda_cudart_symbols.objfile_path, tmp_sym_file,
               strlen (tmp_sym_file) + 1);
    }

  /* Load/unload the CUDA runtime symbols only when necessary */
  if (cuda_focus_is_device () && !cuda_cudart_symbols.objfile)
    {
      cuda_cudart_symbols.objfile =
        symbol_file_add (cuda_cudart_symbols.objfile_path,
                         SYMFILE_DEFER_BP_RESET, NULL, 0);
      if (!cuda_cudart_symbols.objfile)
        error (_("Failed to add cudart symbols."));
    }
  else if (!cuda_focus_is_device () && cuda_cudart_symbols.objfile)
    {
      free_objfile (cuda_cudart_symbols.objfile);
      cuda_cudart_symbols.objfile = NULL;
    }
}

void
cuda_cleanup_cudart_symbols (void)
{
  struct stat s;

  /* Unload any loaded CUDA runtime symbols */
  if (cuda_cudart_symbols.objfile)
    {
      free_objfile (cuda_cudart_symbols.objfile);
      cuda_cudart_symbols.objfile = NULL;
    }

  /* Delete the CUDA runtime symbols file */
  if (cuda_cudart_symbols.objfile_path &&
      !stat (cuda_cudart_symbols.objfile_path, &s))
    {
      if (unlink (cuda_cudart_symbols.objfile_path))
        error (_("Failed to remove cudart symbol file!"));

      cuda_cudart_symbols.created= false;
      xfree (cuda_cudart_symbols.objfile_path);
      cuda_cudart_symbols.objfile_path = NULL;
    }
}

#ifndef __linux__
/* CUDA - cuda-gdb wrapper */
/* Smuggle the DYLD_* environement variables like GDB 6.3.5 used to do. Because
   we cuda-gdb must be part of the procmod group, those variables are not
   passed, for security reasons. Instead we pass them in a GDB_DYLD_*
   equivalent and restore them when launching the inferior. This code was taken
   from GDB 6.3.5, the Apple editon. */ 

struct dyld_smuggle_pairs {
  const char *real_name;
  const char *smuggled_name;
};

static void
smuggle_dyld_settings (struct gdb_environ *e)
{
  int i;
  struct dyld_smuggle_pairs env_names[] = { 
       {"DYLD_FRAMEWORK_PATH",          "GDB_DYLD_FRAMEWORK_PATH"},
       {"DYLD_FALLBACK_FRAMEWORK_PATH", "GDB_DYLD_FALLBACK_FRAMEWORK_PATH"},
       {"DYLD_LIBRARY_PATH",            "GDB_DYLD_LIBRARY_PATH"},
       {"DYLD_FALLBACK_LIBRARY_PATH",   "GDB_DYLD_FALLBACK_LIBRARY_PATH"},
       {"DYLD_ROOT_PATH",               "GDB_DYLD_ROOT_PATH"},
       {"DYLD_PATHS_ROOT",              "GDB_DYLD_PATHS_ROOT"},
       {"DYLD_IMAGE_SUFFIX",            "GDB_DYLD_IMAGE_SUFFIX"},
       {"DYLD_INSERT_LIBRARIES",        "GDB_DYLD_INSERT_LIBRARIES"},
       { NULL,                          NULL } };

  for (i = 0; env_names[i].real_name != NULL; i++)
    {
      const char *real_val     = get_in_environ (e, env_names[i].real_name);
      const char *smuggled_val = get_in_environ (e, env_names[i].smuggled_name);

      if (real_val == NULL && smuggled_val == NULL)
        continue;

      if (smuggled_val == NULL)
        continue;

      /* Is the value of the DYLD_* env var truncated to ""? */
      if (real_val != NULL && real_val[0] != '\0')
        continue;

      /* real_val has a value and it looks legitimate - don't overwrite it
         with the smuggled version.  */
      if (real_val != NULL)
        continue;

      set_in_environ (e, env_names[i].real_name, smuggled_val);
    }
}
#endif

void
cuda_set_environment (struct gdb_environ *environ)
{
  /* CUDA_MEMCHECK */
  if (cuda_options_memcheck ())
      set_in_environ (environ, "CUDA_MEMCHECK", "1");
  else
      unset_in_environ (environ, "CUDA_MEMCHECK");

  /* CUDA_LAUNCH_BLOCKING */
  if (cuda_options_launch_blocking ())
    set_in_environ (environ, "CUDA_LAUNCH_BLOCKING", "1");
  else
    unset_in_environ (environ, "CUDA_LAUNCH_BLOCKING");

#ifndef __linux__
  /* CUDA - cuda-gdb wrapper */
  smuggle_dyld_settings (environ);
#endif
}

/* The host target is overriden by calling when calling cuda_nat_add_target().
   The original is saved into host_target_ops because the cuda_nat target_ops
   will call the host functions when dealing with host requests */
void
cuda_nat_add_target (struct target_ops *t)
{
  static char shortname[128];
  static char longname[128];
  static char doc[256];

  if (!cuda_debugging_enabled)
    return;

  /* Save the original set of target operations */
  host_target_ops = *t;

  /* Build the meta-data strings without using malloc */
  strncpy (shortname, t->to_shortname, sizeof (shortname) - 1);
  strncat (shortname, " + cuda", sizeof (shortname) - 1 - strlen (shortname));
  strncpy (longname, t->to_longname, sizeof (longname) - 1);
  strncat (longname, " + CUDA support", sizeof (longname) - 1 - strlen (longname));
  strncpy (doc, t->to_doc, sizeof (doc) - 1);
  strncat (doc, " with CUDA support", sizeof (doc) - 1 - strlen (doc));

  /* Override what we need to */
  t->to_shortname             = shortname;
  t->to_longname              = longname;
  t->to_doc                   = doc;
  t->to_kill                  = cuda_nat_kill;
  t->to_mourn_inferior        = cuda_nat_mourn_inferior;
  t->to_resume                = cuda_nat_resume;
  t->to_wait                  = cuda_nat_wait;
  t->to_fetch_registers       = cuda_nat_fetch_registers;
  t->to_store_registers       = cuda_nat_store_registers;
  t->to_insert_breakpoint     = cuda_nat_insert_breakpoint;
  t->to_remove_breakpoint     = cuda_nat_remove_breakpoint;
  t->to_xfer_partial          = cuda_nat_xfer_partial;
  t->to_thread_architecture   = cuda_nat_thread_architecture;
}

bool cuda_debugging_enabled = false;

void
_initialize_cuda_nat (void)
{
  struct target_ops *t = NULL;

  /* Check the required CUDA debugger files are present */
  if (cuda_api_get_api ())
    {
      warning ("CUDA support disabled: could not obtain the CUDA debugger API\n");
      cuda_debugging_enabled = false;
      return;
    }

  /* Initialize the CUDA modules */
  cuda_utils_initialize ();
  cuda_commands_initialize ();
  cuda_options_initialize ();
  cuda_notification_initialize ();

  /* Initialize the cleanup routines */
  make_final_cleanup (cuda_final_cleanup, NULL);

  cuda_debugging_enabled = true;
}
