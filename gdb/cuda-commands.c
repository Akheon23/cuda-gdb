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

#include <string.h>
#include "defs.h"
#include "gdb_assert.h"
#include "cuda-tdep.h"
#include "cuda-options.h"
#include "cuda-parser.h"
#include "valprint.h"
#include "filenames.h"
#include "ui-out.h"
#include "command.h"
#include "gdb.h"
#include "exceptions.h"
#include "gdbcmd.h"
#include "symtab.h"
#include "breakpoint.h"
#include "cuda-context.h"
#include "cuda-iterator.h"
#include "cuda-kernel.h"
#include "cuda-state.h"
#include "cuda-tdep.h"

static void
cuda_parser_result_to_coords (cuda_parser_result_t *result, cuda_coords_t *coords)
{
  uint32_t i;
  request_t *request;

  gdb_assert (result);
  gdb_assert (coords);

  for (i = 0, request = result->requests; i < result->num_requests; ++i, ++request)
    switch (request->type)
      {
      case COORD_TYPE_DEVICE : coords->dev = request->value.scalar; break;
      case COORD_TYPE_SM     : coords->sm = request->value.scalar; break;
      case COORD_TYPE_WARP   : coords->wp = request->value.scalar; break;
      case COORD_TYPE_LANE   : coords->ln = request->value.scalar; break;
      case COORD_TYPE_KERNEL : coords->kernelId = request->value.scalar; break;
      case COORD_TYPE_GRID   : coords->gridId = request->value.scalar; break;
      case COORD_TYPE_BLOCK  : coords->blockIdx = request->value.cudim3; break;
      case COORD_TYPE_THREAD : coords->threadIdx = request->value.cudim3; break;
      default                : error (_("Unexpected request type."));
      }
}

static cuda_coords_t
cuda_build_filter (char* filter_string, cuda_coords_t *default_filter)
{
  cuda_coords_t filter;
  cuda_parser_result_t *result;

  if (filter_string && *filter_string != 0)
    {
      /* parse the filter string */
      cuda_parser (filter_string, CMD_FILTER, &result, CUDA_WILDCARD);
      if (result->command != CMD_FILTER)
        error (_("Incorrect filter: '%s'."), filter_string);

      /* build the filter object from the result of the parser */
      filter = CUDA_WILDCARD_COORDS;
      cuda_parser_result_to_coords (result, &filter);
    }
  else if (default_filter)
    {
      /* expand the filter object */
      filter = *default_filter;
    }
  else
    {
      /* No filter means anything is acceptable */
      filter = CUDA_WILDCARD_COORDS;
    }

  /* Evaluate the CUDA_CURRENT tokens */
  cuda_coords_evaluate_current (&filter, false);

  /* sanity check */
  cuda_coords_check_fully_defined (&filter, false, false, true);

  return filter;
}

typedef struct {
  bool        current;
  uint32_t    device;
  const char *description;
  const char *sm_type;
  uint32_t    num_sms;
  uint32_t    num_warps;
  uint32_t    num_lanes;
  uint32_t    num_regs;
  uint32_t    active_sms_mask;
} cuda_info_device_t;

static void
cuda_info_devices (char *filter_string, cuda_info_device_t **devices, uint32_t *num_devices)
{
  uint32_t num_elements;
  cuda_iterator iter;
  cuda_coords_t default_filter, c, filter;
  cuda_info_device_t *d;

  /* sanity checks */
  gdb_assert (devices);
  gdb_assert (num_devices);

  /* get the filter */
  default_filter = CUDA_WILDCARD_COORDS;
  filter = cuda_build_filter (filter_string, &default_filter);

  /* get the list of devices */
  iter = cuda_iterator_create (CUDA_ITERATOR_TYPE_DEVICES, &filter, CUDA_SELECT_ALL);
  num_elements = cuda_iterator_get_size (iter);
  *devices = xmalloc (num_elements * sizeof (**devices));
  *num_devices = 0;

  /* compile the needed info for each device */
  for (cuda_iterator_start (iter), d = *devices;
       !cuda_iterator_end (iter);
       cuda_iterator_next (iter), ++d)
    {
      c  = cuda_iterator_get_current (iter);

      d->current         = cuda_coords_is_current (&c);
      d->device          = c.dev;
      d->description     = device_get_device_type (c.dev);
      d->sm_type         = device_get_sm_type (c.dev);
      d->num_sms         = device_get_num_sms (c.dev);
      d->num_warps       = device_get_num_warps (c.dev);
      d->num_lanes       = device_get_num_lanes (c.dev);
      d->num_regs        = device_get_num_registers (c.dev);
      d->active_sms_mask = device_get_active_sms_mask (c.dev);

      ++*num_devices;
    }
}

static void
cuda_info_devices_destroy (cuda_info_device_t *devices)
{
  xfree (devices);
}

void
info_cuda_devices_command (char *arg)
{
  cuda_info_device_t *devices, *d;
  uint32_t i, num_devices;
  struct cleanup *table_chain, *row_chain;
  struct { uint32_t current, device, description, sm_type, num_sms,
             num_warps, num_lanes, num_regs, active_sms_mask; } width;

  /* column header */
  const char *header_current         = " ";
  const char *header_device          = "Dev";
  const char *header_description     = "Description";
  const char *header_sm_type         = "SM Type";
  const char *header_num_sms         = "SMs";
  const char *header_num_warps       = "Warps/SM";
  const char *header_num_lanes       = "Lanes/Warp";
  const char *header_num_regs        = "Max Regs/Lane";
  const char *header_active_sms_mask = "Active SMs Mask";

  /* get the information */
  cuda_info_devices (arg, &devices, &num_devices);

  /* output message if the list is empty */
  if (num_devices == 0 && !ui_out_is_mi_like_p (uiout))
    {
      ui_out_field_string (uiout, NULL, _("No CUDA devices.\n"));
      return;
    }

  /* column widths */
  width.current         = strlen (header_current);
  width.device          = strlen (header_device);
  width.description     = strlen (header_description);
  width.sm_type         = strlen (header_sm_type);
  width.num_sms         = strlen (header_num_sms);
  width.num_warps       = strlen (header_num_warps);
  width.num_lanes       = strlen (header_num_lanes);
  width.num_regs        = strlen (header_num_regs);
  width.active_sms_mask = strlen (header_active_sms_mask);

  for (d = devices, i = 0; i < num_devices; ++i, ++d)
    {
      width.description     = max (width.description, strlen (d->description));
      width.sm_type         = max (width.sm_type,     strlen (d->sm_type));
      width.active_sms_mask = max (width.active_sms_mask, 10);
    }

  /* print table header */
  table_chain = make_cleanup_ui_out_table_begin_end (uiout, 9, num_devices, "InfoCudaDevicesTable");
  ui_out_table_header (uiout, width.current         , ui_right, "current"         , header_current);
  ui_out_table_header (uiout, width.device          , ui_right, "device"          , header_device);
  ui_out_table_header (uiout, width.description     , ui_right, "description"     , header_description);
  ui_out_table_header (uiout, width.sm_type         , ui_right, "sm_type"         , header_sm_type);
  ui_out_table_header (uiout, width.num_sms         , ui_right, "num_sms"         , header_num_sms);
  ui_out_table_header (uiout, width.num_warps       , ui_right, "num_warps"       , header_num_warps);
  ui_out_table_header (uiout, width.num_lanes       , ui_right, "num_lanes"       , header_num_lanes);
  ui_out_table_header (uiout, width.num_regs        , ui_right, "num_regs"        , header_num_regs);
  ui_out_table_header (uiout, width.active_sms_mask , ui_right, "active_sms_mask" , header_active_sms_mask);
  ui_out_table_body (uiout);

  /* print table rows */
  for (d = devices, i = 0; i < num_devices; ++i, ++d)
    {
      row_chain = make_cleanup_ui_out_tuple_begin_end (uiout, "InfoCudaDevicesRow");
      ui_out_field_string (uiout, "current"        , d->current ? "*" : " ");
      ui_out_field_int    (uiout, "device"         , d->device);
      ui_out_field_string (uiout, "description"    , d->description);
      ui_out_field_string (uiout, "sm_type"        , d->sm_type);
      ui_out_field_int    (uiout, "num_sms"        , d->num_sms);
      ui_out_field_int    (uiout, "num_warps"      , d->num_warps);
      ui_out_field_int    (uiout, "num_lanes"      , d->num_lanes);
      ui_out_field_int    (uiout, "num_regs"       , d->num_regs);
      ui_out_field_fmt    (uiout, "active_sms_mask", "0x%08x", d->active_sms_mask);
      ui_out_text         (uiout, "\n");
      do_cleanups (row_chain);
    }

  do_cleanups (table_chain);

  gdb_flush (gdb_stdout);

  cuda_info_devices_destroy (devices);
}

typedef struct {
  bool     current;
  uint32_t device;
  uint32_t sm;
  uint64_t active_warps_mask;
} cuda_info_sm_t;

