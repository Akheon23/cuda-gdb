/*
 * NVIDIA CUDA Debugger CUDA-GDB Copyright (C) 2007-2013 NVIDIA Corporation
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

#include <string.h>

#include "defs.h"
#include "breakpoint.h"
#include "gdb_assert.h"

#include "cuda-context.h"
#include "cuda-defs.h"
#include "cuda-iterator.h"
#include "cuda-state.h"
#include "cuda-utils.h"
#include "cuda-packet-manager.h"
#include "cuda-options.h"

typedef struct {
  bool thread_idx_p;
  bool pc_p;
  bool exception_p;
  bool virtual_pc_p;
  bool timestamp_p;
  CuDim3           thread_idx;
  uint64_t         pc;
  CUDBGException_t exception;
  uint64_t         virtual_pc;
  cuda_clock_t     timestamp;
} lane_state_t;

typedef struct {
  bool valid_p;
  bool broken_p;
  bool block_idx_p;
  bool kernel_p;
  bool grid_id_p;
  bool valid_lanes_mask_p;
  bool active_lanes_mask_p;
  bool timestamp_p;
  bool     valid;
  bool     broken;
  CuDim3   block_idx;
  kernel_t kernel;
  uint64_t grid_id;
  uint32_t valid_lanes_mask;
  uint32_t active_lanes_mask;
  cuda_clock_t     timestamp;
  lane_state_t ln[CUDBG_MAX_LANES];
} warp_state_t;

typedef struct {
  bool valid_warps_mask_p;
  bool broken_warps_mask_p;
  uint64_t valid_warps_mask;
  uint64_t broken_warps_mask;
  warp_state_t wp[CUDBG_MAX_WARPS];
} sm_state_t;

typedef struct {
  bool valid_p;
  bool num_sms_p;
  bool num_warps_p;
  bool num_lanes_p;
  bool num_registers_p;
  bool dev_type_p;
  bool sm_type_p;
  bool filter_exception_state_p;
  bool valid;             // at least one active lane
  /* the above fields are invalidated on resume */
  bool suspended;         // true if the device is suspended
  char dev_type[256];
  char sm_type[16];
  uint32_t num_sms;
  uint32_t num_warps;
  uint32_t num_lanes;
  uint32_t num_registers;
  sm_state_t sm[CUDBG_MAX_SMS];
  contexts_t contexts;    // state for contexts associated with this device
} device_state_t;

typedef struct {
  bool num_devices_p;
  uint32_t num_devices;
  device_state_t dev[CUDBG_MAX_DEVICES];
  uint32_t suspended_devices_mask;
} cuda_system_t;

const bool CACHED = true; // set to false to disable caching
typedef enum { RECURSIVE, NON_RECURSIVE } recursion_t;

static void device_initialize          (uint32_t dev_id);
static void device_finalize            (uint32_t dev_id);
static void device_cleanup_contexts    (uint32_t dev_id);
static void device_cleanup_breakpoints (uint32_t dev_id);
static void device_resolve_breakpoints (uint32_t dev_id);
static void device_flush_disasm_cache  (uint32_t dev_id);
static void sm_invalidate   (uint32_t dev_id, uint32_t sm_id, recursion_t);
static void sm_set_exception_none (uint32_t dev_id, uint32_t sm_id);
static void warp_invalidate (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id);
static void lane_invalidate (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id, uint32_t ln_id);
static void lane_set_exception_none (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id, uint32_t ln_id);


/******************************************************************************
 *
 *                                  System
 *
 ******************************************************************************/

static cuda_system_t cuda_system_info;

void
cuda_system_initialize (void)
{
  uint32_t dev_id;

  cuda_trace ("system: initialize");
  gdb_assert (cuda_initialized);

  memset (&cuda_system_info, 0, sizeof cuda_system_info);

  for (dev_id = 0; dev_id < cuda_system_get_num_devices (); ++dev_id)
     device_initialize (dev_id);
  cuda_options_force_set_launch_notification_update ();
}

void
cuda_system_finalize (void)
{
  uint32_t dev_id;

  cuda_trace ("system: finalize");
  gdb_assert (cuda_initialized);

  for (dev_id = 0; dev_id < cuda_system_get_num_devices (); ++dev_id)
     device_finalize (dev_id);

  memset (&cuda_system_info, 0, sizeof cuda_system_info);
}

void
cuda_system_set_device_spec (uint32_t dev_id, uint32_t num_sms,
                             uint32_t num_warps, uint32_t num_lanes,
                             uint32_t num_registers, char *dev_type,
                             char *sm_type)
{
  device_state_t *dev;

  gdb_assert (cuda_remote);
  gdb_assert (num_sms <= CUDBG_MAX_SMS);
  gdb_assert (num_warps <= CUDBG_MAX_WARPS);
  gdb_assert (num_lanes <= CUDBG_MAX_LANES);

  dev = &cuda_system_info.dev[dev_id];
  dev->num_sms         = num_sms;
  dev->num_warps       = num_warps;
  dev->num_lanes       = num_lanes;
  dev->num_registers   = num_registers;
  strcpy (dev->dev_type, dev_type);
  strcpy (dev->sm_type, sm_type);

  dev->num_sms_p       = CACHED;
  dev->num_warps_p     = CACHED;
  dev->num_lanes_p     = CACHED;
  dev->num_registers_p = CACHED;
  dev->dev_type_p      = CACHED;
  dev->sm_type_p       = CACHED;
}

