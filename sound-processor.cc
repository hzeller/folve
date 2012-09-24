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

#include "sound-processor.h"

#include <assert.h>
#include <string.h>

#include <boost/thread/mutex.hpp>
#include <boost/thread/locks.hpp>

// There seems to be a bug somewhere inside the fftwf library or the use
// within Convproc::configure()
// It creates a double-delete somewhere if accessed with multiple threads.
static boost::mutex fftw_mutex;

SoundProcessor *SoundProcessor::Create(const std::string &config_file,
                                       int samplerate, int channels) {
  ZitaConfig zita;
  memset(&zita, 0, sizeof(zita));
  zita.fsamp = samplerate;
  zita.ninp = channels;
  zita.nout = channels;
  zita.convproc = new Convproc();
  { // fftw threading bug workaround, see above.
    boost::lock_guard<boost::mutex> l(fftw_mutex);
    if ((config(&zita, config_file.c_str()) != 0)
        || zita.convproc->inpdata(channels - 1) == NULL
        || zita.convproc->outdata(channels - 1) == NULL) {
      return NULL;
    }
  }
  return new SoundProcessor(zita, config_file);
}

SoundProcessor::SoundProcessor(const ZitaConfig &config, const std::string &cfg)
  : zita_config_(config), config_file_(cfg),
    buffer_(new float[config.fragm * config.ninp]),
    channels_(config.ninp),
    input_pos_(0), output_pos_(0),
    max_out_value_observed_(0.0) {
  Reset();
}

SoundProcessor::~SoundProcessor() {
  zita_config_.convproc->stop_process();
  zita_config_.convproc->cleanup();
  delete zita_config_.convproc;
  delete [] buffer_;
}

int SoundProcessor::FillBuffer(SNDFILE *in) {
  const int samples_needed = zita_config_.fragm - input_pos_;
  assert(samples_needed);  // Otherwise, call WriteProcessed() first.
  output_pos_ = -1;
  int r = sf_readf_float(in, buffer_ + input_pos_ * channels_, samples_needed);
  input_pos_ += r;
  return r;
}

void SoundProcessor::WriteProcessed(SNDFILE *out, int sample_count) {
  if (output_pos_ < 0) {
    Process();
  }
  assert(sample_count <= zita_config_.fragm - output_pos_);
  sf_writef_float(out, buffer_ + output_pos_ * channels_, sample_count);
  output_pos_ += sample_count;
  if (output_pos_ == zita_config_.fragm) {
    input_pos_ = 0;
  }
}

void SoundProcessor::Process() {
  const int samples_missing = zita_config_.fragm - input_pos_;
  if (samples_missing) {
    memset(buffer_ + input_pos_ * channels_, 0x00,
           samples_missing * channels_ * sizeof(float));
  }

  // Flatten channels: LRLRLRLRLR -> LLLLL and RRRRR
  for (int ch = 0; ch < channels_; ++ch) {
    float *dest = zita_config_.convproc->inpdata(ch);
    for (int j = 0; j < input_pos_; ++j) {
      dest[j] = buffer_[j * channels_ + ch];
    }
  }

  zita_config_.convproc->process();

  // Join channels again.
  for (int ch = 0; ch < channels_; ++ch) {
    float *source = zita_config_.convproc->outdata(ch);
    for (int j = 0; j < input_pos_; ++j) {
      buffer_[j * channels_ + ch] = source[j];
      const float out_abs = source[j];
      if (out_abs > max_out_value_observed_) {
        max_out_value_observed_ = out_abs;
      }
    }
  }
  output_pos_ = 0;
}

void SoundProcessor::ResetMaxValues() {
  max_out_value_observed_ = 0.0;
}

void SoundProcessor::Reset() {
  zita_config_.convproc->reset();
  input_pos_ = 0;
  output_pos_ = -1;
  ResetMaxValues();
  zita_config_.convproc->start_process(0, 0);
}
