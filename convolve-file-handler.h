//  -*- c++ -*-
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
#ifndef FOLVE_CONVOLVE_FILE_HANDLER_H_
#define FOLVE_CONVOLVE_FILE_HANDLER_H_

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "file-handler.h"
#include "conversion-buffer.h"

class FolveFilesystem;

class ConvolveFileHandler : public FileHandler,
                            public ConversionBuffer::SoundSource {
public:
  // Attempt to create a ConvolveFileHandler from the given file descriptor.
  // This returns NULL if this is not a sound-file or if there is no available
  // convolution filter configuration available.
  // "partial_file_info" will be set to information known so far, including
  // error message.
  static FileHandler *Create(FolveFilesystem *fs,
                             int filedes, const char *fs_path,
                             const std::string &underlying_file,
                             const std::string &filter_subdir,
                             const std::string &zita_config_dir,
                             HandlerStats *partial_file_info);

  virtual ~ConvolveFileHandler();

  // -- FileHandler interface
  virtual int Read(char *buf, size_t size, off_t offset);
  virtual void GetHandlerStatus(HandlerStats *stats);
  virtual int Stat(struct stat *st);
  virtual bool PassoverProcessor(SoundProcessor *passover_processor);
  virtual void NotifyPassedProcessorUnreferenced();

  // -- ConversionBuffer::SoundSource interface.
  virtual void SetOutputSoundfile(ConversionBuffer *out_buffer,
                                  const SF_INFO &info,
                                  SNDFILE *sndfile);
  virtual bool AddMoreSoundData();

private:
  ConvolveFileHandler(FolveFilesystem *fs, const char *fs_path,
                      const std::string &filter_dir,
                      const std::string &underlying_file,
                      int filedes, SNDFILE *snd_in,
                      const SF_INFO &in_info, const HandlerStats &file_info,
                      SoundProcessor *processor);

  bool HasStarted();

  // Generate Header in case this is a FLAC file.
  void CopyFlacHeader(ConversionBuffer *out_buffer);

  // Generate Header from the generic tags.
  void GenerateHeaderFromInputFile(ConversionBuffer *out_buffer);

  void SaveOutputValues();

  // Close all sound files and flush data.
  void Close();

  bool LooksLikeInputIsFlac(const SF_INFO &sndinfo, int filedes);
  int frames_left();

  FolveFilesystem *const fs_;
  const int filedes_;
  SNDFILE *const snd_in_;
  const SF_INFO in_info_;

  folve::Mutex stats_mutex_;
  HandlerStats base_stats_;      // UI information about current file.

  struct stat file_stat_;        // we dynamically report a changing size.
  off_t original_file_size_;
  off_t start_estimating_size_;  // essentially const.

  bool error_;
  bool copy_flac_header_verbatim_;
  ConversionBuffer *output_buffer_;
  SNDFILE *snd_out_;

  // Used in conversion.
  SoundProcessor *processor_;
  int input_frames_left_;
};

#endif  // FOLVE_CONVOLVE_FILE_HANDLER_H_