static void
cuda_info_sms (char *filter_string, cuda_info_sm_t **sms, uint32_t *num_sms)
{
  uint32_t num_elements;
  cuda_iterator iter;
  cuda_coords_t default_filter, c, filter;
  cuda_info_sm_t *s;

  /* sanity checks */
  gdb_assert (sms);
  gdb_assert (num_sms);

  /* set the filter */
  default_filter = CUDA_WILDCARD_COORDS;
  default_filter.dev = CUDA_CURRENT;
  filter = cuda_build_filter (filter_string, &default_filter);

  /* get the list of sms */
  iter = cuda_iterator_create (CUDA_ITERATOR_TYPE_SMS, &filter, CUDA_SELECT_ALL);
  num_elements = cuda_iterator_get_size (iter);
  *sms = xmalloc (num_elements * sizeof (**sms));
  *num_sms = 0;

  /* compile the needed info for each device */
  for (cuda_iterator_start (iter), s = *sms;
       !cuda_iterator_end (iter);
       cuda_iterator_next (iter), ++s)
    {
      c  = cuda_iterator_get_current (iter);

      s->current           = cuda_coords_is_current (&c);
      s->device            = c.dev;
      s->sm                = c.sm;
      s->active_warps_mask = sm_get_valid_warps_mask (c.dev, c.sm);

      ++*num_sms;
    }

  cuda_iterator_destroy (iter);
}

static void
cuda_info_sms_destroy (cuda_info_sm_t *sms)
{
  xfree (sms);
}

void
info_cuda_sms_command (char *arg)
{
  cuda_info_sm_t *sms, *s;
  uint32_t i, num_sms, current_device;
  struct cleanup *table_chain, *row_chain;
  struct { uint32_t current, sm, active_warps_mask; } width;

  /* column headers */
  const char *header_current            = " ";
  const char *header_sm                 = "SM";
  const char *header_active_warps_mask  = "Active Warps Mask";

  /* get the information */
  cuda_info_sms (arg, &sms, &num_sms);

  /* output message if the list is empty */
  if (num_sms == 0 && !ui_out_is_mi_like_p (uiout))
    {
      ui_out_field_string (uiout, NULL, _("No CUDA SMs.\n"));
      return;
    }

  /* column widths */
  width.current            = strlen (header_current);
  width.sm                 = strlen (header_sm);
  width.active_warps_mask  = strlen (header_active_warps_mask);

  width.active_warps_mask  = max (width.active_warps_mask, 18);

  /* print table header */
  table_chain = make_cleanup_ui_out_table_begin_end (uiout, 3, num_sms, "InfoCudaSmsTable");
  ui_out_table_header (uiout, width.current          , ui_right, "current"           , header_current);
  ui_out_table_header (uiout, width.sm               , ui_right, "sm"                , header_sm);
  ui_out_table_header (uiout, width.active_warps_mask, ui_right, "active_warps_mask" , header_active_warps_mask);
  ui_out_table_body (uiout);

  /* print table rows */
  for (s = sms, i = 0, current_device = -1; i < num_sms; ++i, ++s)
    {
      if (!ui_out_is_mi_like_p (uiout) && s->device != current_device)
        {
          ui_out_message (uiout, 0, "Device %u\n", s->device);
          current_device = s->device;
        }

      row_chain = make_cleanup_ui_out_tuple_begin_end (uiout, "InfoCudaSmsRow");
      ui_out_field_string (uiout, "current"          , s->current ? "*" : " ");
      ui_out_field_int    (uiout, "sm"               , s->sm);
      ui_out_field_fmt    (uiout, "active_warps_mask", "0x%016"PRIx64, s->active_warps_mask);
      ui_out_text         (uiout, "\n");
      do_cleanups (row_chain);
    }

  do_cleanups (table_chain);

  gdb_flush (gdb_stdout);

  cuda_info_sms_destroy (sms);
}

typedef struct {
  bool     current;
  uint32_t device;
  uint32_t sm;
  uint32_t wp;
  char     active_lanes_mask[11];
  char     divergent_lanes_mask[11];
  char     active_physical_pc[19];
  char     kernel_id[6];
  char     blockIdx[32];
} cuda_info_warp_t;

static void
cuda_info_warps (char *filter_string, cuda_info_warp_t **warps, uint32_t *num_warps)
{
  uint32_t num_elements;
  cuda_iterator iter;
  cuda_coords_t default_filter, c, filter;
  cuda_info_warp_t *w;
  uint64_t active_physical_pc;
  uint32_t kernel_id, active_lanes_mask, divergent_lanes_mask;
  CuDim3 blockIdx;
  kernel_t kernel;

  /* sanity checks */
  gdb_assert (warps);
  gdb_assert (num_warps);

  /* set the filter */
  default_filter = CUDA_WILDCARD_COORDS;
  default_filter.dev = CUDA_CURRENT;
  default_filter.sm  = CUDA_CURRENT;
  filter = cuda_build_filter (filter_string, &default_filter);

  /* get the list of sms */
  iter = cuda_iterator_create (CUDA_ITERATOR_TYPE_WARPS, &filter, CUDA_SELECT_ALL);
  num_elements = cuda_iterator_get_size (iter);
  *warps = xmalloc (num_elements * sizeof (**warps));
  *num_warps = 0;

  /* compile the needed info for each device */
  for (cuda_iterator_start (iter), w = *warps;
       !cuda_iterator_end (iter);
       cuda_iterator_next (iter), ++w)
    {
      c  = cuda_iterator_get_current (iter);

      w->current              = cuda_coords_is_current (&c);
      w->device               = c.dev;
      w->sm                   = c.sm;
      w->wp                   = c.wp;

      if (warp_is_valid (c.dev, c.sm, c.wp))
        {
          active_lanes_mask    = warp_get_active_lanes_mask (c.dev, c.sm, c.wp);
          divergent_lanes_mask = warp_get_divergent_lanes_mask (c.dev, c.sm, c.wp);
          kernel               = warp_get_kernel (c.dev, c.sm, c.wp);
          kernel_id            = kernel_get_id (kernel);
          blockIdx             = warp_get_block_idx (c.dev, c.sm, c.wp);
          active_physical_pc   = warp_get_active_pc (c.dev, c.sm, c.wp);

          snprintf (w->active_lanes_mask    , sizeof (w->active_lanes_mask)    , "0x%08x"      , active_lanes_mask);
          snprintf (w->divergent_lanes_mask , sizeof (w->divergent_lanes_mask) , "0x%08x"      , divergent_lanes_mask);
          snprintf (w->kernel_id            , sizeof (w->kernel_id)            , "%u"          , kernel_id);
          snprintf (w->blockIdx             , sizeof (w->blockIdx)             , "(%u,%u,%u)"  , blockIdx.x, blockIdx.y, blockIdx.z);
          snprintf (w->active_physical_pc   , sizeof (w->active_physical_pc)   , "0x%016"PRIx64, active_physical_pc);
        }
      else
        {
          snprintf (w->active_lanes_mask    , sizeof (w->active_lanes_mask)    , "0x%08x", 0);
          snprintf (w->divergent_lanes_mask , sizeof (w->divergent_lanes_mask) , "0x%08x", 0);
          snprintf (w->kernel_id            , sizeof (w->kernel_id)            , "n/a");
          snprintf (w->blockIdx             , sizeof (w->blockIdx)             , "n/a");
          snprintf (w->active_physical_pc   , sizeof (w->active_physical_pc   ), "n/a");
        }

      ++*num_warps;
    }

  cuda_iterator_destroy (iter);
}

static void
cuda_info_warps_destroy (cuda_info_warp_t *warps)
{
  xfree (warps);
}

