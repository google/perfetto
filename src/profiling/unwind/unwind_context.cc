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

#include "src/profiling/unwind/unwind_context.h"

namespace perfetto {
namespace profiling {
namespace {

class SimpleUnwoundMapping : public UnwoundMapping {
 public:
  SimpleUnwoundMapping(std::string name,
                       uint64_t start,
                       uint64_t end,
                       uint64_t offset,
                       uint64_t start_offset)
      : name_(std::move(name)),
        start_(start),
        end_(end),
        offset_(offset),
        start_offset_(start_offset) {}

  std::string map_name() const override { return name_; }
  uint64_t map_start() const override { return start_; }
  uint64_t map_end() const override { return end_; }
  uint64_t map_offset() const override { return offset_; }
  uint64_t map_start_offset() const override { return start_offset_; }
  uint64_t map_load_bias() const override { return 0; }
  std::string build_id() const override { return ""; }

 private:
  std::string name_;
  uint64_t start_;
  uint64_t end_;
  uint64_t offset_;
  uint64_t start_offset_;
};

}  // namespace

// static
std::shared_ptr<UnwoundMapping> UnwoundMapping::Create(std::string name,
                                                       uint64_t start,
                                                       uint64_t end,
                                                       uint64_t offset,
                                                       uint64_t start_offset) {
  return std::make_shared<SimpleUnwoundMapping>(std::move(name), start, end,
                                                offset, start_offset);
}

UnwoundMapping::~UnwoundMapping() = default;
UnwindContext::~UnwindContext() = default;

}  // namespace profiling
}  // namespace perfetto