uint32_t
cuda_system_get_num_devices (void)
{
  if (!cuda_initialized)
    return 0;

  if (cuda_system_info.num_devices_p)
    return cuda_system_info.num_devices;

  cuda_api_get_num_devices (&cuda_system_info.num_devices);
  gdb_assert (cuda_system_info.num_devices <= CUDBG_MAX_DEVICES);
  cuda_system_info.num_devices_p = CACHED;

  return cuda_system_info.num_devices;
}

uint32_t
cuda_system_get_num_present_kernels (void)
{
  kernel_t kernel;
  uint32_t num_present_kernel = 0;

  if (!cuda_initialized)
    return 0;

  for (kernel = kernels_get_first_kernel (); kernel; kernel = kernels_get_next_kernel (kernel))
    if (kernel_is_present (kernel))
      ++num_present_kernel;

  return num_present_kernel;
}

/* Brute-force function to resolve all the CUDA breakpoints that can be resolved
   at this point in time. The function iterates all the CUDA ELF images, and
   attempt to resolve breakpoints for each objfile.

   Use this function when there is no easy way to figure which breakpoints or
   contexts or modules should be considered for CUDA breakpoint resolution.

   For instance, CUDA breakpoints set from break_command_1 may require additional
   resolution.  Runtime API breakpoints will be set in the stub (in
   host code), so if we've already loaded a device ELF image that contains the
   actual device address for that breakpoint, we need to properly resolve it
   here.

   Also, when the user sets a breakpoint (break_command_1 too) on a kernel that
   has already launched from a device focus that does not include the kernel in
   question, the breakpoint must be resolved right away. Usually, breakpoints
   are resolved at ELF image load time, or at kernel launch time. Here it would
   be too late, and we must force the resolution just after the breakpoint is
   set.
*/
void
cuda_system_resolve_breakpoints (void)
{
  uint32_t             dev_id;

  cuda_trace ("system: resolve breakpoints\n");

  for (dev_id = 0; dev_id < cuda_system_get_num_devices (); ++dev_id)
    device_resolve_breakpoints (dev_id);
}

void
cuda_system_cleanup_contexts (void)
{
  uint32_t dev_id;

  cuda_trace ("system: clean up contexts");

  for (dev_id = 0; dev_id < cuda_system_get_num_devices (); ++dev_id)
    device_cleanup_contexts (dev_id);
}

void
cuda_system_cleanup_breakpoints (void)
{
  uint32_t dev_id;

  cuda_trace ("system: clean up breakpoints");

  for (dev_id = 0; dev_id < cuda_system_get_num_devices (); ++dev_id)
    device_cleanup_breakpoints (dev_id);
}

void
cuda_system_flush_disasm_cache (void)
{
  uint32_t dev_id;

  cuda_trace ("system: flush disassembly cache");

  for (dev_id = 0; dev_id < cuda_system_get_num_devices (); ++dev_id)
    device_flush_disasm_cache (dev_id);
}

bool
cuda_system_is_broken (cuda_clock_t clock)
{
  cuda_iterator itr;
  cuda_coords_t c, filter = CUDA_WILDCARD_COORDS;
  bool broken = false;

  itr = cuda_iterator_create (CUDA_ITERATOR_TYPE_WARPS, &filter,
                               CUDA_SELECT_VALID);

  for (cuda_iterator_start (itr);
       !cuda_iterator_end (itr);
       cuda_iterator_next (itr))
    {
      /* if we hit a breakpoint at an earlier time, we do not report it again. */
      c = cuda_iterator_get_current (itr);
      if (warp_get_timestamp (c.dev, c.sm, c.wp) < clock)
        continue;

      if (!warp_is_broken (c.dev, c.sm, c.wp))
        continue;

      broken = true;
      break;
    }

  cuda_iterator_destroy (itr);

  return broken;
}

uint32_t
cuda_system_get_suspended_devices_mask (void)
{
  return cuda_system_info.suspended_devices_mask;
}

context_t
cuda_system_find_context_by_addr (CORE_ADDR addr)
{
  uint32_t  dev_id;
  context_t context;

  for (dev_id = 0; dev_id < cuda_system_get_num_devices (); ++dev_id)
    {
      context = device_find_context_by_addr (dev_id, addr);
      if (context)
        return context;
    }

  return NULL;
}

/******************************************************************************
 *
 *                                  Device
 *
 ******************************************************************************/

static void
device_initialize (uint32_t dev_id)
{
  device_state_t *dev;

  cuda_trace ("device %u: initialize", dev_id);
  gdb_assert (dev_id < cuda_system_get_num_devices ());

  dev = &cuda_system_info.dev[dev_id];
  dev->contexts = contexts_new ();
}

static void
device_finalize (uint32_t dev_id)
{
  device_state_t *dev;

  cuda_trace ("device %u: finalize", dev_id);
  gdb_assert (dev_id < cuda_system_get_num_devices ());

  dev = &cuda_system_info.dev[dev_id];
}

static void
device_invalidate_kernels (uint32_t dev_id)
{
  device_state_t *dev;
  kernel_t        kernel;

  cuda_trace ("device %u: invalidate kernels", dev_id);
  gdb_assert (dev_id < cuda_system_get_num_devices ());

  for (kernel = kernels_get_first_kernel (); kernel; kernel = kernels_get_next_kernel (kernel))
    kernel_invalidate (kernel);
}

