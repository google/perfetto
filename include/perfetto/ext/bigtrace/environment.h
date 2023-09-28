/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_BIGTRACE_ENVIRONMENT_H_
#define INCLUDE_PERFETTO_EXT_BIGTRACE_ENVIRONMENT_H_

#include <functional>
#include <memory>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/threading/stream.h"

namespace perfetto {
namespace bigtrace {

// Shim interface allowing embedders to change how operations which interact
// with the OS operate (e.g. IO, networking etc).
class Environment {
 public:
  virtual ~Environment();

  // Opens the file at |path| and reads the contents in chunks, returning the
  // the chunks as a Stream. The size of the chunks is implementation defined
  // but should be sized to balance memory use and syscall count.
  virtual base::StatusOrStream<std::vector<uint8_t>> ReadFile(
      const std::string& path) = 0;
};

}  // namespace bigtrace
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BIGTRACE_ENVIRONMENT_H_
