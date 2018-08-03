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

#include "src/profiling/memory/string_interner.h"

namespace perfetto {

StringInterner::Entry::Entry(std::string s, StringInterner* in)
    : string(s), interner(in) {}

bool StringInterner::Entry::operator<(const Entry& other) const {
  return string < other.string;
}

StringInterner::InternedString::InternedString(Entry* entry) : entry_(entry) {}

const std::string& StringInterner::InternedString::str() const {
  return entry_->string;
}

StringInterner::InternedString::~InternedString() {
  if (entry_ != nullptr)
    entry_->interner->Return(entry_);
}

StringInterner::InternedString StringInterner::Intern(std::string str) {
  auto itr = entries_.emplace(std::move(str), this);
  Entry& entry = const_cast<Entry&>(*itr.first);
  entry.ref_count++;
  return InternedString(&entry);
}

StringInterner::InternedString::InternedString(const InternedString& other)
    : entry_(other.entry_) {
  if (entry_ != nullptr)
    entry_->ref_count++;
}

StringInterner::InternedString::InternedString(InternedString&& other)
    : entry_(other.entry_) {
  other.entry_ = nullptr;
}

StringInterner::InternedString& StringInterner::InternedString::operator=(
    InternedString other) {
  std::swap(*this, other);
  return *this;
}

size_t StringInterner::entry_count_for_testing() {
  return entries_.size();
}

void StringInterner::Return(Entry* entry) {
  if (--entry->ref_count == 0)
    entries_.erase(*entry);
}

}  // namespace perfetto
