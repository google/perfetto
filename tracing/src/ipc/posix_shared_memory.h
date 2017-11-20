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

#ifndef TRACING_SRC_IPC_POSIX_SHARED_MEMORY_H_
#define TRACING_SRC_IPC_POSIX_SHARED_MEMORY_H_

#include <stddef.h>

#include <memory>

#include "base/scoped_file.h"
#include "tracing/core/shared_memory.h"

namespace perfetto {

// Implements the SharedMemory and its factory for the posix-based transport.
// TODO(primiano): implement in next CLs.
class PosixSharedMemory : public SharedMemory {
 public:
  class Factory : public SharedMemory::Factory {
   public:
    ~Factory() override;
    std::unique_ptr<SharedMemory> CreateSharedMemory(size_t) override;
  };

  // SharedMemory implementation.
  void* start() const override { return nullptr; }
  size_t size() const override { return 0; }
};

}  // namespace perfetto

#endif  // TRACING_SRC_IPC_POSIX_SHARED_MEMORY_H_
