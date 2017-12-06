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

#ifndef INCLUDE_PERFETTO_TRACING_CORE_DATA_SOURCE_CONFIG_H_
#define INCLUDE_PERFETTO_TRACING_CORE_DATA_SOURCE_CONFIG_H_

#include <string>

namespace perfetto {

// This class contains the configuration that the Service sends back to the
// Producer when it tells it to enable a given data source. This is the way
// that, for instance, the Service will tell the producer "turn tracing on,
// enable categories 'foo' and 'bar' and emit only the fields X and Y".

// This has to be kept in sync with src/ipc/data_source_config.proto .
// TODO(primiano): find a way to auto-generate this and the glue code that
// converts DataSourceConfig <> proto::DataSourceConfig.
class DataSourceConfig {
 public:
  std::string data_source_name;  // e.g., "org.chromium.trace_events"

  // TODO(primiano): temporary, for testing only.
  std::string trace_category_filters;  // e.g., "ipc,media,toplvel"
};

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_CORE_DATA_SOURCE_CONFIG_H_
