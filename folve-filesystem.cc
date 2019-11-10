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

#include "folve-filesystem.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include <map>
#include <string>
#include <zita-convolver.h>

#include "buffer-thread.h"
#include "convolve-file-handler.h"
#include "file-handler-cache.h"
#include "file-handler.h"
#include "pass-through-handler.h"
#include "util.h"

FolveFilesystem::FolveFilesystem()
  : gapless_processing_(false), toplevel_dir_is_filter_(false),
    pre_buffer_size_(128 << 10),
    open_file_cache_(4),
    processor_pool_(3), buffer_thread_(NULL),
    total_file_openings_(0), total_file_reopen_(0),
    // oversize factor of 1.25 seems to be a good initial size.
    file_oversize_factor_(1.25),
    workaround_flac_header_issue_(false) {
}

void FolveFilesystem::RequestPrebuffer(ConversionBuffer *buffer) {
  if (pre_buffer_size_ <= 0) return;
  if (buffer_thread_ == NULL) {
    buffer_thread_ = new BufferThread(pre_buffer_size_);
    buffer_thread_->Start();
  }
  buffer_thread_->EnqueueWork(buffer);
}

void FolveFilesystem::QuitBuffering(ConversionBuffer *buffer) {
  if (buffer_thread_ != NULL) buffer_thread_->Forget(buffer);
}

FileHandler *FolveFilesystem::CreateFromDescriptor(
     int filedes,
     const std::string &config_dir,
     const char *fs_path,
     const std::string &underlying_file) {
  HandlerStats file_info;
  file_info.filename = fs_path;
  file_info.filter_dir = config_dir;
  if (!config_dir.empty()) {
    const std::string full_config_path = base_config_dir_ + "/" + config_dir;
    FileHandler *handler = ConvolveFileHandler::Create(this, filedes, fs_path,
                                                       underlying_file,
                                                       config_dir,
                                                       full_config_path,
                                                       &file_info);
    if (handler != NULL) return handler;
  }
  // Every other file-type is just passed through as is.
  return new PassThroughHandler(filedes, config_dir, file_info);
}

std::string FolveFilesystem::CacheKey(const std::string &config_path,
                                      const char *fs_path) {
  return config_path + fs_path;
}

bool FolveFilesystem::ExtractFilterName(const char *path,
                                        std::string *filter) const {
  if (toplevel_directory_is_filter()) {
    const char *found = strchr(path + 1, '/');
    if (found == NULL) return false;  // not even a complete first path-element
    filter->assign(path + 1, found - path - 1);
    if (*filter == "_") { filter->clear(); }
    return GetAvailableConfigDirs().count(*filter) != 0;
  } else {
    *filter = current_config_subdir_;
    return true;
  }
}

FileHandler *FolveFilesystem::GetOrCreateHandler(const char *fs_path,
                                                 bool want_gapless) {
  std::string config_path;
  if (!ExtractFilterName(fs_path, &config_path)) {
    errno = ENOENT;   // Invalid toplevel directory.
    return NULL;
  }
  const std::string cache_key = CacheKey(config_path, fs_path);
  const std::string underlying_file = GetUnderlyingFile(fs_path);
  FileHandler *handler = open_file_cache_.FindAndPin(cache_key, want_gapless);
  if (handler == NULL) {
    int filedes = open(underlying_file.c_str(), O_RDONLY);
    if (filedes < 0)
      return NULL;
    ++total_file_openings_;
    handler = CreateFromDescriptor(filedes, config_path,
                                   fs_path, underlying_file);
    handler = open_file_cache_.InsertPinned(cache_key, handler);
  } else {
    ++total_file_reopen_;
  }
  return handler;
}

std::string FolveFilesystem::GetUnderlyingFile(const char *path) const {
  if (toplevel_directory_is_filter()) {  // chuck of the first part.
    const char *found = strchr(path + 1, '/');
    path = found ? found : "";
    // TODO: we're not testing if the toplevel directory=filtername actually
    // exists. So it is possible to ls /mnt/invalid-dir. Not a big deal and
    // costs some config-dir scanning, so avoiding for performance right now.
  }
  return underlying_dir() + path;
}


int FolveFilesystem::StatByFilename(const char *fs_path, struct stat *st) {
  const std::string cache_key = CacheKey(current_config_subdir_, fs_path);
  FileHandler *handler = open_file_cache_.FindAndPin(cache_key);
  if (handler == 0)
    return -1;
  ssize_t result = handler->Stat(st);
  open_file_cache_.Unpin(cache_key);
  return result;
}

void FolveFilesystem::Close(const char *fs_path, const FileHandler *handler) {
  assert(handler != NULL);
  const std::string cache_key = CacheKey(handler->filter_dir(), fs_path);
  open_file_cache_.Unpin(cache_key);
}

