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

#ifndef INCLUDE_PERFETTO_BASE_COMPILER_H_
#define INCLUDE_PERFETTO_BASE_COMPILER_H_

#include <type_traits>

#define PERFETTO_LIKELY(_x) __builtin_expect(!!(_x), 1)
#define PERFETTO_UNLIKELY(_x) __builtin_expect(!!(_x), 0)

#if defined(__GNUC__) || defined(__clang__)
#define PERFETTO_WARN_UNUSED_RESULT __attribute__((warn_unused_result))
#else
#define PERFETTO_WARN_UNUSED_RESULT
#endif

#if defined(__clang__)
#define PERFETTO_ALWAYS_INLINE __attribute__((__always_inline__))
#else
// GCC is too pedantic and often fails with the error:
// "always_inline function might not be inlinable"
#define PERFETTO_ALWAYS_INLINE
#endif

// TODO(lalitm): is_trivially_constructible is currently not available
// in some environments we build in. Reenable when that environment supports
// this.
#if defined(__GLIBCXX__)
#define PERFETTO_IS_TRIVIALLY_CONSTRUCTIBLE(T) true
#else
#define PERFETTO_IS_TRIVIALLY_CONSTRUCTIBLE(T) \
  std::is_trivially_constructible<T>::value
#endif

// TODO(lalitm): is_trivially_copyable is currently not available
// in some environments we build in. Reenable when that environment supports
// this.
#if defined(__GLIBCXX__)
#define PERFETTO_IS_TRIVIALLY_COPYABLE(T) true
#else
#define PERFETTO_IS_TRIVIALLY_COPYABLE(T) std::is_trivially_copyable<T>::value
#endif

namespace perfetto {
namespace base {

template <typename... T>
inline void ignore_result(const T&...) {}

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_COMPILER_H_
