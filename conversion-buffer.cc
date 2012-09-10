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

#include <stdio.h>
#include <string.h>

#include "conversion-buffer.h"

ConversionBuffer::ConversionBuffer(int buffer_size, SoundfileFiller *callback)
  : buffer_size_(buffer_size), fill_data_(callback),
    buffer_(new char[buffer_size_]), buffer_pos_(0),
    buffer_global_offset_(0) {
}

ConversionBuffer::~ConversionBuffer() {
  delete [] buffer_;
}

sf_count_t ConversionBuffer::SndTell(void *userdata) {
  return reinterpret_cast<ConversionBuffer*>(userdata)->Tell();
}
sf_count_t ConversionBuffer::SndWrite(const void *ptr, sf_count_t count,
                                      void *userdata) {
  return reinterpret_cast<ConversionBuffer*>(userdata)
    ->RefillBuffer(ptr, count);
}

// These callbacks we don't care about.
static sf_count_t DummySeek(sf_count_t offset, int whence, void *user_data) {
  fprintf(stderr, "DummySeek called %ld\n", offset);
  return 0;
}
static sf_count_t DummyRead(void *ptr, sf_count_t count, void *user_data) {
  fprintf(stderr, "DummyRead called\n");
  return 0;
}

SNDFILE *ConversionBuffer::CreateOutputSoundfile(SF_INFO *out_info) {
  SF_VIRTUAL_IO virtual_io;
  memset(&virtual_io, 0, sizeof(virtual_io));
  virtual_io.get_filelen = &ConversionBuffer::SndTell;
  virtual_io.write = &ConversionBuffer::SndWrite;
  virtual_io.tell = &ConversionBuffer::SndTell;
  virtual_io.read = &DummyRead;
  virtual_io.seek = &DummySeek;
  return sf_open_virtual(&virtual_io, SFM_WRITE, out_info, this);
}


size_t ConversionBuffer::RefillBuffer(const void *data, size_t count) {
  fprintf(stderr, "Fill buffer with %ld bytes\n", count);
  if (buffer_pos_ + count <= buffer_size_) {
    memcpy(buffer_ + buffer_pos_, data, count);
    buffer_pos_ += count;
  } else {
    // Reached end of buffer. Start from the beginning.
    // If we find that people are skipping backwards a bit, we might need
    // to have a double-buffer to always have a bit of 'older' history.
    // Or actually use a temporary file.
    buffer_global_offset_ += buffer_pos_;
    if (count > buffer_size_) {
      // Uh, too much data. Let's trim that a bit.
      count = buffer_size_;
    }
    memcpy(buffer_, data, count);
    buffer_pos_ = count;
  }
  return count;
}

ssize_t ConversionBuffer::Read(char *buf, size_t size, off_t offset) {
  if (offset < buffer_global_offset_) {
    fprintf(stderr, "Uh, skipped backwards; %ld vs. %ld\n", offset,
            buffer_global_offset_);
    return -1;  // TODO(hzeller): return an errno saying closest problem.
  }
  while (buffer_pos_ == 0 || offset >= Tell()) {
    if (!fill_data_->WriteToSoundfile())
      return 0;  // done.
  }
  if (offset < buffer_global_offset_) {
    fprintf(stderr, "Looks like WriteToSoundfile() filled buffer beyond "
            "expectation; want=%ld, min-pos=%ld\n",
            offset, buffer_global_offset_);
  }
  int bytes_to_write = size;
  const bool not_enough_in_buffer = Tell() - offset < bytes_to_write;
  if (not_enough_in_buffer) {
    bytes_to_write = Tell() - offset;
  }
  const off_t read_pos = offset - buffer_global_offset_;
  memcpy(buf, buffer_ + read_pos, bytes_to_write);
  fprintf(stderr, "Read(%ld @ %ld) = %d %s\n",
          size, offset, bytes_to_write,
          not_enough_in_buffer ? "(buffer now empty)" : "");
  return bytes_to_write;
}
