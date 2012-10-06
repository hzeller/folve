//  -*- c++ -*-
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

#ifndef FOLVE_FILESYSTEM_H
#define FOLVE_FILESYSTEM_H

#include <unistd.h>

#include <string>
#include <vector>
#include <set>

#include "file-handler-cache.h"
#include "file-handler.h"
#include "processor-pool.h"

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

  // Set the base configuration directory.
  void SetBaseConfigDir(const std::string &config_dir) {
    base_config_dir_ = config_dir;
  }

  // Return a set of available named configurations. Essentially the names
  // of subdirectories in the configuration dir.
  const std::set<std::string> GetAvailableConfigDirs() const;

  // Switch the current config to subdir. Returns 'true', if this was a valid
  // choice and we actually did a switch to a new directory.
  bool SwitchCurrentConfigDir(const std::string &subdir);
  const std::string &current_config_subdir() const {
    return current_config_subdir_;
  }

  // Check if properly initialized. Return 'false' if not and print a message
  // to stderr.
  bool CheckInitialized();

  // After startup: choose the initial configuation.
  void SetupInitialConfig();

  // Create a new filter given the filesystem path and the underlying
  // path.
  // Returns NULL, if it cannot be created.
  FileHandler *GetOrCreateHandler(const char *fs_path);
  
  // Inform filesystem that this file handler is not needed anymore
  // (FS still might consider keeping it around for a while).
  void Close(const char *fs_path, const FileHandler *handler);

  // Return dynamic size of file.
  int StatByFilename(const char *fs_path, struct stat *st);

  // List files in given filesystem directory that match the suffix.
  // Returns a set of filesystem paths of existing files.
  // (We don't want globbing as filenames might contain weird characters).
  bool ListDirectory(const std::string &fs_dir, const std::string &suffix,
                     std::set<std::string> *files);

  FileHandlerCache *handler_cache() { return &open_file_cache_; }

  void set_debug_ui_enabled(bool b) { debug_ui_enabled_ = b; }
  bool is_debug_ui_enabled() const { return debug_ui_enabled_; }

  void set_gapless_processing(bool b) { gapless_processing_ = b; }
  bool gapless_processing() const { return gapless_processing_; }

  // Some stats.
  int total_file_openings() { return total_file_openings_; }
  int total_file_reopen() { return total_file_reopen_; }

private:
  // Get cache key, depending on the given configuration.
  std::string CacheKey(const std::string &config_path, const char *fs_path);

  FileHandler *CreateFromDescriptor(int filedes, const std::string &cfg_dir,
                                    const char *fs_path,
                                    const std::string &underlying_file);

  // Sanitize path to configuration subdirectory. Checks if someone tries
  // to break out of the given base directory.
  // Return if this is a sane directory.
  // Passes the sanitized directory in the parameter.
  bool SanitizeConfigSubdir(std::string *subdir_path) const;

  // List available config directories; if "warn_invalid" is true,
  // non-directories or symbolic links breaking out of the directory are
  // reported.
  const std::set<std::string> ListConfigDirs(bool warn_invalid) const;

  std::string underlying_dir_;
  std::string base_config_dir_;
  std::string current_config_subdir_;
  bool debug_ui_enabled_;
  bool gapless_processing_;
  FileHandlerCache open_file_cache_;
  int total_file_openings_;
  int total_file_reopen_;
};

#endif // FOLVE_FILESYSTEM_H
