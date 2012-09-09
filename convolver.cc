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

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "filter-interface.h"

namespace {
class FileFilter : public filter_interface_t {
public:
  // Returns bytes read or a negative value indicating a negative errno.
  virtual int Read(char *buf, size_t size, off_t offset) = 0;
  virtual int Close() = 0;
  virtual ~FileFilter() {}
};

// Very simple filter that just passes the original file through.
class PassThroughFilter : public FileFilter {
public:
  PassThroughFilter(int filedes, const char *path) : filedes_(filedes) {
    fprintf(stderr, "Creating PassThrough filter for '%s'\n", path);
  }
  
  virtual int Read(char *buf, size_t size, off_t offset) {
    const int result = pread(filedes_, buf, size, offset);
    return result == -1 ? -errno : result;
  }
  
  virtual int Close() {
    return close(filedes_) == -1 ? -errno : 0;
  }
  
private:
  const int filedes_;
};

class WavFilter : public FileFilter {
public:
  WavFilter(int filedes, const char *path) : filedes_(filedes) {
    fprintf(stderr, "Creating Wav filter for '%s'\n", path);
  }
  
  virtual int Read(char *buf, size_t size, off_t offset) {
    const int result = pread(filedes_, buf, size, offset);
    return result == -1 ? -errno : result;
  }

  virtual int Close() {
    return close(filedes_) == -1 ? -errno : 0;
  }

private:
  const int filedes_;
};
}  // namespace

// We do a very simple decision which filter to apply by looking at the suffix.
bool HasSuffixString (const char *str, const char *suffix) {
  if (!str || !suffix)
    return false;
  size_t str_len = strlen(str);
  size_t suffix_len = strlen(suffix);
  if (suffix_len > str_len)
    return false;
  return strncasecmp(str + str_len - suffix_len, suffix, suffix_len) == 0;
}

// Implementation of the C functions in filter-interface.h
struct filter_interface_t *create_filter(int filedes, const char *path) {
  if (HasSuffixString(path, ".wav")) {
    return new WavFilter(filedes, path);
  }

  // Everything other file is just passed through as is.
  return new PassThroughFilter(filedes, path);
}

int read_from_filter(struct filter_interface_t *filter,
                     char *buf, size_t size, off_t offset) {
  return reinterpret_cast<FileFilter*>(filter)->Read(buf, size, offset);
}

int close_filter(struct filter_interface_t *filter) {
  FileFilter *file_filter = reinterpret_cast<FileFilter*>(filter);
  int result = file_filter->Close();
  delete file_filter;
  return result;
}
