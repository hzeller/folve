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

#include <sndfile.h>

// A buffer that keeps track
class ConversionBuffer {
 public:
  // Callback called by the conversion buffer requesting write to the
  // SNDFILE. Returns 'true', if there is more, 'false' when done.
  class SoundfileFiller {
  public:
    virtual ~SoundfileFiller() {}
    virtual bool WriteToSoundfile() = 0;
  };

  // Create a conversion buffer that holds "buffer_size" bytes. The
  // "callback" will be called if we need more write operations on
  // the SNDFILE. The user needs to call CreateOutputSoundfile(), otherwise
  // they don't know where to write to :)
  ConversionBuffer(int buffer_size, SoundfileFiller *callback);
  ~ConversionBuffer();

  // Create a SNDFILE the user has to write to in the WriteToSoundfile callback.
  // Can be NULL on error (call sf_strerror() to find out why).
  SNDFILE *CreateOutputSoundfile(SF_INFO *info);

  // Read data from internal buffer that has been filled by write operations to
  // the SNDFILE.
  // If the internal buffer is exhausted, it will call the WriteToSoundfile
  // callback to fill more into our buffer.
  //
  // Attempts to skip backwards might fail, we only guarantee forward looking.
  ssize_t Read(char *buf, size_t size, off_t offset);

  // Current file position.
  off_t Tell() const { return buffer_global_offset_ + buffer_pos_; }

 private:
  static sf_count_t SndTell(void *userdata);
  static sf_count_t SndWrite(const void *ptr, sf_count_t count, void *userdata);

  size_t RefillBuffer(const void *data, size_t count);

  const size_t buffer_size_;
  SoundfileFiller *const fill_data_;
  char *buffer_;
  size_t buffer_pos_;
  off_t buffer_global_offset_;  // since the beginning of writing.
};
