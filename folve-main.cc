//  Folve - A fuse filesystem that convolves audio files on-the-fly.
//
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
#include <fuse/fuse.h>

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
#include "util.h"

// Compilation unit variables to communicate with the fuse callbacks.
static struct FolveRuntime {
  FolveRuntime() : fs(NULL), mount_point(NULL),
                   status_port(-1), refresh_time(10), parameter_error(false) {}
  FolveFilesystem *fs;
  const char *mount_point;
  int status_port;
  int refresh_time;
  bool parameter_error;
} folve_rt;

static char *concat_path(char *buf, const char *a, const char *b) {
  strcpy(buf, a);
  strcat(buf, b);
  return buf;
}

// Given a relative path from the root of the mounted file-system, get the
// original file from the source filesystem.
static const char *assemble_orig_path(char *buf, const char *path) {
  return concat_path(buf, folve_rt.fs->underlying_dir().c_str(), path);
}

// Essentially lstat(). Just forward to the original filesystem (this
// will by lying: our convolved files are of different size...)
static int folve_getattr(const char *path, struct stat *stbuf) {
  // If this is a currently open filename, we might be able to output a better
  // estimate.
  int result = folve_rt.fs->StatByFilename(path, stbuf);
  if (result != 0) {
    char path_buf[PATH_MAX];
    result = lstat(assemble_orig_path(path_buf, path), stbuf);
    if (result == -1)
      return -errno;
  }
  // Whatever write mode was there before: now things are readonly.
  stbuf->st_mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
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

  // Entry size is a bit shaky to calculate, but this should be the upper bound.
  const size_t entry_size = sizeof(struct dirent) + PATH_MAX;
  struct dirent *entry_buf = (struct dirent *) malloc(entry_size);

  while (readdir_r(dp, entry_buf, &de) == 0 && de != NULL) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = de->d_ino;
    st.st_mode = de->d_type << 12;
    const char *entry_name = de->d_name;
    if (filler(buf, entry_name, &st, 0))
      break;
  }
  free(entry_buf);

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
  FileHandler *handler = folve_rt.fs->GetOrCreateHandler(path);
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
  folve_rt.fs->Close(path, reinterpret_cast<FileHandler *>(fi->fh));
  return 0;
}

static int folve_fgetattr(const char *path, struct stat *result,
                          struct fuse_file_info *fi) {
  return reinterpret_cast<FileHandler *>(fi->fh)->Stat(result);
}

static void *folve_init(struct fuse_conn_info *conn) {
  const int ident_len = 20;
  char *ident = (char*) malloc(ident_len);  // openlog() keeps reference. Leaks.
  snprintf(ident, ident_len, "folve[%d]", getpid());
  openlog(ident, LOG_CONS|LOG_PERROR, LOG_USER);
  syslog(LOG_INFO, "Version " FOLVE_VERSION " started. "
         "Serving '%s' on mount point '%s'",
         folve_rt.fs->underlying_dir().c_str(), folve_rt.mount_point);
  if (folve::IsDebugLogEnabled()) {
    syslog(LOG_INFO, "Debug logging enabled (-D)");
  }

  if (folve_rt.status_port > 0) {
    // Need to start status server after we're daemonized.
    StatusServer *status_server = new StatusServer(folve_rt.fs);
    if (status_server->Start(folve_rt.status_port)) {
      syslog(LOG_INFO, "HTTP status server on port %d; refresh=%d",
             folve_rt.status_port, folve_rt.refresh_time);
      status_server->set_meta_refresh(folve_rt.refresh_time);
    } else {
      syslog(LOG_ERR, "Couldn't start HTTP server on port %d\n",
             folve_rt.status_port);
    }
  }

  // Some sanity checks.
  if (folve_rt.fs->config_dirs().size() == 1) {
    syslog(LOG_NOTICE, "No filter configuration directories given. "
           "Any files will be just passed through verbatim.");
  }
  if (folve_rt.fs->config_dirs().size() > 2 && folve_rt.status_port < 0) {
    syslog(LOG_WARNING, "Multiple filter configurations given, but no HTTP "
           "status port. You only can switch filters via the HTTP interface; "
           "add -p <port>");
  }
  return NULL;
}

static void folve_destroy(void *) {
  syslog(LOG_INFO, "Exiting.");
}

