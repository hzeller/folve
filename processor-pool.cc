// -*- c++ -*-
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

#include "processor-pool.h"

#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include <vector>

#include "sound-processor.h"
#include "util.h"

using folve::StringPrintf;
using folve::DLogf;

ProcessorPool::ProcessorPool(int max_available)
  : max_per_config_(max_available) {
}

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

SoundProcessor *ProcessorPool::GetOrCreate(const std::string &base_dir,
                                           int sampling_rate, int channels,
                                           int bits, std::string *errmsg) {
  std::vector<std::string> path_choices;
  // From specific to non-specific.
  path_choices.push_back(StringPrintf("%s/filter-%d-%d-%d.conf",
                                      base_dir.c_str(),
                                      sampling_rate, channels, bits));
  path_choices.push_back(StringPrintf("%s/filter-%d-%d.conf",
                                      base_dir.c_str(),
                                      sampling_rate, channels));
  path_choices.push_back(StringPrintf("%s/filter-%d.conf",
                                      base_dir.c_str(),
                                      sampling_rate));

  std::string config_path;
  if (!FindFirstAccessiblePath(path_choices, &config_path)) {
    const char *short_dir = strrchr(base_dir.c_str(), '/') + 1;
    *errmsg = StringPrintf("No filter in %s for %.1fkHz/%d ch/%d bits",
                           short_dir, sampling_rate / 1000.0, channels, bits);
    return NULL;
  }
  SoundProcessor *result;
  while ((result = CheckOutOfPool(config_path)) != NULL) {
    if (result->ConfigStillUpToDate())
      break;
    DLogf("Processor %p: outdated; Good riddance after config file change %s",
          result, config_path.c_str());
    delete result;
  }
  if (result != NULL) {
    DLogf("Processor %p: Got from pool [%s]", result, config_path.c_str());
    return result;
  }

  result = SoundProcessor::Create(config_path, sampling_rate, channels);
  if (result == NULL) {
    *errmsg = "Problem parsing " + config_path;
    syslog(LOG_ERR, "filter-config %s is broken.", config_path.c_str());
  }
  DLogf("Processor %p: Newly created [%s]", result, config_path.c_str());
  return result;
}

void ProcessorPool::Return(SoundProcessor *processor) {
  if (processor == NULL) return;
  if (!processor->ConfigStillUpToDate()) {
    DLogf("Processor %p: outdated. Not returning back in pool [%s]", processor,
          processor->config_file().c_str());
    delete processor;
    return;
  }
  folve::MutexLock l(&pool_mutex_);
  PoolMap::iterator ins_pos = 
    pool_.insert(make_pair(processor->config_file(),
                           (ProcessorList*) NULL)).first;
  if (ins_pos->second == NULL) {
    ins_pos->second = new ProcessorList();
  }
  if (ins_pos->second->size() < max_per_config_) {
    processor->Reset();
    ins_pos->second->push_back(processor);
    DLogf("Processor %p: Returned to pool (count=%zd) [%s]\n",
          processor, ins_pos->second->size(), processor->config_file().c_str());
  } else {
    DLogf("Processor %p: Getting rid of it; enough processors in pool.",
          processor);
    delete processor;
  } 
}

SoundProcessor *ProcessorPool::CheckOutOfPool(const std::string &config_path) {
  folve::MutexLock l(&pool_mutex_);
  PoolMap::iterator found = pool_.find(config_path);
  if (found == pool_.end())
    return NULL;
  ProcessorList *list = found->second;
  if (list->empty())
    return NULL;
  SoundProcessor *result = list->front();
  list->pop_front();
  return result;
}
