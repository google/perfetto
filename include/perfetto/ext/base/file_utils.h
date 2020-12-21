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

#ifndef INCLUDE_PERFETTO_EXT_BASE_FILE_UTILS_H_
#define INCLUDE_PERFETTO_EXT_BASE_FILE_UTILS_H_

#include <fcntl.h>  // For mode_t & O_RDONLY/RDWR. Exists also on Windows.
#include <stddef.h>

#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/base/export.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/utils.h"

namespace perfetto {
namespace base {

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
using FileOpenMode = int;
#else
using FileOpenMode = mode_t;
#endif

constexpr FileOpenMode kFileModeInvalid = static_cast<FileOpenMode>(-1);

bool ReadPlatformHandle(PlatformHandle, std::string* out);
bool ReadFileDescriptor(int fd, std::string* out);
bool ReadFileStream(FILE* f, std::string* out);
bool ReadFile(const std::string& path, std::string* out);

// A wrapper around read(2). It deals with Linux vs Windows includes. It also
// deals with handling EINTR. Has the same semantics of UNIX's read(2).
ssize_t Read(int fd, void* dst, size_t dst_size);

// Call write until all data is written or an error is detected.
//
// man 2 write:
//   If a write() is interrupted by a signal handler before any bytes are
//   written, then the call fails with the error EINTR; if it is
//   interrupted after at least one byte has been written, the call
//   succeeds, and returns the number of bytes written.
ssize_t WriteAll(int fd, const void* buf, size_t count);

ssize_t WriteAllHandle(PlatformHandle, const void* buf, size_t count);

ScopedFile OpenFile(const std::string& path,
                    int flags,
                    FileOpenMode = kFileModeInvalid);

// This is an alias for close(). It's to avoid leaking Windows.h in headers.
// Exported because ScopedFile is used in the /include/ext API by Chromium
// component builds.
int PERFETTO_EXPORT CloseFile(int fd);

bool FlushFile(int fd);

// Returns true if mkdir succeeds, false if it fails (see errno in that case).
bool Mkdir(const std::string& path);

// Calls rmdir() on UNIX, _rmdir() on Windows.
bool Rmdir(const std::string& path);

// Wrapper around access(path, F_OK).
bool FileExists(const std::string& path);

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_FILE_UTILS_H_
