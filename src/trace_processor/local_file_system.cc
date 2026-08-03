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

#include "src/trace_processor/local_file_system.h"

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"

namespace perfetto::trace_processor::io {
namespace {

class LocalFile final : public File {
 public:
  LocalFile(std::string path, base::ScopedFile file)
      : path_(std::move(path)), file_(std::move(file)) {}

  base::Status Write(const void* data, size_t size) override {
    ssize_t written = base::WriteAll(file_.get(), data, size);
    if (written < 0) {
      return base::ErrStatus("Failed to write file %s: %s", path_.c_str(),
                             strerror(errno));
    }
    if (static_cast<size_t>(written) != size) {
      return base::ErrStatus("Short write to file %s: wrote %zd of %zu bytes",
                             path_.c_str(), written, size);
    }
    return base::OkStatus();
  }

 private:
  std::string path_;
  base::ScopedFile file_;
};

class LocalFileSystem final : public FileSystem {
 public:
  base::Status OpenFile(const std::string& path,
                        std::unique_ptr<File>* file) override {
    file->reset();
    base::ScopedFile scoped_file =
        base::OpenFile(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (!scoped_file) {
      return base::ErrStatus("Failed to open file %s: %s", path.c_str(),
                             strerror(errno));
    }
    file->reset(new LocalFile(path, std::move(scoped_file)));
    return base::OkStatus();
  }
};

}  // namespace

std::unique_ptr<FileSystem> CreateLocalFileSystem() {
  return std::make_unique<LocalFileSystem>();
}

}  // namespace perfetto::trace_processor::io
