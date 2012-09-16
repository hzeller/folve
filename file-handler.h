// -*- c++ -*-
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

#ifndef FOLVE_FILE_HANDLER_H
#define FOLVE_FILE_HANDLER_H

#include <string>

// Status about some handler, filled in by various subsystem.
struct HandlerStats {
  HandlerStats() {}
  std::string filename;
  std::string format;
  int total_duration_seconds;
  float progress;

  enum Status { OPEN, IDLE, RETIRED };
  Status status;
  double last_access;
};

// A handler that deals with operations on files. Since we only provide read
// access, this is limited to very few operations.
// Closing in particular is not done by this file handler as it might
// have a longer life-time than an open()/close() cycle we get from the
// fuse filesystem (see file-handler-cache.h for rationale)
class FileHandler {
public:
  virtual ~FileHandler() {}

  // Returns bytes read or a negative value indicating a negative errno.
  virtual int Read(char *buf, size_t size, off_t offset) = 0;
  virtual int Stat(struct stat *st) = 0;

  // Get handler status.
  virtual void GetHandlerStatus(struct HandlerStats *s) = 0;
};

#endif // FOLVE_FILE_HANDLER_H
