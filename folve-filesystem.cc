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

#include "folve-filesystem.h"

#include <FLAC/metadata.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sndfile.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include <map>
#include <string>
#include <zita-convolver.h>

#include "conversion-buffer.h"
#include "file-handler-cache.h"
#include "file-handler.h"
#include "sound-processor.h"
#include "util.h"

#include "zita-config.h"

using folve::Appendf;
using folve::StringPrintf;
using folve::DLogf;

namespace {

// Very simple filter that just passes the original file through. Used for
// everything that is not a sound-file.
class PassThroughHandler : public FileHandler {
public:
  PassThroughHandler(int filedes, const std::string &filter_id,
                    const HandlerStats &known_stats)
    : FileHandler(filter_id), filedes_(filedes),
      file_size_(-1), max_accessed_(0), info_stats_(known_stats) {
    DLogf("Creating PassThrough filter for '%s'", known_stats.filename.c_str());
    struct stat st;
    file_size_ = (Stat(&st) == 0) ? st.st_size : -1;
    info_stats_.filter_dir = "";  // pass through.
  }
  ~PassThroughHandler() { close(filedes_); }

  virtual int Read(char *buf, size_t size, off_t offset) {
    const int result = pread(filedes_, buf, size, offset);
    max_accessed_ = std::max(max_accessed_, (long unsigned int) offset + result);
    return result == -1 ? -errno : result;
  }
  virtual int Stat(struct stat *st) {
    return fstat(filedes_, st);
  }
  virtual void GetHandlerStatus(HandlerStats *stats) {
    *stats = info_stats_;
    if (file_size_ > 0) {
      stats->progress = 1.0 * max_accessed_ / file_size_;
    }
  }

private:
  const int filedes_;
  size_t file_size_;
  long unsigned int max_accessed_;
  HandlerStats info_stats_;
};

class SndFileHandler :
    public FileHandler,
    public ConversionBuffer::SoundSource {
public:
  // Attempt to create a SndFileHandler from the given file descriptor. This
  // returns NULL if this is not a sound-file or if there is no available
  // convolution filter configuration available.
  // "partial_file_info" will be set to information known so far, including
  // error message.
  static FileHandler *Create(FolveFilesystem *fs,
                             int filedes, const char *fs_path,
                             const std::string &underlying_file,
                             const std::string &filter_subdir,
                             const std::string &zita_config_dir,
                             HandlerStats *partial_file_info) {
    SF_INFO in_info;
    memset(&in_info, 0, sizeof(in_info));
    SNDFILE *snd = sf_open_fd(filedes, SFM_READ, &in_info, 0);
    if (snd == NULL) {
      DLogf("File %s: %s", underlying_file.c_str(), sf_strerror(NULL));
      partial_file_info->message = sf_strerror(NULL);
      return NULL;
    }

    int bits = 16;
    if ((in_info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_PCM_24) bits = 24;
    if ((in_info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_PCM_32) bits = 32;

    // Remember whatever we could got to know in the partial file info.
    Appendf(&partial_file_info->format, "%.1fkHz, %d Bit",
            in_info.samplerate / 1000.0, bits);
    partial_file_info->duration_seconds = in_info.frames / in_info.samplerate;

    SoundProcessor *processor = fs->processor_pool()
      ->GetOrCreate(zita_config_dir, in_info.samplerate, in_info.channels, bits,
                    &partial_file_info->message);
    if (processor == NULL) {
      sf_close(snd);
      return NULL;
    }
    const int seconds = in_info.frames / in_info.samplerate;
    DLogf("File %s, %.1fkHz, %d Bit, %d:%02d: filter config %s",
          underlying_file.c_str(), in_info.samplerate / 1000.0, bits,
          seconds / 60, seconds % 60,
          processor->config_file().c_str());
    return new SndFileHandler(fs, fs_path, filter_subdir,
                              underlying_file, filedes, snd, in_info,
                              *partial_file_info, processor);
  }
  
  virtual ~SndFileHandler() {
    Close();
    delete output_buffer_;
  }

  virtual int Read(char *buf, size_t size, off_t offset) {
    if (error_) return -1;
    // If this is a skip suspiciously at the very end of the file as
    // reported by stat, we don't do any encoding, just return garbage.
    // (otherwise we'd to convolve up to that point).
    //
    // While indexing, media players do this sometimes apparently.
    // And sometimes not even to the very end but 'almost' at the end.
    // So add some FudeOverhang
    static const int kFudgeOverhang = 512;
    // But of course only if this is really a skip, not a regular approaching
    // end-of-file.
    if (output_buffer_->FileSize() < offset
        && (int) (offset + size + kFudgeOverhang) >= file_stat_.st_size) {
      const int pretended_bytes = std::min((off_t)size,
                                           file_stat_.st_size - offset);
      if (pretended_bytes > 0) {
        memset(buf, 0x00, pretended_bytes);
        return pretended_bytes;
      } else {
        return 0;
      }
    }
    // The following read might block and call WriteToSoundfile() until the
    // buffer is filled.
    return output_buffer_->Read(buf, size, offset);
  }

  virtual void GetHandlerStatus(HandlerStats *stats) {
    folve::MutexLock l(&stats_mutex_);
    if (processor_ != NULL) {
      base_stats_.max_output_value = processor_->max_output_value();
    }
    *stats = base_stats_;
    const int frames_done = in_info_.frames - input_frames_left_;
    if (frames_done == 0 || in_info_.frames == 0)
      stats->progress = 0.0;
    else
      stats->progress = 1.0 * frames_done / in_info_.frames;

    if (base_stats_.max_output_value > 1.0) {
      // TODO: the status server could inspect this value and make better
      // rendering.
      base_stats_.message =
        StringPrintf("Output clipping! "
                     "(max=%.3f; Multiply gain with <= %.5f<br/>in %s)",
                     base_stats_.max_output_value,
                     1.0 / base_stats_.max_output_value,
                     processor_ != NULL
                     ? processor_->config_file().c_str()
                     : "filter");
    }
  }

  virtual int Stat(struct stat *st) {
    if (output_buffer_->FileSize() > start_estimating_size_) {
      const int frames_done = in_info_.frames - input_frames_left_;
      if (frames_done > 0) {
        const float estimated_end = 1.0 * in_info_.frames / frames_done;
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
  // TODO(hzeller): trim parameter list.
  SndFileHandler(FolveFilesystem *fs, const char *fs_path,
                 const std::string &filter_dir,
                 const std::string &underlying_file,
                 int filedes, SNDFILE *snd_in,
                 const SF_INFO &in_info, const HandlerStats &file_info,
                 SoundProcessor *processor)
    : FileHandler(filter_dir), fs_(fs),
      filedes_(filedes), snd_in_(snd_in), in_info_(in_info),
      base_stats_(file_info),
      error_(false), output_buffer_(NULL),
      snd_out_(NULL), processor_(processor),
      input_frames_left_(in_info.frames) {

    // Initial stat that we're going to report to clients. We'll adapt
    // the filesize as we see it grow. Some clients continuously monitor
    // the size of the file to check when to stop.
    fstat(filedes_, &file_stat_);
    start_estimating_size_ = 0.4 * file_stat_.st_size;

    // The flac header we get is more rich than what we can create via
    // sndfile. So if we have one, just copy it.
    copy_flac_header_verbatim_ = LooksLikeInputIsFlac(in_info, filedes);

    // Create a conversion buffer that creates a soundfile of a particular
    // format that we choose here. Essentially we want to generate mostly what
    // our input is.
    SF_INFO out_info = in_info;
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
      syslog(LOG_ERR, "Opening output: %s", sf_strerror(NULL));
      base_stats_.message = sf_strerror(NULL);
      return;
    }
    if (copy_flac_header_verbatim_) {
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
    if (copy_flac_header_verbatim_) {
      out_buffer->WriteCharAt((1152 & 0xFF00) >> 8,  8);
      out_buffer->WriteCharAt((1152 & 0x00FF)     ,  9);
      out_buffer->WriteCharAt((1152 & 0xFF00) >> 8, 10);
      out_buffer->WriteCharAt((1152 & 0x00FF)     , 11);
      for (int i = 12; i < 18; ++i) out_buffer->WriteCharAt(0, i); // framesize
    } else {
      // .. and if SNDFILE writes the header, it misses out in writing the
      // number of samples to be expected. So let's fill that in.
      // The MD5 sum starts at position strlen("fLaC") + 4 + 18 = 26
      // The 32 bits before that are the samples (and another 4 bit before that,
      // ignoring that for now).
      out_buffer->WriteCharAt((in_info_.frames & 0xFF000000) >> 24, 22);
      out_buffer->WriteCharAt((in_info_.frames & 0x00FF0000) >> 16, 23);
      out_buffer->WriteCharAt((in_info_.frames & 0x0000FF00) >>  8, 24);
      out_buffer->WriteCharAt((in_info_.frames & 0x000000FF),       25);
    }

    out_buffer->set_sndfile_writes_enabled(true);  // ready for sound-stream.
    DLogf("Header init done (%s).", base_stats_.filename.c_str());
    out_buffer->HeaderFinished();
  }

  bool HasStarted() { return in_info_.frames != input_frames_left_; }
  virtual bool AcceptProcessor(SoundProcessor *passover_processor) {
    if (HasStarted()) {
      DLogf("Gapless attempt: Cannot bridge gap to already open file %s",
            base_stats_.filename.c_str());
      return false;
    }
    assert(processor_);
    if (passover_processor->config_file() != processor_->config_file()
        || (passover_processor->config_file_timestamp()
            != processor_->config_file_timestamp())) {
      DLogf("Gapless: Configuration changed; can't use %p to join gapless.",
            passover_processor);
      return false;
    }
    // Ok, so don't use the processor we already have, but use the other one.
    fs_->processor_pool()->Return(processor_);
    processor_ = passover_processor;
    if (!processor_->is_input_buffer_complete()) {
      // Fill with our beginning so that the donor can finish its processing.
      input_frames_left_ -= processor_->FillBuffer(snd_in_);
    }
    base_stats_.in_gapless = true;
    return true;
  }

  static bool ExtractDirAndSuffix(const std::string &filename,
                                  std::string *dir, std::string *suffix) {
    const std::string::size_type slash_pos = filename.find_last_of('/');
    if (slash_pos == std::string::npos) return false;
    *dir = filename.substr(0, slash_pos + 1);
    const std::string::size_type dot_pos = filename.find_last_of('.');
    if (dot_pos != std::string::npos && dot_pos > slash_pos) {
      *suffix = filename.substr(dot_pos);
    }
    return true;
  }

  virtual bool AddMoreSoundData() {
    if (!input_frames_left_)
      return false;
    if (processor_->pending_writes() > 0) {
      processor_->WriteProcessed(snd_out_, processor_->pending_writes());
      return input_frames_left_;
    }
    const int r = processor_->FillBuffer(snd_in_);
    if (r == 0) {
      syslog(LOG_ERR, "Expected %d frames left, "
             "but got EOF; corrupt file '%s' ?",
             input_frames_left_, base_stats_.filename.c_str());
      base_stats_.message = "Premature EOF in input file.";
      input_frames_left_ = 0;
      Close();
      return false;
    }
    stats_mutex_.Lock();
    input_frames_left_ -= r;
    stats_mutex_.Unlock();
    if (!input_frames_left_ && !processor_->is_input_buffer_complete()
        && fs_->gapless_processing()) {
      typedef std::set<std::string> DirSet;
      DirSet dirset;
      std::string fs_dir, file_suffix;
      FileHandler *next_file = NULL;
      DirSet::const_iterator found;
      const bool passed_processor
        = (ExtractDirAndSuffix(base_stats_.filename, &fs_dir, &file_suffix)
           && fs_->ListDirectory(fs_dir, file_suffix, &dirset)
           && (found = dirset.upper_bound(base_stats_.filename)) != dirset.end()
           && (next_file = fs_->GetOrCreateHandler(found->c_str()))
           && next_file->AcceptProcessor(processor_));
      if (passed_processor) {
        DLogf("Processor %p: Gapless pass-on from "
              "'%s' to alphabetically next '%s'", processor_,
              base_stats_.filename.c_str(), found->c_str());
      }
      stats_mutex_.Lock();
      processor_->WriteProcessed(snd_out_, r);
      stats_mutex_.Unlock();
      if (passed_processor) {
        base_stats_.out_gapless = true;
        SaveOutputValues();
        processor_ = NULL;   // we handed over ownership.
      }
      if (next_file) fs_->Close(found->c_str(), next_file);
    } else {
      stats_mutex_.Lock();
      processor_->WriteProcessed(snd_out_, r);
      stats_mutex_.Unlock();
    }
    if (input_frames_left_ == 0) {
      Close();
    }
    return input_frames_left_;
  }

  // TODO add as a utility function to ConversionBuffer ?
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
    DLogf("Provide FLAC header from original file %s",
          base_stats_.filename.c_str());
    out_buffer->Append("fLaC", 4);
    off_t pos = 4;
    unsigned char header[4];
    bool need_finish_padding = false;
    while (pread(filedes_, header, sizeof(header), pos) == sizeof(header)) {
      pos += sizeof(header);
      bool is_last = header[0] & 0x80;
      unsigned int type = header[0] & 0x7F;
      unsigned int byte_len = (header[1] << 16) + (header[2] << 8) + header[3];
      const char *extra_info = "";
      need_finish_padding = false;
      if (type == FLAC__METADATA_TYPE_STREAMINFO && byte_len == 34) {
        out_buffer->Append(&header, sizeof(header));
        // Copy everything but the MD5 at the end - which we set to empty.
        CopyBytes(filedes_, pos, out_buffer, byte_len - 16);
        for (int i = 0; i < 16; ++i) out_buffer->Append("\0", 1);
        extra_info = "Streaminfo; redact MD5.";
      }
      else if (type == FLAC__METADATA_TYPE_SEEKTABLE) {
        // The SEEKTABLE header we skip, because it is bogus after encoding.
        // TODO append log (skip the seektable)
        need_finish_padding = is_last;  // if we were last, force finish block.
        extra_info = "Skip seektable.";
      }
      else {
        out_buffer->Append(&header, sizeof(header));
        CopyBytes(filedes_, pos, out_buffer, byte_len);
      }
      DLogf(" %02x %02x %02x %02x type: %d, len: %6u %s %s ",
            header[0], header[1], header[2], header[3],
            type, byte_len, is_last ? "(last)" : "(cont)", extra_info);
      pos += byte_len;
      if (is_last)
        break;
    }
    if (need_finish_padding) {  // if the last block was not is_last: pad.
      DLogf("write padding");
      memset(&header, 0, sizeof(header));
      header[0] = 0x80 /* is last */ | FLAC__METADATA_TYPE_PADDING;
      out_buffer->Append(&header, sizeof(header));
    }
  }

  void GenerateHeaderFromInputFile(ConversionBuffer *out_buffer) {
    DLogf("Generate header from original ID3-tags.");
    out_buffer->set_sndfile_writes_enabled(true);
    // Copy ID tags that are supported by sndfile.
    for (int i = SF_STR_FIRST; i <= SF_STR_LAST; ++i) {
      const char *s = sf_get_string(snd_in_, i);
      if (s != NULL) {
        sf_set_string(snd_out_, i, s);
      }
    }
  }

  void SaveOutputValues() {
    if (processor_) {
      base_stats_.max_output_value = processor_->max_output_value();
      processor_->ResetMaxValues();
    }
  }

  void Close() {
    if (snd_out_ == NULL) return;  // done.
    SaveOutputValues();
    if (base_stats_.max_output_value > 1.0) {
      syslog(LOG_ERR, "Observed output clipping in '%s': "
             "Max=%.3f; Multiply gain with <= %.5f in %s",
             base_stats_.filename.c_str(), base_stats_.max_output_value,
             1.0 / base_stats_.max_output_value, 
             processor_ != NULL ? processor_->config_file().c_str() : "filter");
    }
    fs_->processor_pool()->Return(processor_);
    processor_ = NULL;
    // We can't disable buffer writes here, because outfile closing will flush
    // the last couple of sound samples.
    if (snd_in_) sf_close(snd_in_);
    if (snd_out_) sf_close(snd_out_);
    snd_out_ = NULL;
    close(filedes_);
  }

  bool LooksLikeInputIsFlac(const SF_INFO &sndinfo, int filedes) {
    if ((sndinfo.format & SF_FORMAT_TYPEMASK) != SF_FORMAT_FLAC)
      return false;
    // However some files contain flac encoded stuff, but are not flac files
    // by themselve. So we can't copy headers verbatim. Sanity check header.
    char flac_magic[4];
    if (pread(filedes, flac_magic, sizeof(flac_magic), 0) != sizeof(flac_magic))
      return false;
    return memcmp(flac_magic, "fLaC", sizeof(flac_magic)) == 0;
  }

  FolveFilesystem *const fs_;
  const int filedes_;
  SNDFILE *const snd_in_;
  const SF_INFO in_info_;

  folve::Mutex stats_mutex_;
  HandlerStats base_stats_;      // UI information about current file.

  struct stat file_stat_;        // we dynamically report a changing size.
  off_t start_estimating_size_;  // essentially const.

  bool error_;
  bool copy_flac_header_verbatim_;
  ConversionBuffer *output_buffer_;
  SNDFILE *snd_out_;

  // Used in conversion.
  SoundProcessor *processor_;
  int input_frames_left_;
};
}  // namespace

FolveFilesystem::FolveFilesystem()
  : debug_ui_enabled_(false), gapless_processing_(false),
    open_file_cache_(4), processor_pool_(3),
    total_file_openings_(0), total_file_reopen_(0) {
}

FileHandler *FolveFilesystem::CreateFromDescriptor(
     int filedes,
     const std::string &config_dir,
     const char *fs_path,
     const std::string &underlying_file) {
  HandlerStats file_info;
  file_info.filename = fs_path;
  file_info.filter_dir = config_dir;
  if (!config_dir.empty()) {
    const std::string full_config_path = base_config_dir_ + "/" + config_dir;
    FileHandler *filter = SndFileHandler::Create(this, filedes, fs_path,
                                                 underlying_file,
                                                 config_dir, full_config_path,
                                                 &file_info);
    if (filter != NULL) return filter;
  }
  // Every other file-type is just passed through as is.
  return new PassThroughHandler(filedes, config_dir, file_info);
}

std::string FolveFilesystem::CacheKey(const std::string &config_path,
                                      const char *fs_path) {
  return config_path + fs_path;
}

FileHandler *FolveFilesystem::GetOrCreateHandler(const char *fs_path) {
  const std::string config_path = current_config_subdir_;
  const std::string cache_key = CacheKey(config_path, fs_path);
  const std::string underlying_file = underlying_dir() + fs_path;
  FileHandler *handler = open_file_cache_.FindAndPin(cache_key);
  if (handler == NULL) {
    int filedes = open(underlying_file.c_str(), O_RDONLY);
    if (filedes < 0)
      return NULL;
    ++total_file_openings_;
    handler = CreateFromDescriptor(filedes, config_path,
                                   fs_path, underlying_file);
    handler = open_file_cache_.InsertPinned(cache_key, handler);
  } else {
    ++total_file_reopen_;
  }
  return handler;
}

int FolveFilesystem::StatByFilename(const char *fs_path, struct stat *st) {
  const std::string cache_key = CacheKey(current_config_subdir_, fs_path);
  FileHandler *handler = open_file_cache_.FindAndPin(cache_key);
  if (handler == 0)
    return -1;
  ssize_t result = handler->Stat(st);
  open_file_cache_.Unpin(cache_key);
  return result;
}

void FolveFilesystem::Close(const char *fs_path, const FileHandler *handler) {
  assert(handler != NULL);
  const std::string cache_key = CacheKey(handler->filter_dir(), fs_path);
  open_file_cache_.Unpin(cache_key);
}

static bool IsDirectory(const std::string &path) {
  if (path.empty()) return false;
  struct stat st;
  if (stat(path.c_str(), &st) != 0)
    return false;
  return (st.st_mode & S_IFMT) == S_IFDIR;
}

bool FolveFilesystem::ListDirectory(const std::string &fs_dir,
                                    const std::string &suffix,
                                    std::set<std::string> *files) {
  const std::string real_dir = underlying_dir() + fs_dir;
  DIR *dp = opendir(real_dir.c_str());
  if (dp == NULL) return false;
  struct dirent *dent;
  while ((dent = readdir(dp)) != NULL) {
    if (!folve::HasSuffix(dent->d_name, suffix))
      continue;
    files->insert(fs_dir + dent->d_name);
  }
  closedir(dp);
  return true;
}

bool FolveFilesystem::SanitizeConfigSubdir(std::string *subdir_path) const {
  if (base_config_dir_.length() + 1 + subdir_path->length() > PATH_MAX)
    return false;  // uh, someone wants to buffer overflow us ?
  const std::string to_verify_path = base_config_dir_ + "/" + *subdir_path;
  char all_path[PATH_MAX];
  // This will as well eat symbolic links that break out, though one could
  // argue that that would be sane. We could think of some light
  // canonicalization that only removes ./ and ../
  const char *verified = realpath(to_verify_path.c_str(), all_path);
  if (verified == NULL) { // bogus directory.
    return false;
  }
  if (strncmp(verified, base_config_dir_.c_str(),
              base_config_dir_.length()) != 0) {
    // Attempt to break out with ../-tricks.
    return false;
  }
  if (!IsDirectory(verified))
    return false;

  // Derive from sanitized dir. So someone can write lowpass/../highpass
  // or '.' for empty filter. Or ./highpass. And all work.
  *subdir_path = ((strlen(verified) == base_config_dir_.length()) 
                  ? ""   // chose subdir '.'
                  : verified + base_config_dir_.length() + 1 /*slash*/);
  return true;
}

bool FolveFilesystem::SwitchCurrentConfigDir(const std::string &subdir_in) {
  std::string subdir = subdir_in;
  if (!subdir.empty() && !SanitizeConfigSubdir(&subdir)) {
    syslog(LOG_INFO, "Invalid config switch attempt to '%s'",
           subdir_in.c_str());
    return false;
  }
  if (subdir != current_config_subdir_) {
    current_config_subdir_ = subdir;
    if (subdir.empty()) {
      syslog(LOG_INFO, "Switching to pass-through mode.");
    } else {
      syslog(LOG_INFO, "Switching config directory to '%s'", subdir.c_str());
    }
    return true;
  }
  return false;
}

bool FolveFilesystem::CheckInitialized() {
  if (underlying_dir().empty()) {
    fprintf(stderr, "Don't know the underlying directory to read from.\n");
    return false;
  }

  if (!IsDirectory(underlying_dir())) {
    fprintf(stderr, "<underlying-dir>: '%s' not a directory.\n",
            underlying_dir().c_str());
    return false;
  }

  if (base_config_dir_.empty() || !IsDirectory(base_config_dir_)) {
    fprintf(stderr, "<config-dir>: '%s' not a directory.\n",
            base_config_dir_.c_str());
    return false;
  }

  return true;
}

void FolveFilesystem::SetupInitialConfig() {
  std::set<std::string> available_dirs = ListConfigDirs(true);
  // Some sanity checks.
  if (available_dirs.size() == 1) {
    syslog(LOG_NOTICE, "No filter configuration directories given. "
           "Any files will be just passed through verbatim.");
  }
  if (available_dirs.size() > 1) {
    // By default, lets set the index to the first filter the user provided.
    SwitchCurrentConfigDir(*++available_dirs.begin());
  }
}

const std::set<std::string> FolveFilesystem::GetAvailableConfigDirs() const {
  return ListConfigDirs(false);
}

const std::set<std::string> FolveFilesystem::ListConfigDirs(bool warn_invalid)
  const {
  std::set<std::string> result;
  result.insert("");  // empty directory: pass-through.
  DIR *dp = opendir(base_config_dir_.c_str());
  if (dp == NULL) return result;
  struct dirent *dent;
  while ((dent = readdir(dp)) != NULL) {
    std::string subdir = dent->d_name;
    if (subdir == "." || subdir == "..")
      continue;
    if (!SanitizeConfigSubdir(&subdir)) {
      if (warn_invalid) {
        syslog(LOG_INFO, "Note: '%s' ignored in config directory; not a "
               "directory or pointing outside base directory.", dent->d_name);
      }
      continue;
    }
    result.insert(subdir);
  }
  closedir(dp);
  return result;
}