void
device_invalidate (uint32_t dev_id)
{
  device_state_t *dev;
  uint32_t sm_id;

  cuda_trace ("device %u: invalidate", dev_id);
  gdb_assert (dev_id < cuda_system_get_num_devices ());

  for (sm_id = 0; sm_id < device_get_num_sms (dev_id); ++sm_id)
    sm_invalidate (dev_id, sm_id, RECURSIVE);

  device_invalidate_kernels(dev_id);

  dev = &cuda_system_info.dev[dev_id];
  dev->valid_p   = false;
  dev->filter_exception_state_p = false;
}

static void
device_resolve_breakpoints (uint32_t dev_id)
{
  device_state_t      *dev;
  contexts_t           contexts;

  cuda_trace ("device %u: resolve breakpoints", dev_id);
  gdb_assert (dev_id < cuda_system_get_num_devices ());

  if (!device_is_any_context_present (dev_id))
    return;

  dev = &cuda_system_info.dev[dev_id];
  contexts = device_get_contexts (dev_id);
  contexts_resolve_breakpoints (contexts);
}

static void
device_cleanup_breakpoints (uint32_t dev_id)
{
  device_state_t *dev;
  contexts_t contexts;

  cuda_trace ("device %u: clean up breakpoints", dev_id);
  gdb_assert (dev_id < cuda_system_get_num_devices ());

  dev = &cuda_system_info.dev[dev_id];
  contexts = device_get_contexts (dev_id);
  contexts_cleanup_breakpoints (contexts);
}

static void
device_flush_disasm_cache (uint32_t dev_id)
{
  device_state_t *dev;
  kernel_t        kernel;

  cuda_trace ("device %u: flush disassembly cache", dev_id);
  gdb_assert (dev_id < cuda_system_get_num_devices ());

  for (kernel = kernels_get_first_kernel (); kernel; kernel = kernels_get_next_kernel (kernel))
    kernel_flush_disasm_cache (kernel);
}

static void
device_cleanup_contexts (uint32_t dev_id)
{
  device_state_t *dev;
  uint32_t        ctxtid;
  contexts_t      contexts;
  context_t       context;

  cuda_trace ("device %u: clean up contexts", dev_id);
  gdb_assert (dev_id < cuda_system_get_num_devices ());

  dev = &cuda_system_info.dev[dev_id];
  contexts = device_get_contexts (dev_id);

  contexts_delete (contexts);

  xfree (dev->contexts);
}

const char*
device_get_device_type (uint32_t dev_id)
{
  device_state_t *dev;

  gdb_assert (dev_id < cuda_system_get_num_devices ());

  dev = &cuda_system_info.dev[dev_id];

  if (dev->dev_type_p)
    return dev->dev_type;

  cuda_api_get_device_type (dev_id, dev->dev_type, sizeof dev->dev_type);
  dev->dev_type_p = CACHED;
  return dev->dev_type;
}

const char*
device_get_sm_type (uint32_t dev_id)
{
  device_state_t *dev;

  gdb_assert (dev_id < cuda_system_get_num_devices ());

  dev = &cuda_system_info.dev[dev_id];

  if (dev->sm_type_p)
    return dev->sm_type;

  cuda_api_get_sm_type (dev_id, dev->sm_type, sizeof dev->sm_type);
  dev->sm_type_p = CACHED;
  return dev->sm_type;
}

uint32_t
device_get_num_sms (uint32_t dev_id)
{
  device_state_t *dev;

  gdb_assert (dev_id < cuda_system_get_num_devices ());

  dev = &cuda_system_info.dev[dev_id];

  if (dev->num_sms_p)
    return dev->num_sms;

  cuda_api_get_num_sms (dev_id, &dev->num_sms);
  gdb_assert (dev->num_sms <= CUDBG_MAX_SMS);
  dev->num_sms_p = CACHED;

  return dev->num_sms;
}

uint32_t
device_get_num_warps (uint32_t dev_id)
{
  device_state_t *dev;

  gdb_assert (dev_id < cuda_system_get_num_devices ());

  dev = &cuda_system_info.dev[dev_id];

  if (dev->num_warps_p)
    return dev->num_warps;

  cuda_api_get_num_warps (dev_id, &dev->num_warps);
  gdb_assert (dev->num_warps <= CUDBG_MAX_WARPS);
  dev->num_warps_p = CACHED;

  return dev->num_warps;
}

uint32_t
device_get_num_lanes (uint32_t dev_id)
{
  device_state_t *dev;

  gdb_assert (dev_id < cuda_system_get_num_devices ());

  dev = &cuda_system_info.dev[dev_id];

  if (dev->num_lanes_p)
    return dev->num_lanes;

  cuda_api_get_num_lanes (dev_id, &dev->num_lanes);
  gdb_assert (dev->num_lanes <= CUDBG_MAX_LANES);
  dev->num_lanes_p = CACHED;

  return dev->num_lanes;
}

uint32_t
device_get_num_registers (uint32_t dev_id)
{
  device_state_t *dev;

  gdb_assert (dev_id < cuda_system_get_num_devices ());

  dev = &cuda_system_info.dev[dev_id];

  if (dev->num_registers_p)
    return dev->num_registers;

  cuda_api_get_num_registers (dev_id, &dev->num_registers);
  dev->num_registers_p = CACHED;

  return dev->num_registers;
}

