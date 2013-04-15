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


/*-------------------------------------- Notifications -------------------------------------
 *
 * A notification is send by the CUDA debugger API (the producer or sender) and
 * handled by GDB (the consumer or recipient). Three booleans are used to mark
 * the current of CUDA notifications: pending_send, sent, and received.
 *
 * From the producer's point of view, the notification framework can be in 3
 * states only: ready, pending, and sent. When ready, there has been
 * notification. When pending, a notification was tentatively sent but got
 * postponed because the notification mechanism was 'blocked'. When sent, a
 * notification was sent as a SIGTRAP signal. Those 3 producer states are
 * implemeted as:
 *
 *         ready   == !sent && !pending
 *         pending == !sent &&  pending
 *         sent    ==  sent && !pending
 *        (illegal ==  sent &&  pending)
 *
 * From the consumer's point of view, the notification framework can be in 3
 * states as well: none, received, and pending. When none, there is no
 * notification to process. When received, a notification is ready to be
 * processed associated with host thread GDB woke up upon and the SIGTRAP signal
 * that was sent has been consumed. When pending, a notification has been sent
 * but not to the host thread GDB woke up upon, and the SIGTRAP signal that was
 * sent has not been consumed yet. Those 3 consumer states are implemented as:
 *
 *          none     == !sent && !received
 *          received ==  sent &&  received
 *          pending  ==  sent && !received
 *         (illegal) == !sent &&  received)
 *
 * Two extra booleans are used: 'initialized' to remember when
 * cuda_notification_info has already been initialized, and 'blocked'. When
 * 'blocked', a notification cannot be sent, and will be marked as (producer)
 * pending if no notification has been sent yet. The notification will be then
 * sent later, when notifications become unblocked, and the notification will go
 * from (producer) pending state to (producer) sent state.
 * Additionally, if a notification is received before a previous event has been
 * serviced, it is marked as an aliased_event, and an attempt is made to service
 * it before the inferior is resumed. No new SIGTRAP is sent for an aliased_event.
 */

#include <ctype.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "defs.h"
#include "cuda-options.h"
#include "cuda-tdep.h"
#include "gdb_assert.h"
#include "gdbthread.h"
#include "inferior.h"

#include "cuda-notifications.h"

static struct {
  bool initialized;       /* True if the mutex is initialized */
  bool blocked;           /* When blocked, SIGTRAPs will be marked pending and handled later. */
  bool pending_send;      /* True if a SIGTRAP was received while blocked was true. */
  bool aliased_event;     /* True if a SIGTRAP was received while a previous event was being processed. */
  bool sent;              /* If already sent, do not send duplicates. */
  bool received;          /* True if the SIGTRAP has been received. */
  uint32_t tid;           /* The thread id of the thread to which the SIGTRAP was sent to. */
  pthread_mutex_t mutex;  /* Mutex for the cuda_notification_* functions */
  CUDBGEventCallbackData pending_send_data;
} cuda_notification_info;


void
cuda_notification_trace (char *fmt, ...)
{
  va_list ap;

  if (cuda_options_debug_notifications ())
    {
      va_start (ap, fmt);
      fprintf (stderr, "[CUDAGDB] notifications -- ");
      vfprintf (stderr, fmt, ap);
      fprintf (stderr, "\n");
      fflush (stderr);
    }
}

void
cuda_notification_initialize (void)
{
  memset (&cuda_notification_info, 0, sizeof cuda_notification_info);
  pthread_mutex_init (&cuda_notification_info.mutex, NULL);
  cuda_notification_info.initialized = true;
}

void
cuda_notification_reset (void)
{
  gdb_assert (cuda_notification_info.initialized);
  cuda_notification_info.blocked      = false;
  cuda_notification_info.pending_send = false;
  cuda_notification_info.sent         = false;
  cuda_notification_info.received     = false;
  cuda_notification_info.tid          = false;
}

static void
cuda_notification_acquire_lock (void)
{
  gdb_assert (cuda_notification_info.initialized);
  pthread_mutex_lock (&cuda_notification_info.mutex);
}

static void
cuda_notification_release_lock (void)
{
  gdb_assert (cuda_notification_info.initialized);
  pthread_mutex_unlock (&cuda_notification_info.mutex);
}

static int
cuda_notification_notify_thread (int tid)
{
#ifdef HAVE_TKILL_SYSCALL
  return syscall (SYS_tkill, tid , SIGTRAP);
#else
  /* CUDA - MAC OS X specific */
  return kill (tid, SIGTRAP);
#endif

  return 1;
}

static int
cuda_notification_notify_first_valid_thread (struct thread_info *tp, void *data)
{
  int ret, tid;

  tid = cuda_gdb_get_tid (tp->ptid);
  ret = cuda_notification_notify_thread (tid);

  return ret == 0;
}

