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

#ifndef SRC_TRACING_TEST_TEST_SHARED_MEMORY_H_
#define SRC_TRACING_TEST_TEST_SHARED_MEMORY_H_

#include <stddef.h>

#include <memory>

#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/ext/tracing/core/shared_memory.h"
#include "src/tracing/core/in_process_shared_memory.h"

namespace perfetto {

// A dummy implementation of shared memory for single process unittests
// (just a wrapper around malloc() that fits the SharedMemory API).
using TestSharedMemory = InProcessSharedMemory;

}  // namespace perfetto

#endif  // SRC_TRACING_TEST_TEST_SHARED_MEMORY_H_