uint32_t
device_get_num_kernels (uint32_t dev_id)
{
  kernel_t kernel;
  uint32_t num_kernels = 0;

  gdb_assert (dev_id < cuda_system_get_num_devices ());

  for (kernel = kernels_get_first_kernel (); kernel; kernel = kernels_get_next_kernel (kernel))
    if (kernel_get_dev_id (kernel) == dev_id)
      ++num_kernels;

  return num_kernels;
}

bool
device_is_any_context_present (uint32_t dev_id)
{
  contexts_t contexts;

  gdb_assert (dev_id < cuda_system_get_num_devices ());

  contexts = device_get_contexts (dev_id);

  return contexts_is_any_context_present (contexts);
}

bool
device_is_active_context (uint32_t dev_id, context_t context)
{
  contexts_t contexts;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  contexts = device_get_contexts (dev_id);

  return contexts_is_active_context (contexts, context);
}

bool
device_is_valid (uint32_t dev_id)
{
  device_state_t *dev;
  uint32_t sm, wp;

  gdb_assert (dev_id < cuda_system_get_num_devices ());

  dev = &cuda_system_info.dev[dev_id];

  if (!cuda_initialized)
    return false;

  if (dev->valid_p)
    return dev->valid;

  dev->valid = false;

  if (!device_is_any_context_present (dev_id))
    return dev->valid;

  for (sm = 0; sm < device_get_num_sms (dev_id) && !dev->valid; ++sm)
    for (wp = 0; wp < device_get_num_warps (dev_id) && !dev->valid; ++wp)
      if (warp_is_valid (dev_id, sm, wp))
          dev->valid = true;

  dev->valid_p = CACHED;
  return dev->valid;
}

uint64_t
device_get_active_sms_mask (uint32_t dev_id)
{
  device_state_t *dev;
  uint32_t        sm;
  uint32_t        wp;
  uint64_t        mask = 0;

  gdb_assert (dev_id < cuda_system_get_num_devices ());

  dev = &cuda_system_info.dev[dev_id];
  for (sm = 0; sm < device_get_num_sms (dev_id); ++sm)
    for (wp = 0; wp < device_get_num_warps (dev_id); ++wp)
      if (warp_is_valid (dev_id, sm, wp))
        {
          mask |= 1ULL << sm;
          break;
        }

  return mask;
}

contexts_t
device_get_contexts (uint32_t dev_id)
{
  device_state_t *dev;

  gdb_assert (dev_id < cuda_system_get_num_devices ());

  dev = &cuda_system_info.dev[dev_id];

  gdb_assert (dev->contexts);

  return dev->contexts;
}

context_t
device_find_context_by_id (uint32_t dev_id, uint64_t context_id)
{
  contexts_t      contexts;
  context_t       context;

  gdb_assert (dev_id < cuda_system_get_num_devices ());

  contexts = device_get_contexts (dev_id);
  return contexts_find_context_by_id (contexts, context_id);
}

context_t
device_find_context_by_addr (uint32_t dev_id, CORE_ADDR addr)
{
  device_state_t *dev;
  uint32_t        ctxtid;
  contexts_t      contexts;
  context_t       context;

  gdb_assert (dev_id < cuda_system_get_num_devices ());

  dev = &cuda_system_info.dev[dev_id];
  contexts = device_get_contexts (dev_id);

  context  = contexts_find_context_by_address (contexts, addr);

  if (context)
    return context;

  return NULL;
}

void
device_print (uint32_t dev_id)
{
  device_state_t *dev;
  uint32_t        ctxtid;
  contexts_t      contexts;
  context_t       context;

  cuda_trace ("device %u:", dev_id);
  gdb_assert (dev_id < cuda_system_get_num_devices ());

  dev = &cuda_system_info.dev[dev_id];
  contexts = device_get_contexts (dev_id);

  contexts_print (contexts);
}

void
device_resume (uint32_t dev_id)
{
  device_state_t *dev;

  cuda_trace ("device %u: resume", dev_id);
  gdb_assert (dev_id < cuda_system_get_num_devices ());

  device_invalidate (dev_id);

  dev = &cuda_system_info.dev[dev_id];

  if (!dev->suspended)
    return;

  cuda_api_resume_device (dev_id);

  dev->suspended = false;

  cuda_system_info.suspended_devices_mask &= ~(1 << dev_id);
}

static void
device_create_kernel(uint32_t dev_id, uint64_t grid_id)
{
  CUDBGGridInfo gridInfo = {0};

  cuda_api_get_grid_info(dev_id, grid_id, &gridInfo);
  kernels_start_kernel(dev_id, grid_id,
                       gridInfo.functionEntry,
                       gridInfo.context,
                       gridInfo.module,
                       gridInfo.gridDim,
                       gridInfo.blockDim,
                       gridInfo.type,
                       gridInfo.parentGridId,
                       gridInfo.origin);
}

void
device_suspend (uint32_t dev_id)
{
  device_state_t *dev;

  cuda_trace ("device %u: suspend", dev_id);
  gdb_assert (dev_id < cuda_system_get_num_devices ());

  if (!device_is_any_context_present (dev_id))
    return;

  cuda_api_suspend_device (dev_id);

  dev = &cuda_system_info.dev[dev_id];

  dev->suspended = true;

  cuda_system_info.suspended_devices_mask |= (1 << dev_id);
}

