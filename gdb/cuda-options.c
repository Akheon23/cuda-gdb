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

#include "defs.h"
#include "top.h"
#include "command.h"
#include "frame.h"
#include "environ.h"
#include "inferior.h"
#include "gdbcmd.h"

#include "cuda-options.h"

/*List of set/show cuda commands */
struct cmd_list_element *setcudalist;
struct cmd_list_element *showcudalist;

/*List of set/show debug cuda commands */
struct cmd_list_element *setdebugcudalist;
struct cmd_list_element *showdebugcudalist;

/*
 * cuda prefix
 */
static void
set_cuda (char *arg, int from_tty)
{
  printf_unfiltered (_("\"set cuda\" must be followed by the name of a cuda subcommand.\n"));
  help_list (setcudalist, "set cuda ", -1, gdb_stdout);
}

static void
show_cuda (char *args, int from_tty)
{
  cmd_show_list (showcudalist, from_tty, "");
}

void
cuda_options_initialize_cuda_prefix (void)
{
  add_prefix_cmd ("cuda", no_class, set_cuda,
                  _("Generic command for setting gdb cuda variables"),
                  &setcudalist, "set cuda ", 0, &setlist);

  add_prefix_cmd ("cuda", no_class, show_cuda,
                  _("Generic command for showing gdb cuda variables"),
                  &showcudalist, "show cuda ", 0, &showlist);
}

/*
 * set debug cuda
 */
static void
set_debug_cuda (char *arg, int from_tty)
{
  printf_unfiltered (_("\"set debug cuda\" must be followed by the name of a debug cuda subcommand.\n"));
  help_list (setdebugcudalist, "set debug cuda ", -1, gdb_stdout);
}

static void
show_debug_cuda (char *args, int from_tty)
{
  cmd_show_list (showdebugcudalist, from_tty, "");
}

void
cuda_options_initialize_debug_cuda_prefix (void)
{
  add_prefix_cmd ("cuda", no_class, set_debug_cuda,
                  _("Generic command for setting gdb cuda debugging flags"),
                  &setdebugcudalist, "set debug cuda ", 0, &setdebuglist);

  add_prefix_cmd ("cuda", no_class, show_debug_cuda,
                  _("Generic command for showing gdb cuda debugging flags"),
                  &showdebugcudalist, "show debug cuda ", 0, &showdebuglist);
}

/*
 * set debug cuda general
 */
static int cuda_debug_general;

static void
cuda_show_debug_general (struct ui_file *file, int from_tty,
                         struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file, _("CUDA general debug trace is %s.\n"), value);
}

static void
cuda_options_initialize_debug_general ()
{
  cuda_debug_general = false;

  add_setshow_zinteger_cmd ("general", class_maintenance, &cuda_debug_general,
                            _("Set debug trace of the internal CUDA-specific functions"),
                            _("Show debug trace of internal CUDA-specific functions."),
                            _("When non-zero, internal CUDA debugging is enabled."),
                            NULL, cuda_show_debug_general,
                            &setdebugcudalist, &showdebugcudalist);
}

int
cuda_options_debug_general ()
{
  return cuda_debug_general;
}

/*
 * set debug cuda notifications
 */
static int cuda_debug_notifications;

static void
cuda_show_debug_notifications (struct ui_file *file, int from_tty,
                               struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file, _("CUDA notifications debug trace is %s.\n"), value);
}

static void
cuda_options_initialize_debug_notifications ()
{
  cuda_debug_notifications = false;

  add_setshow_zinteger_cmd ("notifications", class_maintenance, &cuda_debug_notifications,
                            _("Set debug trace of the CUDA notification functions"),
                            _("Show debug trace of the CUDA notification functions."),
                            _("When non-zero, internal debugging of the CUDA notification functions is enabled."),
                            NULL, cuda_show_debug_notifications,
                            &setdebugcudalist, &showdebugcudalist);
}

bool
cuda_options_debug_notifications ()
{
  return cuda_debug_notifications;
}

