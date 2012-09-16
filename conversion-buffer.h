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

#ifndef FOLVE_CONVERSION_BUFFER_H
#define FOLVE_CONVERSION_BUFFER_H

#include <sndfile.h>
#include <boost/thread/mutex.hpp>

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
    virtual void SetOutputSoundfile(ConversionBuffer *parent,
                                    SNDFILE *sndfile) = 0;
      
    // This callback is called by the ConversionBuffer if it needs more data.
    // Rerturns 'true' if there is more, 'false' if that was the last available
    // data.
    virtual bool AddMoreSoundData() = 0;
  };

  // Create a conversion buffer providing an sound output described in
  // "out_info".
  // The "source" will be called back whenever this conversion buffer needs
  // more data.
  //
  // Ownership is not taken over for source.
  ConversionBuffer(SoundSource *source, const SF_INFO &out_info);
  ~ConversionBuffer();

  // Read data from buffer. Can block and call the SoundSource first to get
  // more data if needed.
  ssize_t Read(char *buf, size_t size, off_t offset);

  // Append data. Usually called via the SndWrite() virtual-SNFFILE callback,
  // but can be used to write raw data as well (e.g. to write headers in
  // SetOutputSoundfile())
  ssize_t Append(const void *data, size_t count);

  // Write at a particular position. Writes a single character - this is
  // used for chirurgical header editing...
  void WriteCharAt(unsigned char c, off_t offset);

  // Enable writing through the SNDFILE.
  // If set to 'false', writes via the SNDFILE are ignored.
  // To be used to suppress writing of the header or
  // footer if we want to handle that on our own.
  void set_sndfile_writes_enabled(bool b) { snd_writing_enabled_ = b; }
  bool sndfile_writes_enabled() const { return snd_writing_enabled_; }

  // Tell conversion buffer when we're done writing the header. It needs to
  // know so that it can serve reads in these different regions differently.
  // (Long story, see Read() for details).
  void HeaderFinished();

  // Current max file position.
  off_t FileSize() const { return total_written_; }

private:
  static sf_count_t SndTell(void *userdata);
  static sf_count_t SndWrite(const void *ptr, sf_count_t count, void *userdata);

  // Append for the SndWrite callback.
  ssize_t SndAppend(const void *data, size_t count);

  // Create a SNDFILE the user has to write to in the WriteToSoundfile callback.
  // Can be NULL on error.
  SNDFILE *CreateOutputSoundfile(const SF_INFO &info);

  SoundSource *const source_;
  int out_filedes_;
  bool snd_writing_enabled_;
  off_t total_written_;
  off_t header_end_;
  boost::mutex mutex_;
};

#endif  // FOLVE_CONVERSION_BUFFER_H
