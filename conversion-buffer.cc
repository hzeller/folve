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
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "conversion-buffer.h"

ConversionBuffer::ConversionBuffer(SoundSource *source, const SF_INFO &info)
  : source_(source), tmpfile_(-1), total_written_(0) {
  // We need to be able to skip backwards but we don't want to fill our
  // memory. So lets create a temporary file.
  const char *filename = tempnam(NULL, "fuse-");
  tmpfile_ = open(filename, O_RDWR|O_CREAT|O_NOATIME, S_IRUSR|S_IWUSR);
  unlink(filename);

  // After file-open: SetOutputSoundfile() already might attempt to write data.
  source_->SetOutputSoundfile(CreateOutputSoundfile(info));
}

ConversionBuffer::~ConversionBuffer() {
  close(tmpfile_);
}

sf_count_t ConversionBuffer::SndTell(void *userdata) {
  return reinterpret_cast<ConversionBuffer*>(userdata)->Tell();
}
sf_count_t ConversionBuffer::SndWrite(const void *ptr, sf_count_t count,
                                      void *userdata) {
  return reinterpret_cast<ConversionBuffer*>(userdata)->Append(ptr, count);
}

// These callbacks we don't care about.
static sf_count_t DummySeek(sf_count_t offset, int whence, void *user_data) {
  // This seems to be called after we're closing, probably to modify the
  // header. It actually attempts to write that end up at the end of the
  // file. We don't care, it is not accessed for reading anymore.
  // TODO(hzeller): Suppress writing after close() and really warn
  //fprintf(stderr, "DummySeek called %ld\n", offset);
  return 0;
}
static sf_count_t DummyRead(void *ptr, sf_count_t count, void *user_data) {
  fprintf(stderr, "DummyRead called\n");
  return 0;
}

SNDFILE *ConversionBuffer::CreateOutputSoundfile(const SF_INFO &out_info) {
  SF_INFO info_copy = out_info;
  SF_VIRTUAL_IO virtual_io;
  memset(&virtual_io, 0, sizeof(virtual_io));
  virtual_io.get_filelen = &ConversionBuffer::SndTell;
  virtual_io.write = &ConversionBuffer::SndWrite;
  virtual_io.tell = &ConversionBuffer::SndTell;
  virtual_io.read = &DummyRead;
  virtual_io.seek = &DummySeek;
  return sf_open_virtual(&virtual_io, SFM_WRITE, &info_copy, this);
}

ssize_t ConversionBuffer::Append(const void *data, size_t count) {
  if (tmpfile_ < 0) return -1;
  //fprintf(stderr, "Extend horizon by %ld bytes.\n", count);
  int remaining = count;
  const char *buf = (const char*)data;
  while (remaining > 0) {
    int w = write(tmpfile_, data, count);
    if (w < 0) return -errno;
    remaining -= w;
    buf += w;
  }
  total_written_ += count;
  return count;
}

ssize_t ConversionBuffer::Read(char *buf, size_t size, off_t offset) {
  const off_t required_min_written = offset + 1;
  // As soon as someone tries to read beyond of what we already have, we call
  // our WriteToSoundfile() callback that fills more of it.
  while (total_written_ < required_min_written) {
    if (!source_->AddMoreSoundData())
      break;
  }
  ssize_t result = pread(tmpfile_, buf, size, offset);
  //fprintf(stderr, "Read(%ld @ %ld) = %ld\n", size, offset, result);

  return result;
}
