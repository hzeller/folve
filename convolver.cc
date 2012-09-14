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

#include <FLAC/metadata.h>
#include <errno.h>
#include <fcntl.h>
#include <sndfile.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <map>

#include "file-handler.h"
#include "file-handler-cache.h"
#include "convolver-filesystem.h"
#include "conversion-buffer.h"
#include "zita-config.h"

static bool global_debug = false;
const char *global_zita_config_dir = NULL;

#define LOGF if (!global_debug) {} else fprintf
#define LOG_ERROR fprintf

namespace {

// Very simple filter that just passes the original file through. Used for
// everything that is not a sound-file.
class PassThroughFilter : public FileHandler {
public:
  PassThroughFilter(int filedes, const char *path) : filedes_(filedes) {
    LOGF(stderr, "Creating PassThrough filter for '%s'\n", path);
  }
  ~PassThroughFilter() { close(filedes_); }

  virtual int Read(char *buf, size_t size, off_t offset) {
    const int result = pread(filedes_, buf, size, offset);
    return result == -1 ? -errno : result;
  }
  virtual int Stat(struct stat *st) {
    return fstat(filedes_, st);
  }
  
private:
  const int filedes_;
};

class SndFileHandler :
    public FileHandler,
    public ConversionBuffer::SoundSource {
public:
  // Attempt to create a SndFileHandler from the given file descriptor. This
  // returns NULL if this is not a sound-file or if there is no available
  // convolution filter configuration available.
  static FileHandler *Create(int filedes, const char *path) {
    struct SF_INFO in_info;
    memset(&in_info, 0, sizeof(in_info));
    SNDFILE *snd = sf_open_fd(filedes, SFM_READ, &in_info, 0);
    if (snd == NULL) {
      LOG_ERROR(stderr, "File %s: %s\n", path, sf_strerror(NULL));
      return NULL;
    }

    int bits = 16;
    if ((in_info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_PCM_24) bits = 24;
    if ((in_info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_PCM_32) bits = 32;

    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s/filter-%d-%d-%d.conf",
             global_zita_config_dir, in_info.samplerate,
             bits, in_info.channels);
    const bool found_config = (access(config_path, R_OK) == 0);
    if (found_config) {
      LOGF(stderr, "File %s: filter config %s\n", path, config_path);
    } else {
      LOG_ERROR(stderr, "File %s: couldn't find filter config %s\n",
                path, config_path);
      sf_close(snd);
      return NULL;
    }
    char file_info[256];
    snprintf(file_info, sizeof(file_info), "%.1fkHz, %d Bit",
             in_info.samplerate / 1000.0, bits);
    return new SndFileHandler(path, filedes, snd, in_info,
                              file_info, config_path);
  }
  
  virtual ~SndFileHandler() {
    Close();
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
    // If this is a skip suspiciously at the very end of the file as
    // reported by stat, we don't do any encoding, just return garbage.
    // Programs sometimes do this apparently.
    // But of course only if this is really a detected skip.
    if (output_buffer_->FileSize() < offset
        && (int) (offset + size) >= file_stat_.st_size) {
      const int pretended_available_bytes = file_stat_.st_size - offset;
      if (pretended_available_bytes > 0) {
        memset(buf, 0x00, pretended_available_bytes);
        return pretended_available_bytes;
      } else {
        return 0;
      }
    }
    // The following read might block and call WriteToSoundfile() until the
    // buffer is filled.
    return output_buffer_->Read(buf, size, offset);
  }

  virtual float Progress() const {
    const int frames_done = total_frames_ - input_frames_left_;
    if (frames_done == 0 || total_frames_ == 0) return 0.0;
    return 1.0 * frames_done / total_frames_;
  }
  virtual std::string FileInfo() const { return file_description_; }
  virtual int Duration() const { return duration_seconds_; }

  virtual int Stat(struct stat *st) {
    if (output_buffer_->FileSize() > start_estimating_size_) {
      const int frames_done = total_frames_ - input_frames_left_;
      if (frames_done > 0) {
        const float estimated_end = 1.0 * total_frames_ / frames_done;
        off_t new_size = estimated_end * output_buffer_->FileSize();
        // Report a bit bigger size which is less harmful than programs
        // reading short.
        new_size += 16384;
        if (new_size > file_stat_.st_size) {  // Only go forward in size.
          file_stat_.st_size = new_size;
        }
      }
    }
    *st = file_stat_;
    return 0;
  }
    
private:
  SndFileHandler(const char *path, int filedes, SNDFILE *snd_in,
                 const SF_INFO &in_info, const std::string &file_description,
                 const char* config_path)
    : filedes_(filedes), snd_in_(snd_in), total_frames_(in_info.frames),
      channels_(in_info.channels),
      duration_seconds_(in_info.frames / in_info.samplerate),
      file_description_(file_description), config_path_(config_path),
      error_(false), output_buffer_(NULL),
      snd_out_(NULL),
      raw_sample_buffer_(NULL), input_frames_left_(in_info.frames) {

    // Initial stat that we're going to report to clients. We'll adapt
    // the filesize as we see it grow. Some clients continuously monitor
    // the size of the file to check when to stop.
    fstat(filedes_, &file_stat_);
    start_estimating_size_ = 0.4 * file_stat_.st_size;

    // The flac header we get is more rich than what we can create via
    // sndfile. So if we have one, just copy it.
    copy_flac_header_ = (in_info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_FLAC;

    // Initialize zita config, but don't allocate converter quite yet.
    memset(&zita_, 0, sizeof(zita_));
    zita_.fsamp = in_info.samplerate;
    zita_.ninp = in_info.channels;
    zita_.nout = in_info.channels;

    // Create a conversion buffer that creates a soundfile of a particular
    // format that we choose here. Essentially we want to generate mostly what
    // our input is.
    struct SF_INFO out_info = in_info;
    out_info.seekable = 0;
    if ((in_info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_OGG) {
      // If the input was ogg, we're re-coding this to flac, because it
      // wouldn't let us stream the output.
      out_info.format = SF_FORMAT_FLAC;
      out_info.format |= SF_FORMAT_PCM_16;
    }
    else if ((in_info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_WAV) {
      out_info.format = SF_FORMAT_FLAC;  // recode as flac.
      out_info.format |= SF_FORMAT_PCM_24;
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
      LOG_ERROR(stderr, "Opening output: %s\n", sf_strerror(NULL));
      return;
    }
    if (copy_flac_header_) {
      out_buffer->set_sndfile_writes_enabled(false);
      CopyFlacHeader(out_buffer);
    } else {
      out_buffer->set_sndfile_writes_enabled(true);
      GenerateHeaderFromInputFile(out_buffer);
    }
    // Now flush the header: that way if someone only reads the metadata, then
    // our AddMoreSoundData() is never called.
    // We need to do this even if we copied our own header: that way we make
    // sure that the sndfile-header is flushed into the nirwana before we
    // re-enable sndfile_writes.
    sf_command(snd_out_, SFC_UPDATE_HEADER_NOW, NULL, 0);

    // -- time for some hackery ...
    // If we have copied the header over from the original, we need to
    // redact the values for min/max blocksize and min/max framesize with
    // what SNDFILE is going to use, otherwise programs will trip over this.
    // http://flac.sourceforge.net/format.html
    if (copy_flac_header_) {
      out_buffer->WriteCharAt((1152 & 0xFF00) >> 8,  8);
      out_buffer->WriteCharAt((1152 & 0x00FF)     ,  9);
      out_buffer->WriteCharAt((1152 & 0xFF00) >> 8, 10);
      out_buffer->WriteCharAt((1152 & 0x00FF)     , 11);
      for (int i = 12; i < 18; ++i) out_buffer->WriteCharAt(0, i);
    } else {
      // .. and if SNDFILE writes the header, it misses out in writing the
      // number of samples to be expected. So let's fill that in.
      // The MD5 sum starts at position strlen("fLaC") + 4 + 18 = 26
      // The 32 bits before that are the samples (and another 4 bit before that,
      // ignoring that for now).
      out_buffer->WriteCharAt((total_frames_ & 0xFF000000) >> 24, 22);
      out_buffer->WriteCharAt((total_frames_ & 0x00FF0000) >> 16, 23);
      out_buffer->WriteCharAt((total_frames_ & 0x0000FF00) >>  8, 24);
      out_buffer->WriteCharAt((total_frames_ & 0x000000FF),       25);
    }

    out_buffer->set_sndfile_writes_enabled(true);  // ready for sound-stream.
    LOGF(stderr, "Header init done.\n");
    out_buffer->HeaderFinished();
  }

  virtual bool AddMoreSoundData() {
    if (!input_frames_left_)
      return false;
    if (!zita_.convproc) {
      // First time we're called.
      zita_.convproc = new Convproc();
      if (config(&zita_, config_path_.c_str()) != 0) {
        LOG_ERROR(stderr, "** filter-config %s is broken. Please fix. "
                  "Won't play this stream **\n", config_path_.c_str());
        input_frames_left_ = 0;
        Close();
        return false;
      }
      raw_sample_buffer_ = new float[zita_.fragm * channels_];
      zita_.convproc->start_process(0, 0);
    }
    int r = sf_readf_float(snd_in_, raw_sample_buffer_, zita_.fragm);
    if (r < (int) zita_.fragm) {
      // Zero out the rest of the buffer.
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
      Close();
    }
    return input_frames_left_;
  }

  void CopyBytes(int fd, off_t pos, ConversionBuffer *out, size_t len) {
    char buf[256];
    while (len > 0) {
      ssize_t r = pread(fd, buf, std::min(sizeof(buf), len), pos);
      if (r <= 0) return;
      out->Append(buf, r);
      len -= r;
      pos += r;
    }
  }

  void CopyFlacHeader(ConversionBuffer *out_buffer) {
    LOGF(stderr, "Provide FLAC header from original file:\n");
    out_buffer->Append("fLaC", 4);
    off_t pos = 4;
    unsigned char header[4];
    bool need_finish_padding = false;
    while (pread(filedes_, header, sizeof(header), pos) == sizeof(header)) {
      pos += sizeof(header);
      bool is_last = header[0] & 0x80;
      unsigned int type = header[0] & 0x7F;
      unsigned int byte_len = (header[1] << 16) + (header[2] << 8) + header[3];
      LOGF(stderr, " type: %d, len: %6u %s ", type,
           byte_len, is_last ? "(last)" : "(cont)");
      need_finish_padding = false;
      if (type == FLAC__METADATA_TYPE_STREAMINFO && byte_len == 34) {
        out_buffer->Append(&header, sizeof(header));
        // Copy everything but the MD5 at the end - which we set to empty.
        CopyBytes(filedes_, pos, out_buffer, byte_len - 16);
        for (int i = 0; i < 16; ++i) out_buffer->Append("\0", 1);
        LOGF(stderr, " (copy streaminfo, but redacted MD5)\n");
      }
      else if (type == FLAC__METADATA_TYPE_SEEKTABLE) {
        // The SEEKTABLE header we skip, because it is bogus after encoding.
        LOGF(stderr, " (skip the seektable)\n");
        need_finish_padding = is_last;  // if we were last, force finish block.
      }
      else {
        out_buffer->Append(&header, sizeof(header));
        CopyBytes(filedes_, pos, out_buffer, byte_len);
        LOGF(stderr, " (ok)\n");
      }
      pos += byte_len;
      if (is_last)
        break;
    }
    if (need_finish_padding) {  // if the last block was not is_last: pad.
      LOGF(stderr, "write padding\n");
      memset(&header, 0, sizeof(header));
      header[0] = 0x80 /* is last */ | FLAC__METADATA_TYPE_PADDING;
      out_buffer->Append(&header, sizeof(header));
    }
  }

  void GenerateHeaderFromInputFile(ConversionBuffer *out_buffer) {
    LOGF(stderr, "Generate header from original ID3-tags.\n");
    out_buffer->set_sndfile_writes_enabled(true);
    // Copy ID tags that are supported by sndfile.
    for (int i = SF_STR_FIRST; i <= SF_STR_LAST; ++i) {
      const char *s = sf_get_string(snd_in_, i);
      if (s != NULL) {
        sf_set_string(snd_out_, i, s);
      }
    }
  }

  void Close() {
    if (snd_out_ == NULL) return;  // done.
    output_buffer_->set_sndfile_writes_enabled(false);
    if (snd_in_) sf_close(snd_in_);
    if (snd_out_) sf_close(snd_out_);
    snd_out_ = NULL;
    close(filedes_);
  }

  const int filedes_;
  SNDFILE *const snd_in_;
  const unsigned int total_frames_;
  const int channels_;
  const int duration_seconds_;
  const std::string file_description_;
  const std::string config_path_;

  struct stat file_stat_;   // we dynamically report a changing size.
  off_t start_estimating_size_;  // essentially const.

  bool error_;
  bool copy_flac_header_;
  ConversionBuffer *output_buffer_;
  SNDFILE *snd_out_;

  // Used in conversion.
  float *raw_sample_buffer_;
  int input_frames_left_;
  ZitaConfig zita_;
};
}  // namespace


static FileHandler *CreateFilterFromFileType(int filedes,
                                             const char *underlying_file) {
  FileHandler *filter = SndFileHandler::Create(filedes, underlying_file);
  if (filter != NULL) return filter;

  // Every other file-type is just passed through as is.
  return new PassThroughFilter(filedes, underlying_file);
}

// Implementation of the C functions in filter-interface.h
FileHandler *ConvolverFilesystem::CreateHandler(const char *fs_path,
                                                const char *underlying_path) {
  FileHandler *handler = open_file_cache_.FindAndPin(fs_path);
  if (handler == NULL) {
    int filedes = open(underlying_path, O_RDONLY);
    if (filedes < 0)
      return NULL;
    ++total_file_openings_;
    handler = CreateFilterFromFileType(filedes, underlying_path);
    handler = open_file_cache_.InsertPinned(fs_path, handler);
  } else {
    ++total_file_reopen_;
  }
  return handler;
}

int ConvolverFilesystem::StatByFilename(const char *fs_path, struct stat *st) {
  FileHandler *handler = open_file_cache_.FindAndPin(fs_path);
  if (handler == 0)
    return -1;
  ssize_t result = handler->Stat(st);
  open_file_cache_.Unpin(fs_path);
  return result;
}

void ConvolverFilesystem::Close(const char *fs_path) {
  open_file_cache_.Unpin(fs_path);
}

ConvolverFilesystem::ConvolverFilesystem(const char *version_info,
                                         const char *zita_config_dir,
                                         int cache_size)
  : version_info_(version_info), open_file_cache_(cache_size, cache_size),
    total_file_openings_(0), total_file_reopen_(0) {
  global_zita_config_dir = zita_config_dir;
}
