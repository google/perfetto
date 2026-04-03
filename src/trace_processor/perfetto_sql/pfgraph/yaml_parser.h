/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PFGRAPH_YAML_PARSER_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PFGRAPH_YAML_PARSER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "perfetto/ext/base/status_or.h"

namespace perfetto::trace_processor::pfgraph {

// A simple YAML DOM node. Supports the subset of YAML used by PfGraph:
// - Scalars (strings, numbers, booleans, null)
// - Sequences (ordered lists)
// - Mappings (ordered key-value pairs)
//
// Does NOT support: anchors, tags, multi-document, complex keys, merge keys.
class YamlNode {
 public:
  enum Type { kNull, kScalar, kSequence, kMapping };

  YamlNode() : type_(kNull) {}
  explicit YamlNode(std::string scalar) : type_(kScalar), scalar_(std::move(scalar)) {}

  static YamlNode Scalar(std::string s) { return YamlNode(std::move(s)); }
  static YamlNode Sequence() { YamlNode n; n.type_ = kSequence; return n; }
  static YamlNode Mapping() { YamlNode n; n.type_ = kMapping; return n; }

  Type type() const { return type_; }
  bool is_null() const { return type_ == kNull; }
  bool is_scalar() const { return type_ == kScalar; }
  bool is_sequence() const { return type_ == kSequence; }
  bool is_mapping() const { return type_ == kMapping; }

  // Scalar access.
  const std::string& scalar() const { return scalar_; }
  int64_t as_int() const;
  double as_double() const;
  bool as_bool() const;

  // Sequence access.
  const std::vector<YamlNode>& sequence() const { return sequence_; }
  std::vector<YamlNode>& sequence() { return sequence_; }
  void push_back(YamlNode node) { sequence_.push_back(std::move(node)); }
  size_t size() const;

  // Mapping access.
  // Using vector of pairs to preserve insertion order.
  using MapEntry = std::pair<std::string, YamlNode>;
  const std::vector<MapEntry>& mapping() const { return mapping_; }
  std::vector<MapEntry>& mapping() { return mapping_; }
  void insert(std::string key, YamlNode value);
  const YamlNode* find(const std::string& key) const;
  const YamlNode& operator[](const std::string& key) const;

  // Returns a static null node for missing keys.
  static const YamlNode& null_node();

 private:
  Type type_ = kNull;
  std::string scalar_;
  std::vector<YamlNode> sequence_;
  std::vector<MapEntry> mapping_;
};

// Parses a YAML string into a YamlNode DOM tree.
// Supports only the subset used by PfGraph:
// - Block mappings (key: value)
// - Block sequences (- item)
// - Flow sequences ([a, b, c])
// - Flow mappings ({a: b, c: d})
// - Quoted strings ("..." and '...')
// - Block scalars (| for literal)
// - Comments (#)
base::StatusOr<YamlNode> ParseYaml(std::string_view input);

}  // namespace perfetto::trace_processor::pfgraph

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PFGRAPH_YAML_PARSER_H_
