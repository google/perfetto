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

#ifndef INCLUDE_PERFETTO_BASE_SOCK_UTILS_H_
#define INCLUDE_PERFETTO_BASE_SOCK_UTILS_H_

#include "perfetto/base/scoped_file.h"

namespace perfetto {
namespace base {

ssize_t Send(int fd,
             const void* msg,
             size_t len,
             const int* send_fds,
             size_t num_fds);

ssize_t Receive(int fd,
                void* msg,
                size_t len,
                base::ScopedFile* fd_vec,
                size_t max_files);
}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_SOCK_UTILS_H_