void
info_cuda_warps_command (char *arg)
{
  cuda_info_warp_t *warps, *w;
  uint32_t i, num_warps, current_device, current_sm;
  struct cleanup *table_chain, *row_chain;
  struct { uint32_t current, wp, active_lanes_mask, divergent_lanes_mask, active_physical_pc, kernel_id, blockIdx; } width;

  /* column headers */
  const char *header_current              = " ";
  const char *header_wp                   = "Wp";
  const char *header_active_lanes_mask    = "Active Lanes Mask";
  const char *header_divergent_lanes_mask = "Divergent Lanes Mask";
  const char *header_active_physical_pc   = "Active Physical PC";
  const char *header_kernel_id            = "Kernel";
  const char *header_blockIdx             = "BlockIdx";

  /* get the information */
  cuda_info_warps (arg, &warps, &num_warps);

  /* output message if the list is empty */
  if (num_warps == 0 && !ui_out_is_mi_like_p (uiout))
    {
      ui_out_field_string (uiout, NULL, _("No CUDA Warps.\n"));
      return;
    }

  /* column widths */
  width.current              = strlen (header_current);
  width.wp                   = strlen (header_wp);
  width.active_lanes_mask    = strlen (header_active_lanes_mask);
  width.divergent_lanes_mask = strlen (header_divergent_lanes_mask);
  width.active_physical_pc   = strlen (header_active_physical_pc);
  width.kernel_id            = strlen (header_kernel_id);
  width.blockIdx             = strlen (header_blockIdx);

  width.active_lanes_mask    = max (width.active_lanes_mask, 10);
  width.divergent_lanes_mask = max (width.divergent_lanes_mask, 10);
  width.active_physical_pc   = max (width.active_physical_pc, 18);
  for (w = warps, i = 0;i < num_warps; ++i, ++w)
    width.blockIdx = max (width.blockIdx, strlen (w->blockIdx));

  /* print table header */
  table_chain = make_cleanup_ui_out_table_begin_end (uiout, 7, num_warps, "InfoCudaWarpsTable");
  ui_out_table_header (uiout, width.current             , ui_right, "current"             , header_current);
  ui_out_table_header (uiout, width.wp                  , ui_right, "warp"                , header_wp);
  ui_out_table_header (uiout, width.active_lanes_mask   , ui_right, "active_lanes_mask"   , header_active_lanes_mask);
  ui_out_table_header (uiout, width.divergent_lanes_mask, ui_right, "divergent_lanes_mask", header_divergent_lanes_mask);
  ui_out_table_header (uiout, width.active_physical_pc  , ui_right, "active_physical_pc"  , header_active_physical_pc);
  ui_out_table_header (uiout, width.kernel_id           , ui_right, "kernel"              , header_kernel_id);
  ui_out_table_header (uiout, width.blockIdx            , ui_right, "blockIdx"            , header_blockIdx);
  ui_out_table_body (uiout);

  /* print table rows */
  for (w = warps, i = 0, current_device = -1, current_sm = -1; i < num_warps; ++i, ++w)
    {

      if (!ui_out_is_mi_like_p (uiout) &&
          (w->device != current_device || w->sm != current_sm))
        {
          ui_out_message (uiout, 0, "Device %u SM %u\n", w->device, w->sm);
          current_device = w->device;
          current_sm     = w->sm;
        }

      row_chain = make_cleanup_ui_out_tuple_begin_end (uiout, "InfoCudaWarpsRow");
      ui_out_field_string (uiout, "current"             , w->current ? "*" : " ");
      ui_out_field_int    (uiout, "warp"                , w->wp);
      ui_out_field_string (uiout, "active_lanes_mask"   , w->active_lanes_mask);
      ui_out_field_string (uiout, "divergent_lanes_mask", w->divergent_lanes_mask);
      ui_out_field_string (uiout, "active_physical_pc"  , w->active_physical_pc);
      ui_out_field_string (uiout, "kernel"              , w->kernel_id);
      ui_out_field_string (uiout, "blockIdx"            , w->blockIdx);
      ui_out_text         (uiout, "\n");
      do_cleanups (row_chain);
    }

  do_cleanups (table_chain);

  gdb_flush (gdb_stdout);

  cuda_info_warps_destroy (warps);
}

typedef struct {
  bool     current;
  char     state[20];
  uint32_t device;
  uint32_t sm;
  uint32_t wp;
  uint32_t ln;
  char     physical_pc[19];
  char     threadIdx[32];
} cuda_info_lane_t;

static void
cuda_info_lanes (char *filter_string, cuda_info_lane_t **lanes, uint32_t *num_lanes)
{
  CuDim3 threadIdx;
  uint32_t num_elements;
  cuda_iterator iter;
  cuda_coords_t default_filter, c, filter;
  cuda_info_lane_t *l;
  uint64_t physical_pc;
  bool active;

  /* sanity checks */
  gdb_assert (lanes);
  gdb_assert (num_lanes);

  /* set the filter */
  default_filter = CUDA_WILDCARD_COORDS;
  default_filter.dev = CUDA_CURRENT;
  default_filter.sm  = CUDA_CURRENT;
  default_filter.wp  = CUDA_CURRENT;
  filter = cuda_build_filter (filter_string, &default_filter);

  /* get the list of lanes */
  iter = cuda_iterator_create (CUDA_ITERATOR_TYPE_LANES, &filter, CUDA_SELECT_ALL);
  num_elements = cuda_iterator_get_size (iter);
  *lanes = xmalloc (num_elements * sizeof (**lanes));
  *num_lanes = 0;

  /* compile the needed info for each device */
  for (cuda_iterator_start (iter), l = *lanes;
       !cuda_iterator_end (iter);
       cuda_iterator_next (iter), ++l)
    {
      c  = cuda_iterator_get_current (iter);

      l->current     = cuda_coords_is_current (&c);
      l->device      = c.dev;
      l->sm          = c.sm;
      l->wp          = c.wp;
      l->ln          = c.ln;

      if (lane_is_valid (c.dev, c.sm, c.wp, c.ln))
        {
          active      = lane_is_active (c.dev, c.sm, c.wp, c.ln);
          threadIdx   = lane_get_thread_idx (c.dev, c.sm, c.wp,c .ln);
          physical_pc = lane_get_pc (c.dev, c.sm, c.wp, c.ln);

          snprintf (l->state      , sizeof (l->state)      , "%s", active ? "active" : "divergent");
          snprintf (l->threadIdx  , sizeof (l->threadIdx)  , "(%u,%u,%u)", threadIdx.x, threadIdx.y, threadIdx.z);
          snprintf (l->physical_pc, sizeof (l->physical_pc), "0x%016"PRIx64, physical_pc);
        }
      else
        {
          snprintf (l->state      , sizeof (l->state)      , "inactive");
          snprintf (l->threadIdx  , sizeof (l->threadIdx)  , "n/a");
          snprintf (l->physical_pc, sizeof (l->physical_pc), "n/a");
        }

      ++*num_lanes;
    }

  cuda_iterator_destroy (iter);
}

static void
cuda_info_lanes_destroy (cuda_info_lane_t *lanes)
{
  xfree (lanes);
}

void
info_cuda_lanes_command (char *arg)
{
  cuda_info_lane_t *lanes, *l;
  uint32_t i, num_lanes, current_device, current_sm, current_wp;
  struct cleanup *table_chain, *row_chain;
  struct { uint32_t current, ln, state, physical_pc, thread_idx; } width;

  /* column headers */
  const char *header_current     = " ";
  const char *header_ln          = "Ln";
  const char *header_state       = "State";
  const char *header_physical_pc = "Physical PC";
  const char *header_thread_idx  = "ThreadIdx";

  /* get the information */
  cuda_info_lanes (arg, &lanes, &num_lanes);

  /* output message if the list is empty */
  if (num_lanes == 0 && !ui_out_is_mi_like_p (uiout))
    {
      ui_out_field_string (uiout, NULL, _("No CUDA Lanes.\n"));
      return;
    }

  /* column widths */
  width.current     = strlen (header_current);
  width.ln          = strlen (header_ln);
  width.state       = strlen (header_state);
  width.physical_pc = strlen (header_physical_pc);
  width.thread_idx  = strlen (header_thread_idx);

  width.state       = max (width.state, strlen ("divergent"));
  width.physical_pc = max (width.physical_pc, 18);
  for (l = lanes, i = 0; i < num_lanes; ++i, ++l)
    width.thread_idx = max (width.thread_idx, strlen (l->threadIdx));

  /* print table header */
  table_chain = make_cleanup_ui_out_table_begin_end (uiout, 5, num_lanes, "InfoCudaLanesTable");
  ui_out_table_header (uiout, width.current     , ui_right, "current"     , header_current);
  ui_out_table_header (uiout, width.ln          , ui_right, "lane"        , header_ln);
  ui_out_table_header (uiout, width.state       , ui_right, "state"       , header_state);
  ui_out_table_header (uiout, width.physical_pc , ui_right, "physical_pc" , header_physical_pc);
  ui_out_table_header (uiout, width.thread_idx  , ui_right, "threadIdx"   , header_thread_idx);
  ui_out_table_body (uiout);

  /* print table rows */
  for (l = lanes, i = 0, current_device = -1, current_sm = -1, current_wp = -1; i < num_lanes; ++i, ++l)
    {
      if (!ui_out_is_mi_like_p (uiout) &&
          (l->device != current_device || l->sm != current_sm || l->wp != current_wp))
        {
          ui_out_message (uiout, 0, "Device %u SM %u Warp %u\n", l->device, l->sm, l->wp);
          current_device = l->device;
          current_sm     = l->sm;
          current_wp     = l->wp;
        }

      row_chain = make_cleanup_ui_out_tuple_begin_end (uiout, "InfoCudaLanesRow");
      ui_out_field_string (uiout, "current"    , l->current ? "*" : " ");
      ui_out_field_int    (uiout, "lane"       , l->ln);
      ui_out_field_string (uiout, "state"      , l->state);
      ui_out_field_string (uiout, "physical_pc", l->physical_pc);
      ui_out_field_string (uiout, "threadIdx"  , l->threadIdx);
      ui_out_text         (uiout, "\n");
      do_cleanups (row_chain);
    }

  do_cleanups (table_chain);

  gdb_flush (gdb_stdout);

  cuda_info_lanes_destroy (lanes);
}

