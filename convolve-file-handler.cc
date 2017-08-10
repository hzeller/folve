//  -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
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

#include "convolve-file-handler.h"

#include <FLAC/metadata.h>
#include <sndfile.h>
#include <string.h>
#include <syslog.h>
#include <assert.h>

#include "conversion-buffer.h"
#include "folve-filesystem.h"
#include "sound-processor.h"
#include "util.h"
#include "zita-config.h"

/*
 * The blocksize we are going to write the output flac files with. This
 * is essentially a constant of the internals between libsndfile and libflac.
 * There should be a better way to extract this value, but for now we just
 * define it to whatever was observed.
 * (e.g. somewhere in the past it changed from 1152 to 4096).
 * Made an #define so that we can override it on the compile commandline.
 */
#ifndef FLAC_BLOCK_SIZE
#  define FLAC_BLOCK_SIZE 4096
#endif

using folve::DLogf;
using folve::Appendf;
using folve::StringPrintf;

// Attempt to create a ConvolveFileHandler from the given file descriptor. This
// returns NULL if this is not a sound-file or if there is no available
// convolution filter configuration available.
// "partial_file_info" will be set to information known so far, including
// error message.
FileHandler *ConvolveFileHandler::Create(FolveFilesystem *fs,
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

  // Remember whatever we could get to know in the partial file info.
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
  return new ConvolveFileHandler(fs, fs_path, filter_subdir,
                                 underlying_file, filedes, snd, in_info,
                                 *partial_file_info, processor);
}

ConvolveFileHandler::~ConvolveFileHandler() {
  output_buffer_->NotifyFileComplete();
  fs_->QuitBuffering(output_buffer_);  // stop working on our files.
  Close();                             // ... so that we can close them :)
  delete output_buffer_;
}

