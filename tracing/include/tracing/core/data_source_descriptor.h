/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef TRACING_INCLUDE_TRACING_CORE_DATA_SOURCE_DESCRIPTOR_H_
#define TRACING_INCLUDE_TRACING_CORE_DATA_SOURCE_DESCRIPTOR_H_

#include <string>

namespace perfetto {

// This class contains the details of the DataSource that Producer(s) advertise
// to the Service through Service::ProducerEndpoint::RegisterDataSource().
// This is to pass information such as exposed field, supported filters etc.

// This has to be kept in sync with src/ipc/data_source_descriptor.proto .
// TODO(primiano): find a way to auto-generate this and the glue code that
// converts DataSourceDescriptor <> proto::DataSourceDescriptor.
class DataSourceDescriptor {
 public:
  std::string name;  // e.g., org.chromium.trace_events.

  // TODO(primiano): fill this in next CLs.
};

}  // namespace perfetto

#endif  // TRACING_INCLUDE_TRACING_CORE_DATA_SOURCE_DESCRIPTOR_H_
