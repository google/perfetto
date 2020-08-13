/*
 * Copyright (C) 2020 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROFILER_UTIL_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROFILER_UTIL_H_

#include <string>

#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_view.h"

#include "protos/perfetto/trace/profiling/deobfuscation.pbzero.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

std::string FullyQualifiedDeobfuscatedName(
    protos::pbzero::ObfuscatedClass::Decoder& cls,
    protos::pbzero::ObfuscatedMember::Decoder& member);

base::Optional<std::string> PackageFromLocation(TraceStorage* storage,
                                                base::StringView location);

}
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PROFILER_UTIL_H_