/*
 * set debug cuda textures
 */
static int cuda_debug_textures;

static void
cuda_show_debug_textures (struct ui_file *file, int from_tty,
                               struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file, _("CUDA textures debug trace is %s.\n"), value);
}

static void
cuda_options_initialize_debug_textures ()
{
  cuda_debug_textures = false;

  add_setshow_zinteger_cmd ("textures", class_maintenance, &cuda_debug_textures,
                            _("Set debug trace of CUDA texture accesses"),
                            _("Show debug trace of CUDA texture accesses."),
                            _("When non-zero, internal debugging of CUDA texture accesses is enabled."),
                            NULL, cuda_show_debug_textures,
                            &setdebugcudalist, &showdebugcudalist);
}

bool
cuda_options_debug_textures ()
{
  return cuda_debug_textures;
}


/*
 * set debug cuda libcudbg
 */
static int cuda_debug_libcudbg;

static void
cuda_show_debug_libcudbg (struct ui_file *file, int from_tty,
                          struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file, _("CUDA libcudbg debug trace is %s.\n"), value);
}

static void
cuda_options_initialize_debug_libcudbg ()
{
  cuda_debug_libcudbg = false;

  add_setshow_zinteger_cmd ("libcudbg", class_maintenance, &cuda_debug_libcudbg,
                            _("Set debug trace of the CUDA RPC client functions"),
                            _("Show debug trace of the CUDA RPC client functions."),
                            _("When non-zero, internal debugging of the CUDA RPC client functions is enabled."),
                            NULL, cuda_show_debug_libcudbg,
                            &setdebugcudalist, &showdebugcudalist);
}

bool
cuda_options_debug_libcudbg ()
{
  return cuda_debug_libcudbg;
}

/*
 * set debug cuda siginfo
 */
static int cuda_debug_siginfo;

static void
cuda_show_debug_siginfo (struct ui_file *file, int from_tty,
                         struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file, _("Setting of $_siginfo upon CUDA exceptions is %s.\n"), value);
}

static void
cuda_options_initialize_debug_siginfo ()
{
  cuda_debug_siginfo = false;

  add_setshow_boolean_cmd ("siginfo", class_cuda, &cuda_debug_siginfo,
                           _("Turn on/off setting convenience variable $_siginfo upon CUDA exceptions."),
                           _("Show if setting convenience variable $_siginfo upon CUDA exceptions is on/off."),
                           _("When enabled, if the application is signalled by a CUDA exception, "
                             "$_siginfo will be set to the corresponding signal number."),
                           NULL, cuda_show_debug_siginfo,
                           &setdebugcudalist, &showdebugcudalist);
}

bool
cuda_options_debug_siginfo ()
{
  return cuda_debug_siginfo;
}

/*
 * set debug cuda api
 */
static int cuda_debug_api;

static void
cuda_show_debug_api (struct ui_file *file, int from_tty,
                               struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file, _("CUDA api debug trace is %s.\n"), value);
}

static void
cuda_options_initialize_debug_api ()
{
  cuda_debug_api = false;

  add_setshow_zinteger_cmd ("api", class_maintenance, &cuda_debug_api,
                            _("Set debug trace of the CUDA api functions"),
                            _("Show debug trace of the CUDA api functions."),
                            _("When non-zero, internal debugging of the CUDA api functions is enabled."),
                            NULL, cuda_show_debug_api,
                            &setdebugcudalist, &showdebugcudalist);
}

bool
cuda_options_debug_api ()
{
  return cuda_debug_api;
}

/*
 * set cuda memcheck
 */
int cuda_memcheck_auto;
enum auto_boolean cuda_memcheck;

static void
cuda_show_cuda_memcheck (struct ui_file *file, int from_tty,
                         struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file, _("CUDA Memory Checker (cudamemcheck) is %s.\n"), value);
}

