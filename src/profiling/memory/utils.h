/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_PROFILING_MEMORY_UTILS_H_
#define SRC_PROFILING_MEMORY_UTILS_H_

#include <unistd.h>

namespace perfetto {
namespace profiling {

ssize_t ReadAtOffsetClobberSeekPos(int fd,
                                   void* buf,
                                   size_t count,
                                   off64_t addr);
}
}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_UTILS_H_
