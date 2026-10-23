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

#include "src/trace_processor/core/util/file_backed_column_vector.h"

#include <cstdint>
#include <memory>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_macros.h"

namespace perfetto::trace_processor::core {
namespace {

class FileColumnBacking final : public ColumnVectorBacking {
 public:
  FileColumnBacking(uint64_t element_size,
                    std::unique_ptr<io::File> file,
                    std::unique_ptr<io::Mapping> mapping)
      : element_size_(element_size),
        file_(std::move(file)),
        mapping_(std::move(mapping)) {}

  ColumnVectorBackingState state() const override {
    return {mapping_->data(), mapping_->size() / element_size_};
  }

  ColumnVectorBackingState Resize(uint64_t, uint64_t capacity) override {
    uint64_t size = capacity * element_size_;
    // Windows cannot change a file's size while a view of it is mapped.
    // data() is allowed to change after this cold growth path.
    mapping_.reset();
    base::Status status = file_->SetSize(size);
    PERFETTO_CHECK(status.ok());
    status = file_->CreateMapping(0, size, io::MappingAccess::kReadWrite,
                                  &mapping_);
    PERFETTO_CHECK(status.ok());
    return state();
  }

  ColumnVectorBackingState ShrinkToFit(uint64_t size) override {
    return Resize(size, size);
  }

 private:
  uint64_t element_size_;
  std::unique_ptr<io::File> file_;
  std::unique_ptr<io::Mapping> mapping_;
};

}  // namespace

base::StatusOr<std::unique_ptr<ColumnVectorBacking>>
CreateFileColumnBacking(io::FileSystem* file_system,
                        uint64_t element_size,
                        uint64_t capacity) {
  std::unique_ptr<io::File> file;
  RETURN_IF_ERROR(file_system->CreateTemporaryFile(&file));
  uint64_t size = capacity * element_size;
  RETURN_IF_ERROR(file->SetSize(size));
  std::unique_ptr<io::Mapping> mapping;
  RETURN_IF_ERROR(file->CreateMapping(0, size, io::MappingAccess::kReadWrite,
                                      &mapping));
  return std::unique_ptr<ColumnVectorBacking>(new FileColumnBacking(
      element_size, std::move(file), std::move(mapping)));
}

}  // namespace perfetto::trace_processor::core