static void
cuda_options_initialize_memcheck ()
{
  cuda_memcheck = AUTO_BOOLEAN_AUTO;

  add_setshow_auto_boolean_cmd ("memcheck", class_cuda, &cuda_memcheck,
                                _("Turn on/off CUDA Memory Checker next time the inferior application is run."),
                                _("Show if CUDA Memory Checker is turned on/off."),
                                _("When enabled, CUDA Memory Checker checks for out-of-bounds memory accesses. "
                                  "When auto, CUDA Memory Checker is enabled if CUDA_MEMCHECK environment variable is set."),
                                NULL, cuda_show_cuda_memcheck,
                                &setcudalist, &showcudalist);
}

bool
cuda_options_memcheck (void)
{
  struct gdb_environ *env = current_inferior ()->environment;

  /* Memcheck auto value is determined by the CUDA_MEMCHECK env, which
   * can only be evaluated when the inferior is running. */
  cuda_memcheck_auto = env && !!get_in_environ (env, "CUDA_MEMCHECK");

  return cuda_memcheck == AUTO_BOOLEAN_TRUE ||
        (cuda_memcheck == AUTO_BOOLEAN_AUTO && cuda_memcheck_auto);
}

/*
 * set cuda coalescing
 */
int cuda_coalescing_auto;
enum auto_boolean cuda_coalescing;

static void
cuda_set_coalescing (char *args, int from_tty, struct cmd_list_element *c)
{
  printf_filtered ("Coalescing of the CUDA commands output is %s.\n",
                   cuda_options_coalescing () ? "on" : "off");
}

static void
cuda_show_coalescing (struct ui_file *file, int from_tty,
                      struct cmd_list_element *c, const char *value)
{
  printf_filtered ("Coalescing of the CUDA commands output is %s.\n",
                   cuda_options_coalescing () ? "on" : "off");
}

static void
cuda_options_initialize_coalescing ()
{
  cuda_coalescing_auto = true;
  cuda_coalescing      = AUTO_BOOLEAN_AUTO;

  add_setshow_auto_boolean_cmd ("coalescing", class_cuda, &cuda_coalescing,
                                _("Turn on/off coalescing of the CUDA commands output."),
                                _("Show if coalescing of the CUDA commands output is turned on/off."),
                                _("When enabled, the output of the CUDA commands will be coalesced when possible."),
                                cuda_set_coalescing, cuda_show_coalescing,
                                &setcudalist, &showcudalist);
}

bool
cuda_options_coalescing (void)
{
  return cuda_coalescing == AUTO_BOOLEAN_TRUE ||
         (cuda_coalescing == AUTO_BOOLEAN_AUTO && cuda_coalescing_auto);
}

/*
 * set cuda break_on_launch
 */
const char  cuda_break_on_launch_none[]        = "none";
const char  cuda_break_on_launch_application[] = "application";
const char  cuda_break_on_launch_system[]      = "system";
const char  cuda_break_on_launch_all[]         = "all";

const char *cuda_break_on_launch_enums[] = {
  cuda_break_on_launch_none,
  cuda_break_on_launch_application,
  cuda_break_on_launch_system,
  cuda_break_on_launch_all,
  NULL
};

const char *cuda_break_on_launch;

static void
cuda_show_break_on_launch (struct ui_file *file, int from_tty,
                           struct cmd_list_element *c, const char *value)
{
  printf_filtered ("Break on every kernel launch is set to '%s'.\n", value);
}

static void
cuda_set_break_on_launch (char *args, int from_tty, struct cmd_list_element *c)
{
  // reset the auto breakpoints
  cuda_cleanup_auto_breakpoints (NULL);
}

static void
cuda_options_initialize_break_on_launch (void)
{
  cuda_break_on_launch = cuda_break_on_launch_none;

  add_setshow_enum_cmd ("break_on_launch", class_cuda,
                        cuda_break_on_launch_enums, &cuda_break_on_launch,
                        _("Automatically set a breakpoint at the entrance of kernels."),
                        _("Show if the debugger stops the application on kernel launches."),
                        _("When enabled, a breakpoint is hit on kernel launches:\n"
                          "  none        : no breakpoint is set (default)\n"
                          "  application : a breakpoint is set at the entrance of all the application kernels\n"
                          "  system      : a breakpoint is set at the entrance of all the system kernels\n"
                          "  all         : a breakpoint is set at the entrance of all kernels"),
                        cuda_set_break_on_launch, cuda_show_break_on_launch,
                        &setcudalist, &showcudalist);
}

