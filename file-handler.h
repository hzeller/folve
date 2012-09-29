// -*- c++ -*-
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

#ifndef FOLVE_FILE_HANDLER_H
#define FOLVE_FILE_HANDLER_H

#include <string>

// Status about some handler, filled in by various subsystem.
// This is mostly used to be displayed in the HTTP server. And to survive
// after the FileHandler is long gone, to show 'retired' elements in the
// status server.
class HandlerStats {
public:
  HandlerStats()
    : duration_seconds(-1), progress(-1), status(OPEN), last_access(0),
      max_output_value(0), in_gapless(false), out_gapless(false) {}

  // Copy constructor: test to work around some gcc 2.7.1 seen in the field.
  // To be removed after test.
  HandlerStats(const HandlerStats &other)
    : filename(other.filename), format(other.format), message(other.message),
      duration_seconds(other.duration_seconds), progress(other.progress),
      status(other.status), last_access(other.last_access),
      max_output_value(other.max_output_value),
      in_gapless(other.in_gapless), out_gapless(other.out_gapless),
      filter_id(other.filter_id) {
  }

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
  int  filter_id;               // which filter-id is in use.
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
  virtual void GetHandlerStatus(HandlerStats *s) = 0;
  virtual bool AcceptProcessor(SoundProcessor *s) { return false; }

private:
  const int filter_id_;
};

#endif // FOLVE_FILE_HANDLER_H
