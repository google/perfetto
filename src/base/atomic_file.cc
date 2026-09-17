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

#include "perfetto/ext/base/atomic_file.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "perfetto/base/build_config.h"
#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/uuid.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace perfetto::base {

AtomicFile::AtomicFile(const std::string& destination)
    : destination_(destination),
      temp_path_(destination + ".tmp." + Uuidv4().ToPrettyString()) {}

AtomicFile::~AtomicFile() {
  fd_.reset();
  if (owns_temp_file_)
    Unlink(temp_path_.c_str());
}

base::Status AtomicFile::Open() {
  PERFETTO_CHECK(!fd_ && !owns_temp_file_);
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  DWORD attributes = GetFileAttributesA(destination_.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
    return base::ErrStatus(
        "output must be a regular file, not a directory or link");
  HANDLE handle =
      CreateFileA(temp_path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return base::ErrStatus(
        "cannot create temporary output beside '%s' (Windows error %lu)",
        destination_.c_str(), GetLastError());
  owns_temp_file_ = true;
  fd_.reset(_open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY));
  if (!fd_)
    CloseHandle(handle);
#else
  struct stat output{};
  bool exists = lstat(destination_.c_str(), &output) == 0;
  PERFETTO_MSAN_UNPOISON(&output, sizeof(output));
  if (exists && !S_ISREG(output.st_mode))
    return base::ErrStatus(
        "output must be a regular file, not a directory or link: '%s'. Specify "
        "the target path directly.",
        destination_.c_str());
  fd_ = base::OpenFile(temp_path_, O_CREAT | O_EXCL | O_RDWR, 0644);
  owns_temp_file_ = static_cast<bool>(fd_);
  if (fd_ && exists && fchmod(*fd_, output.st_mode & 0777) != 0)
    return base::ErrStatus("cannot preserve output permissions: %s",
                           strerror(errno));
#endif
  if (!fd_)
    return base::ErrStatus(
        "cannot create temporary output beside '%s': %s. Check that the parent "
        "directory exists and is writable.",
        destination_.c_str(), strerror(errno));
  return base::OkStatus();
}

base::ScopedFile AtomicFile::DuplicateFD() const {
  PERFETTO_CHECK(fd_);
  return DupFile(*fd_);
}

base::Status AtomicFile::Commit() && {
  PERFETTO_CHECK(fd_);
  if (!base::FlushFile(*fd_))
    return base::ErrStatus("failed to flush output: %s", strerror(errno));
  fd_.reset();
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  if (!MoveFileExA(temp_path_.c_str(), destination_.c_str(),
                   MOVEFILE_REPLACE_EXISTING))
    return base::ErrStatus("failed to publish '%s' (Windows error %lu)",
                           destination_.c_str(), GetLastError());
#else
  if (rename(temp_path_.c_str(), destination_.c_str()) != 0)
    return base::ErrStatus("failed to publish '%s': %s", destination_.c_str(),
                           strerror(errno));
#endif
  owns_temp_file_ = false;
  return base::OkStatus();
}
}  // namespace perfetto::base
