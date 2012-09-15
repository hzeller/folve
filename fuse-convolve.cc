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

// This fuse filesystem only provides read access to another filesystem that
// contains *.flac and *.wav files. Accessing these files passes them
// through a zita-filter transparently.
// This is a pure C implementation, providing basic file operations and passing
// actual reading to a filter aquired using the functions in filter-interface.h

// Use latest version.
#define FUSE_USE_VERSION 26

#define FUSE_CONVOLVE_VERSION_INFO "v. 0.79 &mdash; 2012-09-15"

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

#include "convolver-filesystem.h"
#include "status-server.h"

ConvolverFilesystem *convolver_fs = NULL;
const char *orig_dir;

static int has_suffix_string (const char *str, const char *suffix) {
  if (!str || !suffix)
    return 0;
  size_t str_len = strlen(str);
  size_t suffix_len = strlen(suffix);
  if (suffix_len > str_len)
    return 0;
  return strncmp(str + str_len - suffix_len, suffix, suffix_len) == 0;
}

static char *concat_path(char *buf, const char *a, const char *b) {
  strcpy(buf, a);
  strcat(buf, b);
  return buf;
}

// Given a relative path from the root of the mounted file-system, get the
// original file from the source filesystem.
// TODO(hzeller): move the ogg.fuse.flac logic into convolver-filesystem.
static const char *assemble_orig_path(char *buf, const char *path) {
  char *result = concat_path(buf, orig_dir, path);
  static const char magic_ogg_rewrite[] = ".ogg.fuse.flac";
  if (has_suffix_string(result, magic_ogg_rewrite)) {
    *(result + strlen(result) - strlen(".fuse.flac")) = '\0';
  }
  return result;
}

// Essentially lstat(). Just forward to the original filesystem (this
// will by lying: our convolved files are of different size...)
static int fuseconv_getattr(const char *path, struct stat *stbuf) {
  // If this is a currently open filename, we might be able to output a better
  // estimate.
  int result = convolver_fs->StatByFilename(path, stbuf);
  if (result == 0) return result;

  char path_buf[PATH_MAX];
  result = lstat(assemble_orig_path(path_buf, path), stbuf);
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

  char ogg_rewrite_buffer[PATH_MAX];

  while ((de = readdir(dp)) != NULL) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = de->d_ino;
    st.st_mode = de->d_type << 12;
    const char *entry_name = de->d_name;
    if (has_suffix_string(entry_name, ".ogg")) {
      // For ogg files, we pretend they actually end with 'flac', because
      // we transparently rewrite them.
      entry_name = concat_path(ogg_rewrite_buffer, entry_name, ".fuse.flac");
    }
    if (filler(buf, entry_name, &st, 0))
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
  // We want to be allowed to only return part of the requested data in read().
  // That way, we can separate reading the ID3-tags from
  // decoding of the music stream - that way indexing should be fast.
  // Setting the flag 'direct_io' allows us to return partial results.
  fi->direct_io = 1;

  // The file-handle has the neat property to be 64 bit - so we can actually
  // store a pointer to our filte robject in there :)
  // (Yay, someone was thinking while developing that API).
  char path_buf[PATH_MAX];
  const char *orig_path = assemble_orig_path(path_buf, path);
  FileHandler * handler = convolver_fs->CreateHandler(path, orig_path);
  if (handler == NULL)
    return -errno;
  fi->fh = (uint64_t) handler;
  return 0;
}

static int fuseconv_read(const char *path, char *buf, size_t size, off_t offset,
                         struct fuse_file_info *fi) {
  return reinterpret_cast<FileHandler *>(fi->fh)->Read(buf, size, offset);
}

static int fuseconv_release(const char *path, struct fuse_file_info *fi) {
  convolver_fs->Close(path);
  return 0;
}

static int fuseconv_fgetattr(const char *path, struct stat *result,
                             struct fuse_file_info *fi) {
  return reinterpret_cast<FileHandler *>(fi->fh)->Stat(result);
}


static int usage(const char *prog) {
  fprintf(stderr, "usage: %s <config-dir> <original-dir> <mount-point>\n",
          prog);
  return 1;
}

int main(int argc, char *argv[]) {
  if (argc < 4) {
    return usage(argv[0]);
  }
  
  // First, let's extract our configuration.
  const char *config_dir = argv[1];
  orig_dir   = argv[2];
  argc -=2;
  argv += 2;
  convolver_fs = new ConvolverFilesystem(FUSE_CONVOLVE_VERSION_INFO,
                                         orig_dir, config_dir);
  
  // TODO(hzeller): make this configurable
  StatusServer *statusz = new StatusServer(convolver_fs);
  statusz->Start(17322);

  struct fuse_operations fuseconv_operations;
  memset(&fuseconv_operations, 0, sizeof(fuseconv_operations));

  // Basic operations to make navigation work.
  fuseconv_operations.readdir	= fuseconv_readdir;
  fuseconv_operations.readlink	= fuseconv_readlink;

  // open() and close() file.
  fuseconv_operations.open	= fuseconv_open;
  fuseconv_operations.release   = fuseconv_release;

  // Actual workhorse: reading a file and returning predicted file-size
  fuseconv_operations.read	= fuseconv_read;
  fuseconv_operations.fgetattr  = fuseconv_fgetattr;
  fuseconv_operations.getattr	= fuseconv_getattr;

  // Lazy: let the rest handle by fuse provided main.
  return fuse_main(argc, argv, &fuseconv_operations, NULL);
}
