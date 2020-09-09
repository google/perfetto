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

#include "perfetto/profiling/deobfuscator.h"
#include "perfetto/ext/base/string_splitter.h"

#include "perfetto/ext/base/optional.h"

namespace perfetto {
namespace profiling {
namespace {

struct ProguardClass {
  std::string obfuscated_name;
  std::string deobfuscated_name;
};

base::Optional<ProguardClass> ParseClass(std::string line) {
  base::StringSplitter ss(std::move(line), ' ');

  if (!ss.Next()) {
    PERFETTO_ELOG("Missing deobfuscated name.");
    return base::nullopt;
  }
  std::string deobfuscated_name(ss.cur_token(), ss.cur_token_size());

  if (!ss.Next() || ss.cur_token_size() != 2 ||
      strncmp("->", ss.cur_token(), 2) != 0) {
    PERFETTO_ELOG("Missing ->");
    return base::nullopt;
  }

  if (!ss.Next()) {
    PERFETTO_ELOG("Missing obfuscated name.");
    return base::nullopt;
  }
  std::string obfuscated_name(ss.cur_token(), ss.cur_token_size());
  if (obfuscated_name.empty()) {
    PERFETTO_ELOG("Empty obfuscated name.");
    return base::nullopt;
  }
  if (obfuscated_name.back() != ':') {
    PERFETTO_ELOG("Expected colon.");
    return base::nullopt;
  }

  obfuscated_name.resize(obfuscated_name.size() - 1);
  if (ss.Next()) {
    PERFETTO_ELOG("Unexpected data.");
    return base::nullopt;
  }
  return ProguardClass{std::move(obfuscated_name),
                       std::move(deobfuscated_name)};
}

enum class ProguardMemberType {
  kField,
  kMethod,
};

struct ProguardMember {
  ProguardMemberType type;
  std::string obfuscated_name;
  std::string deobfuscated_name;
};

base::Optional<ProguardMember> ParseMember(std::string line) {
  base::StringSplitter ss(std::move(line), ' ');

  if (!ss.Next()) {
    PERFETTO_ELOG("Missing type name.");
    return base::nullopt;
  }
  std::string type_name(ss.cur_token(), ss.cur_token_size());

  if (!ss.Next()) {
    PERFETTO_ELOG("Missing deobfuscated name.");
    return base::nullopt;
  }
  std::string deobfuscated_name(ss.cur_token(), ss.cur_token_size());

  if (!ss.Next() || ss.cur_token_size() != 2 ||
      strncmp("->", ss.cur_token(), 2) != 0) {
    PERFETTO_ELOG("Missing ->");
    return base::nullopt;
  }

  if (!ss.Next()) {
    PERFETTO_ELOG("Missing obfuscated name.");
    return base::nullopt;
  }
  std::string obfuscated_name(ss.cur_token(), ss.cur_token_size());

  if (ss.Next()) {
    PERFETTO_ELOG("Unexpected data.");
    return base::nullopt;
  }

  ProguardMemberType member_type;
  auto paren_idx = deobfuscated_name.find("(");
  if (paren_idx != std::string::npos) {
    member_type = ProguardMemberType::kMethod;
    deobfuscated_name = deobfuscated_name.substr(0, paren_idx);
    auto colon_idx = type_name.find(":");
    if (colon_idx != std::string::npos) {
      type_name = type_name.substr(colon_idx + 1);
    }
  } else {
    member_type = ProguardMemberType::kField;
  }
  return ProguardMember{member_type, std::move(obfuscated_name),
                        std::move(deobfuscated_name)};
}

}  // namespace

// See https://www.guardsquare.com/en/products/proguard/manual/retrace for the
// file format we are parsing.
bool ProguardParser::AddLine(std::string line) {
  if (line.length() == 0)
    return true;
  bool is_member = line[0] == ' ';
  if (is_member && !current_class_) {
    PERFETTO_ELOG("Failed to parse proguard map. Saw member before class.");
    return false;
  }
  if (!is_member) {
    auto opt_cls = ParseClass(std::move(line));
    if (!opt_cls)
      return false;
    auto p = mapping_.emplace(std::move(opt_cls->obfuscated_name),
                              std::move(opt_cls->deobfuscated_name));
    if (!p.second) {
      PERFETTO_ELOG("Duplicate class.");
      return false;
    }
    current_class_ = &p.first->second;
  } else {
    auto opt_member = ParseMember(std::move(line));
    if (!opt_member)
      return false;
    switch (opt_member->type) {
      case (ProguardMemberType::kField): {
        auto p = current_class_->deobfuscated_fields.emplace(
            opt_member->obfuscated_name, opt_member->deobfuscated_name);
        if (!p.second && p.first->second != opt_member->deobfuscated_name) {
          PERFETTO_ELOG("Member redefinition: %s.%s. Proguard map invalid",
                        current_class_->deobfuscated_name.c_str(),
                        opt_member->deobfuscated_name.c_str());
          return false;
        }
        break;
      }
      case (ProguardMemberType::kMethod): {
        auto p = current_class_->deobfuscated_methods.emplace(
            opt_member->obfuscated_name, opt_member->deobfuscated_name);
        if (!p.second && p.first->second != opt_member->deobfuscated_name) {
          // TODO(fmayer): Add docs that explain method redefinition.
          PERFETTO_ELOG(
              "Member redefinition: %s.%s. Some methods will not get "
              "deobfuscated. Change your obfuscator settings to fix.",
              current_class_->deobfuscated_name.c_str(),
              opt_member->deobfuscated_name.c_str());
          return true;
        }
        break;
      }
    }
  }
  return true;
}

}  // namespace profiling
}  // namespace perfetto
