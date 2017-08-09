//  -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
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
#include <sndfile.h>  // for sf_version_string
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>   // need to call gettid syscall.
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include "folve-filesystem.h"
#include "status-server.h"
#include "util.h"

static const char kStatusFileName[] = "/folve-status.html";
static const int kUsefulMinBuf = 64;
static const int kUsefulMaxBuf = 16384;

// Compilation unit variables to communicate with the fuse callbacks.
static struct FolveRuntime {
  FolveRuntime() : fs(NULL), mount_point(NULL), pid_file(NULL),
                   status_port(-1), refresh_time(10), parameter_error(false),
                   readdir_dump_file(NULL), status_server(NULL) {}
  FolveFilesystem *fs;
  const char *mount_point;
  const char *pid_file;
  int status_port;
  int refresh_time;
  bool parameter_error;
  FILE *readdir_dump_file;
  StatusServer *status_server;
} folve_rt;

// Logger that only prints to stderr; used for
class ReaddirLogger {
public:
  ReaddirLogger() : start_time_(folve::CurrentTime()) {}

  void WriteInit() {
    if (!folve_rt.readdir_dump_file) return;
    fprintf (folve_rt.readdir_dump_file, "%-11s %-8s: <log>\n",
             "#  time", "  tid");
    fflush(folve_rt.readdir_dump_file);
  }

  ReaddirLogger &Log(const char *fmt, ...)
  __attribute__ ((format (printf, 2, 3))) {
    if (!folve_rt.readdir_dump_file) return *this;
    fprintf (folve_rt.readdir_dump_file, "%011.6f %08lx: ",
             folve::CurrentTime() - start_time_, syscall(SYS_gettid));
    va_list ap;
    va_start(ap, fmt);
    folve::MutexLock l(&io_mutex_);
    vfprintf(folve_rt.readdir_dump_file, fmt, ap);
    va_end(ap);
    return *this;
  }

  void Flush() {
    if (folve_rt.readdir_dump_file) {
      folve::MutexLock l(&io_mutex_);
      fflush(folve_rt.readdir_dump_file);
    }
  }

private:
  const double start_time_;
  folve::Mutex io_mutex_;
} rlog;

// Essentially lstat(). Just forward to the original filesystem (this
// will by lying: our convolved files are of different size...)
static int folve_getattr(const char *path, struct stat *stbuf) {
  if (strcmp(path, kStatusFileName) == 0) {
    FileHandler *status = folve_rt.status_server->CreateStatusFileHandler();
    status->Stat(stbuf);
    delete status;
    return 0;
  }
  // If this is a currently open filename, we might be able to output a better
  // estimate.
  int result = folve_rt.fs->StatByFilename(path, stbuf);
  if (result != 0) {
    result = lstat(folve_rt.fs->GetUnderlyingFile(path).c_str(), stbuf);
    rlog.Log("STAT %s mode=%03o %s %s %s", path,
             stbuf->st_mode & 0777, S_ISDIR(stbuf->st_mode) ? "DIR" : "",
             (result == -1) ? strerror(errno) : "",
             ctime(&stbuf->st_mtime));  // ctime ends with \n, so put that last
    stbuf->st_size *= folve_rt.fs->file_oversize_factor();
    if (result == -1)
      return -errno;
  } else {
    rlog.Log("FOLVE-Stat %s\n", path);
  }
  // Whatever write mode was there before: now things are readonly.
  stbuf->st_mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
  return 0;
}

// readdir(). Just forward to original filesystem.
static int folve_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi) {
  if (strcmp(path, "/") == 0) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    filler(buf, kStatusFileName + 1, &st, 0);

    // If configured, toplevel directories represent the filter names
    if (folve_rt.fs->toplevel_directory_is_filter()) {
      typedef std::set<std::string> dirset_t;
      const dirset_t &dirs = folve_rt.fs->GetAvailableConfigDirs();
      for (dirset_t::const_iterator it = dirs.begin(); it != dirs.end(); ++it) {
        // Use underscore for the passthrough-path
        const char *pathname = it->empty() ? "_" : it->c_str();
        memset(&st, 0, sizeof(st));
        filler(buf, pathname, &st, 0);
      }
      return 0;
    }
  }

  DIR *dp;
  dp = opendir(folve_rt.fs->GetUnderlyingFile(path).c_str());
  if (dp == NULL)
    return -errno;

  rlog.Log("LIST %s\n", path);
  struct dirent *de;
  while ((de = readdir(dp)) != NULL) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = de->d_ino;
    st.st_mode = de->d_type << 12;
    const char *entry_name = de->d_name;
    rlog.Log("ITEM %s%s%s\n", path, strlen(path) > 1 ? "/" : "", de->d_name);
    if (filler(buf, entry_name, &st, 0)) {
      rlog.Log("DONE (%s)\n", de->d_name);
      break;
    }
  }

  rlog.Log("DONE %s\n", path).Flush();
  closedir(dp);
  return 0;
}

