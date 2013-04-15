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

#include <sys/stat.h>

#include "defs.h"
#include "breakpoint.h"
#include "gdb_assert.h"
#include "source.h"

#include "cuda-context.h"
#include "cuda-elf-image.h"
#include "cuda-modules.h"
#include "cuda-options.h"
#include "cuda-state.h"
#include "cuda-tdep.h"
#include "cuda-utils.h"

struct elf_image_st {
  void              *image;       /* the relocated ELF image */
  struct objfile    *objfile;     /* pointer to the ELF image as managed by GDB */
  uint64_t           size;        /* the size of the relocated ELF image */
  bool               loaded;      /* is the ELF image in memory? */
  bool               uses_abi;    /* does the ELF image uses the ABI to call functions */
  module_t           module;      /* the parent module */
};


elf_image_t
cuda_elf_image_new (void *image, uint64_t size, module_t module)
{
  elf_image_t elf_image;

  elf_image = xmalloc (sizeof (*elf_image));
  elf_image->image    = image;
  elf_image->objfile  = NULL;
  elf_image->size     = size;
  elf_image->loaded   = false;
  elf_image->uses_abi = false;
  elf_image->module   = module;

  return elf_image;
}

void
cuda_elf_image_delete (elf_image_t elf_image)
{
  gdb_assert (elf_image);
  gdb_assert (elf_image->image);
  xfree (elf_image->image);
  xfree (elf_image);
}

void*
cuda_elf_image_get_image (elf_image_t elf_image)
{
  gdb_assert (elf_image);
  return elf_image->image;
}

struct objfile *
cuda_elf_image_get_objfile (elf_image_t elf_image)
{
  gdb_assert (elf_image);
  return elf_image->objfile;
}

uint64_t
cuda_elf_image_get_size (elf_image_t elf_image)
{
  gdb_assert (elf_image);
  return elf_image->size;
}

module_t
cuda_elf_image_get_module (elf_image_t elf_image)
{
  gdb_assert (elf_image);
  return elf_image->module;
}

bool
cuda_elf_image_is_loaded (elf_image_t elf_image)
{
  gdb_assert (elf_image);
  return elf_image->loaded;
}

bool
cuda_elf_image_contains_address (elf_image_t elf_image, CORE_ADDR addr)
{
  struct objfile       *objfile;
  struct obj_section   *osect = NULL;
  asection             *section = NULL;

  gdb_assert (elf_image);

  if (!cuda_elf_image_is_loaded (elf_image))
    return false;

  objfile = cuda_elf_image_get_objfile (elf_image);
  ALL_OBJFILE_OSECTIONS (objfile, osect)
    {
      section = osect->the_bfd_section;
      if (section && section->vma <= addr &&
          addr < section->vma + section->size)
        return true;
    }

  return false;
}

void
cuda_elf_image_resolve_breakpoints (elf_image_t elf_image)
{
  context_t context, saved_context;
  module_t module;

  gdb_assert (elf_image);

  saved_context = get_current_context ();

  /* tricky: we need to make sure the ELF image is loaded, but we do not want
     to change the current context explicitly. The reason is that resolving
     cuda breakpoints may call breapoint_re_set_one (driver API bpts), which
     reset the focus and the frame. And the frame reconstruction needs the
     correct current context. Switching the focus there will reload the
     required ELF images, therefore we are safe. If breakpoint_re_set_one() is
     not called, we force the reloading of the correct ELF image with the
     set_current_context below. */
  if (!cuda_elf_image_is_loaded (elf_image))
    {
      module = cuda_elf_image_get_module (elf_image);
      context = module_get_context (module);
      context_unload_elf_images (context);
      cuda_elf_image_load (elf_image);
    }

  cuda_trace ("elf image %p: resolve breakpoints", elf_image);
  cuda_resolve_breakpoints (elf_image);

  set_current_context (saved_context);
}

