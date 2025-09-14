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

#include "src/trace_processor/util/args_utils.h"

#include <algorithm>
#include <cstdlib>
#include <optional>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_splitter.h"
#include "perfetto/ext/base/string_utils.h"

namespace perfetto::trace_processor {

ArgNode::ArgNode(Variadic value)
    : type_(Type::kPrimitive), primitive_value_(value) {}

// static
ArgNode ArgNode::Array() {
  ArgNode node(Variadic::Null());
  node.type_ = Type::kArray;
  node.array_ = std::make_unique<std::vector<ArgNode>>();
  node.primitive_value_ = Variadic::Null();
  return node;
}

// static
ArgNode ArgNode::Dict() {
  ArgNode node(Variadic::Null());
  node.type_ = Type::kDict;
  node.dict_ = std::make_unique<std::vector<std::pair<std::string, ArgNode>>>();
  node.primitive_value_ = Variadic::Null();
  return node;
}

Variadic ArgNode::GetPrimitiveValue() const {
  PERFETTO_CHECK(type_ == Type::kPrimitive);
  return primitive_value_;
}

const std::vector<ArgNode>& ArgNode::GetArray() const {
  PERFETTO_CHECK(type_ == Type::kArray);
  PERFETTO_CHECK(array_);
  return *array_;
}

const std::vector<std::pair<std::string, ArgNode>>& ArgNode::GetDict() const {
  PERFETTO_CHECK(type_ == Type::kDict);
  PERFETTO_CHECK(dict_);
  return *dict_;
}

ArgNode& ArgNode::AppendOrGet(size_t index) {
  PERFETTO_CHECK(type_ == Type::kArray);
  while (array_->size() <= index) {
    array_->push_back(ArgNode(Variadic::Null()));
  }
  return (*array_)[index];
}

ArgNode& ArgNode::AddOrGet(const std::string_view key) {
  PERFETTO_CHECK(type_ == Type::kDict);
  auto it =
      std::find_if(dict_->begin(), dict_->end(),
                   [&key](const auto& pair) { return pair.first == key; });
  if (it != dict_->end()) {
    return it->second;
  }
  dict_->emplace_back(key, ArgNode(Variadic::Null()));
  return dict_->back().second;
}

ArgSet::ArgSet() : root_(ArgNode::Dict()) {}

base::Status ArgSet::AppendArg(const std::string& key, Variadic value) {
  // Parse the key path (e.g., "foo.bar[0].baz")
  ArgNode* target = &root_;

  for (base::StringSplitter parts(key, '.'); parts.Next();) {
    std::string_view part{parts.cur_token(), parts.cur_token_size()};
    if (target->IsNull()) {
      *target = ArgNode::Dict();
    }
    if (target->GetType() != ArgNode::Type::kDict) {
      return base::ErrStatus(
          "Failed to insert key %s: tried to insert %s into a non-dictionary "
          "object",
          key.c_str(), std::string(part).c_str());
    }
    size_t bracket_pos = part.find('[');
    if (bracket_pos == std::string::npos) {
      // A single item.
      target = &target->AddOrGet(part);
    } else {
      target = &target->AddOrGet(part.substr(0, bracket_pos));
      while (bracket_pos != std::string::npos) {
        // We constructed this string from an int earlier in trace_processor
        // so it shouldn't be possible for this (or the StringToUInt32
        // below) to fail.
        std::string_view s = part.substr(
            bracket_pos + 1, part.find(']', bracket_pos) - bracket_pos - 1);
        std::optional<uint32_t> index = base::StringToUInt32(std::string(s));
        if (PERFETTO_UNLIKELY(!index)) {
          return base::ErrStatus(
              "Expected to be able to extract index from %s of key %s",
              std::string(part).c_str(), key.c_str());
        }
        if (target->IsNull()) {
          *target = ArgNode::Array();
        }
        if (target->GetType() != ArgNode::Type::kArray) {
          return base::ErrStatus(
              "Failed to insert key %s: tried to insert %s into a non-array"
              "object",
              key.c_str(), std::string(part).c_str());
        }
        target = &target->AppendOrGet(index.value());
        bracket_pos = part.find('[', bracket_pos + 1);
      }
    }
  }
  *target = ArgNode(value);
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor