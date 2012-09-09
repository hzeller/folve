//  Copyright (C) 2012 Henner Zeller <h.zeller@acm.org>
//    
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

// Use latest version.
#define FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <limits.h>
#include <stdlib.h>

// Configuration for fuse convolver filesystem.
static struct {
  const char *config_dir;
  const char *orig_dir;
} context;

// Given a relative path from the root of the mounted file-system, get the
// original file from the source filesystem.
static const char *assemble_orig_path(char *buf, const char *path) {
  strcpy(buf, context.orig_dir);
  strcat(buf, path);
  return buf;
}

// Essentially stat(). Just forward to the original filesystem (this
// will by lying: our convolved files are of different size...)
static int fuseconv_getattr(const char *path, struct stat *stbuf) {
  char path_buf[PATH_MAX];
  const int result = lstat(assemble_orig_path(path_buf, path), stbuf);
  if (result == -1)
    return -errno;

  return 0;
}

// readdir(). Just forward to original filesystem.
static int fuseconv_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                            off_t offset, struct fuse_file_info *fi) {
  DIR *dp;
  struct dirent *de;
  char path_buf[PATH_MAX];

  dp = opendir(assemble_orig_path(path_buf, path));
  if (dp == NULL)
    return -errno;

  while ((de = readdir(dp)) != NULL) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = de->d_ino;
    st.st_mode = de->d_type << 12;
    if (filler(buf, de->d_name, &st, 0))
      break;
  }

  closedir(dp);
  return 0;
}

// readlink(): forward to original filesystem.
static int fuseconv_readlink(const char *path, char *buf, size_t size) {
  char path_buf[PATH_MAX];
  const int result = readlink(assemble_orig_path(path_buf, path),
                              buf, size - 1);
  if (result == -1)
    return -errno;

  buf[result] = '\0';
  return 0;
}

static int fuseconv_open(const char *path, struct fuse_file_info *fi) {
  fprintf(stderr, "HZ ===== open('%s')\n", path);
  char path_buf[PATH_MAX];
  const int result = open(assemble_orig_path(path_buf, path), fi->flags);

  // We want to return partial reads. That way, we can separate reading the
  // ID3-tags from the stream.
  // In order to return partical content, we need to set the direct_io.
  fi->direct_io = 1;

  if (result == -1)
    return -errno;
  fi->fh = result;
  return 0;
}

static int fuseconv_read(const char *path, char *buf, size_t size, off_t offset,
                         struct fuse_file_info *fi) {
  const int result = pread(fi->fh, buf, size, offset);
  if (result == -1)
    return -errno;
  return result;
}

static int fuseconv_release(const char *path, struct fuse_file_info *fi) {
  fprintf(stderr, "HZ ===== close('%s')\n", path);
  return close(fi->fh) == -1 ? -errno : 0;
}

static struct fuse_operations fuseconv_operations = {
  // Basic operations to make navigation work.
  .readdir	= fuseconv_readdir,
  .getattr	= fuseconv_getattr,
  .readlink	= fuseconv_readlink,

  // open() and close() file.
  .open		= fuseconv_open,
  .release      = fuseconv_release,

  // Actual workhorse: reading a file.
  .read		= fuseconv_read,
};

static void usage(const char *prog) {
  fprintf(stderr, "usage: %s <config-dir> <original-dir> <mount-point>\n",
          prog);
  exit(1);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    usage(argv[0]);
  }
  
  // First, let's extract our configuration.
  context.config_dir = argv[1];
  context.orig_dir   = argv[2];
  argc -=2;
  argv += 2;

  // Lazy: let the rest handle by fuse provided main.
  return fuse_main(argc, argv, &fuseconv_operations, NULL);
}