static void
info_cuda_kernels_print_frame_args (kernel_t kernel)
{
  cuda_coords_t *coords, requested, candidates[CK_MAX];
  struct frame_info *prev_frame, *frame;

  /* Find an active lane for the kernel */
  requested = CUDA_WILDCARD_COORDS;
  requested.kernelId = kernel_get_id (kernel);
  cuda_coords_find_valid (requested, candidates, CUDA_SELECT_VALID);
  coords = &candidates[CK_EXACT_LOGICAL];
  if (!cuda_coords_equal (&requested, coords))
    return;

  /* Switch focus and ELF image to that lane/kernel, temporarily */
  kernel_load_elf_images (kernel);
  switch_to_cuda_thread (coords);

  /* Find the outermost frame */
  frame = get_current_frame ();
  while ((prev_frame = get_prev_frame (frame)))
    frame = prev_frame;

  /* Print the arguments */
  print_args_frame (frame);
}

typedef struct {
  bool           current;
  kernel_t       kernel;
  uint32_t       kernel_id;
  uint32_t       device;
  uint32_t       grid_id;
  uint32_t       sms_mask;
  char           grid_dim[32];
  char           block_dim[32];
  const char*    name;
} cuda_info_kernel_t;

static void
cuda_info_kernels_build (char *filter_string, cuda_info_kernel_t **kernels, uint32_t *num_kernels)
{
  uint32_t num_elements;
  cuda_iterator iter;
  kernel_t kernel;
  cuda_coords_t default_filter, c, filter;
  cuda_info_kernel_t *k;
  CuDim3 grid_dim;
  CuDim3 block_dim;

  /* sanity checks */
  gdb_assert (kernels);
  gdb_assert (num_kernels);

  /* get the filter */
  default_filter = CUDA_WILDCARD_COORDS;
  filter = cuda_build_filter (filter_string, &default_filter);

  /* get the list of kernels */
  iter = cuda_iterator_create (CUDA_ITERATOR_TYPE_KERNELS, &filter, CUDA_SELECT_VALID);
  num_elements = cuda_iterator_get_size (iter);
  *kernels = xmalloc (num_elements * sizeof (**kernels));
  *num_kernels = 0;

  /* compile the needed info for each kernel */
  for (cuda_iterator_start (iter), k = *kernels;
       !cuda_iterator_end (iter);
       cuda_iterator_next (iter), ++k)
    {
      c  = cuda_iterator_get_current (iter);
      kernel = device_find_kernel_by_grid_id (c.dev, c.gridId);

      k->kernel      = kernel;
      k->current     = cuda_coords_is_current (&c);
      k->kernel_id   = kernel_get_id (kernel);
      k->name        = kernel_get_name (kernel);
      k->device      = c.dev;
      k->grid_id     = c.gridId;
      k->sms_mask    = kernel_compute_sms_mask (kernel);

      grid_dim = kernel_get_grid_dim (kernel);
      snprintf(k->grid_dim, sizeof (k->grid_dim), "(%u,%u,%u)", grid_dim.x, grid_dim.y, grid_dim.z);
      block_dim = kernel_get_block_dim (kernel);
      snprintf(k->block_dim, sizeof (k->block_dim), "(%u,%u,%u)", block_dim.x, block_dim.y, block_dim.z);

      ++*num_kernels;
    }

  cuda_iterator_destroy (iter);
}

static void
cuda_info_kernels_destroy (cuda_info_kernel_t *kernels)
{
  xfree (kernels);
}

void
info_cuda_kernels_command (char *arg)
{
  cuda_info_kernel_t *kernels, *k;
  uint32_t i, num_kernels;
  struct cleanup *table_chain, *row_chain;
  struct { uint32_t current, kernel, device, grid, sms_mask, grid_dim, block_dim, name, args; } width;

  /* column headers */
  const char *header_current   = " ";
  const char *header_kernel    = "Kernel";
  const char *header_device    = "Dev";
  const char *header_grid      = "Grid";
  const char *header_sms_mask  = "SMs Mask";
  const char *header_grid_dim  = "GridDim";
  const char *header_block_dim = "BlockDim";
  const char *header_name      = "Name";
  const char *header_args      = "Args";

  /* get the information */
  cuda_info_kernels_build (arg, &kernels, &num_kernels);

  /* output message if the list is empty */
  if (num_kernels == 0 && !ui_out_is_mi_like_p (uiout))
    {
      ui_out_field_string (uiout, NULL, _("No CUDA kernels.\n"));
      return;
    }

  /* column widths */
  width.current   = strlen (header_current);
  width.kernel    = strlen (header_kernel);
  width.device    = strlen (header_device);
  width.grid      = strlen (header_grid);
  width.sms_mask  = strlen (header_sms_mask);
  width.grid_dim  = strlen (header_grid_dim);
  width.block_dim = strlen (header_block_dim);
  width.name      = strlen (header_name);
  width.args      = strlen (header_args);

  for (k = kernels, i = 0; i < num_kernels; ++i, ++k)
    {
      width.sms_mask  = max (width.sms_mask, 10);
      width.grid_dim  = max (width.grid_dim,  strlen (k->grid_dim));
      width.block_dim = max (width.block_dim, strlen (k->block_dim));
      width.name      = max (width.name, strlen (k->name));
    }

  /* print table header */
  table_chain = make_cleanup_ui_out_table_begin_end (uiout, 9, num_kernels, "InfoCudaKernelsTable");
  ui_out_table_header (uiout, width.current  , ui_right, "current"  , header_current);
  ui_out_table_header (uiout, width.kernel   , ui_right, "kernel"   , header_kernel);
  ui_out_table_header (uiout, width.device   , ui_right, "device"   , header_device);
  ui_out_table_header (uiout, width.grid     , ui_right, "grid"     , header_grid);
  ui_out_table_header (uiout, width.sms_mask , ui_right, "sms_mask" , header_sms_mask);
  ui_out_table_header (uiout, width.grid_dim , ui_right, "gridDim"  , header_grid_dim);
  ui_out_table_header (uiout, width.block_dim, ui_right, "blockDim" , header_block_dim);
  ui_out_table_header (uiout, width.name     , ui_right, "name"     , header_name);
  ui_out_table_header (uiout, width.args     , ui_left , "arguments", header_args);
  ui_out_table_body (uiout);

  /* print table rows */
  for (k = kernels, i = 0; i < num_kernels; ++i, ++k)
    {
      row_chain = make_cleanup_ui_out_tuple_begin_end (uiout, "InfoCudaKernelsRow");
      ui_out_field_string (uiout, "current"   , k->current ? "*" : " ");
      ui_out_field_int    (uiout, "kernel"    , k->kernel_id);
      ui_out_field_int    (uiout, "device"    , k->device);
      ui_out_field_int    (uiout, "grid"      , k->grid_id);
      ui_out_field_fmt    (uiout, "sms_mask"  , "0x%08x", k->sms_mask);
      ui_out_field_string (uiout, "gridDim"   , k->grid_dim);
      ui_out_field_string (uiout, "blockDim"  , k->block_dim);
      ui_out_field_string (uiout, "name"      , k->name);
      info_cuda_kernels_print_frame_args (k->kernel);
      ui_out_text         (uiout, "\n");
      do_cleanups (row_chain);
    }

  do_cleanups (table_chain);

  gdb_flush (gdb_stdout);

  cuda_info_kernels_destroy (kernels);
}

typedef struct {
  bool           current;
  kernel_t       kernel;
  uint64_t       kernel_id;
  CuDim3         start_block_idx;
  CuDim3         end_block_idx;
  const char    *kernel_name;
  const char    *kernel_dim;
  char           start_block_idx_string[32];
  char           end_block_idx_string[32];
  uint32_t       count;
  uint32_t       device;
  uint32_t       sm;
} cuda_info_block_t;

