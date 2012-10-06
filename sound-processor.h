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

#ifndef FOLVE_SOUND_PROCESSOR_H
#define FOLVE_SOUND_PROCESSOR_H

#include <string>
#include <sndfile.h>

#include "zita-config.h"

// The workhorse of processing data from soundfiles.
class SoundProcessor {
public:
  static SoundProcessor *Create(const std::string &config_file,
                                int samplerate, int channels);
  ~SoundProcessor();

  // Fill Buffer from given sound file. Returns number of samples read.
  int FillBuffer(SNDFILE *in);

  // Returns if the input buffer has enought samples for the FIR-filter
  // to process. If not, another call to FillBuffer() is needed.
  bool is_input_buffer_complete() const {
    return zita_config_.fragm == input_pos_;
  }

  // Number of samples that are pending. Typically once we have this passed
  // over to a new file.
  int pending_writes() const {
    return output_pos_ >= 0 ? zita_config_.fragm - output_pos_ : 0;
  }

  // Write number of processed samples out to given soundfile. Processes
  // the data first if necessary. assert(), that there is at least 1 sample
  // to process.
  void WriteProcessed(SNDFILE *out, int sample_count);

  // Reset procesor for re-use
  void Reset();

  // Maximum absolute output value observe (>= 0.0).
  float max_output_value() const { return max_out_value_observed_; }
  void ResetMaxValues();

  // Config file used to create this processor.
  const std::string &config_file() const { return config_file_; }
  const time_t config_file_timestamp() const { return config_file_timestamp_; }

  // Verifies if configuration is still up-to-date.
  bool ConfigStillUpToDate() const;

private:
  SoundProcessor(const ZitaConfig &config, const std::string &cfg_file);
  void Process();

  const ZitaConfig zita_config_;
  const std::string config_file_;
  const time_t config_file_timestamp_;

  float *const buffer_;
  const int channels_;
  // TODO: instead of two positions, better have one position and two states
  // READ, WRITE
  int input_pos_;
  int output_pos_;  // written position. -1, if not processed yet.
  float max_out_value_observed_;
};

#endif  // FOLVE_SOUND_PROCESSOR_H
