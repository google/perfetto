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

#ifndef INCLUDE_PERFETTO_TRACING_CORE_FORWARD_DECLS_H_
#define INCLUDE_PERFETTO_TRACING_CORE_FORWARD_DECLS_H_

// Forward declares classes that are generated at build-time from protos.
// First of all, why are we forward declaring at all?
//  1. Chromium diverges from the Google style guide on this, because forward
//     declarations typically make build times faster, and that's a desirable
//     property for a large and complex codebase.
//  2. Adding #include to build-time-generated headers from headers typically
//     creates subtle build errors that are hard to spot in GN. This is because
//     once a standard header (say foo.h9 has an #include "protos/foo.gen.h",
//     the build target that depends on foo.h needs to depend on the genrule
//     that generates foo.gen.h. This is achievable using public_deps in GN but
//     is not testable / enforceable, hence too easy to get wrong.

// TODO(primiano): update forward declarations and add the rest of the story in
// the next CLs.

namespace perfetto {

class ChromeConfig;
class CommitDataRequest;
class DataSourceConfig;
class DataSourceDescriptor;
class ObservableEvents;
class TraceConfig;
class TraceStats;
class TracingServiceState;

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_CORE_FORWARD_DECLS_H_