static void
cuda_info_blocks_build (char *filter_string, cuda_info_block_t **blocks, uint32_t *num_blocks)
{
  uint32_t i, num_elements;
  cuda_iterator iter;
  cuda_coords_t default_filter, filter, c, expected;
  CuDim3 prev_block_idx = { CUDA_INVALID, CUDA_INVALID, CUDA_INVALID };
  kernel_t kernel;
  cuda_info_block_t *b;
  bool first_entry, break_of_contiguity;

  /* sanity checks */
  gdb_assert (blocks);
  gdb_assert (num_blocks);

  /* get the filter */
  default_filter = CUDA_WILDCARD_COORDS;
  default_filter.kernelId = CUDA_CURRENT;
  filter = cuda_build_filter (filter_string, &default_filter);

  /* get the list of blocks */
  iter = cuda_iterator_create (CUDA_ITERATOR_TYPE_BLOCKS, &filter, CUDA_SELECT_VALID);
  num_elements = cuda_iterator_get_size (iter);
  *blocks = xmalloc (num_elements * sizeof (**blocks));
  *num_blocks = 0;

  /* compile the needed info for each block */
  for (cuda_iterator_start (iter), first_entry = true, i = 0, b = *blocks;
       !cuda_iterator_end (iter);
       cuda_iterator_next (iter),  first_entry = false, ++i)
    {
      c  = cuda_iterator_get_current (iter);
      kernel = device_find_kernel_by_grid_id (c.dev, c.gridId);

      /* data for the current iteration */
      break_of_contiguity = cuda_coords_compare_logical (&expected, &c) != 0;

      /* close the current range */
      if (!first_entry && (break_of_contiguity || !cuda_options_coalescing ()))
        {
          b->end_block_idx = prev_block_idx;
          snprintf (b->end_block_idx_string, sizeof (b->end_block_idx_string),
                    "(%u,%u,%u)", prev_block_idx.x, prev_block_idx.y, prev_block_idx.z);
          ++b;
          ++*num_blocks;
        }

      /* start a new range */
      if (first_entry || break_of_contiguity || !cuda_options_coalescing ())
        {
          b->kernel          = kernel;
          b->current         = false;
          b->start_block_idx = c.blockIdx;
          b->count           = 0;
          b->kernel_id       = kernel_get_id (kernel);
          b->kernel_name     = kernel_get_name (kernel);
          b->kernel_dim      = kernel_get_dimensions (kernel);
          b->device          = c.dev;
          b->sm              = c.sm;
          snprintf (b->start_block_idx_string, sizeof (b->start_block_idx_string),
                    "(%u,%u,%u)", c.blockIdx.x, c.blockIdx.y, c.blockIdx.z);
        }

      /* update the current range */
      b->current |= cuda_coords_is_current (&c);
      ++b->count;

      /* data for the next iteration */
      prev_block_idx = c.blockIdx;
      expected = CUDA_WILDCARD_COORDS;
      expected.kernelId = c.kernelId;
      expected.blockIdx = c.blockIdx;
      cuda_coords_increment_block (&expected, kernel_get_grid_dim (kernel));
    }

  /* close the last range */
  if (num_elements > 0)
    {
      b->end_block_idx = c.blockIdx;
      snprintf (b->end_block_idx_string, sizeof (b->end_block_idx_string),
                "(%u,%u,%u)", c.blockIdx.x, c.blockIdx.y, c.blockIdx.z);
      ++*num_blocks;
    }

  cuda_iterator_destroy (iter);
}

static void
cuda_info_blocks_destroy (cuda_info_block_t *blocks)
{
  xfree (blocks);
}

static void
info_cuda_blocks_print_uncoalesced (cuda_info_block_t *blocks, uint32_t num_blocks)
{
  cuda_info_block_t *b;
  uint32_t i, num_columns;
  uint64_t kernel_id;
  struct cleanup *table_chain, *row_chain;
  struct { uint32_t current, kernel, block_idx, state, device, sm; } width;

  /* column headers */
  const char *header_current   = " ";
  const char *header_kernel    = "Kernel";
  const char *header_block_idx = "BlockIdx";
  const char *header_state     = "State";
  const char *header_device    = "Dev";
  const char *header_sm        = "SM";

  /* sanity checks */
  gdb_assert (!cuda_options_coalescing ());

  /* output message if the list is empty */
  if (num_blocks == 0 && !ui_out_is_mi_like_p (uiout))
    {
      ui_out_field_string (uiout, NULL, _("No CUDA blocks.\n"));
      return;
    }

  /* column widths */
  width.current   = strlen (header_current);
  width.kernel    = strlen (header_kernel);
  width.block_idx = strlen (header_block_idx);
  width.state     = strlen (header_state);
  width.device    = strlen (header_device);
  width.sm        = strlen (header_sm);

  width.state = max (width.state, sizeof ("running") - 1);
  for (b = blocks, i = 0; i < num_blocks; ++i, ++b)
    width.block_idx = max (width.block_idx, strlen (b->start_block_idx_string));

  /* print table header ('kernel' is only present in MI output) */
  num_columns = ui_out_is_mi_like_p (uiout) ? 6 : 5;
  table_chain = make_cleanup_ui_out_table_begin_end (uiout, num_columns, num_blocks, "UncoalescedInfoCudaBlocksTable");
  ui_out_table_header (uiout, width.current  , ui_right, "current"  , header_current);
  if (ui_out_is_mi_like_p (uiout))
    ui_out_table_header (uiout, width.kernel , ui_right, "kernel"   , header_kernel);
  ui_out_table_header (uiout, width.block_idx, ui_right, "blockIdx" , header_block_idx);
  ui_out_table_header (uiout, width.state    , ui_right, "state"    , header_state);
  ui_out_table_header (uiout, width.device   , ui_right, "device"   , header_device);
  ui_out_table_header (uiout, width.sm       , ui_right, "sm"       , header_sm);
  ui_out_table_body (uiout);

  /* print table rows */
  for (b = blocks, i = 0, kernel_id = ~0ULL; i < num_blocks; ++i, ++b)
    {
      if (!ui_out_is_mi_like_p (uiout) && b->kernel_id != kernel_id)
        {
          /* row are grouped per kernel only in CLI output */
          ui_out_message (uiout, 0, "Kernel %"PRIu64"\n", b->kernel_id),
          kernel_id = b->kernel_id;
        }

      row_chain = make_cleanup_ui_out_tuple_begin_end (uiout, "UncoalescedInfoCudaBlocksRow");
      ui_out_field_string (uiout, "current" , b->current ? "*" : " ");
      if (ui_out_is_mi_like_p (uiout))
        ui_out_field_int  (uiout, "kernel"  , b->kernel_id);
      ui_out_field_string (uiout, "blockIdx", b->start_block_idx_string);
      ui_out_field_string (uiout, "state"   , "running");
      ui_out_field_int    (uiout, "device"  , b->device);
      ui_out_field_int    (uiout, "sm"      , b->sm);
      ui_out_text         (uiout, "\n");
      do_cleanups (row_chain);
    }

  do_cleanups (table_chain);
}

static void
info_cuda_blocks_print_coalesced (cuda_info_block_t *blocks, uint32_t num_blocks)
{
  cuda_info_block_t *b;
  uint32_t i, num_columns;
  uint64_t kernel_id;
  struct cleanup *table_chain, *row_chain;
  struct { uint32_t current, kernel, from, to, count, state; } width;

  /* column headers */
  const char *header_current   = " ";
  const char *header_kernel    = "Kernel";
  const char *header_from      = "BlockIdx";
  const char *header_to        = "To BlockIdx";
  const char *header_count     = "Count";
  const char *header_state     = "State";

  /* sanity checks */
  gdb_assert (cuda_options_coalescing ());

  /* output message if the list is empty */
  if (num_blocks == 0 && !ui_out_is_mi_like_p (uiout))
    {
      ui_out_field_string (uiout, NULL, _("No CUDA blocks.\n"));
      return;
    }

  /* column widths */
  width.current   = strlen (header_current);
  width.kernel    = strlen (header_kernel);
  width.from      = strlen (header_from);
  width.to        = strlen (header_to);
  width.count     = strlen (header_count);
  width.state     = strlen (header_state);

  width.state = max (width.state, sizeof ("running") - 1);
  for (b = blocks, i = 0; i < num_blocks; ++i, ++b)
    {
      width.from = max (width.from, strlen (b->start_block_idx_string));
      width.to   = max (width.to  , strlen (b->end_block_idx_string));
    }

  /* print table header ('kernel' is only present in MI output) */
  num_columns = ui_out_is_mi_like_p (uiout) ? 6 : 5;
  table_chain = make_cleanup_ui_out_table_begin_end (uiout, num_columns, num_blocks, "CoalescedInfoCudaBlocksTable");
  ui_out_table_header (uiout, width.current  , ui_right, "current"  , header_current);
  if (ui_out_is_mi_like_p (uiout))
    ui_out_table_header (uiout, width.kernel , ui_right, "kernel"   , header_kernel);
  ui_out_table_header (uiout, width.from     , ui_right, "from"     , header_from);
  ui_out_table_header (uiout, width.to       , ui_right, "to"       , header_to);
  ui_out_table_header (uiout, width.count    , ui_right, "count"    , header_count);
  ui_out_table_header (uiout, width.state    , ui_right, "state"    , header_state);
  ui_out_table_body (uiout);

  /* print table rows */
  for (b = blocks, i = 0, kernel_id = ~0ULL; i < num_blocks; ++i, ++b)
    {
      if (!ui_out_is_mi_like_p (uiout) && b->kernel_id != kernel_id)
        {
          /* row are grouped per kernel only in CLI output */
          ui_out_message (uiout, 0, "Kernel %"PRIu64"\n", b->kernel_id),
          kernel_id = b->kernel_id;
        }

      row_chain = make_cleanup_ui_out_tuple_begin_end (uiout, "CoalescedInfoCudaBlocksRow");
      ui_out_field_string (uiout, "current" , b->current ? "*" : " ");
      if (ui_out_is_mi_like_p (uiout))
        ui_out_field_int  (uiout, "kernel"  , b->kernel_id);
      ui_out_field_string (uiout, "from"    , b->start_block_idx_string);
      ui_out_field_string (uiout, "to"      , b->end_block_idx_string);
      ui_out_field_int    (uiout, "count"   , b->count);
      ui_out_field_string (uiout, "state"   , "running");
      ui_out_text         (uiout, "\n");
      do_cleanups (row_chain);
    }

  do_cleanups (table_chain);
}