static bool IsDirectory(const std::string &path) {
  if (path.empty()) return false;
  struct stat st;
  if (stat(path.c_str(), &st) != 0)
    return false;
  return (st.st_mode & S_IFMT) == S_IFDIR;
}

bool FolveFilesystem::ListDirectory(const std::string &fs_dir,
                                    const std::string &suffix,
                                    std::set<std::string> *files) {
  const std::string real_dir = GetUnderlyingFile(fs_dir.c_str());
  DIR *dp = opendir(real_dir.c_str());
  if (dp == NULL) return false;
  struct dirent *dent;
  while ((dent = readdir(dp)) != NULL) {
    if (!folve::HasSuffix(dent->d_name, suffix))
      continue;
    files->insert(fs_dir + dent->d_name);
  }
  closedir(dp);
  return true;
}

bool FolveFilesystem::SanitizeConfigSubdir(std::string *subdir_path) const {
  if (base_config_dir_.length() + 1 + subdir_path->length() > PATH_MAX)
    return false;  // uh, someone wants to buffer overflow us ?
  const std::string to_verify_path = base_config_dir_ + "/" + *subdir_path;
  char all_path[PATH_MAX];
  // This will as well eat symbolic links that break out, though one could
  // argue that that would be sane. We could think of some light
  // canonicalization that only removes ./ and ../
  const char *verified = realpath(to_verify_path.c_str(), all_path);
  if (verified == NULL) { // bogus directory.
    return false;
  }
  if (strncmp(verified, base_config_dir_.c_str(),
              base_config_dir_.length()) != 0) {
    // Attempt to break out with ../-tricks.
    return false;
  }
  if (!IsDirectory(verified))
    return false;

  // Derive from sanitized dir. So someone can write lowpass/../highpass
  // or '.' for empty filter. Or ./highpass. And all work.
  *subdir_path = ((strlen(verified) == base_config_dir_.length())
                  ? ""   // chose subdir '.'
                  : verified + base_config_dir_.length() + 1 /*slash*/);
  return true;
}

bool FolveFilesystem::SwitchCurrentConfigDir(const std::string &subdir_in) {
  std::string subdir = subdir_in;
  if (!subdir.empty() && !SanitizeConfigSubdir(&subdir)) {
    syslog(LOG_INFO, "Invalid config switch attempt to '%s'",
           subdir_in.c_str());
    return false;
  }
  if (subdir != current_config_subdir_) {
    current_config_subdir_ = subdir;
    if (subdir.empty()) {
      syslog(LOG_INFO, "Switching to pass-through mode.");
    } else {
      syslog(LOG_INFO, "Switching config directory to '%s'", subdir.c_str());
    }
    return true;
  }
  return false;
}

bool FolveFilesystem::CheckInitialized() {
  if (underlying_dir().empty()) {
    fprintf(stderr, "Don't know the underlying directory to read from.\n");
    return false;
  }

  if (!IsDirectory(underlying_dir())) {
    fprintf(stderr, "<underlying-dir>: '%s' not a directory.\n",
            underlying_dir().c_str());
    return false;
  }

  if (base_config_dir_.empty() || !IsDirectory(base_config_dir_)) {
    fprintf(stderr, "<config-dir>: '%s' not a directory.\n",
            base_config_dir_.c_str());
    return false;
  }

  return true;
}

void FolveFilesystem::SetupInitialConfig() {
  std::set<std::string> available_dirs = ListConfigDirs(true);
  // Some sanity checks.
  if (available_dirs.size() == 1) {
    syslog(LOG_NOTICE, "No filter configuration directories given. "
           "Any files will be just passed through verbatim.");
  }
  if (available_dirs.size() > 1) {
    // By default, lets set the index to the first filter the user provided.
    SwitchCurrentConfigDir(*++available_dirs.begin());
  }
}

const std::set<std::string> FolveFilesystem::GetAvailableConfigDirs() const {
  return ListConfigDirs(false);
}

const std::set<std::string> FolveFilesystem::ListConfigDirs(bool warn_invalid)
  const {
  std::set<std::string> result;
  result.insert("");  // empty directory: pass-through.
  DIR *dp = opendir(base_config_dir_.c_str());
  if (dp == NULL) return result;
  struct dirent *dent;
  while ((dent = readdir(dp)) != NULL) {
    std::string subdir = dent->d_name;
    if (subdir == "." || subdir == "..")
      continue;
    if (!SanitizeConfigSubdir(&subdir)) {
      if (warn_invalid) {
        syslog(LOG_INFO, "Note: '%s' ignored in config directory; not a "
               "directory or pointing outside base directory.", dent->d_name);
      }
      continue;
    }
    result.insert(subdir);
  }
  closedir(dp);
  return result;
}
