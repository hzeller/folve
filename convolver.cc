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
#include <sndfile.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <string>

#include "filter-interface.h"
#include "conversion-buffer.h"
#include "zita-config.h"

const char *global_zita_config_dir = NULL;

// We do a very simple decision which filter to apply by looking at the suffix.
static bool HasSuffixString (const char *str, const char *suffix) {
  if (!str || !suffix)
    return false;
  size_t str_len = strlen(str);
  size_t suffix_len = strlen(suffix);
  if (suffix_len > str_len)
    return false;
  return strncasecmp(str + str_len - suffix_len, suffix, suffix_len) == 0;
}

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
  // Attempt to create a SndFileFilter from the given file descriptor. This
  // returns NULL if this is not a sound-file or if there is no available
  // convolution filter configuration available.
  static FileFilter *Create(int filedes, const char *path) {
    struct SF_INFO in_info;
    memset(&in_info, 0, sizeof(in_info));
    SNDFILE *snd = sf_open_fd(filedes, SFM_READ, &in_info, 0);
    if (snd == NULL) {
      fprintf(stderr, "Opening input: %s\n", sf_strerror(NULL));
      return NULL;
    }

    int bits = 16;
    if ((in_info.format & SF_FORMAT_PCM_24) != 0) bits = 24;
    if ((in_info.format & SF_FORMAT_PCM_32) != 0) bits = 32;
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s/filter-%d-%d-%d.conf",
             global_zita_config_dir, in_info.samplerate,
             bits, in_info.channels);
    fprintf(stderr, "Looking for config %s ", config_path);
    if (access(config_path, R_OK) != 0) {
      fprintf(stderr, "- cannot access.\n");
      sf_close(snd);
      return NULL;
    } else {
      fprintf(stderr, "- found.\n");
    }
    return new SndFileFilter(path, filedes, snd, in_info, config_path);
  }
  
  virtual ~SndFileFilter() {
    if (zita_.convproc) {
      zita_.convproc->stop_process();
      zita_.convproc->cleanup();
      delete zita_.convproc;
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
    if (snd_in_) sf_close(snd_in_);
    if (snd_out_) sf_close(snd_out_);
    return close(filedes_) == -1 ? -errno : 0;
  }
    
private:
  SndFileFilter(const char *path, int filedes, SNDFILE *snd_in,
                const SF_INFO &in_info,
                const char* config_path)
    : filedes_(filedes), snd_in_(snd_in), config_path_(config_path),
      error_(false), output_buffer_(NULL), channels_(0), snd_out_(NULL),
      raw_sample_buffer_(NULL), input_frames_left_(0) {

    // Initialize zita config, but don't allocate converter quite yet.
    memset(&zita_, 0, sizeof(zita_));
    zita_.fsamp = in_info.samplerate;
    zita_.ninp = in_info.channels;
    zita_.nout = in_info.channels;

    channels_ = in_info.channels;
    input_frames_left_ = in_info.frames;

    // Create a conversion buffer that creates a soundfile of a particular
    // format that we choose here. Essentially we want to have mostly what
    // our input is.
    struct SF_INFO out_info = in_info;
    // The input seems to contain several bits indicating all kinds major
    // types. So look at the filename.
    if (HasSuffixString(path, ".wav")) {
      out_info.format = SF_FORMAT_WAV;
    } else {
      out_info.format = SF_FORMAT_FLAC;
    }
    // same number of bits format as input. If the input was ogg, we're
    // re-coding this to flac/24.
    if ((in_info.format & SF_FORMAT_OGG) != 0) {
      out_info.format |= SF_FORMAT_PCM_24;
    } else {
      out_info.format |= in_info.format & SF_FORMAT_SUBMASK;
    }
    out_info.seekable = 0;  // no point in making it seekable.
    output_buffer_ = new ConversionBuffer(this, out_info);
  }

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
    fprintf(stderr, "Header init done.\n");
  }

  virtual bool AddMoreSoundData() {
    if (!input_frames_left_)
      return false;
    if (!zita_.convproc) {
      zita_.convproc = new Convproc();
      config(&zita_, config_path_.c_str());
      zita_.convproc->start_process(0, 0);
      fprintf(stderr, "Convolver initialized; chunksize=%d\n", zita_.fragm);
    }
    if (raw_sample_buffer_ == NULL) {
      raw_sample_buffer_ = new float[zita_.fragm * channels_];
    }
    int r = sf_readf_float(snd_in_, raw_sample_buffer_, zita_.fragm);
    if (r == 0) { fprintf(stderr, "eeeempty\n"); return false; }
    if (r == (int) zita_.fragm) {
      fprintf(stderr, ".");
    } else {
      fprintf(stderr, "[%d]\n", r);
    }
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
  SNDFILE *const snd_in_;
  const std::string config_path_;

  bool error_;
  ConversionBuffer *output_buffer_;
  int channels_;
  SNDFILE *snd_out_;

  // Used in conversion.
  float *raw_sample_buffer_;
  int input_frames_left_;
  ZitaConfig zita_;
};
}  // namespace

// Implementation of the C functions in filter-interface.h
struct filter_object_t *create_filter(int filedes, const char *path) {
  FileFilter *filter = SndFileFilter::Create(filedes, path);
  if (filter != NULL) return filter;

  fprintf(stderr, "Cound't create filtered output\n");
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

void initialize_convolver_filter(const char *zita_config_dir) {
  global_zita_config_dir = zita_config_dir;
}