// readlink(): forward to original filesystem.
static int folve_readlink(const char *path, char *buf, size_t size) {
  const int result = readlink(folve_rt.fs->GetUnderlyingFile(path).c_str(),
                              buf, size - 1);
  if (result == -1)
    return -errno;

  buf[result] = '\0';
  return 0;
}

static int folve_open(const char *path, struct fuse_file_info *fi) {
  if (strcmp(path, kStatusFileName) == 0) {
    fi->fh = (uint64_t) folve_rt.status_server->CreateStatusFileHandler();
    return 0;
  }

  // We want to be allowed to only return part of the requested data in read().
  // That way, we can separate reading the ID3-tags from
  // decoding of the music stream - that way indexing should be fast.
  // Setting the flag 'direct_io' allows us to return partial results.
  fi->direct_io = 1;

  // The file-handle has the neat property to be 64 bit - so we can actually
  // stuff a pointer to our file handler object in there :)
  // (Yay, someone was thinking while developing that API).
  FileHandler *handler = folve_rt.fs->GetOrCreateHandler(path);
  fi->fh = (uint64_t) handler;
  if (handler == NULL)
    return -errno;
  return 0;
}

static int folve_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
  return reinterpret_cast<FileHandler *>(fi->fh)->Read(buf, size, offset);
}

static int folve_release(const char *path, struct fuse_file_info *fi) {
  if (strcmp(path, kStatusFileName) == 0) {
    delete reinterpret_cast<FileHandler *>(fi->fh);
  } else {
    folve_rt.fs->Close(path, reinterpret_cast<FileHandler *>(fi->fh));
  }
  return 0;
}

static int folve_fgetattr(const char *path, struct stat *result,
                          struct fuse_file_info *fi) {
  return reinterpret_cast<FileHandler *>(fi->fh)->Stat(result);
}

static void *folve_init(struct fuse_conn_info *conn) {
  if (folve_rt.pid_file) {
    FILE *p = fopen(folve_rt.pid_file, "w+");
    if (p) {
      fprintf(p, "%d\n", getpid());
      fclose(p);
    }
  }
  const int ident_len = 20;
  char *ident = (char*) malloc(ident_len);  // openlog() keeps reference. Leaks.
  snprintf(ident, ident_len, "folve[%d]", getpid());
  openlog(ident, LOG_CONS|LOG_PERROR, LOG_USER);
  syslog(LOG_INFO, "Version " FOLVE_VERSION " started "
         "(with fuse=%d.%d; sndfile=%s). ",
         FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION, sf_version_string());
  syslog(LOG_INFO, "Serving '%s' on mount point '%s'",
         folve_rt.fs->underlying_dir().c_str(), folve_rt.mount_point);
  if (folve::IsDebugLogEnabled()) {
    syslog(LOG_INFO, "Debug logging enabled (-D)");
  }

  // Status server is always used - it serves the status as an HTML file.
  folve_rt.status_server = new StatusServer(folve_rt.fs);
  if (folve_rt.status_port > 0) {
    // Need to start status server after we're daemonized.
    if (folve_rt.status_server->Start(folve_rt.status_port)) {
      syslog(LOG_INFO, "HTTP status server on port %d; refresh=%d",
             folve_rt.status_port, folve_rt.refresh_time);
      folve_rt.status_server->set_meta_refresh(folve_rt.refresh_time);
    } else {
      syslog(LOG_ERR, "Couldn't start HTTP server on port %d\n",
             folve_rt.status_port);
    }
  }

  folve_rt.fs->SetupInitialConfig();
  return NULL;
}

static void folve_destroy(void *) {
  if (folve_rt.readdir_dump_file) {
    fclose(folve_rt.readdir_dump_file);
  }
  syslog(LOG_INFO, "Exiting.");
}

static int usage(const char *prg) {
  printf("usage: %s [options] <original-dir> <mount-point-dir>\n", prg);
  printf("Options: (in sequence of usefulness)\n"
         "\t-C <cfg-dir> : Convolver base configuration directory.\n"
         "\t               Sub-directories name the different filters.\n"
         "\t               Select on the HTTP status page.\n"
         "\t-t           : Filternames show up as toplevel directory instead\n"
         "\t               of being switched in the HTTP status server.\n"
         "\t-p <port>    : Port to run the HTTP status server on.\n"
         "\t-r <refresh> : Seconds between refresh of status page;\n"
         "\t               Default is %d seconds; switch off with -1.\n"
         "\t-g           : Gapless convolving alphabetically adjacent files.\n"
         "\t-b <KibiByte>: Predictive pre-buffer by given KiB (%d...%d). "
         "Disable with -1. Default 128.\n"
         "\t-O <factor>  : Oversize: Multiply orig. file sizes with this. "
         "Default 1.25.\n"
         "\t-o <mnt-opt> : other generic mount parameters passed to FUSE.\n"
         "\t-P <pid-file>: Write PID to this file.\n"
         "\t-D           : Moderate volume Folve debug messages to syslog,\n"
         "\t               and some more detailed configuration info in UI\n"
         "\t-f           : Operate in foreground; useful for debugging.\n"
         "\t-d           : High volume FUSE debug log. Implies -f.\n"
         "\t-R <file>    : Debug readdir() & stat() calls. Output to file.\n",
         folve_rt.refresh_time, kUsefulMinBuf, kUsefulMaxBuf);
  return 1;
}

