/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_EXT_BASE_FLAGS_LIST_H_
#define INCLUDE_PERFETTO_EXT_BASE_FLAGS_LIST_H_

namespace perfetto::base::flags {

enum class NonAndroidPlatformDefault {
  kFalse = 0,
  kTrue = 1,
};

}  // namespace perfetto::base::flags

// The list of all the read-only flags accessible to the Perfetto codebase.
//
// The first argument is the name of the flag. Should match 1:1 with the name
// in `perfetto_flags.aconfig`.
// The second argument is the default value of the flag in non-Android platform
// contexts.
#define PERFETTO_READ_ONLY_FLAGS(X)                         \
  X(test_read_only_flag, NonAndroidPlatformDefault::kFalse) \
  X(use_murmur_hash_for_flat_hash_map, NonAndroidPlatformDefault::kFalse)

#endif  // INCLUDE_PERFETTO_EXT_BASE_FLAGS_LIST_H_
