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
#  define FOLVE_VERSION "[unknown version - compile from git]"
#endif

class ConversionBuffer;
class BufferThread;

class FolveFilesystem {
public:
  // Create a new filesystem. At least SetBasedir() needs to be called
  // for this to be properly initialized.
  FolveFilesystem();

  // Enable workaround for flac header flushing.
  void set_workaround_flac_header_issue(bool b) {
    workaround_flac_header_issue_ = b;
  }
  bool workaround_flac_header_issue() const { return workaround_flac_header_issue_; }

  // Underlying directory - the directory we read files from.
  void set_underlying_dir(const std::string &dir) { underlying_dir_ = dir; }
  const std::string &underlying_dir() const { return underlying_dir_; }

  // Set the base configuration directory.
  void SetBaseConfigDir(const std::string &config_dir) {
    base_config_dir_ = config_dir;
  }
  const std::string &base_config_dir() const { return base_config_dir_; }

  // Given the path in the folve-filesystem, return the underlying operating
  // system path.
  std::string GetUnderlyingFile(const char *path) const;

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
  // path. If "want_gapless", then attempt to only return one that starts
  // gapless or can me made into one.
  // Returns NULL, if it cannot be created.
  FileHandler *GetOrCreateHandler(const char *fs_path, bool want_gapless);

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
  ProcessorPool *processor_pool() { return &processor_pool_; }

  void set_gapless_processing(bool b) { gapless_processing_ = b; }
  bool gapless_processing() const { return gapless_processing_; }

  void set_toplevel_directory_is_filter(bool b) { toplevel_dir_is_filter_ = b; }
  bool toplevel_directory_is_filter() const { return toplevel_dir_is_filter_; }

  // Initial choice of filter
  void set_initial_filter_config(const std::string& filter_cfg) {
    initial_filter_config_ = filter_cfg;
  }
  const std::string &initial_filter_config() const {
    return initial_filter_config_;
  }

  // Should we attempt to pre-buffer files ?
  void set_pre_buffer_size(int b) { pre_buffer_size_ = b; }
  int pre_buffer_size() const { return pre_buffer_size_; }

  // Some media servers look at the file size initially to decide which is
  // the file-size they need to serve. However, the final file-size after
  // convolving might be different (compression not really predictable) and
  // we don't know that beforehand.
  // So, we can't really hand out the original file size. But we don't really
  // know the final file-size as well. So we multiply the original file size
  // with a factor - overestimating seems to be less of a problem than
  // understimating.
  float file_oversize_factor() { return file_oversize_factor_; }
  void set_file_oversize_factor(float v) { file_oversize_factor_ = v; }

  // Some stats.
  int total_file_openings() { return total_file_openings_; }
  int total_file_reopen() { return total_file_reopen_; }

  // Allows sound conversions to use the pre-buffer thread.
  void RequestPrebuffer(ConversionBuffer *buffer);
  void QuitBuffering(ConversionBuffer *buffer);

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

  // Extract filter name from path if needed.
  bool ExtractFilterName(const char *path, std::string *filter) const;

  std::string underlying_dir_;
  std::string base_config_dir_;
  std::string initial_filter_config_;

  std::string current_config_subdir_;
  bool gapless_processing_;
  bool toplevel_dir_is_filter_;
  int pre_buffer_size_;
  FileHandlerCache open_file_cache_;
  ProcessorPool processor_pool_;
  BufferThread *buffer_thread_;
  int total_file_openings_;
  int total_file_reopen_;
  float file_oversize_factor_;

  // Work around a range of versions of libsndfile/libflac that can't deal with
  // flushing headers first.
  // fixed in https://github.com/erikd/libsndfile/commit/a81308ee40dc11ebffa2740272b611170f069ec7
  // Need to keep this here for a while until usual distributions have moved
  // on.
  bool workaround_flac_header_issue_;
};

#endif // FOLVE_FILESYSTEM_H
