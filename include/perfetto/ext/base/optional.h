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

#ifndef INCLUDE_PERFETTO_EXT_BASE_OPTIONAL_H_
#define INCLUDE_PERFETTO_EXT_BASE_OPTIONAL_H_

#include <functional>
#include <optional>

namespace perfetto {
namespace base {

template <typename T>
using Optional = std::optional<T>;

inline constexpr std::nullopt_t nullopt = std::nullopt;

template <class T>
constexpr std::optional<std::decay_t<T>> make_optional(T&& value) {
  return std::make_optional<T>(std::forward<T>(value));
}

template <class T, class... Args>
constexpr std::optional<T> make_optional(Args&&... args) {
  return std::make_optional<T>(std::forward<Args...>(args)...);
}

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_OPTIONAL_H_
