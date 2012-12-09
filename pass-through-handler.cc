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

#include "pass-through-handler.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#include <algorithm>

#include "util.h"

using folve::DLogf;

PassThroughHandler::PassThroughHandler(int filedes, const std::string &filter_id,
                                       const HandlerStats &known_stats)
  : FileHandler(filter_id), filedes_(filedes),
    file_size_(-1), max_accessed_(0), info_stats_(known_stats) {
  DLogf("Creating PassThrough filter for '%s'", known_stats.filename.c_str());
  struct stat st;
  file_size_ = (Stat(&st) == 0) ? st.st_size : -1;
  info_stats_.filter_dir = "";  // pass through.
}

PassThroughHandler::~PassThroughHandler() { close(filedes_); }

int PassThroughHandler::Read(char *buf, size_t size, off_t offset) {
  const int result = pread(filedes_, buf, size, offset);
  if (result < 0)
    return -errno;
  max_accessed_ = std::max<off_t>(max_accessed_, offset + result);
  return result;
}

int PassThroughHandler::Stat(struct stat *st) {
  return fstat(filedes_, st);
}

void PassThroughHandler::GetHandlerStatus(HandlerStats *stats) {
  *stats = info_stats_;
  if (file_size_ > 0) {
    max_accessed_ = std::min(max_accessed_, file_size_);
    stats->access_progress = 1.0 * max_accessed_ / file_size_;
    stats->buffer_progress = stats->access_progress;
  }
}
