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

#ifndef INCLUDE_PERFETTO_EXT_BASE_ATOMIC_FILE_H_
#define INCLUDE_PERFETTO_EXT_BASE_ATOMIC_FILE_H_

#include <string>
#include "perfetto/base/status.h"
#include "perfetto/ext/base/scoped_file.h"

namespace perfetto::base {

// Writes a sibling temporary file and atomically replaces the destination on
// Commit(). Until then an existing destination is untouched. Does not follow
// destination symlinks. An uncommitted temporary file is deleted on
// destruction.
class AtomicFile {
 public:
  explicit AtomicFile(const std::string& destination);
  ~AtomicFile();
  AtomicFile(const AtomicFile&) = delete;
  AtomicFile& operator=(const AtomicFile&) = delete;

  Status Open();
  ScopedFile DuplicateFD() const;
  // Final operation: close all DuplicateFD() handles before committing.
  // On failure, destruction still attempts to remove the temporary file.
  Status Commit() &&;
  const std::string& temp_path() const { return temp_path_; }

 private:
  const std::string destination_;
  const std::string temp_path_;
  ScopedFile fd_;
  bool owns_temp_file_ = false;
};

}  // namespace perfetto::base
#endif  // INCLUDE_PERFETTO_EXT_BASE_ATOMIC_FILE_H_
