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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <Windows.h>
#include <direct.h>
#include <fileapi.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/string_utils.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
namespace {
std::string GetTempName() {
  char name[] = "perfetto-XXXXXX";
  PERFETTO_CHECK(_mktemp_s(name, sizeof(name)) == 0);
  return name;
}
}  // namespace
#endif

namespace perfetto {
namespace base {

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
TempFile TempFile::Create() {
  TempFile temp_file;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  temp_file.path_ = GetSysTempDir() + "\\" + GetTempName();
  temp_file.fd_.reset(
      _open(temp_file.path_.c_str(),
            O_CREAT | _O_TEMPORARY | _O_BINARY | _O_RDWR | _O_TRUNC,
            _S_IREAD | _S_IWRITE));
#else
  temp_file.path_ = GetSysTempDir() + "/perfetto-XXXXXXXX";
  temp_file.fd_.reset(mkstemp(&temp_file.path_[0]));
#endif
  if (PERFETTO_UNLIKELY(!temp_file.fd_)) {
    PERFETTO_FATAL("Could not create temp file %s", temp_file.path_.c_str());
  }
  return temp_file;
}

// static
TempFile TempFile::CreateUnlinked() {
  TempFile temp_file = TempFile::Create();
  temp_file.Unlink();
  return temp_file;
}

TempFile::TempFile() = default;

TempFile::~TempFile() {
  Unlink();
}

ScopedFile TempFile::ReleaseFD() {
  Unlink();
  return std::move(fd_);
}

void TempFile::Unlink() {
  if (path_.empty())
    return;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // If the FD is still open DeleteFile will mark the file as pending deletion
  // and delete it only when the process exists.
  PERFETTO_CHECK(DeleteFileA(path_.c_str()));
#else
  PERFETTO_CHECK(unlink(path_.c_str()) == 0);
#endif
  path_.clear();
}

TempFile::TempFile(TempFile&&) noexcept = default;
TempFile& TempFile::operator=(TempFile&&) = default;

// static
TempDir TempDir::Create() {
  TempDir temp_dir;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  temp_dir.path_ = GetSysTempDir() + "\\" + GetTempName();
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
