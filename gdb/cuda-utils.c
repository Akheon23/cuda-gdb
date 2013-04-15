/*
 * NVIDIA CUDA Debugger CUDA-GDB Copyright (C) 2007-2011 NVIDIA Corporation
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

/* Utility functions for cuda-gdb */
#include "defs.h"
#include <ctype.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>

#include <fcntl.h>
#include "cuda-utils.h"
#include "cuda-defs.h"
#include "gdb_assert.h"

#define CUDA_GDB_RECORD_FORMAT_WORK   "WORK:%10d\n"
#define CUDA_GDB_RECORD_FORMAT_DEVICE  "%4d:%10d\n"
#define CUDA_GDB_RECORD_SIZE                     16
#define CUDA_GDB_RECORD_WORK_LOCK                 0
#define CUDA_GDB_RECORD_DEVICE(i)         ((i) + 1)

static const char cuda_gdb_lock_file[] = "cuda-gdb.lock";
static char cuda_gdb_tmp_basedir[CUDA_GDB_TMP_BUF_SIZE];
static int cuda_gdb_lock_fd;
static char* cuda_gdb_tmp_dir;
static uint64_t dev_mask;

static void
cuda_gdb_tmpdir_create_basedir ()
{
  DIR* dir;
  struct stat st;
  mode_t old_umask;
  int ret = 0;

  if (getenv ("TMPDIR"))
    snprintf (cuda_gdb_tmp_basedir, sizeof (cuda_gdb_tmp_basedir), 
              "%s/cuda-dbg", getenv ("TMPDIR"));
  else
    snprintf (cuda_gdb_tmp_basedir, sizeof (cuda_gdb_tmp_basedir),
              "/tmp/cuda-dbg");

  if (stat (cuda_gdb_tmp_basedir, &st) || !(S_ISDIR(st.st_mode)))
    {
      /* Save the old umask and reset it */
      old_umask = umask (0);

      ret = mkdir (cuda_gdb_tmp_basedir, S_IRWXU | S_IRWXG | S_IRWXO);

      /* Restore the old umask */
      umask (old_umask);

      if (ret)
        error (_("Error creating temporary directory %s\n"),
               cuda_gdb_tmp_basedir);
    }
}

static void
cuda_gdb_tmpdir_cleanup_files (char* dirpath)
{
  char path[CUDA_GDB_TMP_BUF_SIZE];
  DIR* dir = opendir (dirpath);
  struct dirent* dir_ent = NULL;

  if (!dir)
    return;

  while ((dir_ent = readdir (dir)))
    {
      snprintf (path, sizeof (path), "%s/%s", dirpath, dir_ent->d_name);
      unlink (path);
    };

  closedir (dir);
}

static void
cuda_gdb_tmpdir_cleanup_dir (char* dirpath)
{
  cuda_gdb_tmpdir_cleanup_files (dirpath);
  rmdir (dirpath);
}

static void
cuda_gdb_tmpdir_cleanup_self (void *unused)
{
  cuda_gdb_tmpdir_cleanup_dir (cuda_gdb_tmp_dir);
  xfree (cuda_gdb_tmp_dir);
}

static void
cuda_gdb_record_read_pid (int record_idx, int *dev_id, int *pid)
{
  char record[CUDA_GDB_TMP_BUF_SIZE];
  int res;

  *pid = 0;

  res = lseek (cuda_gdb_lock_fd, record_idx * CUDA_GDB_RECORD_SIZE, SEEK_SET);
  if (res == -1)
    return;

  res = read (cuda_gdb_lock_fd, record, CUDA_GDB_TMP_BUF_SIZE);
  if (res == -1)
    return;

  if (record_idx == 0)
    sscanf (record, CUDA_GDB_RECORD_FORMAT_WORK, pid);
  else
    sscanf (record, CUDA_GDB_RECORD_FORMAT_DEVICE, dev_id, pid);
}