void
device_filter_exception_state (uint32_t dev_id)
{
  uint64_t sm_mask;
  device_state_t *dev;
  uint32_t sm_id;

  cuda_trace ("device %u: Looking for exception SMs\n");
  gdb_assert (dev_id < cuda_system_get_num_devices ());

  if (!device_is_any_context_present (dev_id))
    return;

  dev = &cuda_system_info.dev[dev_id];

  if (dev->filter_exception_state_p)
    return;

  sm_mask = 0;
  cuda_api_read_device_exception_state (dev_id, &sm_mask);

  for (sm_id = 0; sm_id < device_get_num_sms (dev_id); ++sm_id)
    if (!((1ULL << sm_id) & sm_mask))
      sm_set_exception_none (dev_id, sm_id);

  dev->filter_exception_state_p = true;
}

/******************************************************************************
 *
 *                                    SM
 *
 ******************************************************************************/

static void
sm_invalidate (uint32_t dev_id, uint32_t sm_id, recursion_t recursion)
{
  sm_state_t *sm;
  uint32_t wp_id;

  cuda_trace ("device %u sm %u: invalidate", dev_id, sm_id);
  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));

  if (recursion == RECURSIVE)
    for (wp_id = 0; wp_id < device_get_num_warps (dev_id); ++wp_id)
      warp_invalidate (dev_id, sm_id, wp_id);

  sm = &cuda_system_info.dev[dev_id].sm[sm_id];
  sm->valid_warps_mask_p  = false;
  sm->broken_warps_mask_p = false;
}

bool
sm_is_valid (uint32_t dev_id, uint32_t sm_id)
{
  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));

  return sm_get_valid_warps_mask (dev_id, sm_id);
}

uint64_t
sm_get_valid_warps_mask (uint32_t dev_id, uint32_t sm_id)
{
  sm_state_t *sm;
  uint64_t    valid_warps_mask = 0ULL;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));

  sm = &cuda_system_info.dev[dev_id].sm[sm_id];

  if (sm->valid_warps_mask_p)
    return sm->valid_warps_mask;

  cuda_api_read_valid_warps (dev_id, sm_id, &valid_warps_mask);

  sm->valid_warps_mask   = valid_warps_mask;
  sm->valid_warps_mask_p = CACHED;

  return valid_warps_mask;
}

uint64_t
sm_get_broken_warps_mask (uint32_t dev_id, uint32_t sm_id)
{
  sm_state_t *sm;
  uint64_t    broken_warps_mask = 0ULL;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));

  sm = &cuda_system_info.dev[dev_id].sm[sm_id];

  if (sm->broken_warps_mask_p)
    return sm->broken_warps_mask;

  cuda_api_read_broken_warps (dev_id, sm_id, &broken_warps_mask);

  sm->broken_warps_mask   = broken_warps_mask;
  sm->broken_warps_mask_p = CACHED;

  return broken_warps_mask;
}

static void
sm_set_exception_none (uint32_t dev_id, uint32_t sm_id)
{
  sm_state_t *sm;
  uint32_t wp_id;
  uint32_t ln_id;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));

  cuda_trace ("device %u sm %u: setting no exceptions", dev_id, sm_id);

  sm = &cuda_system_info.dev[dev_id].sm[sm_id];

  for (wp_id = 0; wp_id < device_get_num_warps (dev_id); ++wp_id)
    for (ln_id = 0; ln_id < device_get_num_lanes (dev_id); ++ln_id)
      lane_set_exception_none (dev_id, sm_id, wp_id, ln_id);

}

/******************************************************************************
 *
 *                                   Warps
 *
 ******************************************************************************/

static void
warp_invalidate (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id)
{
  sm_state_t   *sm;
  warp_state_t *wp;
  uint32_t      ln_id;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));

  for (ln_id = 0; ln_id < device_get_num_lanes (dev_id); ++ln_id)
    lane_invalidate (dev_id, sm_id, wp_id, ln_id);

  // XXX decouple the masks from the SM state data structure to avoid this
  // little hack.
  /* If a warp is invalidated, we have to invalidate the warp masks in the
     corresponding SM. */
  sm = &cuda_system_info.dev[dev_id].sm[sm_id];
  sm->valid_warps_mask_p  = false;
  sm->broken_warps_mask_p = false;

  wp = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id];
  wp->valid_p             = false;
  wp->broken_p            = false;
  wp->block_idx_p         = false;
  wp->kernel_p            = false;
  wp->grid_id_p           = false;
  wp->valid_lanes_mask_p  = false;
  wp->active_lanes_mask_p = false;
  wp->timestamp_p         = false;
}

void
warp_single_step (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id,
                  uint64_t *single_stepped_warp_mask)
{
  kernel_t kernel;
  uint64_t kernel_id;
  CuDim3   block_idx;
  uint32_t i;

  cuda_trace ("device %u sm %u warp %u: single-step", dev_id, sm_id, wp_id);

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));

  *single_stepped_warp_mask = 0ULL;
  cuda_api_single_step_warp (dev_id, sm_id, wp_id, single_stepped_warp_mask);

  if (cuda_options_software_preemption ())
    device_invalidate (dev_id);
  else
    {
      if (*single_stepped_warp_mask & ~(1ULL << wp_id))
        {
          warning ("Warp(s) other than the current warp had to be single-stepped.");
          device_invalidate (dev_id);
        }
      /* invalidate the cache for the warps that have been single-stepped. */
      for (i = 0; i < device_get_num_warps (dev_id); ++i)
        if ((1ULL << i) & *single_stepped_warp_mask)
          warp_invalidate (dev_id, sm_id, i);

      /* must invalidate the SM since that's where the warp valid mask lives */
      sm_invalidate (dev_id, sm_id, NON_RECURSIVE);
    }
}