enum {
  FOLVE_OPT_PORT = 42,
  FOLVE_OPT_PREBUFFER,
  FOLVE_OPT_REFRESH_TIME,
  FOLVE_OPT_CONFIG,
  FOLVE_OPT_OVERSIZE_PREDICT,
  FOLVE_OPT_PID_FILE,
  FOLVE_OPT_DEBUG,
  FOLVE_OPT_DEBUG_READDIR,
  FOLVE_OPT_GAPLESS,
  FOLVE_OPT_TOPLEVEL_DIR_FILTER,
};

int FolveOptionHandling(void *data, const char *arg, int key,
                        struct fuse_args *outargs) {
  char realpath_buf[PATH_MAX];  // Running as daemon, need absolute names.
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

  case FOLVE_OPT_OVERSIZE_PREDICT: {
    char *end;
    const float value = strtof(arg + 2, &end);
    if (*end != '\0') {
      fprintf(stderr, "-O: Invalid number %s\n", arg + 2);
      rt->parameter_error= true;
    } else {
      rt->fs->set_file_oversize_factor(value);
    }
    return 0;
  }

  case FOLVE_OPT_PID_FILE: {
    const char *pid_file = arg + 2;
    if (pid_file[0] != '/') {
      // We need to canonicalize the filename because our
      // cwd will change after becoming a daemon.
      char *buf = (char*) malloc(PATH_MAX);  // will leak. Ok.
      char *result = getcwd(buf, PATH_MAX);
      result = strcat(result, "/");
      rt->pid_file = strcat(result, pid_file);
    } else {
      rt->pid_file = strdup(pid_file);
    }
    return 0;
  }

  case FOLVE_OPT_PREBUFFER: {
    char *end;
    const double value = strtod(arg + 2, &end);
    if (*end != '\0') {
      fprintf(stderr, "Invalid number %s\n", arg + 2);
      rt->parameter_error= true;
    } else if (value > kUsefulMaxBuf) {
      fprintf(stderr, "-b %.1f out of range. More than %d KiB prebuffer ("
              "that is a lot!).\n", value, kUsefulMaxBuf);
      rt->parameter_error= true;
    } else if (value >= 0 && value < kUsefulMinBuf) {
      fprintf(stderr, "-b %.1f is really small. You want more than %d KiB to "
              "be useful, typically between 1024 and 8192 "
              "(roughly 100 KiB is ~1 second buffer).\n",
              value, kUsefulMinBuf);
      rt->parameter_error= true;
    } else {
      rt->fs->set_pre_buffer_size(value < 0 ? -1 : value * (1 << 10));
    }
    return 0;
  }

  case FOLVE_OPT_REFRESH_TIME:
    rt->refresh_time = atoi(arg + 2);  // strip "-r"
    return 0;

  case FOLVE_OPT_CONFIG: {
    const char *config_dir = realpath(arg + 2, realpath_buf);  // strip "-C"
    if (config_dir != NULL) {
      rt->fs->SetBaseConfigDir(config_dir);
    } else {
      fprintf(stderr, "Invalid config dir '%s': %s\n", arg + 2, strerror(errno));
      rt->parameter_error = true;
    }
    return 0;
  }

  case FOLVE_OPT_DEBUG:
    folve::EnableDebugLog(true);
    return 0;

  case FOLVE_OPT_DEBUG_READDIR:
    rt->readdir_dump_file = fopen(arg + 2, "w");
    rlog.WriteInit();
    return 0;

  case FOLVE_OPT_GAPLESS:
    rt->fs->set_gapless_processing(true);
    return 0;

  case FOLVE_OPT_TOPLEVEL_DIR_FILTER:
    rt->fs->set_toplevel_directory_is_filter(true);
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
    FUSE_OPT_KEY("-b ", FOLVE_OPT_PREBUFFER),
    FUSE_OPT_KEY("-r ", FOLVE_OPT_REFRESH_TIME),
    FUSE_OPT_KEY("-C ", FOLVE_OPT_CONFIG),
    FUSE_OPT_KEY("-D",  FOLVE_OPT_DEBUG),
    FUSE_OPT_KEY("-R ",  FOLVE_OPT_DEBUG_READDIR),
    FUSE_OPT_KEY("-O ",  FOLVE_OPT_OVERSIZE_PREDICT),
    FUSE_OPT_KEY("-P ",  FOLVE_OPT_PID_FILE),
    FUSE_OPT_KEY("-g",  FOLVE_OPT_GAPLESS),
    FUSE_OPT_KEY("-t",  FOLVE_OPT_TOPLEVEL_DIR_FILTER),
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
