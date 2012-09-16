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

#include "conversion-buffer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <boost/thread/locks.hpp>

ConversionBuffer::ConversionBuffer(SoundSource *source, const SF_INFO &info)
  : source_(source), out_filedes_(-1), snd_writing_enabled_(true),
    total_written_(0), header_end_(0) {
  const char *filename = tempnam(NULL, "folve");
  out_filedes_ = open(filename, O_RDWR|O_CREAT|O_NOATIME, S_IRUSR|S_IWUSR);
  if (out_filedes_ < 0) {
    perror("Problem opening buffer file");
  }
  unlink(filename);

  // After file-open: SetOutputSoundfile() already might attempt to write data.
  source_->SetOutputSoundfile(this, CreateOutputSoundfile(info));
}

ConversionBuffer::~ConversionBuffer() {
  close(out_filedes_);
}

sf_count_t ConversionBuffer::SndTell(void *userdata) {
  return reinterpret_cast<ConversionBuffer*>(userdata)->FileSize();
}
sf_count_t ConversionBuffer::SndWrite(const void *ptr, sf_count_t count,
                                      void *userdata) {
  return reinterpret_cast<ConversionBuffer*>(userdata)->SndAppend(ptr, count);
}

// These callbacks we don't care about.
static sf_count_t DummySeek(sf_count_t offset, int whence, void *userdata) {
  // This seems to be called after we're closing, probably to modify the
  // header. It then actually attempts to write, but we're already not
  // sndfile write enabled. So print this as a warning if we're write enabled,
  // because it would mess up the file.
  if (offset > 0 &&
      reinterpret_cast<ConversionBuffer*>(userdata)->sndfile_writes_enabled()) {
    fprintf(stderr, "DummySeek called %ld\n", offset);
  }
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
  if (out_filedes_ < 0) return -1;
  //fprintf(stderr, "Extend horizon by %ld bytes.\n", count);
  int remaining = count;
  const char *buf = (const char*)data;
  while (remaining > 0) {
    int w = write(out_filedes_, data, count);
    if (w < 0) return -errno;
    remaining -= w;
    buf += w;
  }
  total_written_ += count;
  return count;
}

void ConversionBuffer::WriteCharAt(unsigned char c, off_t offset) {
  if (out_filedes_ < 0) return;
  if (pwrite(out_filedes_, &c, 1, offset) != 1) fprintf(stderr, "Oops.");
}

ssize_t ConversionBuffer::SndAppend(const void *data, size_t count) {
  if (!snd_writing_enabled_) return count;
  return Append(data, count);
}

void ConversionBuffer::HeaderFinished() { header_end_ = FileSize(); }

ssize_t ConversionBuffer::Read(char *buf, size_t size, off_t offset) {
  // As long as we're reading only within the header area, allow 'short' reads,
  // i.e. reads that return less bytes than requested (but up to the headers'
  // size). That means:
  //     required_min_written = offset + 1; // at least one byte.
  // That way, we don't need to start the convolver if someone only reads
  // the header: we stop at the header boundary.
  //
  // After beginning to read the sound stream, some programs (e.g. kaffeine)
  // behave finicky if they don't get the full number of bytes they
  // requested in a read() call (this is a bug in these programs, but we've
  // to work around it). So that means in that case we make sure that we have
  // at least the number of bytes available that are requested:
  //     required_min_written = offset + size;  // all requested bytes.
  const off_t required_min_written = offset + (offset >= header_end_ ? size : 1);

  // As soon as someone tries to read beyond of what we already have, we call
  // our WriteToSoundfile() callback that fills more of it.
  // We are shared between potentially several open files. Serialize threads.
  {
    boost::lock_guard<boost::mutex> l(mutex_);
    while (total_written_ < required_min_written) {
      if (!source_->AddMoreSoundData())
        break;
    }
  }

  return pread(out_filedes_, buf, size, offset);
}