static void
cuda_gdb_record_write (int record_idx, int dev_id, int pid)
{
  char record[CUDA_GDB_TMP_BUF_SIZE];
  int res;

  if (record_idx == 0)
    snprintf (record, CUDA_GDB_TMP_BUF_SIZE, CUDA_GDB_RECORD_FORMAT_WORK,
              pid);
  else
    snprintf (record, CUDA_GDB_TMP_BUF_SIZE, CUDA_GDB_RECORD_FORMAT_DEVICE,
              dev_id, pid);

  res = lseek (cuda_gdb_lock_fd, record_idx * CUDA_GDB_RECORD_SIZE, SEEK_SET);
  if (res == -1)
    return;

  res = write (cuda_gdb_lock_fd, record, strlen (record));
  if (res == -1)
    return;
}

static void
cuda_gdb_record_remove_all (void *unused)
{
  char buf[CUDA_GDB_TMP_BUF_SIZE];
  int res, i;

  snprintf (buf, sizeof (buf), "%s/%s",
            cuda_gdb_tmp_basedir, cuda_gdb_lock_file);

  for (i = 0; i < CUDBG_MAX_DEVICES; i++)
    {
      if (dev_mask & (1 << i))
        {
          /* Clear the record for this device */
          cuda_gdb_record_write (CUDA_GDB_RECORD_DEVICE(i), i, 0);

          dev_mask &= ~(1 << i);
        }
    }

  if (cuda_gdb_lock_fd != -1)
    close (cuda_gdb_lock_fd);
}

static int
cuda_gdb_record_set_lock (int record_idx, bool enable_lock)
{
  struct flock lock = {0};

  lock.l_type = enable_lock ? F_WRLCK: F_UNLCK;
  lock.l_whence = SEEK_SET;
  lock.l_start = record_idx * CUDA_GDB_RECORD_SIZE;
  lock.l_len = CUDA_GDB_RECORD_SIZE;

  return fcntl (cuda_gdb_lock_fd, F_SETLK, &lock);
}

static void
cuda_gdb_error_multiple_instances (int pid, int dev_id)
{
  error (_("An instance of cuda-gdb (pid %d) is already using device %d. "
           "If you believe\nyou are seeing this message in error, try "
           "deleting %s/%s.\n"), pid, dev_id, cuda_gdb_tmp_basedir,
           cuda_gdb_lock_file);
}

static void
cuda_gdb_lock_file_initialize ()
{
  uint32_t i;

  for (i = 0; i < CUDBG_MAX_DEVICES; i++)
    {
      cuda_gdb_record_write (CUDA_GDB_RECORD_DEVICE(i), i, 0);
    }
}

/* Check for the presence of the CUDA_VISIBLE_DEVICES variable. If it is
 * present, lock records */