bool
warp_is_valid (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id)
{
  uint64_t valid_warps_mask;
  bool     valid;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));

  valid_warps_mask = sm_get_valid_warps_mask (dev_id, sm_id);
  valid            = (valid_warps_mask >> wp_id) & 1ULL;

  return valid;
}

bool
warp_is_broken (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id)
{
  uint64_t broken_warps_mask;
  bool     broken;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));

  broken_warps_mask = sm_get_broken_warps_mask (dev_id, sm_id);
  broken            = (broken_warps_mask >> wp_id) & 1ULL;

  return broken;
}

uint64_t
warp_get_grid_id (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id)
{
  warp_state_t *wp;
  uint64_t grid_id;

  wp = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id];

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));

  if (cuda_remote && !(wp->grid_id_p)
      && sm_is_valid (dev_id, sm_id))
    cuda_remote_update_grid_id_in_sm (dev_id, sm_id);

  if (wp->grid_id_p)
    return wp->grid_id;

  cuda_api_read_grid_id (dev_id, sm_id, wp_id, &grid_id);

  wp->grid_id   = grid_id;
  wp->grid_id_p = CACHED;

  return wp->grid_id;
}

kernel_t
warp_get_kernel (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id)
{
  warp_state_t *wp;
  uint64_t      grid_id;
  kernel_t      kernel;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));

  wp = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id];

  if (wp->kernel_p)
    return wp->kernel;

  grid_id = warp_get_grid_id (dev_id, sm_id, wp_id);
  kernel  = kernels_find_kernel_by_grid_id (dev_id, grid_id);

  if (!kernel && cuda_options_defer_kernel_launch_notifications())
    {
      device_create_kernel(dev_id, grid_id);
      kernel = kernels_find_kernel_by_grid_id (dev_id, grid_id);
    }

  wp->kernel   = kernel;
  wp->kernel_p = CACHED;

  return wp->kernel;
}

CuDim3
warp_get_block_idx (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id)
{
  warp_state_t *wp;
  CuDim3        block_idx;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));

  wp = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id];

  if (cuda_remote && !(wp->block_idx_p)
      && sm_is_valid (dev_id, sm_id))
    cuda_remote_update_block_idx_in_sm (dev_id, sm_id);

  if (wp->block_idx_p)
    return wp->block_idx;

  cuda_api_read_block_idx (dev_id, sm_id, wp_id, &block_idx);

  wp->block_idx   = block_idx;
  wp->block_idx_p = CACHED;

  return wp->block_idx;
}

uint32_t
warp_get_valid_lanes_mask (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id)
{
  warp_state_t *wp;
  uint32_t      valid_lanes_mask;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));

  wp = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id];

  if (wp->valid_lanes_mask_p)
    return wp->valid_lanes_mask;

  valid_lanes_mask = 0;
  if (warp_is_valid (dev_id, sm_id, wp_id))
    cuda_api_read_valid_lanes (dev_id, sm_id, wp_id, &valid_lanes_mask);

  wp->valid_lanes_mask   = valid_lanes_mask;
  wp->valid_lanes_mask_p = CACHED;

  if (!wp->timestamp_p)
    {
      wp->timestamp_p = true;
      wp->timestamp = cuda_clock ();
    }

  return wp->valid_lanes_mask;
}

uint32_t
warp_get_active_lanes_mask (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id)
{
  warp_state_t *wp;
  uint32_t      active_lanes_mask;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));

  wp = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id];

  if (wp->active_lanes_mask_p)
    return wp->active_lanes_mask;

  cuda_api_read_active_lanes (dev_id, sm_id, wp_id, &active_lanes_mask);

  wp->active_lanes_mask   = active_lanes_mask;
  wp->active_lanes_mask_p = CACHED;

  return wp->active_lanes_mask;
}

uint32_t
warp_get_divergent_lanes_mask (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id)
{
  uint32_t valid_lanes_mask;
  uint32_t active_lanes_mask;
  uint32_t divergent_lanes_mask;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));

  valid_lanes_mask     = warp_get_valid_lanes_mask  (dev_id, sm_id, wp_id);
  active_lanes_mask    = warp_get_active_lanes_mask (dev_id, sm_id, wp_id);
  divergent_lanes_mask = valid_lanes_mask & ~active_lanes_mask;

  return divergent_lanes_mask;
}

uint32_t
warp_get_lowest_active_lane (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id)
{
  uint32_t active_lanes_mask;
  uint32_t ln_id;
  
  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));

  active_lanes_mask = warp_get_active_lanes_mask (dev_id, sm_id, wp_id);

  for (ln_id = 0; ln_id < device_get_num_lanes (dev_id); ++ln_id)
    if ((active_lanes_mask >> ln_id) & 1)
      break;

  return ln_id;
}

uint64_t
warp_get_active_pc (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id)
{
  uint32_t ln_id;
  uint64_t pc;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));

  ln_id = warp_get_lowest_active_lane (dev_id, sm_id, wp_id);
  pc = lane_get_pc (dev_id, sm_id, wp_id, ln_id);

  return pc;
}

uint64_t
warp_get_active_virtual_pc (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id)
{
  uint32_t ln_id;
  uint64_t pc;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));

  ln_id = warp_get_lowest_active_lane (dev_id, sm_id, wp_id);
  pc = lane_get_virtual_pc (dev_id, sm_id, wp_id, ln_id);

  return pc;
}

