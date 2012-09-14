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

#ifndef _FUSE_CONVOLVER_FILESYSTEM_H
#define _FUSE_CONVOLVER_FILESYSTEM_H

#include <unistd.h>

#include "file-handler.h"
#include "file-handler-cache.h"

class ConvolverFilesystem {
public:
  ConvolverFilesystem(const char *version_info,
                      const char *zita_config_dir,
                      int cache_size);

  // Create a new filter given the open filedescriptor and the path.
  // Returns NULL, if it cannot be created.
  FileHandler *CreateHandler(const char *fs_path,
                             const char *underlying_path);
  
  // Return dynamic size of file.
  int StatByFilename(const char *fs_path, struct stat *st);
  
  // At the end of the operation, close filter. Return 0 on success or negative
  // errno value on failure.
  void Close(const char *fs_path);

  const std::string &version() { return version_info_; }
  FileHandlerCache *handler_cache() { return &open_file_cache_; }
  int total_file_openings() { return total_file_openings_; }
  int total_file_reopen() { return total_file_reopen_; }

private:
  const std::string version_info_;
  FileHandlerCache open_file_cache_;
  int total_file_openings_;
  int total_file_reopen_;
};

#endif // _FUSE_CONVOLVER_FILESYSTEM_H
