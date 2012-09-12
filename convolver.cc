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
#include <FLAC/metadata.h>

#include <string>

#include "filter-interface.h"
#include "conversion-buffer.h"
#include "zita-config.h"

const char *global_zita_config_dir = NULL;

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
    if ((in_info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_PCM_24) bits = 24;
    if ((in_info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_PCM_32) bits = 32;

    long int seconds = in_info.frames / in_info.samplerate;
    fprintf(stderr, "%ld samples @ %.1fkHz, %d Bit; duration %ld:%02ld\n",
            in_info.frames, in_info.samplerate / 1000.0,
            bits, seconds / 60, seconds % 60);

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
    output_buffer_->set_sndfile_writes_enabled(false);
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

    // The flac header we get is more rich than what we can do
    // with sndfile. In that case, just copy that.
    copy_flac_header_ = (in_info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_FLAC;

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
    out_info.seekable = 0;
    if ((in_info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_OGG) {
      // If the input was ogg, we're re-coding this to flac/16.
      out_info.format = SF_FORMAT_FLAC;
      out_info.format |= SF_FORMAT_PCM_16;
    }
    else if ((in_info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_WAV
             && (in_info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16) {
      // WAV format seems to create garbage when we attempt to output PCM_24
      // Output float for now; still mplayer seems to trip about length.
      // Probably the header is incomplete. Investigate.
      out_info.format = SF_FORMAT_WAV;
      out_info.format |= SF_FORMAT_FLOAT;
      out_info.format |= SF_ENDIAN_CPU;
    }
    else { // original format.
      out_info.format = in_info.format;
    }

    output_buffer_ = new ConversionBuffer(this, out_info);
  }

  virtual void SetOutputSoundfile(ConversionBuffer *out_buffer,
                                  SNDFILE *sndfile) {
    snd_out_ = sndfile;
    if (snd_out_ == NULL) {
      error_ = true;
      fprintf(stderr, "Opening output: %s\n", sf_strerror(NULL));
      return;
    }
    out_buffer->set_sndfile_writes_enabled(false);
    if (copy_flac_header_) {
      CopyFlacHeader(out_buffer);
    } else {
      out_buffer->set_sndfile_writes_enabled(true);
      // Copy strings. Everything else that follows will be stream bytes.
      for (int i = SF_STR_FIRST; i <= SF_STR_LAST; ++i) {
        const char *s = sf_get_string(snd_in_, i);
        if (s != NULL) {
          sf_set_string(snd_out_, i, s);
        }
      }
    }
    // Now flush the header: that way if someone only reads the metadata, then
    // our AddMoreSoundData() is never called.
    sf_command(snd_out_, SFC_UPDATE_HEADER_NOW, NULL, 0);
    fprintf(stderr, "Header init done.\n");

    out_buffer->set_sndfile_writes_enabled(true);  // ready for sound-stream.
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
    if (r == (int) zita_.fragm) {
      fprintf(stderr, ".");
    } else {
      fprintf(stderr, "[%d]", r);
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
    if (input_frames_left_ == 0) {
      fprintf(stderr, "(fully decoded)\n");
    }
    return input_frames_left_;
  }

  off_t CopyBytes(int fd, off_t pos, ConversionBuffer *out, size_t len) {
    char buf[256];
    while (len > 0) {
      //fprintf(stderr, "read at %ld\n", pos);
      ssize_t r = pread(fd, buf, std::min(sizeof(buf), len), pos);
      if (r <= 0) return pos;
      //fprintf(stderr, "append %ld bytes\n", r);
      out->Append(buf, r);
      len -= r;
      pos += r;
    }
    return pos;
  }

  void CopyFlacHeader(ConversionBuffer *out_buffer) {
    fprintf(stderr, "Copy raw flac header\n");
    out_buffer->Append("fLaC", 4);
    off_t pos = 4;
    unsigned char header[4];
    bool need_finish_padding = false;
    while (pread(filedes_, header, sizeof(header), pos) == sizeof(header)) {
      pos += sizeof(header);
      bool is_last = header[0] & 0x80;
      unsigned int type = header[0] & 0x7F;
      unsigned int byte_len = (header[1] << 16) + (header[2] << 8) + header[3];
      fprintf(stderr, " type: %d, len: %6u %s ", type,
              byte_len, is_last ? "(last)" : "(cont)");
      need_finish_padding = false;
      if (type == FLAC__METADATA_TYPE_STREAMINFO && byte_len == 34) {
        out_buffer->Append(&header, sizeof(header));
        CopyBytes(filedes_, pos, out_buffer, byte_len - 16);
        for (int i = 0; i < 16; ++i) out_buffer->Append("\0", 1);
        fprintf(stderr, " (copy streaminfo, but redacted MD5)\n");
      }
      else if (type == FLAC__METADATA_TYPE_SEEKTABLE) {
        // The SEEKTABLE header we skip, because it is bogus after encoding.
        fprintf(stderr, " (skip the seektable)\n");
        need_finish_padding = is_last;  // if we were last, force finish block.
      }
      else {
        out_buffer->Append(&header, sizeof(header));
        CopyBytes(filedes_, pos, out_buffer, byte_len);
        fprintf(stderr, " (ok)\n");
      }
      pos += byte_len;
      if (is_last)
        break;
    }
    if (need_finish_padding) {  // if the last block was not is_last: pad.
      fprintf(stderr, "write padding\n");
      memset(&header, 0, sizeof(header));
      header[0] = 0x80 /* is last */ | FLAC__METADATA_TYPE_PADDING;
      out_buffer->Append(&header, sizeof(header));
    }
  }

  const int filedes_;
  SNDFILE *const snd_in_;
  const std::string config_path_;

  bool error_;
  bool copy_flac_header_;
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