void
cuda_options_disable_break_on_launch (void)
{
  cuda_break_on_launch = cuda_break_on_launch_none;

  cuda_set_break_on_launch (NULL, 0, NULL);
}

bool
cuda_options_break_on_launch_system (void)
{
  return (cuda_break_on_launch == cuda_break_on_launch_system ||
          cuda_break_on_launch == cuda_break_on_launch_all);
}

bool
cuda_options_break_on_launch_application (void)
{
  return (cuda_break_on_launch == cuda_break_on_launch_application ||
          cuda_break_on_launch == cuda_break_on_launch_all);
}

/*
 * set cuda disassemble_from
 */
const char  cuda_disassemble_from_device_memory [] = "device_memory";
const char  cuda_disassemble_from_elf_image[]      = "elf_image";

const char *cuda_disassemble_from_enums[] = {
  cuda_disassemble_from_device_memory,
  cuda_disassemble_from_elf_image,
  NULL
};

const char *cuda_disassemble_from;

static void
cuda_show_disassemble_from (struct ui_file *file, int from_tty,
                            struct cmd_list_element *c, const char *value)
{
  printf_filtered ("CUDA code is dissassembled from %s.\n", value);
}

static void
cuda_set_disassemble_from (char *args, int from_tty, struct cmd_list_element *c)
{
  // XXX: flush all the disassembly caches
}
static void
cuda_options_initialize_disassemble_from (void)
{
  cuda_disassemble_from = cuda_disassemble_from_elf_image;

  add_setshow_enum_cmd ("disassemble_from", class_cuda,
                        cuda_disassemble_from_enums, &cuda_disassemble_from,
                        _("Choose whether to disassemble from the device memory "
                          "(slow) or the ELF image (fast)."),
                        _("Show where the device code is disassembled from."),
                        _("Choose where the device code is disassembled from:\n"
                          "  device_memory : the device code memory (slow)\n"
                          "  elf_image     : the device ELF image on the host (fast)\n"),
                        cuda_set_disassemble_from, cuda_show_disassemble_from,
                        &setcudalist, &showcudalist);
}

bool
cuda_options_disassemble_from_device_memory (void)
{
  return cuda_disassemble_from == cuda_disassemble_from_device_memory;
}

bool
cuda_options_disassemble_from_elf_image (void)
{
  return cuda_disassemble_from == cuda_disassemble_from_elf_image;
}

/*
 * set cuda hide_internal_frames
 */
int cuda_hide_internal_frames;

static void
cuda_set_hide_internal_frames (char *args, int from_tty, struct cmd_list_element *c)
{
  // force rebuilding frame stack to see the change
  reinit_frame_cache ();
}

static void
cuda_show_hide_internal_frames (struct ui_file *file, int from_tty,
                                struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file, _("Hiding of CUDA internal frames is %s.\n"), value);
}

static void
cuda_options_initialize_hide_internal_frames ()
{
  cuda_hide_internal_frames = 1;

  add_setshow_zinteger_cmd ("hide_internal_frame", class_cuda, &cuda_hide_internal_frames,
                            _("Set hiding of the internal CUDA frames when printing the call stack"),
                            _("Show hiding of the internal CUDA frames when printing the call stack."),
                            _("When non-zero, internal CUDA frames are omitted when printing the call stack."),
                            cuda_set_hide_internal_frames, cuda_show_hide_internal_frames,
                            &setcudalist, &showcudalist);
}

bool
cuda_options_hide_internal_frames (void)
{
  return cuda_hide_internal_frames;
}

/*
 * set cuda show_kernel_events
 */
int cuda_show_kernel_events;