static int usage(const char *prg) {
  printf("usage: %s [options] <original-dir> <mount-point-dir>\n", prg);
  printf("Options: (in sequence of usefulness)\n"
         "\t-c <cfg-dir> : Convolver configuration directory.\n"
         "\t               You can supply this option multiple times:\n"
         "\t               Select on the HTTP status page.\n"
         "\t-p <port>    : Port to run the HTTP status server on.\n"
         "\t-r <refresh> : Seconds between refresh of status page;\n"
         "\t               Default is %d seconds; switch off with -1.\n"
         "\t-g           : Gapless convolving alphabetically adjacent files.\n"
         "\t-D           : Moderate volume Folve debug messages to syslog,\n"
         "\t               and some more detailed configuration info in UI\n"
         "\t-f           : Operate in foreground; useful for debugging.\n"
         "\t-o <mnt-opt> : other generic mount parameters passed to FUSE.\n"
         "\t-d           : High volume FUSE debug log. Implies -f.\n",
         folve_rt.refresh_time);
  return 1;
}

struct FolveConfig {
  FolveConfig() : base_dir(NULL), config_dir(NULL), port(-1) {}
  const char *base_dir;
  const char *mount_point;
  const char *config_dir;
  int port;
};

enum {
  FOLVE_OPT_PORT = 42,
  FOLVE_OPT_REFRESH_TIME,
  FOLVE_OPT_CONFIG,
  FOLVE_OPT_DEBUG,
  FOLVE_OPT_GAPLESS,
};

int FolveOptionHandling(void *data, const char *arg, int key,
                        struct fuse_args *outargs) {
  char realpath_buf[PATH_MAX];  // running as daemon, need absolute names.
  FolveRuntime *rt = (FolveRuntime*) data;
  switch (key) {
  case FUSE_OPT_KEY_NONOPT:
    // First non-opt: our underlying dir.
    if (rt->fs->underlying_dir().empty()) {
      const char *base_dir = realpath(arg, realpath_buf);
      if (base_dir != NULL) {
        rt->fs->set_underlying_dir(base_dir);
      } else {
        fprintf(stderr, "Invalid base path '%s': %s\n",
                arg, strerror(errno));
        rt->parameter_error = true;
      }
      return 0;   // we consumed this.
    } else {
      rt->mount_point = strdup(arg);  // remmber as FYI
      return 1;   // .. but leave it to fuse
    }
  case FOLVE_OPT_PORT:
    rt->status_port = atoi(arg + 2);  // strip "-p"
    return 0;
  case FOLVE_OPT_REFRESH_TIME:
    rt->refresh_time = atoi(arg + 2);  // strip "-r"
    return 0;
  case FOLVE_OPT_CONFIG: {
    const char *config_dir = realpath(arg + 2, realpath_buf);  // strip "-c"
    if (config_dir != NULL) {
      rt->fs->add_config_dir(config_dir);
    } else {
      fprintf(stderr, "Invalid config dir '%s': %s\n", 
              arg + 2, strerror(errno));
      rt->parameter_error = true;
    }
    return 0;
  }
  case FOLVE_OPT_DEBUG:
    // rt->fs->set_debug_ui_enabled(true); // Disabled in status-server.
    folve::EnableDebugLog(true);
    return 0;
  case FOLVE_OPT_GAPLESS:
    rt->fs->set_gapless_processing(true);
    return 0;
  }
  return 1;
}

int main(int argc, char *argv[]) {
  const char *progname = argv[0];
  if (argc < 4) {
    return usage(progname);
  }

  folve_rt.fs = new FolveFilesystem();

  static struct fuse_opt folve_options[] = {
    FUSE_OPT_KEY("-p ", FOLVE_OPT_PORT),
    FUSE_OPT_KEY("-r ", FOLVE_OPT_REFRESH_TIME),
    FUSE_OPT_KEY("-c ", FOLVE_OPT_CONFIG),
    FUSE_OPT_KEY("-D",  FOLVE_OPT_DEBUG),
    FUSE_OPT_KEY("-g",  FOLVE_OPT_GAPLESS),
    FUSE_OPT_END   // This fails to compile for fuse <= 2.8.1; get >= 2.8.4
  };
  struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  fuse_opt_parse(&args, &folve_rt, folve_options, FolveOptionHandling);

  if (folve_rt.parameter_error || !folve_rt.fs->CheckInitialized()) {
    return usage(progname);
  }

  struct fuse_operations folve_operations;
  memset(&folve_operations, 0, sizeof(folve_operations));

  // Start/stop. Will write to syslog and start auxiliary http service.
  folve_operations.init      = folve_init;
  folve_operations.destroy   = folve_destroy;

  // Basic operations to make navigation work.
  folve_operations.readdir   = folve_readdir;
  folve_operations.readlink  = folve_readlink;

  // open() and close() file.
  folve_operations.open	     = folve_open;
  folve_operations.release   = folve_release;

  // Actual workhorse: reading a file and returning predicted file-size
  folve_operations.read      = folve_read;
  folve_operations.fgetattr  = folve_fgetattr;
  folve_operations.getattr   = folve_getattr;

  return fuse_main(args.argc, args.argv, &folve_operations, NULL);
}
