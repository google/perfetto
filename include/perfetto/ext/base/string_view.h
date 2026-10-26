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

#ifndef INCLUDE_PERFETTO_EXT_BASE_STRING_VIEW_H_
#define INCLUDE_PERFETTO_EXT_BASE_STRING_VIEW_H_

#include <string.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/fnv_hash.h"

namespace perfetto {
namespace base {

// A string-like object that refers to a non-owned piece of memory.
// Strings are internally NOT null terminated.
class StringView {
 public:
  // Allow hashing with base::FnvHash.
  static constexpr bool kHashable = true;
  static constexpr size_t npos = static_cast<size_t>(-1);

  StringView() = default;
  StringView(const StringView&) = default;
  StringView& operator=(const StringView&) = default;
  StringView(const char* data, size_t size) : sv_(data, size) {
    PERFETTO_DCHECK(size == 0 || data != nullptr);
  }

  // Allow implicit conversion from any class that has a |data| and |size| field
  // and has the kConvertibleToStringView trait (e.g., protozero::ConstChars).
  template <typename T, typename = std::enable_if<T::kConvertibleToStringView>>
  StringView(const T& x) : StringView(x.data, x.size) {
    PERFETTO_DCHECK(x.size == 0 || x.data != nullptr);
  }

  // Creates a StringView from a null-terminated C string.
  // Deliberately not "explicit".
  StringView(const char* cstr) : sv_(cstr) { PERFETTO_DCHECK(cstr != nullptr); }

  StringView(std::string_view str) : sv_(str) {}

  // This instead has to be explicit, as creating a StringView out of a
  // std::string can be subtle.
  explicit StringView(const std::string& str) : sv_(str) {}

  bool empty() const { return sv_.empty(); }
  size_t size() const { return sv_.size(); }
  const char* data() const { return sv_.data(); }
  const char* begin() const { return sv_.data(); }
  const char* end() const { return sv_.data() + sv_.size(); }

  // Unlike std::string_view::at, out of range is only checked in debug builds.
  char at(size_t pos) const {
    PERFETTO_DCHECK(pos < size());
    return sv_[pos];
  }

  size_t find(char c, size_t start_pos = 0) const {
    return sv_.find(c, start_pos);
  }

  size_t find(const StringView& str, size_t start_pos = 0) const {
    return sv_.find(str.sv_, start_pos);
  }

  size_t find(const char* str, size_t start_pos = 0) const {
    return find(StringView(str), start_pos);
  }

  size_t rfind(char c) const { return sv_.rfind(c); }

  // Unlike std::string_view::substr, |pos| past the end returns an empty view
  // instead of throwing.
  StringView substr(size_t pos, size_t count = npos) const {
    if (pos >= size())
      return StringView("", 0);
    return StringView(sv_.substr(pos, count));
  }

  bool CaseInsensitiveEq(const StringView& other) const {
    if (size() != other.size())
      return false;
    if (size() == 0)
      return true;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    return _strnicmp(data(), other.data(), size()) == 0;
#else
    return strncasecmp(data(), other.data(), size()) == 0;
#endif
  }

  bool CaseInsensitiveOneOf(const std::vector<StringView>& others) const {
    for (const StringView& other : others) {
      if (CaseInsensitiveEq(other)) {
        return true;
      }
    }
    return false;
  }

  bool StartsWith(const StringView& other) const {
    if (other.size() == 0)
      return true;
    if (size() == 0)
      return false;
    if (other.size() > size())
      return false;
    return memcmp(data(), other.data(), other.size()) == 0;
  }

  bool EndsWith(const StringView& other) const {
    if (other.size() == 0)
      return true;
    if (size() == 0)
      return false;
    if (other.size() > size())
      return false;
    size_t off = size() - other.size();
    return memcmp(data() + off, other.data(), other.size()) == 0;
  }

  std::string ToStdString() const { return empty() ? "" : std::string(sv_); }

  uint64_t Hash() const {
    base::FnvHasher hasher;
    hasher.Update(data(), size());
    return hasher.digest();
  }

 private:
  std::string_view sv_;
};

inline bool operator==(const StringView& x, const StringView& y) {
  if (x.size() != y.size())
    return false;
  if (x.size() == 0)
    return true;
  return memcmp(x.data(), y.data(), x.size()) == 0;
}

inline bool operator!=(const StringView& x, const StringView& y) {
  return !(x == y);
}

inline bool operator<(const StringView& x, const StringView& y) {
  auto size = std::min(x.size(), y.size());
  if (size == 0)
    return x.size() < y.size();
  int result = memcmp(x.data(), y.data(), size);
  return result < 0 || (result == 0 && x.size() < y.size());
}

inline bool operator>=(const StringView& x, const StringView& y) {
  return !(x < y);
}

inline bool operator>(const StringView& x, const StringView& y) {
  return y < x;
}

inline bool operator<=(const StringView& x, const StringView& y) {
  return !(y < x);
}

}  // namespace base
}  // namespace perfetto

template <>
struct std::hash<::perfetto::base::StringView> {
  size_t operator()(const ::perfetto::base::StringView& sv) const {
    return static_cast<size_t>(sv.Hash());
  }
};

#endif  // INCLUDE_PERFETTO_EXT_BASE_STRING_VIEW_H_