void
info_cuda_blocks_command (char *arg)
{
  cuda_info_block_t *blocks;
  uint32_t num_blocks;

  cuda_info_blocks_build (arg, &blocks, &num_blocks);

  if (cuda_options_coalescing ())
    info_cuda_blocks_print_coalesced (blocks, num_blocks);
  else
    info_cuda_blocks_print_uncoalesced (blocks, num_blocks);

  gdb_flush (gdb_stdout);

  cuda_info_blocks_destroy (blocks);
}


typedef struct {
  bool           current;
  kernel_t       kernel;
  uint64_t       kernel_id;
  uint64_t       pc;
  char           filename[256];
  uint32_t       line;
  CuDim3         start_block_idx;
  CuDim3         start_thread_idx;
  CuDim3         end_block_idx;
  CuDim3         end_thread_idx;
  uint32_t       count;
  const char    *kernel_name;
  const char    *kernel_dim;
  char           start_block_idx_string[32];
  char           start_thread_idx_string[32];
  char           end_block_idx_string[32];
  char           end_thread_idx_string[32];
  uint32_t       device;
  uint32_t       sm;
  uint32_t       wp;
  uint32_t       ln;
} cuda_info_thread_t;

static void
cuda_info_threads_build (char *filter_string, cuda_info_thread_t **threads, uint32_t *num_threads)
{
  uint32_t i, num_elements;
  uint64_t pc = 0, prev_pc = 0;
  cuda_iterator iter;
  cuda_coords_t default_filter, filter, c, expected;
  CuDim3 prev_block_idx = { CUDA_INVALID, CUDA_INVALID, CUDA_INVALID };
  CuDim3 prev_thread_idx = { CUDA_INVALID, CUDA_INVALID, CUDA_INVALID };
  kernel_t kernel, prev_kernel = 0;
  cuda_info_thread_t *t;
  struct symtab_and_line sal;
  struct symtab_and_line prev_sal;
  bool first_entry, break_of_contiguity;
  struct value_print_options opts;

  /* sanity checks */
  gdb_assert (threads);
  gdb_assert (num_threads);

  /* make valgrind not complain */
  expected = CUDA_INVALID_COORDS;
  init_sal (&sal);
  init_sal (&prev_sal);

  /* get the print options */
  get_user_print_options (&opts);

  /* get the filter */
  default_filter = CUDA_WILDCARD_COORDS;
  default_filter.kernelId = CUDA_CURRENT;
  filter = cuda_build_filter (filter_string, &default_filter);

  /* get the list of threads */
  iter = cuda_iterator_create (CUDA_ITERATOR_TYPE_THREADS, &filter, CUDA_SELECT_VALID);
  num_elements = cuda_iterator_get_size (iter);
  *threads = xmalloc (num_elements * sizeof (**threads));
  *num_threads = 0;

  /* compile the needed info for each block */
  for (cuda_iterator_start (iter), first_entry = true, i = 0, t = *threads;
       !cuda_iterator_end (iter);
       cuda_iterator_next (iter),  first_entry = false, ++i)
    {
      c  = cuda_iterator_get_current (iter);
      kernel = device_find_kernel_by_grid_id (c.dev, c.gridId);
      if (kernel != prev_kernel)
        kernel_load_elf_images (kernel);
      pc = lane_get_virtual_pc (c.dev, c.sm, c.wp, c.ln);
      if (pc != prev_pc) /* optimization */
        sal = find_pc_line (pc, 0);

      /* data for the current iteration */
      break_of_contiguity =
        (cuda_coords_compare_logical (&expected, &c) != 0) ||
        (opts.addressprint && pc != prev_pc) ||
        (!opts.addressprint && sal.line != prev_sal.line);

      /* close the current range */
      if (!first_entry && (break_of_contiguity || !cuda_options_coalescing ()))
        {
          t->end_block_idx  = prev_block_idx;
          t->end_thread_idx = prev_thread_idx;
          snprintf (t->end_block_idx_string, sizeof (t->end_block_idx_string),
                    "(%u,%u,%u)", prev_block_idx.x, prev_block_idx.y, prev_block_idx.z);
          snprintf (t->end_thread_idx_string, sizeof (t->end_thread_idx_string),
                    "(%u,%u,%u)", prev_thread_idx.x, prev_thread_idx.y, prev_thread_idx.z);
          ++t;
          ++*num_threads;
        }

      /* start a new range */
      if (first_entry || break_of_contiguity || !cuda_options_coalescing ())
        {
          t->kernel           = kernel;
          t->current          = false;
          t->pc               = pc;
          t->line             = sal.line;
          t->start_block_idx  = c.blockIdx;
          t->start_thread_idx = c.threadIdx;
          t->count            = 0;
          t->kernel_id        = kernel_get_id (kernel);
          t->kernel_name      = kernel_get_name (kernel);
          t->kernel_dim       = kernel_get_dimensions (kernel);
          t->device           = c.dev;
          t->sm               = c.sm;
          t->wp               = c.wp;
          t->ln               = c.ln;
          snprintf (t->filename, sizeof (t->filename),
                    "%s", sal.symtab ? sal.symtab->filename : "n/a");
          snprintf (t->start_block_idx_string, sizeof (t->start_block_idx_string),
                    "(%u,%u,%u)", c.blockIdx.x, c.blockIdx.y, c.blockIdx.z);
          snprintf (t->start_thread_idx_string, sizeof (t->start_thread_idx_string),
                    "(%u,%u,%u)", c.threadIdx.x, c.threadIdx.y, c.threadIdx.z);
        }

      /* update the current range */
      t->current |= cuda_coords_is_current (&c);
      ++t->count;

      /* data for the next iteration */
      prev_kernel = kernel;
      prev_pc  = pc;
      prev_sal = sal;
      prev_block_idx  = c.blockIdx;
      prev_thread_idx = c.threadIdx;
      expected = CUDA_WILDCARD_COORDS;
      expected.kernelId  = c.kernelId;
      expected.blockIdx  = c.blockIdx;
      expected.threadIdx = c.threadIdx;
      cuda_coords_increment_thread (&expected, kernel_get_grid_dim (kernel),
                                    kernel_get_block_dim (kernel));
    }

  /* close the last range */
  if (num_elements > 0)
    {
      t->end_block_idx  = c.blockIdx;
      t->end_thread_idx = c.threadIdx;
      snprintf (t->end_block_idx_string, sizeof (t->end_block_idx_string),
                "(%u,%u,%u)", c.blockIdx.x, c.blockIdx.y, c.blockIdx.z);
      snprintf (t->end_thread_idx_string, sizeof (t->end_thread_idx_string),
                "(%u,%u,%u)", c.threadIdx.x, c.threadIdx.y, c.threadIdx.z);
      ++*num_threads;
    }

  cuda_iterator_destroy (iter);
}

static void
cuda_info_threads_destroy (cuda_info_thread_t *threads)
{
  xfree (threads);
}

