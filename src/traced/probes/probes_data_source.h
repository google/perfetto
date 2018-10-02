/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACED_PROBES_PROBES_DATA_SOURCE_H_
#define SRC_TRACED_PROBES_PROBES_DATA_SOURCE_H_

#include "perfetto/tracing/core/basic_types.h"

namespace perfetto {

// Base class for all data sources in traced_probes.
class ProbesDataSource {
 public:
  // |type_id| is a home-brewed RTTI, e.g. InodeFileDataSource::kTypeId.
  ProbesDataSource(TracingSessionID, int type_id);
  virtual ~ProbesDataSource();

  virtual void Start() = 0;
  virtual void Flush() = 0;

  const TracingSessionID tracing_session_id;
  const int type_id;
  bool started = false;  // Set by probes_producer.cc.

 private:
  ProbesDataSource(const ProbesDataSource&) = delete;
  ProbesDataSource& operator=(const ProbesDataSource&) = delete;
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_PROBES_DATA_SOURCE_H_
