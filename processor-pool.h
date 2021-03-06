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
#ifndef FOLVE_PROCESSOR_POOL_
#define FOLVE_PROCESSOR_POOL_

#include <map>
#include <deque>
#include <string>

#include "util.h"

class SoundProcessor;

// An object pool for SoundProcessors. They are expensive to create, in
// particular on slow machines, but they only change if the configuration
// file is touched. Good candidates for re-use.
class ProcessorPool {
public:
  // Create a processor pool. Stores at most "max_per_config" processors in
  // pool per configuration file.
  ProcessorPool(int max_per_config);

  // Get a new SoundProcesor from this pool with the given configuration.
  // If this isn't possible, NULL is returned an an error message stored in
  // "errmsg".
  SoundProcessor *GetOrCreate(const std::string &base_dir,
                              int sampling_rate, int channels, int bits,
                              std::string *errmsg);

  // Return a processor pack to the pool.
  void Return(SoundProcessor *processor);

private:
  typedef std::deque<SoundProcessor*> ProcessorList;
  typedef std::map<std::string, ProcessorList*> PoolMap;

  SoundProcessor *CheckOutOfPool(const std::string &config_path);

  const size_t max_per_config_;
  folve::Mutex pool_mutex_;
  PoolMap pool_;
};

#endif  // FOLVE_PROCESSOR_POOL_
