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

#include "tracing/src/ipc/posix_shared_memory.h"

#include <memory>
#include <utility>

#include "base/logging.h"

namespace perfetto {

PosixSharedMemory::Factory::~Factory() {}

std::unique_ptr<SharedMemory> PosixSharedMemory::Factory::CreateSharedMemory(
    size_t size) {
  return nullptr;
}

}  // namespace perfetto