static void
cuda_gdb_lock_file_create ()
{
  struct stat st;
  char buf[CUDA_GDB_TMP_BUF_SIZE];
  char *visible_devices;
  uint32_t dev_id, num_devices = 0;
  struct flock lock = {0};
  int res, pid, i, ignored;
  bool initialize_lock_file = false;
  mode_t old_umask;

  snprintf (buf, sizeof (buf), "%s/%s",
              cuda_gdb_tmp_basedir, cuda_gdb_lock_file);

  visible_devices = getenv ("CUDA_VISIBLE_DEVICES");

  if (stat (buf, &st) || !(S_ISREG(st.st_mode)))
    initialize_lock_file = true;

  /* Save the old umask and reset it */
  old_umask = umask (0);
  cuda_gdb_lock_fd = open (buf, O_CREAT | O_RDWR,
                           S_IRWXU | S_IRWXG | S_IRWXO);
  /* Restore the old umask */
  umask (old_umask);

  if (cuda_gdb_lock_fd == -1)
    error (_("Cannot open %s. \n"), buf);

  /* Register cleanup routine */
  make_final_cleanup (cuda_gdb_record_remove_all, NULL);

  /* Get the mutex ("work") lock before doing anything */
  res = cuda_gdb_record_set_lock (CUDA_GDB_RECORD_WORK_LOCK, true);
  if (res)
      error (_("Another cuda-gdb instance is working with the lock file, "
               "try again\n"));

  if (initialize_lock_file)
    cuda_gdb_lock_file_initialize ();

  if (NULL == visible_devices)
    {
      /* Lock all devices */
      for (i = 0; i < CUDBG_MAX_DEVICES; i++)
        {
          res = cuda_gdb_record_set_lock (CUDA_GDB_RECORD_DEVICE(i), true);
          if (res)
            {
              pid = 0;
              cuda_gdb_record_read_pid (CUDA_GDB_RECORD_DEVICE(i), &ignored, &pid);
              cuda_gdb_error_multiple_instances (pid, i);
            }
 
          cuda_gdb_record_write (CUDA_GDB_RECORD_DEVICE(i), i, (int)getpid());
          dev_mask |= 1 << i;
        }
    }
  else
    {
      /* Copy to local storage to prevent buffer overflows */
      strncpy (buf, visible_devices, CUDA_GDB_TMP_BUF_SIZE);

      visible_devices = buf;

      do
        {
          if (*visible_devices == ',')
            visible_devices++;

          if ((sscanf (visible_devices, "%u,", &dev_id) > 0) &&
              (++num_devices < CUDBG_MAX_DEVICES) &&
              (dev_id < CUDBG_MAX_DEVICES))
            {
              res = cuda_gdb_record_set_lock (CUDA_GDB_RECORD_DEVICE(dev_id), true);

              if (res)
                {
                  cuda_gdb_record_read_pid (CUDA_GDB_RECORD_DEVICE(dev_id),
                                            &dev_id, &pid);

                  cuda_gdb_error_multiple_instances (pid, dev_id);
                }
                
                cuda_gdb_record_write (CUDA_GDB_RECORD_DEVICE(dev_id), dev_id, (int)getpid());

                dev_mask |= 1 << dev_id;
            }
          else
            break;
        } while ((visible_devices = strstr (visible_devices, ",")));
    }
    
    cuda_gdb_record_set_lock (CUDA_GDB_RECORD_WORK_LOCK, false);
}

static void
cuda_gdb_tmpdir_setup (void)
{
  char dirpath [CUDA_GDB_TMP_BUF_SIZE];
  struct stat st;

  snprintf (dirpath, sizeof (dirpath), "%s/%u", cuda_gdb_tmp_basedir,
            getpid ());

  if (stat (dirpath, &st) || !(S_ISDIR(st.st_mode)))
    {
      if (mkdir (dirpath, S_IRWXU | S_IRWXG))
        error (_("Error creating temporary directory %s\n"), dirpath);
    }
  else
      cuda_gdb_tmpdir_cleanup_files (dirpath);

  cuda_gdb_tmp_dir = xmalloc (strlen (dirpath) + 1);
  strncpy (cuda_gdb_tmp_dir, dirpath, strlen (dirpath) + 1);
  make_final_cleanup (cuda_gdb_tmpdir_cleanup_self, NULL);
}

const char*
cuda_gdb_tmpdir_getdir (void)
{
  return cuda_gdb_tmp_dir;
}

static cuda_clock_t cuda_clock_ = 0;

cuda_clock_t
cuda_clock (void)
{
  return cuda_clock_;
}

void
cuda_clock_increment (void)
{
  ++cuda_clock_;
  if (cuda_clock_ == 0)
    warning (_("The internal clock counter used for cuda debugging wrapped around.\n"));
}

void
cuda_utils_initialize (void)
{
  /* Create the base temporary directory */
  cuda_gdb_tmpdir_create_basedir ();

  /* Create a lockfile to prevent multiple instances of cuda-gdb from
   * interfering with each other */
  cuda_gdb_lock_file_create ();

  /* Populate the temporary directory with a unique subdirectory for this
   * instance. */
  cuda_gdb_tmpdir_setup ();
}