cuda_clock_t
warp_get_timestamp (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id)
{
  warp_state_t *wp;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (warp_is_valid (dev_id, sm_id, wp_id));

  wp = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id];

  gdb_assert (wp->timestamp_p);

  return wp->timestamp;
}

void
warp_set_grid_id (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id, uint64_t grid_id)
{
  warp_state_t *wp;

  gdb_assert (cuda_remote);
  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (warp_is_valid (dev_id, sm_id, wp_id));

  wp = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id];
  wp->grid_id = grid_id;
  wp->grid_id_p = true;
}

void
warp_set_block_idx (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id, CuDim3 *block_idx)
{
  warp_state_t *wp;

  gdb_assert (cuda_remote);
  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (warp_is_valid (dev_id, sm_id, wp_id));

  wp = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id];
  wp->block_idx = *block_idx;
  wp->block_idx_p = true;
}

/******************************************************************************
 *
 *                                   Lanes
 *
 ******************************************************************************/

static void
lane_invalidate (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id, uint32_t ln_id)
{
  lane_state_t *ln;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));

  ln = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id].ln[ln_id];

  ln->pc_p         = false;
  ln->virtual_pc_p = false;
  ln->thread_idx_p = false;
  ln->exception_p  = false;
  ln->timestamp_p  = false;
}

bool
lane_is_valid (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id, uint32_t ln_id)
{
  uint32_t valid_lanes_mask;
  bool     valid;
  lane_state_t *ln;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));

  ln = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id].ln[ln_id];

  valid_lanes_mask = warp_get_valid_lanes_mask (dev_id, sm_id, wp_id);
  valid = (valid_lanes_mask >> ln_id) & 1;

  if (!ln->timestamp_p)
    {
      ln->timestamp_p = true;
      ln->timestamp = cuda_clock ();
    }

  return valid;
}

bool
lane_is_active (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id, uint32_t ln_id)
{
  uint32_t active_lanes_mask;
  bool     active;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));
  gdb_assert (lane_is_valid (dev_id, sm_id, wp_id, ln_id));

  active_lanes_mask = warp_get_active_lanes_mask (dev_id, sm_id, wp_id);
  active = (active_lanes_mask >> ln_id) & 1;

  return active;
}

bool
lane_is_divergent (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id, uint32_t ln_id)
{
  uint32_t divergent_lanes_mask;
  bool     divergent;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));
  gdb_assert (lane_is_valid (dev_id, sm_id, wp_id, ln_id));

  divergent_lanes_mask = warp_get_divergent_lanes_mask (dev_id, sm_id, wp_id);
  divergent = (divergent_lanes_mask >> ln_id) & 1;

  return divergent;
}

CuDim3
lane_get_thread_idx (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id, uint32_t ln_id)
{
  lane_state_t *ln;
  CuDim3 thread_idx;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));
  gdb_assert (lane_is_valid (dev_id, sm_id, wp_id, ln_id));

  ln = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id].ln[ln_id];

  /* In a remote session, we fetch the threadIdx of all valid thread in the warp using
   * one rsp packet to reduce the amount of communication. */
  if (cuda_remote && !(ln->thread_idx_p)
      && warp_is_valid (dev_id, sm_id, wp_id))
    cuda_remote_update_thread_idx_in_warp (dev_id, sm_id, wp_id);

  if (ln->thread_idx_p)
    return ln->thread_idx;

  cuda_api_read_thread_idx (dev_id, sm_id, wp_id, ln_id, &thread_idx);

  ln->thread_idx_p = CACHED;
  ln->thread_idx   = thread_idx;

  return ln->thread_idx;
}

uint64_t
lane_get_virtual_pc (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id, uint32_t ln_id)
{
  lane_state_t *ln;
  warp_state_t *wp;
  uint64_t      virtual_pc;
  uint32_t      other_ln_id;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));
  gdb_assert (lane_is_valid (dev_id, sm_id, wp_id, ln_id));

  wp = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id];
  ln = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id].ln[ln_id];

  if (ln->virtual_pc_p)
    return ln->virtual_pc;

  cuda_api_read_virtual_pc (dev_id, sm_id, wp_id, ln_id, &virtual_pc);

  ln->virtual_pc_p = CACHED;
  ln->virtual_pc   = virtual_pc;

  /* Optimization: all the active lanes share the same virtual PC */
  if (lane_is_active (dev_id, sm_id, wp_id, ln_id))
    for (other_ln_id = 0; other_ln_id < device_get_num_lanes (dev_id); ++other_ln_id)
      if (lane_is_valid (dev_id, sm_id, wp_id, other_ln_id) &&
          lane_is_active (dev_id, sm_id, wp_id, other_ln_id))
        {
          wp->ln[other_ln_id].virtual_pc_p = CACHED;
          wp->ln[other_ln_id].virtual_pc   = virtual_pc;
        }

  return ln->virtual_pc;
}

uint64_t
lane_get_pc (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id, uint32_t ln_id)
{
  lane_state_t *ln;
  warp_state_t *wp;
  uint64_t      pc;
  uint32_t      other_ln_id;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));
  gdb_assert (lane_is_valid (dev_id, sm_id, wp_id, ln_id));

  wp = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id];
  ln = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id].ln[ln_id];

  if (ln->pc_p)
    return ln->pc;

  cuda_api_read_pc (dev_id, sm_id, wp_id, ln_id, &pc);

  ln->pc_p = CACHED;
  ln->pc   = pc;

  /* Optimization: all the active lanes share the same virtual PC */
  if (lane_is_active (dev_id, sm_id, wp_id, ln_id))
    for (other_ln_id = 0; other_ln_id < device_get_num_lanes (dev_id); ++other_ln_id)
      if (lane_is_valid (dev_id, sm_id, wp_id, other_ln_id) &&
          lane_is_active (dev_id, sm_id, wp_id, other_ln_id))
        {
          wp->ln[other_ln_id].pc_p = CACHED;
          wp->ln[other_ln_id].pc   = pc;
        }

  return ln->pc;
}