static void
cuda_show_show_kernel_events (struct ui_file *file, int from_tty,
                               struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file, _("Show CUDA kernel events is %s.\n"), value);
}

static void
cuda_options_initialize_show_kernel_events (void)
{
  cuda_show_kernel_events = 1;

  add_setshow_zinteger_cmd ("kernel_events", class_cuda, &cuda_show_kernel_events,
                            _("Turn on/off kernel events (launch/termination) output messages."),
                            _("Show kernel events."),
                            _("When turned on, launch/termination kernel events are displayed."),
                            NULL,
                            cuda_show_show_kernel_events,
                            &setcudalist, &showcudalist);
}

bool
cuda_options_show_kernel_events (void)
{
  return cuda_show_kernel_events;
}

/*
 * set cuda show_context_events
 */
int cuda_show_context_events;

static void
cuda_show_show_context_events (struct ui_file *file, int from_tty,
                               struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file, _("Show CUDA context events is %s.\n"), value);
}

static void
cuda_options_initialize_show_context_events (void)
{
  cuda_show_context_events = 1;

  add_setshow_zinteger_cmd ("context_events", class_cuda, &cuda_show_context_events,
                            _("Turn on/off context events (push/pop/create/destroy) output messages."),
                            _("Show context events."),
                            _("When turned on, push/pop/create/destroy context events are displayed."),
                            NULL,
                            cuda_show_show_context_events,
                            &setcudalist, &showcudalist);
}

bool
cuda_options_show_context_events (void)
{
  return cuda_show_context_events;
}

/*
 * set cuda launch_blocking
 */
int cuda_launch_blocking;

static void
cuda_set_launch_blocking (char *args, int from_tty, struct cmd_list_element *c)
{
  if (cuda_launch_blocking)
      printf_filtered ("On the next run, the CUDA kernel launches will be blocking.\n");
  else
      printf_filtered ("On the next run, the CUDA kernel launches will be non-blocking.\n");
}

static void
cuda_show_launch_blocking (struct ui_file *file, int from_tty,
                           struct cmd_list_element *c, const char *value)
{
  if (cuda_launch_blocking)
    fprintf_filtered (file, _("On the next run, the CUDA kernel launches will be blocking.\n"));
  else
    fprintf_filtered (file, _("On the next run, the CUDA kernel launches will be non-blocking.\n"));
}

static void
cuda_options_initialize_launch_blocking (void)
{
  cuda_launch_blocking = 0;

  add_setshow_zinteger_cmd ("launch_blocking", class_cuda, &cuda_launch_blocking,
                            _("Turn on/off CUDA kernel launch blocking (effective starting from the next run)"),
                            _("Show whether CUDA kernel launches are blocking."),
                            _("When turned on, CUDA kernel launches are blocking (effective starting from the next run."),
                            cuda_set_launch_blocking,
                            cuda_show_launch_blocking,
                            &setcudalist, &showcudalist);
}

bool
cuda_options_launch_blocking (void)
{
  return cuda_launch_blocking;
}

/*
 * set cuda thread_selection
 */
const char  cuda_thread_selection_policy_logical[]  = "logical";
const char  cuda_thread_selection_policy_physical[] = "physical";
const char *cuda_thread_selection_policy_enums[]    = {
  cuda_thread_selection_policy_logical,
  cuda_thread_selection_policy_physical,
  NULL
};
const char *cuda_thread_selection_policy;

static void
show_cuda_thread_selection_policy (struct ui_file *file, int from_tty,
                                   struct cmd_list_element *c, const char *value)
{
  fprintf_filtered (file, _("CUDA thread selection policy is %s.\n"), value);
}

