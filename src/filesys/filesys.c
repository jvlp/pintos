#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/cache.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);
static bool parse_path (const char *path, struct dir **dir, char name[NAME_MAX + 1]);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  cache_init ();
  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  cache_shutdown_threads ();
  free_map_close ();
  cache_flush ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  char file_name[NAME_MAX + 1];
  struct dir *dir = NULL;
  bool success = (parse_path (name, &dir, file_name)
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, false)
                  && dir_add (dir, file_name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct dir *dir = NULL;
  struct inode *inode = NULL;
  char file_name[NAME_MAX + 1];

  if (name != NULL && !strcmp (name, "/"))
    inode = inode_open (ROOT_DIR_SECTOR);
  else if (parse_path (name, &dir, file_name))
    dir_lookup (dir, file_name, &inode);
  dir_close (dir);

  if (inode != NULL && inode_is_dir (inode))
    {
      inode_close (inode);
      return NULL;
    }
  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  struct dir *dir = NULL;
  struct inode *inode = NULL;
  char file_name[NAME_MAX + 1];
  bool success = false;

  if (!parse_path (name, &dir, file_name))
    return false;

  if (!strcmp (file_name, ".") || !strcmp (file_name, ".."))
    goto done;

  if (!dir_lookup (dir, file_name, &inode))
    goto done;

  if (inode_is_dir (inode))
    {
      struct dir *victim = dir_open (inode_reopen (inode));
      if (victim == NULL || !dir_is_empty (victim))
        {
          dir_close (victim);
          goto done;
        }
      dir_close (victim);
    }
  success = dir_remove (dir, file_name);

 done:
  inode_close (inode);
  dir_close (dir); 

  return success;
}

bool
filesys_create_dir (const char *name)
{
  block_sector_t inode_sector = 0;
  char dir_name[NAME_MAX + 1];
  struct dir *parent = NULL;
  bool success = false;

  if (!parse_path (name, &parent, dir_name))
    return false;

  success = (free_map_allocate (1, &inode_sector)
             && dir_create (inode_sector, 16,
                            inode_get_inumber (dir_get_inode (parent)))
             && dir_add (parent, dir_name, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  dir_close (parent);
  return success;
}

struct dir *
filesys_open_dir (const char *name)
{
  struct dir *dir = NULL;
  struct inode *inode = NULL;
  char file_name[NAME_MAX + 1];

  if (name != NULL && !strcmp (name, "/"))
    inode = inode_open (ROOT_DIR_SECTOR);
  else if (parse_path (name, &dir, file_name))
    dir_lookup (dir, file_name, &inode);
  dir_close (dir);

  if (inode == NULL || !inode_is_dir (inode))
    {
      inode_close (inode);
      return NULL;
    }
  return dir_open (inode);
}

bool
filesys_chdir (const char *name)
{
  struct dir *dir = filesys_open_dir (name);
  if (dir == NULL)
    return false;

  dir_close (thread_current ()->cwd);
  thread_current ()->cwd = dir;
  return true;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16, ROOT_DIR_SECTOR))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

static struct dir *
start_dir_for_path (const char *path)
{
  if (path == NULL)
    return NULL;
  if (path[0] == '/')
    return dir_open_root ();
  if (thread_current ()->cwd != NULL)
    return dir_reopen (thread_current ()->cwd);
  return dir_open_root ();
}

static bool
parse_path (const char *path, struct dir **dir, char name[NAME_MAX + 1])
{
  struct dir *cur;
  char *copy;
  char *token;
  char *next;
  char *save_ptr;
  bool success = false;

  if (path == NULL || path[0] == '\0' || dir == NULL || name == NULL)
    return false;

  copy = malloc (strlen (path) + 1);
  if (copy == NULL)
    return false;
  strlcpy (copy, path, strlen (path) + 1);

  cur = start_dir_for_path (path);
  if (cur == NULL)
    goto done;
  if (inode_is_removed (dir_get_inode (cur)))
    goto done;

  token = strtok_r (copy, "/", &save_ptr);
  if (token == NULL)
    {
      strlcpy (name, ".", NAME_MAX + 1);
      *dir = cur;
      success = true;
      goto done_keep_dir;
    }

  while (token != NULL)
    {
      next = strtok_r (NULL, "/", &save_ptr);
      if (next == NULL)
        {
          if (strlen (token) > NAME_MAX)
            goto done;
          strlcpy (name, token, NAME_MAX + 1);
          *dir = cur;
          success = true;
          goto done_keep_dir;
        }
      else
        {
          struct inode *inode = NULL;
          struct dir *next_dir;
          if (!dir_lookup (cur, token, &inode) || !inode_is_dir (inode))
            {
              inode_close (inode);
              goto done;
            }
          if (inode_is_removed (inode))
            {
              inode_close (inode);
              goto done;
            }
          next_dir = dir_open (inode);
          dir_close (cur);
          cur = next_dir;
        }
      token = next;
    }

 done:
  dir_close (cur);
 done_keep_dir:
  free (copy);
  return success;
}
