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

#include "perfetto/ext/base/temp_file.h"

#include "perfetto/base/build_config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <utility>

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <windows.h>

#include <direct.h>
#include <fileapi.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"

namespace perfetto {
namespace base {

namespace {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
StatusOr<std::string> GetTempFilePathWin() {
  std::string path = GetSysTempDir() + "\\perfetto-XXXXXX";
  int error = _mktemp_s(path.data(), path.size() + 1);
  if (error != 0)
    return ErrStatus("_mktemp_s() failed: %s", strerror(error));
  return path;
}
#endif

Status UnlinkTempFile(const std::string& path) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // If the file is still open, deletion completes when its last handle closes.
  if (!DeleteFileA(path.c_str())) {
    return ErrStatus("DeleteFileA(%s) failed: %lu", path.c_str(),
                     GetLastError());
  }
#else
  if (unlink(path.c_str()) != 0)
    return ErrStatus("unlink(%s) failed: %s", path.c_str(), strerror(errno));
#endif
  return OkStatus();
}

}  // namespace

std::string GetSysTempDir() {
  const char* tmpdir = nullptr;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  if ((tmpdir = getenv("TMP")))
    return tmpdir;
  if ((tmpdir = getenv("TEMP")))
    return tmpdir;
  return "C:\\TEMP";
#else
  if ((tmpdir = getenv("TMPDIR")))
    return base::StripSuffix(tmpdir, "/");
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  return "/data/local/tmp";
#else
  return "/tmp";
#endif  // !OS_ANDROID
#endif  // !OS_WIN
}

// static
StatusOr<TempFile> TempFile::Create(LinkMode mode) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  ASSIGN_OR_RETURN(std::string path, GetTempFilePathWin());

  // TempFile exposes both a descriptor and a path. Match POSIX by allowing the
  // path to be reopened while the descriptor remains open. These sharing flags
  // are not settable through _open(), hence the CreateFileA +
  // _open_osfhandle dance.
  ScopedPlatformHandle h(
      ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr));
  if (!h) {
    return ErrStatus("CreateFileA(%s) failed: %lu", path.c_str(),
                     GetLastError());
  }

  // According to MSDN, when using _open_osfhandle the caller must not call
  // CloseHandle(). Ownership is moved to the file descriptor, which then needs
  // to be closed with just with _close().
  ScopedFile fd(_open_osfhandle(reinterpret_cast<intptr_t>(h.get()), 0));
  if (!fd) {
    auto status = ErrStatus("_open_osfhandle() failed: %s", strerror(errno));
    UnlinkTempFile(path);  // Best effort.
    return status;
  }
  h.release();
#else
  std::string path = GetSysTempDir() + "/perfetto-XXXXXXXX";
  ScopedFile fd(mkstemp(path.data()));
  if (!fd)
    return ErrStatus("mkstemp(%s) failed: %s", path.c_str(), strerror(errno));
#endif

  // Transfer ownership to TempFile only once all fallible operations succeed,
  // because ~TempFile() aborts if unlinking fails.
  if (mode == LinkMode::kUnlinked) {
    RETURN_IF_ERROR(UnlinkTempFile(path));
    return TempFile(std::move(fd), std::string());
  }
  return TempFile(std::move(fd), std::move(path));
}

TempFile::TempFile(ScopedFile fd, std::string path)
    : fd_(std::move(fd)), path_(std::move(path)) {}

TempFile::~TempFile() {
  PERFETTO_CHECK(Unlink());
}

ScopedFile TempFile::ReleaseFD() {
  PERFETTO_CHECK(Unlink());
  return std::move(fd_);
}

bool TempFile::Unlink() {
  if (path_.empty())
    return true;
  if (!UnlinkTempFile(path_).ok())
    return false;
  path_.clear();
  return true;
}

TempFile::TempFile(TempFile&&) noexcept = default;
TempFile& TempFile::operator=(TempFile&&) = default;

// static
TempDir TempDir::Create() {
  TempDir temp_dir;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  temp_dir.path_ = GetTempFilePathWin().ValueOrDie();
  PERFETTO_CHECK(_mkdir(temp_dir.path_.c_str()) == 0);
#else
  temp_dir.path_ = GetSysTempDir() + "/perfetto-XXXXXXXX";
  PERFETTO_CHECK(mkdtemp(&temp_dir.path_[0]));
#endif
  return temp_dir;
}

TempDir::TempDir() = default;
TempDir::TempDir(TempDir&&) noexcept = default;
TempDir& TempDir::operator=(TempDir&&) = default;

TempDir::~TempDir() {
  if (path_.empty())
    return;  // For objects that get std::move()d.
  PERFETTO_CHECK(Rmdir(path_));
}

}  // namespace base
}  // namespace perfetto