static void
cuda_options_initialize_thread_selection (void)
{
  cuda_thread_selection_policy = cuda_thread_selection_policy_logical;

  add_setshow_enum_cmd ("thread_selection", class_cuda,
                        cuda_thread_selection_policy_enums, &cuda_thread_selection_policy,
                        _("Set the automatic thread selection policy to use when the current thread cannot be selected.\n"),
                        _("Show the automatic thread selection policy to use when the current thread cannot be selected.\n"),
                        _("logical  == the thread with the lowest logical coordinates (blockIdx/threadIdx) is selected\n"
                          "physical == the thread with the lowest physical coordinates (dev/sm/wp/ln) is selected."),
                        NULL, show_cuda_thread_selection_policy,
                        &setcudalist, &showcudalist);
}

bool
cuda_options_thread_selection_logical (void)
{
  return cuda_thread_selection_policy == cuda_thread_selection_policy_logical;
}

bool
cuda_options_thread_selection_physical (void)
{
  return cuda_thread_selection_policy == cuda_thread_selection_policy_physical;
}

static void
show_cuda_copyright_command (char *ignore, int from_tty)
{
  print_gdb_version (gdb_stdout);
  printf_filtered ("\n");
}

static void
cuda_options_initialize_copyright (void)
{
  add_cmd ("copyright", class_cuda, show_cuda_copyright_command,
           _("Copyright for GDB with CUDA support."),
           &showcudalist);
}

/*
 * set cuda api_failures
 */
const char  cuda_api_failures_option_ignore[]   = "ignore";
const char  cuda_api_failures_option_stop[]     = "stop";
const char  cuda_api_failures_option_hide[]     = "hide";

const char *cuda_api_failures_options_enums[] = {
    cuda_api_failures_option_ignore,
    cuda_api_failures_option_stop,
    cuda_api_failures_option_hide,
    NULL
};

const char *cuda_api_failures_option;

static void
cuda_show_api_failures (struct ui_file *file, int from_tty,
                          struct cmd_list_element *c, const char *value)
{
  printf_filtered ("api_failures is set to '%s'.\n", value);
}

static void
cuda_options_initialize_api_failures (void)
{
  cuda_api_failures_option = cuda_api_failures_option_ignore;

  add_setshow_enum_cmd ("api_failures", class_cuda,
                        cuda_api_failures_options_enums, &cuda_api_failures_option,
                        _("Set the api_failures to ignore/stop/hide on CUDA driver API call errors."), 
                        _("Show if cuda-gdb ignores/stops/hides on CUDA driver API call errors."),
                        _("  ignore : no breakpoint is hit, only warning message is printed for every CUDA driver API call failure\n"
                          "  stop   : a breakpoint is hit when a CUDA driver API call returns an error\n"
                          "  hide   : no breakpoint is hit, no warning message is printed"),
                        NULL, cuda_show_api_failures,
                        &setcudalist, &showcudalist);
}

bool
cuda_options_api_failures_ignore(void)
{
  return (cuda_api_failures_option == cuda_api_failures_option_ignore);
}

bool
cuda_options_api_failures_stop(void)
{
  return (cuda_api_failures_option == cuda_api_failures_option_stop);
}

bool
cuda_options_api_failures_hide(void)
{
  return (cuda_api_failures_option == cuda_api_failures_option_hide);
}

/*Initialization */
void
cuda_options_initialize ()
{
  cuda_options_initialize_cuda_prefix ();
  cuda_options_initialize_debug_cuda_prefix ();
  cuda_options_initialize_debug_general ();
  cuda_options_initialize_debug_notifications ();
  cuda_options_initialize_debug_libcudbg ();
  cuda_options_initialize_debug_siginfo ();
  cuda_options_initialize_debug_textures ();
  cuda_options_initialize_debug_api ();
  cuda_options_initialize_memcheck ();
  cuda_options_initialize_coalescing ();
  cuda_options_initialize_break_on_launch ();
  cuda_options_initialize_api_failures ();
  cuda_options_initialize_disassemble_from ();
  cuda_options_initialize_hide_internal_frames ();
  cuda_options_initialize_show_kernel_events ();
  cuda_options_initialize_show_context_events ();
  cuda_options_initialize_launch_blocking ();
  cuda_options_initialize_thread_selection ();
  cuda_options_initialize_copyright ();
}
