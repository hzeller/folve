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
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sndfile.h>

#include "filter-interface.h"
#include "conversion-buffer.h"
#include "zita-config.h"

const char *sGlobal_zita_config;

namespace {
class FileFilter : public filter_object_t {
public:
  // Returns bytes read or a negative value indicating a negative errno.
  virtual int Read(char *buf, size_t size, off_t offset) = 0;
  virtual int Close() = 0;
  virtual ~FileFilter() {}
};

// Very simple filter that just passes the original file through. Used for
// everything that is not a sound-file.
class PassThroughFilter : public FileFilter {
public:
  PassThroughFilter(int filedes, const char *path) : filedes_(filedes) {
    fprintf(stderr, "Creating PassThrough filter for '%s'\n", path);
  }
  
  virtual int Read(char *buf, size_t size, off_t offset) {
    const int result = pread(filedes_, buf, size, offset);
    return result == -1 ? -errno : result;
  }
  
  virtual int Close() {
    return close(filedes_) == -1 ? -errno : 0;
  }
  
private:
  const int filedes_;
};

class SndFileFilter :
    public FileFilter,
    public ConversionBuffer::SoundSource {
public:
  SndFileFilter(int filedes, const char *path)
    : filedes_(filedes), error_(false),
      output_buffer_(NULL), snd_in_(NULL), snd_out_(NULL),
      raw_sample_buffer_(NULL), input_frames_left_(0) {
    fprintf(stderr, "Creating sound-file filter for '%s'\n", path);

    // Open input file.
    struct SF_INFO in_info;
    memset(&in_info, 0, sizeof(in_info));
    snd_in_ = sf_open_fd(filedes, SFM_READ, &in_info, 0);
    if (snd_in_ == NULL) {
      error_ = true;
      fprintf(stderr, "Opening input: %s\n", sf_strerror(NULL));
      return;
    }

    channels_ = in_info.channels;
    input_frames_left_ = in_info.frames;

    // Create a conversion buffer that creates a soundfile of a particular
    // format that we choose here. Essentially we want to have mostly what
    // our input is.
    struct SF_INFO out_info = in_info;
    out_info.format = SF_FORMAT_FLAC;
    // same number of bits format as input.
    out_info.format |= in_info.format & SF_FORMAT_SUBMASK;
    out_info.seekable = 0;  // no point in making it seekable.
    output_buffer_ = new ConversionBuffer(this, out_info);
  }
  
  virtual ~SndFileFilter() {
#if 0
    // This crashes if we cleanup/re-create the process. Find out why.
    if (zita_.convproc) {
      // don't destroy. Something fishy is going on.
      zita_.convproc->stop_process ();
      zita_.convproc->cleanup ();
      delete zita_.convproc;
    }
#endif
    if (zita_.convproc) {
      zita_.convproc->reset();
    }
    delete output_buffer_;
    delete [] raw_sample_buffer_;
  }

  virtual int Read(char *buf, size_t size, off_t offset) {
    if (error_) return -1;
    // The following read might block and call WriteToSoundfile() until the
    // buffer is filled.
    return output_buffer_->Read(buf, size, offset);
  }

  virtual int Close() {
    if (snd_in_) { sf_close(snd_in_); snd_in_ = NULL; }
    if (snd_out_) { sf_close(snd_out_); snd_out_ = NULL; }
    return close(filedes_) == -1 ? -errno : 0;
  }
    
private:
  virtual void SetOutputSoundfile(SNDFILE *sndfile) {
    snd_out_ = sndfile;
    if (snd_out_ == NULL) {
      error_ = true;
      fprintf(stderr, "Opening output: %s\n", sf_strerror(NULL));
      return;
    }
    // Copy header. Everything else that follows will be stream bytes.
    for (int i = SF_STR_FIRST; i <= SF_STR_LAST; ++i) {
      const char *s = sf_get_string(snd_in_, i);
      if (s != NULL) {
        sf_set_string(snd_out_, i, s);
      }
    }
    // Now flush the header: that way if someone only reads the metadata, then
    // our AddMoreSoundData() is never called.
    sf_command(snd_out_, SFC_UPDATE_HEADER_NOW, NULL, 0);
    fprintf(stderr, "Header copy done.\n");
  }

  virtual bool AddMoreSoundData() {
    if (!input_frames_left_)
      return false;
    if (!zita_.convproc) {
      zita_.convproc = new Convproc();
      config(&zita_, sGlobal_zita_config);
      zita_.convproc->start_process(0, 0);
      fprintf(stderr, "Create conversion processor, chunksize=%d\n",
              zita_.fragm);
    }
    if (raw_sample_buffer_ == NULL) {
      raw_sample_buffer_ = new float[zita_.fragm * channels_];
    }
    int r = sf_readf_float(snd_in_, raw_sample_buffer_, zita_.fragm);
    fprintf(stderr, "** conversion callback; %d new samples **\n", r);
    if (r < (int) zita_.fragm) {
      // zero out the rest of the buffer
      const int missing = zita_.fragm - r;
      memset(raw_sample_buffer_ + r * channels_, 0,
             missing * channels_ * sizeof(float));
    }

    // Separate channels.
    for (int ch = 0; ch < channels_; ++ch) {
      float *dest = zita_.convproc->inpdata(ch);
      for (int j = 0; j < r; ++j) {
        dest[j] = raw_sample_buffer_[j * channels_ + ch];
      }
    }

    zita_.convproc->process();

    // Join channels again.
    for (int ch = 0; ch < channels_; ++ch) {
      float *source = zita_.convproc->outdata(ch);
      for (int j = 0; j < r; ++j) {
        raw_sample_buffer_[j * channels_ + ch] = source[j];
      }
    }
    sf_writef_float(snd_out_, raw_sample_buffer_, r);
    input_frames_left_ -= r;

    return input_frames_left_;
  }

  const int filedes_;
  bool error_;
  ConversionBuffer *output_buffer_;
  int channels_;
  SNDFILE *snd_in_;
  SNDFILE *snd_out_;

  // Used in conversion.
  float *raw_sample_buffer_;
  int input_frames_left_;
  static ZitaConfig zita_;   // for now: only do it once.
};
  ZitaConfig SndFileFilter::zita_;
}  // namespace

// We do a very simple decision which filter to apply by looking at the suffix.
bool HasSuffixString (const char *str, const char *suffix) {
  if (!str || !suffix)
    return false;
  size_t str_len = strlen(str);
  size_t suffix_len = strlen(suffix);
  if (suffix_len > str_len)
    return false;
  return strncasecmp(str + str_len - suffix_len, suffix, suffix_len) == 0;
}

// Implementation of the C functions in filter-interface.h
struct filter_object_t *create_filter(int filedes, const char *path) {
  if (HasSuffixString(path, ".flac")) {
    return new SndFileFilter(filedes, path);
  }

  // Every other file-type is just passed through as is.
  return new PassThroughFilter(filedes, path);
}

int read_from_filter(struct filter_object_t *filter,
                     char *buf, size_t size, off_t offset) {
  return reinterpret_cast<FileFilter*>(filter)->Read(buf, size, offset);
}

int close_filter(struct filter_object_t *filter) {
  FileFilter *file_filter = reinterpret_cast<FileFilter*>(filter);
  int result = file_filter->Close();
  delete file_filter;
  return result;
}

void initialize_convolver_filter(const char *zita_config_file) {
  sGlobal_zita_config = zita_config_file;
}