bool
cuda_elf_image_uses_abi (elf_image_t elf_image)
{
  gdb_assert (elf_image);

  return elf_image->uses_abi;
}

void cuda_decode_line_table (struct objfile *objfile);

void
cuda_elf_image_load (elf_image_t elf_image)
{
  bfd *abfd;
  int object_file_fd;
  struct stat object_file_stat;
  struct objfile *objfile = NULL;
  const struct bfd_arch_info *arch_info;
  char object_file_path[CUDA_GDB_TMP_BUF_SIZE] = {0};
  context_t context;
  uint64_t context_id;
  uint64_t module_id;
  uint64_t nbytes = 0;

  gdb_assert (elf_image);
  gdb_assert (!elf_image->loaded);

  context    = module_get_context (elf_image->module);
  context_id = context_get_id (context);
  module_id  = module_get_id (elf_image->module);
  snprintf (object_file_path, sizeof (object_file_path),
            "%s/elf.%"PRIx64".%"PRIx64".o.XXXXXX",
            cuda_gdb_session_get_dir (), context_id, module_id);

  object_file_fd = mkstemp (object_file_path);
  if (object_file_fd == -1)
    error (_("Error: Failed to create device ELF symbol file!"));

  nbytes = write (object_file_fd, elf_image->image, elf_image->size);
  close (object_file_fd);
  if (nbytes != elf_image->size)
    error (_("Error: Failed to write the ELF image file"));

  if (stat (object_file_path, &object_file_stat))
    error (_("Error: Failed to stat device ELF symbol file!"));
  else if (object_file_stat.st_size != elf_image->size)
    error (_("Error: The device ELF file size is incorrect!"));

  /* Open the object file and make sure to adjust its arch_info before reading
     its symbols. */
  abfd = symfile_bfd_open (object_file_path);
  arch_info = bfd_lookup_arch (bfd_arch_m68k, 0);
  bfd_set_arch_info (abfd, arch_info);

  /* Load in the device ELF object file, forcing a symbol read and while
   * making sure that the breakpoints are not re-set automatically. */
  objfile = symbol_file_add_from_bfd (abfd, SYMFILE_DEFER_BP_RESET,
                                      NULL, OBJF_READNOW);
  if (!objfile)
    error (_("Error: Failed to add symbols from device ELF symbol file!\n"));

  /* Identify this gdb objfile as being cuda-specific */
  objfile->cuda_objfile   = 1;
  objfile->gdbarch        = cuda_get_gdbarch ();
  /* CUDA - skip prologue - temporary */
  objfile->cuda_producer_is_open64 = cuda_producer_is_open64;

  /* CUDA - line info */
  if (!objfile->symtabs)
    cuda_decode_line_table (objfile);

  /* Initialize the elf_image object */
  elf_image->objfile  = objfile;
  elf_image->loaded   = true;
  elf_image->uses_abi = cuda_is_bfd_version_call_abi (objfile->obfd);
  cuda_trace ("loaded ELF image (name=%s, module=%"PRIx64", abi=%d, objfile=%p)",
              objfile->name, elf_image->module, elf_image->uses_abi, objfile);
}

void
cuda_elf_image_unload (elf_image_t elf_image)
{
  struct objfile *objfile = elf_image->objfile;

  gdb_assert (elf_image->loaded);
  gdb_assert (objfile);
  gdb_assert (objfile->cuda_objfile);

  cuda_trace ("unloading ELF image (name=%s, module=%"PRIx64")",
              objfile->name, elf_image->module);

  /* Make sure that all its users will be cleaned up. */
  clear_current_source_symtab_and_line ();
  clear_displays ();
  if (!cuda_options_debug_general () && objfile->name)
    if (unlink (objfile->name))
      cuda_trace ("unable to unlink file %s", objfile->name);
  free_objfile (objfile);

  elf_image->objfile = NULL;
  elf_image->loaded = false;
  elf_image->uses_abi = false;
}


