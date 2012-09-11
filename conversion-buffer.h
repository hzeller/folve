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

// A file-backed buffer for a SNDFILE, that is only filled on demand via
// a SoundSource.
// If Read() is called beyond the current available data, a callback is
// called to write more into the SNDFILE.
class ConversionBuffer {
 public:
  // SoundSource, a instance of which needs to be passed to the
  // ConversionBuffer.
  class SoundSource {
  public:
    virtual ~SoundSource() {}

    // The soundfile is set by this conversion buffer and to be filled when
    // requested. There can be an error in opening the sound-file, in that
    // case SetOutputSoundfile() will be called with NULL.
    // Ask sf_strerror() to find out why.
    // Ownership is passed to the SoundSource, receiver needs to 
    // sf_close() the file.
    virtual void SetOutputSoundfile(SNDFILE *sndfile) = 0;

    // This callback is called by the ConversionBuffer if it needs more data.
    // Rerturns 'true' if there is more, 'false' if that was the last available
    // data.
    virtual bool AddMoreSoundData() = 0;
  };

  // Create a conversion buffer that holds "buffer_size" bytes.
  // The "source" will be informed to what SNDFILE to write to and whenever
  // this ConversionBuffer lusts for more data (it then calls
  // AddMoreSoundData()).
  // Ownership is not taken over for source.
  ConversionBuffer(SoundSource *source, const SF_INFO &info);
  ~ConversionBuffer();

  // Read data from buffer. Can block and call the SoundSource first to get
  // more data if needed.
  ssize_t Read(char *buf, size_t size, off_t offset);

 private:
  static sf_count_t SndTell(void *userdata);
  static sf_count_t SndWrite(const void *ptr, sf_count_t count, void *userdata);

  // Append data. Called via the SndWrite() virtual file callback.
  ssize_t Append(const void *data, size_t count);

  // Current max file position.
  off_t Tell() const { return total_written_; }

  // Create a SNDFILE the user has to write to in the WriteToSoundfile callback.
  // Can be NULL on error.
  SNDFILE *CreateOutputSoundfile(const SF_INFO &info);

  SoundSource *const source_;
  int tmpfile_;
  off_t total_written_;
};