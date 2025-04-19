/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_PROTOVM_UNIQUE_PTR_H_
#define SRC_PROTOVM_UNIQUE_PTR_H_

#include <memory>

#include "perfetto/base/logging.h"

namespace perfetto {
namespace protovm {

// Custom deleter for std::unique_ptr to ensure the resource is released and its
// memory deallocated upon destruction.
struct Deleter {
  void operator()(void* p) const { PERFETTO_DCHECK(p == nullptr); }
};

template <class T>
using UniquePtr = std::unique_ptr<T, Deleter>;

}  // namespace protovm
}  // namespace perfetto

#endif  // SRC_PROTOVM_UNIQUE_PTR_H_
