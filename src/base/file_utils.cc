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

#include "perfetto/ext/base/file_utils.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/utils.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <Windows.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

namespace perfetto {
namespace base {
namespace {
constexpr size_t kBufSize = 2048;
}

ssize_t Read(int fd, void* dst, size_t dst_size) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return _read(fd, dst, static_cast<unsigned>(dst_size));
#else
  return PERFETTO_EINTR(read(fd, dst, dst_size));
#endif
}

bool ReadFileDescriptor(int fd, std::string* out) {
  // Do not override existing data in string.
  size_t i = out->size();

  struct stat buf {};
  if (fstat(fd, &buf) != -1) {
    if (buf.st_size > 0)
      out->resize(i + static_cast<size_t>(buf.st_size));
  }

  ssize_t bytes_read;
  for (;;) {
    if (out->size() < i + kBufSize)
      out->resize(out->size() + kBufSize);

    bytes_read = Read(fd, &((*out)[i]), kBufSize);
    if (bytes_read > 0) {
      i += static_cast<size_t>(bytes_read);
    } else {
      out->resize(i);
      return bytes_read == 0;
    }
  }
}

bool ReadFileStream(FILE* f, std::string* out) {
  return ReadFileDescriptor(fileno(f), out);
}

bool ReadFile(const std::string& path, std::string* out) {
  base::ScopedFile fd = base::OpenFile(path, O_RDONLY);
  if (!fd)
    return false;

  return ReadFileDescriptor(*fd, out);
}

ssize_t WriteAll(int fd, const void* buf, size_t count) {
  size_t written = 0;
  while (written < count) {
    // write() on windows takes an unsigned int size.
    uint32_t bytes_left = static_cast<uint32_t>(
        std::min(count - written, static_cast<size_t>(UINT32_MAX)));
    ssize_t wr = PERFETTO_EINTR(
        write(fd, static_cast<const char*>(buf) + written, bytes_left));
    if (wr == 0)
      break;
    if (wr < 0)
      return wr;
    written += static_cast<size_t>(wr);
  }
  return static_cast<ssize_t>(written);
}

bool FlushFile(int fd) {
  PERFETTO_DCHECK(fd != 0);
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  return !PERFETTO_EINTR(fdatasync(fd));
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return !PERFETTO_EINTR(_commit(fd));
#else
  return !PERFETTO_EINTR(fsync(fd));
#endif
}

bool Mkdir(const std::string& path) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return _mkdir(path.c_str()) == 0;
#else
  return mkdir(path.c_str(), 0755) == 0;
#endif
}

bool Rmdir(const std::string& path) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return _rmdir(path.c_str()) == 0;
#else
  return rmdir(path.c_str()) == 0;
#endif
}

int CloseFile(int fd) {
  return close(fd);
}

ScopedFile OpenFile(const std::string& path, int flags, FileOpenMode mode) {
  PERFETTO_DCHECK((flags & O_CREAT) == 0 || mode != kFileModeInvalid);
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // Always use O_BINARY on Windows, to avoid silly EOL translations.
  ScopedFile fd(_open(path.c_str(), flags | O_BINARY, mode));
#else
  // Always open a ScopedFile with O_CLOEXEC so we can safely fork and exec.
  ScopedFile fd(open(path.c_str(), flags | O_CLOEXEC, mode));
#endif
  return fd;
}

bool FileExists(const std::string& path) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return _access(path.c_str(), 0) == 0;
#else
  return access(path.c_str(), F_OK) == 0;
#endif
}

}  // namespace base
}  // namespace perfetto
