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
  : source_(source), tmpfile_(-1), snd_writing_enabled_(true),
    total_written_(0), header_end_(0) {
  // We need to be able to skip backwards but we don't want to fill our
  // memory. So lets create a temporary file.
  const char *filename = tempnam(NULL, "fuse-");
  tmpfile_ = open(filename, O_RDWR|O_CREAT|O_NOATIME, S_IRUSR|S_IWUSR);
  unlink(filename);

  // After file-open: SetOutputSoundfile() already might attempt to write data.
  source_->SetOutputSoundfile(this, CreateOutputSoundfile(info));
}

ConversionBuffer::~ConversionBuffer() {
  close(tmpfile_);
}

sf_count_t ConversionBuffer::SndTell(void *userdata) {
  return reinterpret_cast<ConversionBuffer*>(userdata)->Tell();
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

ssize_t ConversionBuffer::SndAppend(const void *data, size_t count) {
  if (!snd_writing_enabled_) return count;
  return Append(data, count);
}

void ConversionBuffer::HeaderFinished() { header_end_ = Tell(); }

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

  // Skipping the file looks like reading beyond what the user already
  // consumed. Right now, we have to fill the buffer up to that point, but
  // we might need to find a shortcut for that: some programs just skip to the
  // end of the file apparently - which makes us convolve the while file.
  if (total_written_ + 1 < offset) {
    fprintf(stderr, "(skip> %ld -> %ld)", total_written_, offset);
  }

  // As soon as someone tries to read beyond of what we already have, we call
  // our WriteToSoundfile() callback that fills more of it.
  while (total_written_ < required_min_written) {
    // We skip up until 32k before the requested start-offset.
    // TODO(hzeller): remember that the skipped parts are actually not convolved
    // so if someone skips back we know that we need to re-do that.
    const bool skip_mode = total_written_ + (32 << 10) < offset;
    if (!source_->AddMoreSoundData(skip_mode))
      break;
  }

  return pread(tmpfile_, buf, size, offset);
}
