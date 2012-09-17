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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <syslog.h>
#include <unistd.h>

#include "folve-filesystem.h"
#include "status-server.h"

#ifndef FOLVE_VERSION
#define FOLVE_VERSION "[unknown version - compile from git]"
#endif

// Compilation unit variables to communicate with the fuse callbacks.
static struct FolveParams {
  FolveParams() : fs(NULL), status_port(-1) {}
  FolveFilesystem *fs;
  int status_port;
} folve;

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
  char *result = concat_path(buf, folve.fs->underlying_dir(), path);
  static const char magic_ogg_rewrite[] = ".ogg.fuse.flac";
  if (has_suffix_string(result, magic_ogg_rewrite)) {
    *(result + strlen(result) - strlen(".fuse.flac")) = '\0';
  }
  return result;
}

// Essentially lstat(). Just forward to the original filesystem (this
// will by lying: our convolved files are of different size...)
static int folve_getattr(const char *path, struct stat *stbuf) {
  // If this is a currently open filename, we might be able to output a better
  // estimate.
  int result = folve.fs->StatByFilename(path, stbuf);
  if (result == 0) return result;

  char path_buf[PATH_MAX];
  result = lstat(assemble_orig_path(path_buf, path), stbuf);
  if (result == -1)
    return -errno;

  return 0;
}

// readdir(). Just forward to original filesystem.
static int folve_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
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
static int folve_readlink(const char *path, char *buf, size_t size) {
  char path_buf[PATH_MAX];
  const int result = readlink(assemble_orig_path(path_buf, path),
                              buf, size - 1);
  if (result == -1)
    return -errno;

  buf[result] = '\0';
  return 0;
}

static int folve_open(const char *path, struct fuse_file_info *fi) {
  // We want to be allowed to only return part of the requested data in read().
  // That way, we can separate reading the ID3-tags from
  // decoding of the music stream - that way indexing should be fast.
  // Setting the flag 'direct_io' allows us to return partial results.
  fi->direct_io = 1;

  // The file-handle has the neat property to be 64 bit - so we can actually
  // stuff a pointer to our file handler object in there :)
  // (Yay, someone was thinking while developing that API).
  char path_buf[PATH_MAX];
  const char *orig_path = assemble_orig_path(path_buf, path);
  FileHandler *handler = folve.fs->CreateHandler(path, orig_path);
  if (handler == NULL)
    return -errno;
  fi->fh = (uint64_t) handler;
  return 0;
}

static int folve_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
  return reinterpret_cast<FileHandler *>(fi->fh)->Read(buf, size, offset);
}

static int folve_release(const char *path, struct fuse_file_info *fi) {
  folve.fs->Close(path);
  return 0;
}

static int folve_fgetattr(const char *path, struct stat *result,
                          struct fuse_file_info *fi) {
  return reinterpret_cast<FileHandler *>(fi->fh)->Stat(result);
}

static void *folve_init(struct fuse_conn_info *conn) {
  // If we're running in the foreground, we like to be seen on stderr as well.
  const int ident_len = 20;
  char *ident = (char*) malloc(ident_len);  // openlog() keeps reference. Leaks.
  snprintf(ident, ident_len, "[folve:%d]", getpid());
  openlog(ident, LOG_CONS|LOG_PERROR, LOG_USER);
  syslog(LOG_INFO, "started.");

  if (folve.status_port > 0) {
    // Need to start status server after we're daemonized.
    StatusServer *status_server = new StatusServer(folve.fs);
    if (status_server->Start(folve.status_port)) {
      syslog(LOG_INFO, "HTTP status server on port %d",
             folve.status_port);
    } else {
      syslog(LOG_ERR, "Couldn't start HTTP server on port %d\n",
             folve.status_port);
    }
  }
  return NULL;
}

static void folve_destroy(void *) {
  syslog(LOG_INFO, "exiting.");
}

static int usage(const char *prg) {
  fprintf(stderr, "usage: %s <config-dir> <original-dir> <mount-point>\n", prg);
  return 1;
}

static bool IsDirectory(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  return (st.st_mode & S_IFMT) == S_IFDIR;
}

int main(int argc, char *argv[]) {
  const char *progname = argv[0];
  if (argc < 4) {
    return usage(progname);
  }

  // First, let's extract our own configuration, redact them from argv[] and
  // then pass everything on to fo
  const char *config_dir = argv[1];
  const char *underlying_dir   = argv[2];
  argc -=2;
  argv += 2;
  if (!IsDirectory(config_dir)) {
    fprintf(stderr, "%s <config-dir>: not a directory.\n", config_dir);
    return usage(progname);
  }
  if (!IsDirectory(underlying_dir)) {
    fprintf(stderr, "%s <underlying-dir>: not a directory.\n", underlying_dir);
    return usage(progname);
  }
  folve.fs = new FolveFilesystem(FOLVE_VERSION, underlying_dir, config_dir);
  
  // TODO(hzeller): make this configurable
  folve.status_port = 17322;

  struct fuse_operations folve_operations;
  memset(&folve_operations, 0, sizeof(folve_operations));

  // Start/stop. Will write to syslog and start auxiliary http service.
  folve_operations.init      = folve_init;
  folve_operations.destroy   = folve_destroy;

  // Basic operations to make navigation work.
  folve_operations.readdir   = folve_readdir;
  folve_operations.readlink  = folve_readlink;

  // open() and close() file.
  folve_operations.open	= folve_open;
  folve_operations.release   = folve_release;

  // Actual workhorse: reading a file and returning predicted file-size
  folve_operations.read      = folve_read;
  folve_operations.fgetattr  = folve_fgetattr;
  folve_operations.getattr   = folve_getattr;

  // Lazy: let the rest handle by fuse provided main.
  return fuse_main(argc, argv, &folve_operations, NULL);
}
