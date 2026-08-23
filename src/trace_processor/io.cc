/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "perfetto/trace_processor/io.h"

namespace perfetto::trace_processor::io {

Mapping::Mapping() = default;
Mapping::~Mapping() = default;
File::File() = default;
File::~File() = default;
base::Status File::CreateMapping(uint64_t,
                                 uint64_t,
                                 MappingAccess,
                                 std::unique_ptr<Mapping>*) {
  return base::ErrStatus("File mappings are not supported");
}
FileSystem::FileSystem() = default;
FileSystem::~FileSystem() = default;
base::Status FileSystem::CreateTemporaryFile(std::unique_ptr<File>*) {
  return base::ErrStatus("Temporary files are not supported");
}

}  // namespace perfetto::trace_processor::io
