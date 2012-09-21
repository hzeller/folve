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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sndfile.h>
#include <stdarg.h>
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

static bool global_debug = false;

static void DebugLogf(const char *format, ...)
  __attribute__ ((format (printf, 1, 2)));

void DebugLogf(const char *format, ...) {
  if (!global_debug) return;
  va_list ap;
  va_start(ap, format);
  vsyslog(LOG_DEBUG, format, ap);
  va_end(ap);
}

namespace {

// Very simple filter that just passes the original file through. Used for
// everything that is not a sound-file.
class PassThroughFilter : public FileHandler {
public:
  PassThroughFilter(int filedes, int filter_id,
                    const HandlerStats &known_stats)
    : FileHandler(filter_id), filedes_(filedes), info_stats_(known_stats) {
    info_stats_.message.append("; pass through.");
    DebugLogf("Creating PassThrough filter for '%s'",
              known_stats.filename.c_str());
  }
  ~PassThroughFilter() { close(filedes_); }

  virtual int Read(char *buf, size_t size, off_t offset) {
    const int result = pread(filedes_, buf, size, offset);
    return result == -1 ? -errno : result;
  }
  virtual int Stat(struct stat *st) {
    return fstat(filedes_, st);
  }
  virtual void GetHandlerStatus(struct HandlerStats *stats) {
    *stats = info_stats_;
  }
  
private:
  const int filedes_;
  HandlerStats info_stats_;
};

static bool FindFirstAccessiblePath(const std::vector<std::string> &path,
                                    std::string *match) {
  for (size_t i = 0; i < path.size(); ++i) {
    if (access(path[i].c_str(), R_OK) == 0) {
      *match = path[i];
      return true;
    }
  }
  return false;
}

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
                             int filter_id,
                             const std::string &zita_config_dir,
                             HandlerStats *partial_file_info) {
    SF_INFO in_info;
    memset(&in_info, 0, sizeof(in_info));
    SNDFILE *snd = sf_open_fd(filedes, SFM_READ, &in_info, 0);
    if (snd == NULL) {
      DebugLogf("File %s: %s", underlying_file.c_str(), sf_strerror(NULL));
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

    std::vector<std::string> path_choices;
    // From specific to non-specific.
    path_choices.push_back(StringPrintf("%s/filter-%d-%d-%d.conf",
                                        zita_config_dir.c_str(),
                                        in_info.samplerate,
                                        in_info.channels, bits));
    path_choices.push_back(StringPrintf("%s/filter-%d-%d.conf",
                                        zita_config_dir.c_str(),
                                        in_info.samplerate,
                                        in_info.channels));
    path_choices.push_back(StringPrintf("%s/filter-%d.conf",
                                        zita_config_dir.c_str(),
                                        in_info.samplerate));
    const int seconds = in_info.frames / in_info.samplerate;
    const int max_choice = path_choices.size() - 1;
    std::string config_path;
    const bool found_config = FindFirstAccessiblePath(path_choices,
                                                      &config_path);
    if (found_config) {
      DebugLogf("File %s, %.1fkHz, %d Bit, %d:%02d: filter config %s",
                underlying_file.c_str(), in_info.samplerate / 1000.0, bits,
                seconds / 60, seconds % 60,
                config_path.c_str());
    } else {
      DebugLogf("File %s: couldn't find filter config %s...%s",
                underlying_file.c_str(),
                path_choices[0].c_str(), path_choices[max_choice].c_str());
      partial_file_info->message = "Missing ( " + path_choices[0]
        + "<br/> ... " + path_choices[max_choice] + " )";
      sf_close(snd);
      return NULL;
    }
    return new SndFileHandler(fs, fs_path, filter_id,
                              underlying_file, filedes, snd, in_info,
                              *partial_file_info, config_path);
  }
  
  virtual ~SndFileHandler() {
    Close();
    delete processor_;
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

  virtual void GetHandlerStatus(struct HandlerStats *stats) {
    *stats = base_stats_;
    const int frames_done = in_info_.frames - input_frames_left_;
    if (frames_done == 0 || in_info_.frames == 0)
      stats->progress = 0.0;
    else
      stats->progress = 1.0 * frames_done / in_info_.frames;
    if (processor_ && processor_->max_output_value() > 1.0) {
      base_stats_.message =
        StringPrintf("Output clipping! "
                     "(max=%.3f; Multiply gain with <= %.5f<br/>in %s)",
                     processor_->max_output_value(),
                     1.0 / processor_->max_output_value(),
                     config_path_.c_str());
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
  SndFileHandler(FolveFilesystem *fs, const char *fs_path, int filter_id,
                 const std::string &underlying_file,
                 int filedes, SNDFILE *snd_in,
                 const SF_INFO &in_info, const HandlerStats &file_info,
                 const std::string &config_path)
    : FileHandler(filter_id), fs_(fs),
      filedes_(filedes), snd_in_(snd_in), in_info_(in_info),
      config_path_(config_path),
      base_stats_(file_info),
      error_(false), output_buffer_(NULL),
      snd_out_(NULL), processor_(NULL),
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
      out_buffer->WriteCharAt((32768 & 0xFF00) >> 8, 10);
      out_buffer->WriteCharAt((32768 & 0x00FF)     , 11);
      for (int i = 12; i < 18; ++i) out_buffer->WriteCharAt(0, i);  // MD5
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
    DebugLogf("Header init done (%s).", base_stats_.filename.c_str());
    out_buffer->HeaderFinished();
  }

  virtual bool AcceptProcessor(SoundProcessor *processor) {
    if (processor_ != NULL || !input_frames_left_) {
      DebugLogf("Gapless attempt: Cannot bridge gap to alrady open file %s",
                base_stats_.filename.c_str());
      return false;  // We already have one.
    }
    // TODO: check that other parameters such as sampling rate and channels
    // match (should be a are problem).
    processor_ = processor;
    if (!processor_->is_input_buffer_complete()) {
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
    if (processor_ && processor_->pending_writes() > 0) {
      processor_->WriteProcessed(snd_out_, processor_->pending_writes());
      return input_frames_left_;
    }
    if (!input_frames_left_)
      return false;
    if (!processor_) {
      // First time we're called and we don't have any processor yet.
      processor_ = SoundProcessor::Create(config_path_, in_info_.samplerate,
                                          in_info_.channels);
      if (processor_ == NULL) {
        syslog(LOG_ERR, "Oops - filter-config %s is broken. Please fix. "
               "Won't play this stream %s (simulating empty file)",
               config_path_.c_str(), base_stats_.filename.c_str());
        base_stats_.message = "Problem parsing " + config_path_;
        input_frames_left_ = 0;
        Close();
        return false;
      }
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
    input_frames_left_ -= r;
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
        DebugLogf("Gapless pass-on from '%s' to alphabetically next '%s'",
                  base_stats_.filename.c_str(), found->c_str());
      }
      processor_->WriteProcessed(snd_out_, r);
      if (passed_processor) {
        base_stats_.out_gapless = true;
        LogClipping();
        processor_ = NULL;   // we handed over ownership.
      }
      if (next_file) fs_->Close(found->c_str(), next_file);
    } else {
      processor_->WriteProcessed(snd_out_, r);
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
    DebugLogf("Provide FLAC header from original file %s",
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
      DebugLogf(" %02x %02x %02x %02x type: %d, len: %6u %s %s ",
                header[0], header[1], header[2], header[3],
                type, byte_len, is_last ? "(last)" : "(cont)", extra_info);
      pos += byte_len;
      if (is_last)
        break;
    }
    if (need_finish_padding) {  // if the last block was not is_last: pad.
      DebugLogf("write padding");
      memset(&header, 0, sizeof(header));
      header[0] = 0x80 /* is last */ | FLAC__METADATA_TYPE_PADDING;
      out_buffer->Append(&header, sizeof(header));
    }
  }

  void GenerateHeaderFromInputFile(ConversionBuffer *out_buffer) {
    DebugLogf("Generate header from original ID3-tags.");
    out_buffer->set_sndfile_writes_enabled(true);
    // Copy ID tags that are supported by sndfile.
    for (int i = SF_STR_FIRST; i <= SF_STR_LAST; ++i) {
      const char *s = sf_get_string(snd_in_, i);
      if (s != NULL) {
        sf_set_string(snd_out_, i, s);
      }
    }
  }

  void LogClipping() {
    if (processor_ && processor_->max_output_value() > 1.0) {
      syslog(LOG_ERR, "Observed output clipping (%s). "
             "Max=%.3f; Multiply gain with <= %.5f in %s",
             base_stats_.filename.c_str(), processor_->max_output_value(),
             1.0 / processor_->max_output_value(), config_path_.c_str());
      processor_->ResetMaxValues();
    }
  }
  void Close() {
    if (snd_out_ == NULL) return;  // done.
    LogClipping();
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
  const std::string config_path_;

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
  : current_cfg_index_(0), debug_ui_enabled_(false), gapless_processing_(false),
    open_file_cache_(3), total_file_openings_(0), total_file_reopen_(0) {
  config_dirs_.push_back("");  // The first config is special: empty.
}

FileHandler *FolveFilesystem::CreateFromDescriptor(
     int filedes,
     int cfg_idx,
     const char *fs_path,
     const std::string &underlying_file) {
  HandlerStats file_info;
  file_info.filename = fs_path;
  if (cfg_idx != 0) {
    FileHandler *filter = SndFileHandler::Create(this, filedes, fs_path,
                                                 underlying_file,
                                                 cfg_idx,
                                                 config_dirs()[cfg_idx],
                                                 &file_info);
    if (filter != NULL) return filter;
  } else {
    file_info.message = "No filter config selected.";
  }

  // Every other file-type is just passed through as is.
  return new PassThroughFilter(filedes, cfg_idx, file_info);
}

std::string FolveFilesystem::CacheKey(int config_idx, const char *fs_path) {
  std::string result;
  Appendf(&result, "%d/%s", config_idx, fs_path);
  return result;
}

FileHandler *FolveFilesystem::GetOrCreateHandler(const char *fs_path) {
  const int config_idx = current_cfg_index_;
  const std::string cache_key = CacheKey(config_idx, fs_path);
  const std::string underlying_file = underlying_dir() + fs_path;
  FileHandler *handler = open_file_cache_.FindAndPin(cache_key);
  if (handler == NULL) {
    int filedes = open(underlying_file.c_str(), O_RDONLY);
    if (filedes < 0)
      return NULL;
    ++total_file_openings_;
    handler = CreateFromDescriptor(filedes, config_idx,
                                   fs_path, underlying_file);
    handler = open_file_cache_.InsertPinned(cache_key, handler);
  } else {
    ++total_file_reopen_;
  }
  return handler;
}

int FolveFilesystem::StatByFilename(const char *fs_path, struct stat *st) {
  const std::string cache_key = CacheKey(current_cfg_index_, fs_path);
  FileHandler *handler = open_file_cache_.FindAndPin(cache_key);
  if (handler == 0)
    return -1;
  ssize_t result = handler->Stat(st);
  open_file_cache_.Unpin(cache_key);
  return result;
}

void FolveFilesystem::Close(const char *fs_path, const FileHandler *handler) {
  const std::string cache_key = CacheKey(handler->filter_id(), fs_path);
  open_file_cache_.Unpin(cache_key);
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

void FolveFilesystem::SwitchCurrentConfigIndex(int i) {
  if (i < 0 || i >= (int) config_dirs_.size())
    return;
  if (i != current_cfg_index_) {
    if (i == 0) {
      syslog(LOG_INFO, "Switching to pass-through mode.");
    } else {
      syslog(LOG_INFO, "Switching config directory to '%s'",
             config_dirs()[i].c_str());
    }
    current_cfg_index_ = i;
  }
}

static bool IsDirectory(const std::string &path) {
  if (path.empty()) return false;
  struct stat st;
  if (stat(path.c_str(), &st) != 0)
    return false;
  return (st.st_mode & S_IFMT) == S_IFDIR;
}

void FolveFilesystem::SetDebugMode(bool b) {
  if (b != global_debug) {
    syslog(LOG_INFO, "Switch debug mode %s.", b ? "on" : "off");
    global_debug = b;
  }
}
bool FolveFilesystem::IsDebugMode() const { return global_debug; }

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

  for (size_t i = 1; i < config_dirs_.size(); ++i) {
    if (!IsDirectory(config_dirs_[i])) {
      fprintf(stderr, "<config-dir>: '%s' not a directory.\n",
              config_dirs_[i].c_str());
      return false;
    }
  }
  if (config_dirs_.size() > 1) {
    // By default, lets set the index to the first filter the user provided.
    SwitchCurrentConfigIndex(1);
  }
  return true;
}
