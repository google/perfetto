/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_BASE_TEMP_FILE_H_
#define INCLUDE_PERFETTO_EXT_BASE_TEMP_FILE_H_

#include <string>

#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/status_or.h"

namespace perfetto {
namespace base {

std::string GetSysTempDir();

class TempFile {
 public:
  enum class LinkMode { kLinked, kUnlinked };

  // Creates a temporary file.
  // If |mode| is kUnlinked, unlinks the file before returning, keeping fd()
  // open and leaving path() empty.
  // Returns an error status if creation or unlinking fails.
  static StatusOr<TempFile> Create(LinkMode mode = LinkMode::kLinked);

  TempFile(TempFile&&) noexcept;
  TempFile& operator=(TempFile&&);
  ~TempFile();

  const std::string& path() const { return path_; }
  int fd() const { return *fd_; }
  int operator*() const { return *fd_; }

  // Unlinks the file and clears path(), keeping fd() open.
  // Returns true on success, or if the file is already unlinked.
  // On failure, returns false and leaves path() unchanged.
  bool Unlink();

  // Unlinks the file if needed and transfers ownership of its descriptor to
  // the caller.
  // Aborts if unlinking fails.
  ScopedFile ReleaseFD();

 private:
  TempFile(ScopedFile fd, std::string path);
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  ScopedFile fd_;
  std::string path_;
};

class TempDir {
 public:
  static TempDir Create();

  TempDir(TempDir&&) noexcept;
  TempDir& operator=(TempDir&&);
  ~TempDir();

  const std::string& path() const { return path_; }

 private:
  TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string path_;
};

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_TEMP_FILE_H_