static void
info_cuda_threads_print_uncoalesced (cuda_info_thread_t *threads, uint32_t num_threads)
{
  cuda_info_thread_t *b;
  uint32_t i, num_columns;
  uint64_t kernel_id;
  struct cleanup *table_chain, *row_chain;
  struct { uint32_t current, kernel, block_idx, thread_idx, pc, device, sm, wp, ln, filename, line; } width;

  /* column headers */
  const char *header_current    = " ";
  const char *header_kernel     = "Kernel";
  const char *header_block_idx  = "BlockIdx";
  const char *header_thread_idx = "ThreadIdx";
  const char *header_pc         = "Virtual PC";
  const char *header_device     = "Dev";
  const char *header_sm         = "SM";
  const char *header_warp       = "Wp";
  const char *header_lane       = "Ln";
  const char *header_filename   = "Filename";
  const char *header_line       = "Line";

  /* sanity checks */
  gdb_assert (!cuda_options_coalescing ());

  /* output message if the list is empty */
  if (num_threads == 0 && !ui_out_is_mi_like_p (uiout))
    {
      ui_out_field_string (uiout, NULL, _("No CUDA threads.\n"));
      return;
    }

  /* column widths */
  width.current    = strlen (header_current);
  width.kernel     = strlen (header_kernel);
  width.block_idx  = strlen (header_block_idx);
  width.thread_idx = strlen (header_thread_idx);
  width.pc         = strlen (header_pc);
  width.device     = strlen (header_device);
  width.sm         = strlen (header_sm);
  width.wp         = strlen (header_warp);
  width.ln         = strlen (header_lane);
  width.filename   = strlen (header_filename);
  width.line       = strlen (header_line);

  for (b = threads, i = 0; i < num_threads; ++i, ++b)
    {
      width.pc         = max (width.pc, 18);
      width.block_idx  = max (width.block_idx, strlen (b->start_block_idx_string));
      width.thread_idx = max (width.thread_idx, strlen (b->start_thread_idx_string));
      width.filename   = max (width.filename, strlen (b->filename));
      width.line       = max (width.line, 5);
    }

  /* print table header ('kernel' is only present in MI output) */
  num_columns = ui_out_is_mi_like_p (uiout) ? 11 : 10;
  table_chain = make_cleanup_ui_out_table_begin_end (uiout, num_columns, num_threads, "UncoalescedInfoCudaThreadsTable");
  ui_out_table_header (uiout, width.current   , ui_right, "current"   , header_current);
  if (ui_out_is_mi_like_p (uiout))
    ui_out_table_header (uiout, width.kernel  , ui_right, "kernel"    , header_kernel);
  ui_out_table_header (uiout, width.block_idx , ui_right, "blockIdx"  , header_block_idx);
  ui_out_table_header (uiout, width.thread_idx, ui_right, "threadIdx" , header_thread_idx);
  ui_out_table_header (uiout, width.pc        , ui_right, "virtual_pc", header_pc);
  ui_out_table_header (uiout, width.device    , ui_right, "device"    , header_device);
  ui_out_table_header (uiout, width.sm        , ui_right, "sm"        , header_sm);
  ui_out_table_header (uiout, width.wp        , ui_right, "warp"      , header_warp);
  ui_out_table_header (uiout, width.ln        , ui_right, "lane"      , header_lane);
  ui_out_table_header (uiout, width.filename  , ui_right, "filename"  , header_filename);
  ui_out_table_header (uiout, width.line      , ui_right, "line"      , header_line);
  ui_out_table_body (uiout);

  /* print table rows */
  for (b = threads, i = 0, kernel_id = ~0ULL; i < num_threads; ++i, ++b)
    {
      if (!ui_out_is_mi_like_p (uiout) && b->kernel_id != kernel_id)
        {
          /* row are grouped per kernel only in CLI output */
          ui_out_message (uiout, 0, "Kernel %"PRIu64"\n", b->kernel_id),
          kernel_id = b->kernel_id;
        }

      row_chain = make_cleanup_ui_out_tuple_begin_end (uiout, "UncoalescedInfoCudaThreadsRow");
      ui_out_field_string (uiout, "current"   , b->current ? "*" : " ");
      if (ui_out_is_mi_like_p (uiout))
        ui_out_field_int  (uiout, "kernel"    , b->kernel_id);
      ui_out_field_string (uiout, "blockIdx"  , b->start_block_idx_string);
      ui_out_field_string (uiout, "threadIdx" , b->start_thread_idx_string);
      ui_out_field_fmt    (uiout, "virtual_pc", "0x%016"PRIx64, b->pc);
      ui_out_field_int    (uiout, "device"    , b->device);
      ui_out_field_int    (uiout, "sm"        , b->sm);
      ui_out_field_int    (uiout, "warp"      , b->wp);
      ui_out_field_int    (uiout, "lane"      , b->ln);
      ui_out_field_string (uiout, "filename"  , b->filename);
      ui_out_field_int    (uiout, "line"      , b->line);
      ui_out_text         (uiout, "\n");
      do_cleanups (row_chain);
    }

  do_cleanups (table_chain);
}

static void
info_cuda_threads_print_coalesced (cuda_info_thread_t *threads, uint32_t num_threads)
{
  cuda_info_thread_t *b;
  uint32_t i, num_columns;
  uint64_t kernel_id;
  struct cleanup *table_chain, *row_chain;
  struct { uint32_t current, kernel, start_block_idx, start_thread_idx, end_block_idx,
             end_thread_idx, count, pc, filename, line; } width;

  /* column headers */
  const char *header_current          = " ";
  const char *header_kernel           = "Kernel";
  const char *header_start_block_idx  = "BlockIdx";
  const char *header_start_thread_idx = "ThreadIdx";
  const char *header_end_block_idx    = "To BlockIdx";
  const char *header_end_thread_idx   = "ThreadIdx";
  const char *header_count            = "Count";
  const char *header_pc               = "Virtual PC";
  const char *header_filename         = "Filename";
  const char *header_line             = "Line";

  /* sanity checks */
  gdb_assert (cuda_options_coalescing ());

  /* output message if the list is empty */
  if (num_threads == 0 && !ui_out_is_mi_like_p (uiout))
    {
      ui_out_field_string (uiout, NULL, _("No CUDA threads.\n"));
      return;
    }

  /* column widths */
  width.current          = strlen (header_current);
  width.kernel           = strlen (header_kernel);
  width.start_block_idx  = strlen (header_start_block_idx);
  width.start_thread_idx = strlen (header_start_thread_idx);
  width.end_block_idx    = strlen (header_end_block_idx);
  width.end_thread_idx   = strlen (header_end_thread_idx);
  width.count            = strlen (header_count);
  width.pc               = strlen (header_pc);
  width.filename         = strlen (header_filename);
  width.line             = strlen (header_line);

  for (b = threads, i = 0; i < num_threads; ++i, ++b)
    {
      width.pc               = max (width.pc, 18);
      width.start_block_idx  = max (width.start_block_idx, strlen (b->start_block_idx_string));
      width.start_thread_idx = max (width.start_thread_idx, strlen (b->start_thread_idx_string));
      width.end_block_idx    = max (width.end_block_idx, strlen (b->end_block_idx_string));
      width.end_thread_idx   = max (width.end_thread_idx, strlen (b->end_thread_idx_string));
      width.filename         = max (width.filename, strlen (b->filename));
      width.line             = max (width.line, 5);
    }

  /* print table header ('kernel' is only present in MI output) */
  num_columns = ui_out_is_mi_like_p (uiout) ? 10 : 9;
  table_chain = make_cleanup_ui_out_table_begin_end (uiout, num_columns, num_threads, "CoalescedInfoCudaThreadsTable");
  ui_out_table_header (uiout, width.current         , ui_right, "current"        , header_current);
  if (ui_out_is_mi_like_p (uiout))
    ui_out_table_header (uiout, width.kernel        , ui_right, "kernel"         , header_kernel);
  ui_out_table_header (uiout, width.start_block_idx , ui_right, "from_blockIdx"  , header_start_block_idx);
  ui_out_table_header (uiout, width.start_thread_idx, ui_right, "from_threadIdx" , header_start_thread_idx);
  ui_out_table_header (uiout, width.end_block_idx   , ui_right, "to_blockIdx"    , header_end_block_idx);
  ui_out_table_header (uiout, width.end_thread_idx  , ui_right, "to_threadIdx"   , header_end_thread_idx);
  ui_out_table_header (uiout, width.count           , ui_right, "count"          , header_count);
  ui_out_table_header (uiout, width.pc              , ui_right, "virtual_pc"     , header_pc);
  ui_out_table_header (uiout, width.filename        , ui_right, "filename"       , header_filename);
  ui_out_table_header (uiout, width.line            , ui_right, "line"           , header_line);
  ui_out_table_body (uiout);

  /* print table rows */
  for (b = threads, i = 0, kernel_id = ~0ULL; i < num_threads; ++i, ++b)
    {
      if (!ui_out_is_mi_like_p (uiout) && b->kernel_id != kernel_id)
        {
          /* row are grouped per kernel only in CLI output */
          ui_out_message (uiout, 0, "Kernel %"PRIu64"\n", b->kernel_id),
          kernel_id = b->kernel_id;
        }

      row_chain = make_cleanup_ui_out_tuple_begin_end (uiout, "CoalescedInfoCudaThreadsRow");
      ui_out_field_string (uiout, "current"       , b->current ? "*" : " ");
      if (ui_out_is_mi_like_p (uiout))
        ui_out_field_int  (uiout, "kernel"        , b->kernel_id);
      ui_out_field_string (uiout, "from_blockIdx" , b->start_block_idx_string);
      ui_out_field_string (uiout, "from_threadIdx", b->start_thread_idx_string);
      ui_out_field_string (uiout, "to_blockIdx"   , b->end_block_idx_string);
      ui_out_field_string (uiout, "to_threadIdx"  , b->end_thread_idx_string);
      ui_out_field_int    (uiout, "count"         , b->count);
      ui_out_field_fmt    (uiout, "virtual_pc"    , "0x%016"PRIx64, b->pc);
      ui_out_field_string (uiout, "filename"      , b->filename);
      ui_out_field_int    (uiout, "line"          , b->line);
      ui_out_text         (uiout, "\n");
      do_cleanups (row_chain);
    }

  do_cleanups (table_chain);
}

void
info_cuda_threads_command (char *filter_string)
{
  cuda_info_thread_t *threads;
  uint32_t num_threads;

  cuda_info_threads_build (filter_string, &threads, &num_threads);

  if (cuda_options_coalescing ())
    info_cuda_threads_print_coalesced (threads, num_threads);
  else
    info_cuda_threads_print_uncoalesced (threads, num_threads);

  gdb_flush (gdb_stdout);

  cuda_info_threads_destroy (threads);
}

static void
cleanup_info_cuda_command (void* dummy)
{
  restore_current_context ();
  cuda_restore_focus ();
}