CUDBGException_t
lane_get_exception (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id, uint32_t ln_id)
{
  lane_state_t    *ln;
  CUDBGException_t exception;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));
  gdb_assert (lane_is_valid (dev_id, sm_id, wp_id, ln_id));

  ln = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id].ln[ln_id];

  if (ln->exception_p)
    return ln->exception;

  cuda_api_read_lane_exception (dev_id, sm_id, wp_id, ln_id, &exception);

  ln->exception_p = CACHED;
  ln->exception   = exception;

  return ln->exception;
}

uint32_t
lane_get_register (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id, uint32_t ln_id,
                   uint32_t regno)
{
  uint32_t value;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));
  gdb_assert (lane_is_valid (dev_id, sm_id, wp_id, ln_id));

  cuda_api_read_register (dev_id, sm_id, wp_id, ln_id, regno, &value);

  return value;
}

int32_t
lane_get_call_depth (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id, uint32_t ln_id)
{
  int32_t call_depth;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));
  gdb_assert (lane_is_valid (dev_id, sm_id, wp_id, ln_id));

  cuda_api_read_call_depth (dev_id, sm_id, wp_id, ln_id, &call_depth);

  return call_depth;
}

int32_t
lane_get_syscall_call_depth (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id, uint32_t ln_id)
{
  int32_t syscall_call_depth;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));
  gdb_assert (lane_is_valid (dev_id, sm_id, wp_id, ln_id));

  cuda_api_read_syscall_call_depth (dev_id, sm_id, wp_id, ln_id, &syscall_call_depth);

  return syscall_call_depth;
}

uint64_t
lane_get_virtual_return_address (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id,
                                 uint32_t ln_id, int32_t level)
{
  uint64_t virtual_return_address;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));
  gdb_assert (lane_is_valid (dev_id, sm_id, wp_id, ln_id));

  cuda_api_read_virtual_return_address (dev_id, sm_id, wp_id, ln_id, level,
                                             &virtual_return_address);

  return virtual_return_address;
}

cuda_clock_t
lane_get_timestamp (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id,uint32_t ln_id)
{
  lane_state_t *ln;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));
  gdb_assert (lane_is_valid (dev_id, sm_id, wp_id, ln_id));

  ln = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id].ln[ln_id];

  gdb_assert (ln->timestamp_p);

  return ln->timestamp;
}

uint64_t
lane_get_memcheck_error_address (uint32_t dev_id, uint32_t sm_id,
                                 uint32_t wp_id, uint32_t ln_id)
{
  CUDBGException_t exception;
  uint64_t address = 0;
  ptxStorageKind segment = ptxUNSPECIFIEDStorage;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));
  gdb_assert (lane_is_valid (dev_id, sm_id, wp_id, ln_id));

  exception = lane_get_exception (dev_id, sm_id, wp_id, ln_id);

  if (exception == CUDBG_EXCEPTION_LANE_ILLEGAL_ADDRESS)
    cuda_api_memcheck_read_error_address (dev_id, sm_id, wp_id, ln_id,
                                          &address, &segment);
  return address;
}

ptxStorageKind
lane_get_memcheck_error_address_segment (uint32_t dev_id, uint32_t sm_id,
                                         uint32_t wp_id, uint32_t ln_id)
{
  CUDBGException_t exception;
  uint64_t address = 0;
  ptxStorageKind segment = ptxUNSPECIFIEDStorage;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));
  gdb_assert (lane_is_valid (dev_id, sm_id, wp_id, ln_id));

  exception = lane_get_exception (dev_id, sm_id, wp_id, ln_id);

  if (exception == CUDBG_EXCEPTION_LANE_ILLEGAL_ADDRESS)
    cuda_api_memcheck_read_error_address (dev_id, sm_id, wp_id, ln_id,
                                          &address, &segment);
  return segment;
}

void
lane_set_thread_idx (uint32_t dev_id, uint32_t sm_id,
                     uint32_t wp_id, uint32_t ln_id, CuDim3 *thread_idx)
{
  lane_state_t *ln;

  gdb_assert (cuda_remote);
  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));
  gdb_assert (lane_is_valid (dev_id, sm_id, wp_id, ln_id));

  ln = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id].ln[ln_id];
  ln->thread_idx = *thread_idx;
  ln->thread_idx_p = true;
}

static void
lane_set_exception_none (uint32_t dev_id, uint32_t sm_id, uint32_t wp_id,
                         uint32_t ln_id)
{
  lane_state_t *ln;

  gdb_assert (dev_id < cuda_system_get_num_devices ());
  gdb_assert (sm_id < device_get_num_sms (dev_id));
  gdb_assert (wp_id < device_get_num_warps (dev_id));
  gdb_assert (ln_id < device_get_num_lanes (dev_id));

  ln = &cuda_system_info.dev[dev_id].sm[sm_id].wp[wp_id].ln[ln_id];

  ln->exception = CUDBG_EXCEPTION_NONE;
  ln->exception_p = true;
}
