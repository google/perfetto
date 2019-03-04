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

#ifndef INCLUDE_PERFETTO_BASE_STRING_VIEW_H_
#define INCLUDE_PERFETTO_BASE_STRING_VIEW_H_

#include <string.h>

#include <string>

#include "perfetto/base/hash.h"
#include "perfetto/base/logging.h"

namespace perfetto {
namespace base {

// A string-like object that refers to a non-owned piece of memory.
// Strings are internally NOT null terminated.
class StringView {
 public:
  static constexpr size_t npos = static_cast<size_t>(-1);

  StringView() : data_(""), size_(0) {}
  StringView(const StringView&) = default;
  StringView& operator=(const StringView&) = default;
  StringView(const char* data, size_t size) : data_(data), size_(size) {}

  // Creates a StringView from a null-terminated C string.
  // Deliberately not "explicit".
  StringView(const char* cstr) : data_(cstr), size_(strlen(cstr)) {}

  // This instead has to be explicit, as creating a StringView out of a
  // std::string can be subtle.
  explicit StringView(const std::string& str)
      : data_(str.data()), size_(str.size()) {}

  bool empty() const { return size_ == 0; }
  size_t size() const { return size_; }
  const char* data() const { return data_; }

  char at(size_t pos) const {
    PERFETTO_DCHECK(pos < size_);
    return data_[pos];
  }

  size_t find(char c) const {
    for (size_t i = 0; i < size_; ++i) {
      if (data_[i] == c)
        return i;
    }
    return npos;
  }

  size_t rfind(char c) const {
    for (size_t i = size_; i > 0; --i) {
      if (data_[i - 1] == c)
        return i - 1;
    }
    return npos;
  }

  StringView substr(size_t pos, size_t count = npos) const {
    if (pos >= size_)
      return StringView();
    size_t rcount = std::min(count, size_ - pos);
    return StringView(data_ + pos, rcount);
  }

  std::string ToStdString() const { return std::string(data_, size_); }

  uint64_t Hash() const {
    base::Hash hasher;
    hasher.Update(data_, size_);
    return hasher.digest();
  }

 private:
  const char* data_ = nullptr;
  size_t size_ = 0;
};

inline bool operator==(const StringView& x, const StringView& y) {
  if (x.size() != y.size())
    return false;
  return memcmp(x.data(), y.data(), x.size()) == 0;
}

inline bool operator!=(const StringView& x, const StringView& y) {
  return !(x == y);
}

}  // namespace base
}  // namespace perfetto

namespace std {

template <>
struct hash<::perfetto::base::StringView> {
  size_t operator()(const ::perfetto::base::StringView& sv) const {
    return static_cast<size_t>(sv.Hash());
  }
};

}  // namespace std

#endif  // INCLUDE_PERFETTO_BASE_STRING_VIEW_H_
