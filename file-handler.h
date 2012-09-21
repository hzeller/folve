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
  HandlerStats()
    : duration_seconds(-1), progress(-1), status(OPEN), last_access(0),
      max_output_value(0), in_gapless(false), out_gapless(false) {}
  std::string filename;         // filesystem name.
  std::string format;           // File format info if recognized.
  std::string message;          // Per file (error) message if any.
  int duration_seconds;         // Audio file length if known; -1 otherwise.
  float progress;               // Convolving progress if known; -1 otherwise.

  enum Status { OPEN, IDLE, RETIRED };
  Status status;                // Status of this file handler.
  double last_access;           // Last access in hi-res seconds since epoch.
  float max_output_value;       // Clipping ? Should be [0 .. 1]
  bool in_gapless;              // Were we handed a processor to continue.
  bool out_gapless;             // Did we pass on our processor.
};

class SoundProcessor;
// A handler that deals with operations on files. Since we only provide read
// access, this is limited to very few operations.
// Closing in particular is not done by this file handler as it might
// have a longer life-time than an open()/close() cycle we get from the
// fuse filesystem (see file-handler-cache.h for rationale)
class FileHandler {
public:
  explicit FileHandler(int filter_id) : filter_id_(filter_id) {}
  virtual ~FileHandler() {}

  int filter_id() const { return filter_id_; }

  // Returns bytes read or a negative value indicating a negative errno.
  virtual int Read(char *buf, size_t size, off_t offset) = 0;
  virtual int Stat(struct stat *st) = 0;

  // Get handler status.
  virtual void GetHandlerStatus(struct HandlerStats *s) = 0;
  virtual bool AcceptProcessor(SoundProcessor *s) { return false; }

private:
  const int filter_id_;
};

#endif // FOLVE_FILE_HANDLER_H