void
run_info_cuda_command (void (*command)(char *), char *arg)
{
  struct cleanup *cleanups;

  /* Save the current focus and ELF image */
  save_current_context ();
  cuda_save_focus ();
  cleanups = make_cleanup (cleanup_info_cuda_command, NULL);

  /* Execute the proper info cuda command */
  command (arg);

  /* Restore the original focus and ELF images */
  do_cleanups (cleanups);
}

static void
info_cuda_command (char *arg, int from_tty)
{
  char *argument;
  bool found = 0;
  void (*command)(char *) = NULL;

  if (!arg)
    error (_("Missing option."));

  /* Sanity check and save which command (with correct argument) to invoke. */

#define MAP_ARG_TO_COMMAND(name,cmd)                           \
  if (!found && !strncmp (arg, name, sizeof (name) - 1))  \
    {                                                     \
      command = &cmd;                                     \
      argument = arg + sizeof (name) - 1;                 \
      found = 1;;                                         \
    }

  MAP_ARG_TO_COMMAND ("devices", info_cuda_devices_command);
  MAP_ARG_TO_COMMAND ("sms",     info_cuda_sms_command);
  MAP_ARG_TO_COMMAND ("warps",   info_cuda_warps_command);
  MAP_ARG_TO_COMMAND ("lanes",   info_cuda_lanes_command);
  MAP_ARG_TO_COMMAND ("kernels", info_cuda_kernels_command);
  MAP_ARG_TO_COMMAND ("blocks",  info_cuda_blocks_command);
  MAP_ARG_TO_COMMAND ("threads", info_cuda_threads_command);

#undef MAP_ARG_TO_COMMAND

  if (!found)
    error (_("Unrecognized option: '%s'."), arg);

  run_info_cuda_command (command, argument);
}

struct cmd_list_element *cudalist;

void
cuda_command_switch (char *switch_string)
{
  bool ignore_solution;
  uint32_t i;
  cuda_coords_kind_t ck;
  cuda_coords_t current, requested, processed, candidates[CK_MAX], *solution;
  request_t *request;
  cuda_parser_result_t *command;
  cuda_coords_special_value_t default_value;

  /* Read the current coordinates. */
  current = CUDA_INVALID_COORDS;
  cuda_coords_get_current (&current);

  /* Read the user request, including the uninitialized values. */
  requested = CUDA_INVALID_COORDS;
  default_value = cuda_focus_is_device () ? CUDA_CURRENT : CUDA_WILDCARD;
  cuda_parser (switch_string, CMD_SWITCH, &command, default_value);
  cuda_parser_result_to_coords (command, &requested);

  /* Replace the uninitialized user values with the current coordinates if
     any. Otherwise use wildcards. */
  processed = current.valid ? current : CUDA_WILDCARD_COORDS;
  cuda_parser_result_to_coords (command, &processed);
  cuda_coords_evaluate_current (&processed, true);
  cuda_coords_check_fully_defined (&processed, false, false, true);

  /* Physical or logical coordinates. Physical coordinates have priority. */
  ck = CK_CLOSEST_LOGICAL;
  for (i = 0, request = command->requests; i < command->num_requests; ++i, ++request)
    if (request->type == COORD_TYPE_DEVICE || request->type == COORD_TYPE_SM ||
        request->type == COORD_TYPE_WARP   || request->type == COORD_TYPE_LANE)
      ck = CK_CLOSEST_PHYSICAL;

  /* Find the closest match */
  cuda_coords_find_valid (processed, candidates, CUDA_SELECT_VALID);
  solution = &candidates[ck];

  /* Weed out the solution if the user request cannot be honored */
  ignore_solution = !cuda_coords_equal (&requested, solution);

  /* Do the actual switch if possible */
  if (!solution->valid)
    ui_out_field_string (uiout, NULL, _("Invalid coordinates. CUDA focus unchanged."));
  else if (ignore_solution)
    ui_out_field_string (uiout, NULL, _("Request cannot be satisfied. CUDA focus unchanged."));
  else if (current.valid && cuda_coords_equal (solution, &current))
    ui_out_field_string (uiout, NULL, _("CUDA focus unchanged.\n"));
  else
    {
      cuda_coords_set_current (solution);
      cuda_update_convenience_variables ();
      cuda_update_cudart_symbols ();
      cuda_update_elf_images ();
      switch_to_cuda_thread (NULL);
      cuda_print_message_focus (true);
      print_stack_frame (get_selected_frame (NULL), 0, SRC_LINE);
      do_displays ();
    }
}

void
cuda_command_query (char *query_string)
{
  cuda_coords_t wished;
  const int string_size = 1000;
  char string[string_size];
  cuda_parser_result_t *command;

  /* Bail out if focus not set on a CUDA device */
  if (!cuda_focus_is_device ())
    {
      ui_out_field_string (uiout, NULL, _("Focus not set on any active CUDA kernel."));
      return;
    }

  /* Build the coordinates based on the user request */
  wished = CUDA_INVALID_COORDS;
  cuda_parser (query_string, CMD_QUERY, &command, CUDA_CURRENT);
  cuda_parser_result_to_coords (command, &wished);
  cuda_coords_evaluate_current (&wished, false);
  cuda_coords_check_fully_defined (&wished, true, false, false);

  /* Print the current focus */
  cuda_coords_to_fancy_string (&wished, (char*)string, string_size);
  ui_out_field_fmt (uiout, NULL, "%s\n", string);
  gdb_flush (gdb_stdout);
}

static void
cuda_command_all (char *first_word, char *args)
{
  char *input;
  uint32_t len1, len2;
  cuda_parser_result_t *result;

  /* Reassemble the whole command */
  len1 = first_word ? strlen (first_word) : 0;
  len2 = args ? strlen (args) : 0;
  input = xmalloc (len1 + 1 + len2 + 1);
  strncpy (input, first_word, len1);
  strncpy (input + len1, " ", 1);
  strncpy (input + len1 + 1, args, len2);
  input[len1 + 1 + len2] = 0;

 /* Dispatch to the right handler based on the command type */
  cuda_parser (input, CMD_QUERY | CMD_SWITCH, &result, CUDA_WILDCARD);
  switch (result->command)
    {
    case CMD_QUERY  : cuda_command_query (input); break;
    case CMD_SWITCH : cuda_command_switch (input); break;
    default: error (_("Unrecognized argument(s)."));
    }

  /* Clean up */
  xfree (input);
}

static void
cuda_device_command (char *arg, int from_tty)
{
  cuda_command_all ("device", arg);
}

static void
cuda_sm_command (char *arg, int from_tty)
{
  cuda_command_all ("sm", arg);
}

static void
cuda_warp_command (char *arg, int from_tty)
{
  cuda_command_all ("warp", arg);
}

static void
cuda_lane_command (char *arg, int from_tty)
{
  cuda_command_all ("lane", arg);
}

static void
cuda_kernel_command (char *arg, int from_tty)
{
  cuda_command_all ("kernel", arg);
}

static void
cuda_grid_command (char *arg, int from_tty)
{
  cuda_command_all ("grid", arg);
}

static void
cuda_block_command (char *arg, int from_tty)
{
  cuda_command_all ("block", arg);
}

static void
cuda_thread_command (char *arg, int from_tty)
{
  cuda_command_all ("thread", arg);
}

static void
cuda_command (char *arg, int from_tty)
{
  if (!arg)
    error (_("Missing argument(s)."));
}

void
cuda_commands_initialize ()
{
  add_prefix_cmd ("cuda", class_cuda, cuda_command,
                  _("Print or select the CUDA focus."),
                  &cudalist, "cuda ", 0, &cmdlist);

  add_cmd ("device", no_class, cuda_device_command,
           _("Print or select the current CUDA device."), &cudalist);

  add_cmd ("sm", no_class, cuda_sm_command,
           _("Print or select the current CUDA SM."), &cudalist);

  add_cmd ("warp", no_class, cuda_warp_command,
           _("Print or select the current CUDA warp."), &cudalist);

  add_cmd ("lane", no_class, cuda_lane_command,
           _("Print or select the current CUDA lane."), &cudalist);

  add_cmd ("kernel", no_class, cuda_kernel_command,
           _("Print or select the current CUDA kernel."), &cudalist);

  add_cmd ("grid", no_class, cuda_grid_command,
           _("Print or select the current CUDA grid."), &cudalist);

  add_cmd ("block", no_class, cuda_block_command,
           _("Print or select the current CUDA block."), &cudalist);

  add_cmd ("thread", no_class, cuda_thread_command,
           _("Print or select the current CUDA thread."), &cudalist);

  add_info ("cuda", info_cuda_command,
            _("Print informations about the current CUDA activities. Available options:\n"
              "  devices : information about all the devices\n"
              "  sms     : information about all the SMs in the current device\n"
              "  warps   : information about all the warps in the current SM\n"
              "  lanes   : information about all the lanes in the current warp\n"
              "  kernels : information about all the active kernels\n"
              "  blocks  : information about all the active blocks in the current kernel\n"
              "  threads : information about all the active threads in the current kernel\n"));
}