static void
cuda_notification_send (CUDBGEventCallbackData *data)
{
  struct thread_info *tp = NULL;
  int err = 1;

  if (cuda_platform_supports_tid () && data && data->tid)
    {
      // use the host thread id if given to us
      err = cuda_notification_notify_thread (data->tid);
      cuda_notification_trace ("sent specifically to the given host thread: tid %d -> %s",
                                data->tid, err ? "FAILED" : "success");
      if (!err)
        {
          cuda_notification_info.tid = data->tid;
          cuda_notification_info.sent = true;
          return;
        }
    }

  // otherwise, use the first valid host thread to send the notification to.
  tp = iterate_over_threads (cuda_notification_notify_first_valid_thread, NULL);
  cuda_notification_trace ("sent to the first valid thread: tid %ld -> %s",
                            tp ? cuda_gdb_get_tid (tp->ptid) : 0, tp ? "success" : "FAILED");
  if (tp)
    {
      cuda_notification_info.tid = cuda_gdb_get_tid (tp->ptid);
      cuda_notification_info.sent = true;
      return;;
    }
}

void
cuda_notification_accept (void)
{
  cuda_notification_acquire_lock ();

  cuda_notification_info.blocked = false;

  if (cuda_notification_info.pending_send)
    {
      cuda_notification_trace ("accept: sending pending notification");
      cuda_notification_send (&cuda_notification_info.pending_send_data);
      cuda_notification_info.pending_send = false;
      memset (&cuda_notification_info.pending_send_data, 0, sizeof cuda_notification_info.pending_send_data);
    }

  cuda_notification_release_lock ();
}

void
cuda_notification_block (void)
{
  cuda_notification_acquire_lock ();

  cuda_notification_info.blocked = true;

  cuda_notification_release_lock ();
}

void
cuda_notification_notify (CUDBGEventCallbackData *data)
{
  cuda_notification_acquire_lock ();

  if (data->timeout)
    {
      /* Was there a timeout waiting for a response? */
      if (cuda_notification_info.sent && !cuda_notification_info.received)
        {
          cuda_notification_trace ("timeout: resending notification");
          cuda_notification_send (data);
        }
    }
  else if (cuda_notification_info.sent)
    {
      cuda_notification_trace ("aliased event: will examine before resuming");
      cuda_notification_info.aliased_event = true;
    }
  else if (cuda_notification_info.pending_send)
    cuda_notification_trace ("ignoring: another notification is already pending");
  else if (cuda_notification_info.blocked)
    {
      cuda_notification_trace ("blocked: marking notification as pending_send");
      cuda_notification_info.pending_send = true;
      cuda_notification_info.pending_send_data = *data;
    }
  else
    cuda_notification_send (data);

  cuda_notification_release_lock ();
}

bool
cuda_notification_aliased_event (void)
{
  bool aliased_event;

  cuda_notification_acquire_lock ();

  aliased_event = cuda_notification_info.aliased_event;

  cuda_notification_release_lock ();

  return aliased_event;
}

void
cuda_notification_reset_aliased_event (void)
{
  cuda_notification_acquire_lock ();

  cuda_notification_info.aliased_event = false;

  cuda_notification_release_lock ();
}

bool
cuda_notification_pending (void)
{
  bool pending;

  cuda_notification_acquire_lock ();

  pending = cuda_notification_info.sent && !cuda_notification_info.received;

  cuda_notification_release_lock ();

  return pending;
}

bool
cuda_notification_received (void)
{
  bool received;

  cuda_notification_acquire_lock ();

  received = cuda_notification_info.received;

  cuda_notification_release_lock ();

  return received;
}

void
cuda_notification_analyze (ptid_t ptid, struct target_waitstatus *ws)
{
  struct thread_info *tp = inferior_thread ();

  cuda_notification_acquire_lock ();

  /* A notification is deemed received when its corresponding SIGTRAP is the
     reason we stopped. */
  if (cuda_notification_info.sent &&
      cuda_notification_info.tid == cuda_gdb_get_tid (ptid) &&
      ws->kind == TARGET_WAITKIND_STOPPED &&
      ws->value.sig == TARGET_SIGNAL_TRAP &&
      !tp->trap_expected)
    {
      cuda_notification_trace ("received notification to thread %d", cuda_notification_info.tid);
      cuda_notification_info.received = true;
    }

  cuda_notification_release_lock ();
}

void
cuda_notification_mark_consumed (void)
{
  int status = 0, options = 0;

  cuda_notification_acquire_lock ();

  if (cuda_notification_info.received)
    {
      cuda_notification_trace ("consuming notification to thread %d", cuda_notification_info.tid);
      cuda_notification_info.sent = false;
      cuda_notification_info.received = false;
      cuda_notification_info.tid = 0;
    }

  cuda_notification_release_lock ();
}

void
cuda_notification_consume_pending (void)
{
  cuda_notification_info.pending_send = false;
}
