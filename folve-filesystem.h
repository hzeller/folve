//  -*- c++ -*-
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

#ifndef FOLVE_FILESYSTEM_H
#define FOLVE_FILESYSTEM_H

#include <unistd.h>

#include <string>
#include <vector>

#include "file-handler-cache.h"
#include "file-handler.h"

#ifndef FOLVE_VERSION
#define FOLVE_VERSION "[unknown version - compile from git]"
#endif

class FolveFilesystem {
public:
  // Create a new filesystem. At least SetBasedir() needs to be called
  // for this to be properly initialized.
  FolveFilesystem();

  // Underlying directory - the directory we read files from.
  void set_underlying_dir(const std::string &dir) { underlying_dir_ = dir; }
  const std::string &underlying_dir() const { return underlying_dir_; }

  // Config directories contain the filter configurations.
  void add_config_dir(const char *config_dir) {
    config_dirs_.push_back(config_dir);
  }
  const std::vector<std::string> &config_dirs() const { return config_dirs_; }

  // Switch the current config to i. Values out of range are not accepted.
  void SwitchCurrentConfigIndex(int i);
  int current_cfg_index() const { return current_cfg_index_; }

  // Check if properly initialized. Return 'false' if not and print a message
  // to stderr.
  bool CheckInitialized();

  // Create a new filter given the filesystem path and the underlying
  // path.
  // Returns NULL, if it cannot be created.
  FileHandler *CreateHandler(const char *fs_path,
                             const char *underlying_path);
  
  // Return dynamic size of file.
  int StatByFilename(const char *fs_path, struct stat *st);
  
  // Inform filesystem that this file handler is not needed anymore
  // (FS still might consider keeping it around for a while).
  void Close(const char *fs_path, const FileHandler *handler);

  FileHandlerCache *handler_cache() { return &open_file_cache_; }

  // Some stats.
  int total_file_openings() { return total_file_openings_; }
  int total_file_reopen() { return total_file_reopen_; }

private:
  // Get cache key, depending on the given configuration.
  std::string CacheKey(int config_idx, const char *fs_path);

  FileHandler *CreateFromDescriptor(int filedes, int cfg_idx,
                                    const char *fs_path,
                                    const char *underlying_file);

  std::string underlying_dir_;
  std::vector<std::string> config_dirs_;
  int current_cfg_index_;

  FileHandlerCache open_file_cache_;
  int total_file_openings_;
  int total_file_reopen_;
};

#endif // FOLVE_FILESYSTEM_H
