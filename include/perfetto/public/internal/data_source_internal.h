/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_PUBLIC_INTERNAL_DATA_SOURCE_INTERNAL_H_
#define INCLUDE_PERFETTO_PUBLIC_INTERNAL_DATA_SOURCE_INTERNAL_H_

namespace perfetto {

class DataSourceBase;
class TraceWriterBase;

namespace internal {

// This object maintains the internal state of a data source that is used only
// to implement the tracing mechanics and is not exposed to the API client.
// There is one of these object per DataSource instance (up to
// kMaxDataSourceInstances).
struct DataSourceState {
  // TODO(primiano): fill in next CLs.
};

// Per-DataSource-type global state.
struct DataSourceStaticState {
  // TODO(primiano): fill in next CLs.
};

// Per-DataSource-type thread-local state.
struct DataSourceThreadLocalState {
  // TODO(primiano): fill in next CLs.
};

}  // namespace internal
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_PUBLIC_INTERNAL_DATA_SOURCE_INTERNAL_H_