int ConvolveFileHandler::Read(char *buf, size_t size, off_t offset) {
  if (error_) return -1;
  const off_t current_filesize = output_buffer_->FileSize();
  const off_t read_horizon = offset + size;
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
  if (current_filesize < offset
      && (int) (read_horizon + kFudgeOverhang) >= file_stat_.st_size) {
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
  int result = output_buffer_->Read(buf, size, offset);

  // Only if the user obviously read beyond our header, we start the
  // pre-buffering; otherwise things will get sluggish because any header
  // access that goes a bit overboard triggers pre-buffer (i.e. while indexing)
  // Amarok for instance seems to read up to 16k beyond the header.
  //
  // In general, this is a fine heuristic: this happens the first time we
  // access the file, the first or second stream-read should trigger the
  // pre-buffering (64k is less than a second music). If we're in gapless
  // mode, we already start pre-buffering anyway early (see
  // NotifyPassedProcessorUnreferenced()) - so that important use-case is
  // covered.
  const off_t well_beyond_header = output_buffer_->HeaderSize() + (64 << 10);
  const bool should_request_prebuffer = read_horizon > well_beyond_header
    && read_horizon + fs_->pre_buffer_size() > current_filesize
    && !output_buffer_->IsFileComplete();
  if (should_request_prebuffer) {
    fs_->RequestPrebuffer(output_buffer_);
  }
  return result;
}

void ConvolveFileHandler::GetHandlerStatus(HandlerStats *stats) {
  const off_t file_size = output_buffer_->FileSize();
  const off_t max_access = output_buffer_->MaxAccessed();
  if (processor_ != NULL) {
    base_stats_.max_output_value = processor_->max_output_value();
  }
  *stats = base_stats_;
  const int frames_done = in_info_.frames - frames_left();
  if (frames_done == 0 || in_info_.frames == 0) {
    stats->buffer_progress = 0.0;
    stats->access_progress = 0.0;
  } else {
    stats->buffer_progress = 1.0 * frames_done / in_info_.frames;
    stats->access_progress = stats->buffer_progress * max_access / file_size;
  }

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

int ConvolveFileHandler::Stat(struct stat *st) {
  const off_t current_file_size = output_buffer_->FileSize();
  if (current_file_size > start_estimating_size_) {
    const int frames_done = in_info_.frames - frames_left();
    if (frames_done > 0) {
      const float estimated_end = 1.0 * in_info_.frames / frames_done;
      off_t new_size = estimated_end * current_file_size;
      // Report a bit bigger size which is less harmful than programs
      // reading short.
      new_size += 65535;
      if (new_size > file_stat_.st_size) {  // Only go forward in size.
        file_stat_.st_size = new_size;
      }
    }
  }
  *st = file_stat_;
  return 0;
}

// TODO(hzeller): trim parameter list.
ConvolveFileHandler::ConvolveFileHandler(FolveFilesystem *fs,
                                         const char *fs_path,
                                         const std::string &filter_dir,
                                         const std::string &underlying_file,
                                         int filedes, SNDFILE *snd_in,
                                         const SF_INFO &in_info,
                                         const HandlerStats &file_info,
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
  original_file_size_ = file_stat_.st_size;
  file_stat_.st_size *= fs->file_oversize_factor();

  // The flac header we get is more rich than what we can create via
  // sndfile. So if we have one, just copy it.
  copy_flac_header_verbatim_ = LooksLikeInputIsFlac(in_info, filedes);

  if (fs_->workaround_flac_header_issue()) {
    copy_flac_header_verbatim_ = false;  // Disable again in that case.
  }

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

  out_info.channels = processor->output_channels();
  DLogf("Output channels: %d", out_info.channels);

  output_buffer_ = new ConversionBuffer(this, out_info);
}

void ConvolveFileHandler::SetOutputSoundfile(ConversionBuffer *out_buffer,
                                             const SF_INFO &info,
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
  if (!fs_->workaround_flac_header_issue()) {
    sf_command(snd_out_, SFC_UPDATE_HEADER_NOW, NULL, 0);
  }

  // -- time for some hackery ...
  // If we have copied the header over from the original, we need to
  // redact the values for min/max blocksize and min/max framesize with
  // what SNDFILE is going to use, otherwise programs will trip over this.
  // http://flac.sourceforge.net/format.html
  // Also, number of output channels might be different.
  if (copy_flac_header_verbatim_) {
    out_buffer->WriteCharAt((FLAC_BLOCK_SIZE & 0xFF00) >> 8,  8);
    out_buffer->WriteCharAt((FLAC_BLOCK_SIZE & 0x00FF)     ,  9);
    out_buffer->WriteCharAt((FLAC_BLOCK_SIZE & 0xFF00) >> 8, 10);
    out_buffer->WriteCharAt((FLAC_BLOCK_SIZE & 0x00FF)     , 11);
    for (int i = 12; i < 18; ++i) out_buffer->WriteCharAt(0, i); // framesize
    // Byte 20:
    //  XXXX YYY Z
    //  X: lowest 4 bit samplerate; Y: channels - 1; Z: upper bit bit/sample
    int bits = 16;
    if ((info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_PCM_24) bits = 24;
    if ((info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_PCM_32) bits = 32;
    out_buffer->WriteCharAt((in_info_.samplerate  & 0x0f) << 4
                            | (info.channels - 1)  << 1
                            | ((bits - 1 ) & 0x10) >> 4,
                            20);
  } else if ((info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_FLAC) {
    // .. and if SNDFILE writes the header, it misses out in writing the
    // number of samples to be expected. So let's fill that in for flac files.
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

bool ConvolveFileHandler::HasStarted() {
  return in_info_.frames != input_frames_left_;
}

bool ConvolveFileHandler::PassoverProcessor(SoundProcessor *passover_processor) {
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

void ConvolveFileHandler::NotifyPassedProcessorUnreferenced() {
  // This is gapless. Good idea to pre-buffer the beginning.
  fs_->RequestPrebuffer(output_buffer_);
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

bool ConvolveFileHandler::AddMoreSoundData() {
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
         && next_file->PassoverProcessor(processor_));
    if (passed_processor) {
      DLogf("Processor %p: Gapless pass-on from "
            "'%s' to alphabetically next '%s'", processor_,
            base_stats_.filename.c_str(), found->c_str());
    }
    processor_->WriteProcessed(snd_out_, r);
    if (passed_processor) {
      base_stats_.out_gapless = true;
      SaveOutputValues();
      processor_ = NULL;   // we handed over ownership.
      Close();  // make sure that our thread is done.
      next_file->NotifyPassedProcessorUnreferenced();
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
static void CopyBytes(int fd, off_t pos, ConversionBuffer *out, size_t len) {
  char buf[256];
  while (len > 0) {
    ssize_t r = pread(fd, buf, std::min(sizeof(buf), len), pos);
    if (r <= 0) return;
    out->Append(buf, r);
    len -= r;
    pos += r;
  }
}

void ConvolveFileHandler::CopyFlacHeader(ConversionBuffer *out_buffer) {
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

void ConvolveFileHandler::GenerateHeaderFromInputFile(
             ConversionBuffer *out_buffer) {
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

void ConvolveFileHandler::SaveOutputValues() {
  if (processor_) {
    base_stats_.max_output_value = processor_->max_output_value();
    processor_->ResetMaxValues();
  }
}

void ConvolveFileHandler::Close() {
  if (snd_out_ == NULL) return;  // done.
  input_frames_left_ = 0;
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

  const double factor = 1.0 * output_buffer_->FileSize() / original_file_size_;
  if (factor > fs_->file_oversize_factor()) {
    syslog(LOG_WARNING, "File larger than prediction: "
           "%lldx%.2f=%lld < %lld (x%4.2f) '%s'; "
           "naive streamer implementations might trip "
           "(adapt prediction with -O %.2f)",
           (long long)original_file_size_, fs_->file_oversize_factor(),
           (long long)(original_file_size_ * fs_->file_oversize_factor()),
           (long long)output_buffer_->FileSize(), factor,
           base_stats_.filename.c_str(), factor);
  }
}

bool ConvolveFileHandler::LooksLikeInputIsFlac(const SF_INFO &sndinfo,
                                               int filedes) {
  if ((sndinfo.format & SF_FORMAT_TYPEMASK) != SF_FORMAT_FLAC)
    return false;
  // However some files contain flac encoded stuff, but are not flac files
  // by themselve. So we can't copy headers verbatim. Sanity check header.
  char flac_magic[4];
  if (pread(filedes, flac_magic, sizeof(flac_magic), 0) != sizeof(flac_magic))
    return false;
  return memcmp(flac_magic, "fLaC", sizeof(flac_magic)) == 0;
}

int ConvolveFileHandler::frames_left() {
  folve::MutexLock l(&stats_mutex_);
  return input_frames_left_;
}
