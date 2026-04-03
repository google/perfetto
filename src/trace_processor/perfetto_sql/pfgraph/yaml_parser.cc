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

#include "src/trace_processor/perfetto_sql/pfgraph/yaml_parser.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"

namespace perfetto::trace_processor::pfgraph {
namespace {

// Represents a single line of YAML input with its indentation level.
struct YamlLine {
  uint32_t line_number;  // 1-based line number for error messages.
  uint32_t indent;       // Number of leading spaces.
  std::string content;   // Line content with leading spaces stripped.
};

// Returns the number of leading spaces in a string.
uint32_t CountIndent(std::string_view line) {
  uint32_t n = 0;
  while (n < line.size() && line[n] == ' ') {
    n++;
  }
  return n;
}

// Strips leading and trailing whitespace from a string_view.
std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

// Splits the input into YamlLine structs, skipping blank lines and comments.
std::vector<YamlLine> Tokenize(std::string_view input) {
  std::vector<YamlLine> lines;
  uint32_t line_num = 0;
  while (!input.empty()) {
    line_num++;
    auto nl = input.find('\n');
    std::string_view raw_line;
    if (nl == std::string_view::npos) {
      raw_line = input;
      input = {};
    } else {
      raw_line = input.substr(0, nl);
      input.remove_prefix(nl + 1);
    }
    // Strip trailing \r for Windows line endings.
    if (!raw_line.empty() && raw_line.back() == '\r') {
      raw_line.remove_suffix(1);
    }
    uint32_t indent = CountIndent(raw_line);
    std::string_view content = raw_line.substr(indent);
    // Skip blank lines and comment-only lines.
    if (content.empty() || content[0] == '#') {
      continue;
    }
    lines.push_back({line_num, indent, std::string(content)});
  }
  return lines;
}

// Strips an inline comment from a value string (outside of quotes).
std::string_view StripInlineComment(std::string_view val) {
  bool in_single = false;
  bool in_double = false;
  for (size_t i = 0; i < val.size(); i++) {
    char c = val[i];
    if (c == '\'' && !in_double) {
      in_single = !in_single;
    } else if (c == '"' && !in_single) {
      in_double = !in_double;
    } else if (c == '#' && !in_single && !in_double && i > 0 &&
               val[i - 1] == ' ') {
      return Trim(val.substr(0, i));
    }
  }
  return val;
}

// Unquotes a string if it's wrapped in matching single or double quotes.
std::string Unquote(std::string_view s) {
  if (s.size() >= 2) {
    if ((s.front() == '"' && s.back() == '"') ||
        (s.front() == '\'' && s.back() == '\'')) {
      return std::string(s.substr(1, s.size() - 2));
    }
  }
  return std::string(s);
}

class YamlParser {
 public:
  explicit YamlParser(std::vector<YamlLine> lines)
      : lines_(std::move(lines)) {}

  base::StatusOr<YamlNode> Parse() {
    if (lines_.empty()) {
      return YamlNode();
    }
    // If the first line starts with "- ", parse as a top-level sequence.
    if (lines_[0].content.size() >= 2 && lines_[0].content[0] == '-' &&
        lines_[0].content[1] == ' ') {
      return ParseSequence(lines_[0].indent);
    }
    return ParseMapping(lines_[0].indent);
  }

 private:
  // Parse a flow sequence: [a, b, c]
  // `pos` points into the string starting after '['.
  base::StatusOr<YamlNode> ParseFlowSequence(std::string_view str,
                                              uint32_t line_num) {
    auto node = YamlNode::Sequence();
    str = Trim(str);
    if (str.empty() || str[0] == ']') {
      return node;
    }
    while (!str.empty()) {
      str = Trim(str);
      if (str[0] == ']') {
        break;
      }
      // Find the end of this element (comma or closing bracket).
      std::string_view elem;
      if (str[0] == '{') {
        // Nested flow mapping.
        auto close = FindMatchingBrace(str, '{', '}');
        if (close == std::string_view::npos) {
          return base::ErrStatus("yaml:%u: unmatched '{'", line_num);
        }
        auto inner = str.substr(1, close - 1);
        auto child = ParseFlowMapping(inner, line_num);
        if (!child.ok()) return child.status();
        node.push_back(std::move(*child));
        str.remove_prefix(close + 1);
        str = Trim(str);
        if (!str.empty() && str[0] == ',') str.remove_prefix(1);
        continue;
      }
      if (str[0] == '[') {
        auto close = FindMatchingBrace(str, '[', ']');
        if (close == std::string_view::npos) {
          return base::ErrStatus("yaml:%u: unmatched '['", line_num);
        }
        auto inner = str.substr(1, close - 1);
        auto child = ParseFlowSequence(inner, line_num);
        if (!child.ok()) return child.status();
        node.push_back(std::move(*child));
        str.remove_prefix(close + 1);
        str = Trim(str);
        if (!str.empty() && str[0] == ',') str.remove_prefix(1);
        continue;
      }
      auto end = FindFlowEnd(str);
      elem = Trim(str.substr(0, end));
      node.push_back(YamlNode::Scalar(Unquote(elem)));
      str.remove_prefix(end);
      str = Trim(str);
      if (!str.empty() && str[0] == ',') {
        str.remove_prefix(1);
      }
    }
    return node;
  }

  // Parse a flow mapping: {a: b, c: d}
  base::StatusOr<YamlNode> ParseFlowMapping(std::string_view str,
                                             uint32_t line_num) {
    auto node = YamlNode::Mapping();
    str = Trim(str);
    if (str.empty() || str[0] == '}') {
      return node;
    }
    while (!str.empty()) {
      str = Trim(str);
      if (str[0] == '}') {
        break;
      }
      // Parse key.
      auto colon = FindFlowColon(str);
      if (colon == std::string_view::npos) {
        return base::ErrStatus("yaml:%u: expected ':' in flow mapping",
                               line_num);
      }
      auto key = Trim(str.substr(0, colon));
      str.remove_prefix(colon + 1);
      str = Trim(str);
      // Parse value.
      if (!str.empty() && str[0] == '{') {
        auto close = FindMatchingBrace(str, '{', '}');
        if (close == std::string_view::npos) {
          return base::ErrStatus("yaml:%u: unmatched '{'", line_num);
        }
        auto inner = str.substr(1, close - 1);
        auto child = ParseFlowMapping(inner, line_num);
        if (!child.ok()) return child.status();
        node.insert(Unquote(key), std::move(*child));
        str.remove_prefix(close + 1);
        str = Trim(str);
        if (!str.empty() && str[0] == ',') str.remove_prefix(1);
        continue;
      }
      if (!str.empty() && str[0] == '[') {
        auto close = FindMatchingBrace(str, '[', ']');
        if (close == std::string_view::npos) {
          return base::ErrStatus("yaml:%u: unmatched '['", line_num);
        }
        auto inner = str.substr(1, close - 1);
        auto child = ParseFlowSequence(inner, line_num);
        if (!child.ok()) return child.status();
        node.insert(Unquote(key), std::move(*child));
        str.remove_prefix(close + 1);
        str = Trim(str);
        if (!str.empty() && str[0] == ',') str.remove_prefix(1);
        continue;
      }
      auto end = FindFlowEnd(str);
      auto val = Trim(str.substr(0, end));
      node.insert(Unquote(key), YamlNode::Scalar(Unquote(val)));
      str.remove_prefix(end);
      str = Trim(str);
      if (!str.empty() && str[0] == ',') {
        str.remove_prefix(1);
      }
    }
    return node;
  }

  // Find matching closing brace/bracket, respecting nesting.
  static size_t FindMatchingBrace(std::string_view s, char open, char close) {
    int depth = 0;
    bool in_single = false;
    bool in_double = false;
    for (size_t i = 0; i < s.size(); i++) {
      char c = s[i];
      if (c == '\'' && !in_double) {
        in_single = !in_single;
      } else if (c == '"' && !in_single) {
        in_double = !in_double;
      } else if (!in_single && !in_double) {
        if (c == open) {
          depth++;
        } else if (c == close) {
          depth--;
          if (depth == 0) return i;
        }
      }
    }
    return std::string_view::npos;
  }

  // Find the end of a flow element (comma, ']', or '}').
  static size_t FindFlowEnd(std::string_view s) {
    bool in_single = false;
    bool in_double = false;
    int brace_depth = 0;
    int bracket_depth = 0;
    for (size_t i = 0; i < s.size(); i++) {
      char c = s[i];
      if (c == '\'' && !in_double) {
        in_single = !in_single;
      } else if (c == '"' && !in_single) {
        in_double = !in_double;
      } else if (!in_single && !in_double) {
        if (c == '{') brace_depth++;
        else if (c == '}') {
          if (brace_depth == 0) return i;
          brace_depth--;
        }
        else if (c == '[') bracket_depth++;
        else if (c == ']') {
          if (bracket_depth == 0) return i;
          bracket_depth--;
        }
        else if (c == ',' && brace_depth == 0 && bracket_depth == 0) {
          return i;
        }
      }
    }
    return s.size();
  }

  // Find ':' in a flow context (not inside quotes or nested structures).
  static size_t FindFlowColon(std::string_view s) {
    bool in_single = false;
    bool in_double = false;
    for (size_t i = 0; i < s.size(); i++) {
      char c = s[i];
      if (c == '\'' && !in_double) {
        in_single = !in_single;
      } else if (c == '"' && !in_single) {
        in_double = !in_double;
      } else if (c == ':' && !in_single && !in_double) {
        return i;
      }
    }
    return std::string_view::npos;
  }

  // Finds the colon separator in a block mapping line like "key: value".
  // Returns npos if no valid mapping colon is found.
  // A valid colon must be followed by a space, end of string, or a flow
  // indicator.
  static size_t FindBlockColon(std::string_view line) {
    bool in_single = false;
    bool in_double = false;
    for (size_t i = 0; i < line.size(); i++) {
      char c = line[i];
      if (c == '\'' && !in_double) {
        in_single = !in_single;
      } else if (c == '"' && !in_single) {
        in_double = !in_double;
      } else if (c == ':' && !in_single && !in_double) {
        // Colon must be followed by space, EOL, or flow indicator.
        if (i + 1 >= line.size() || line[i + 1] == ' ' ||
            line[i + 1] == '\t') {
          return i;
        }
      }
    }
    return std::string_view::npos;
  }

  // Parse a block scalar (literal style with '|').
  // Collects all subsequent lines that are indented more than the current line.
  std::string ParseBlockScalar(uint32_t base_indent) {
    // The block scalar content is on lines indented more than base_indent.
    // We need to determine the content indentation from the first line.
    std::string result;
    if (pos_ >= lines_.size()) {
      return result;
    }
    uint32_t content_indent = lines_[pos_].indent;
    if (content_indent <= base_indent) {
      return result;
    }
    while (pos_ < lines_.size() && lines_[pos_].indent >= content_indent) {
      if (!result.empty()) {
        result += '\n';
      }
      // Preserve relative indentation beyond content_indent.
      uint32_t extra = lines_[pos_].indent - content_indent;
      for (uint32_t i = 0; i < extra; i++) {
        result += ' ';
      }
      result += lines_[pos_].content;
      pos_++;
    }
    result += '\n';
    return result;
  }

  // Parse a block mapping at a given indentation level.
  base::StatusOr<YamlNode> ParseMapping(uint32_t expected_indent) {
    auto node = YamlNode::Mapping();
    while (pos_ < lines_.size()) {
      const auto& line = lines_[pos_];
      // If this line is less indented, we're done with this mapping.
      if (line.indent < expected_indent) {
        break;
      }
      // If this line is at a different indentation than expected, break.
      if (line.indent != expected_indent) {
        break;
      }

      std::string_view content(line.content);

      // Check if this is a sequence entry instead of a mapping entry.
      if (content.size() >= 2 && content[0] == '-' && content[1] == ' ') {
        // This shouldn't happen in a mapping context.
        return base::ErrStatus(
            "yaml:%u: unexpected sequence entry in mapping context",
            line.line_number);
      }

      // Find the colon for a mapping key.
      auto colon = FindBlockColon(content);
      if (colon == std::string_view::npos) {
        return base::ErrStatus("yaml:%u: expected mapping key (missing ':')",
                               line.line_number);
      }

      auto key = std::string(Trim(content.substr(0, colon)));
      auto after_colon = content.substr(colon + 1);
      auto val_str = Trim(after_colon);
      val_str = StripInlineComment(val_str);

      if (!val_str.empty() && val_str[0] == '|') {
        // Block scalar.
        pos_++;
        auto scalar = ParseBlockScalar(line.indent);
        node.insert(std::move(key), YamlNode::Scalar(std::move(scalar)));
      } else if (!val_str.empty() && val_str[0] == '[') {
        // Flow sequence.
        auto close = FindMatchingBrace(val_str, '[', ']');
        if (close == std::string_view::npos) {
          return base::ErrStatus("yaml:%u: unmatched '['", line.line_number);
        }
        auto inner = val_str.substr(1, close - 1);
        auto child = ParseFlowSequence(inner, line.line_number);
        if (!child.ok()) return child.status();
        node.insert(std::move(key), std::move(*child));
        pos_++;
      } else if (!val_str.empty() && val_str[0] == '{') {
        // Flow mapping.
        auto close = FindMatchingBrace(val_str, '{', '}');
        if (close == std::string_view::npos) {
          return base::ErrStatus("yaml:%u: unmatched '{'", line.line_number);
        }
        auto inner = val_str.substr(1, close - 1);
        auto child = ParseFlowMapping(inner, line.line_number);
        if (!child.ok()) return child.status();
        node.insert(std::move(key), std::move(*child));
        pos_++;
      } else if (val_str.empty()) {
        // Value is on the next line(s), indented.
        pos_++;
        if (pos_ < lines_.size() && lines_[pos_].indent > expected_indent) {
          auto& next_line = lines_[pos_];
          if (next_line.content.size() >= 2 && next_line.content[0] == '-' &&
              next_line.content[1] == ' ') {
            // Nested sequence.
            auto child = ParseSequence(next_line.indent);
            if (!child.ok()) return child.status();
            node.insert(std::move(key), std::move(*child));
          } else {
            // Nested mapping.
            auto child = ParseMapping(next_line.indent);
            if (!child.ok()) return child.status();
            node.insert(std::move(key), std::move(*child));
          }
        } else {
          // Null value.
          node.insert(std::move(key), YamlNode());
        }
      } else {
        // Inline scalar value.
        node.insert(std::move(key),
                    YamlNode::Scalar(Unquote(val_str)));
        pos_++;
      }
    }
    return node;
  }

  // Parse a block sequence at a given indentation level.
  base::StatusOr<YamlNode> ParseSequence(uint32_t expected_indent) {
    auto node = YamlNode::Sequence();
    while (pos_ < lines_.size()) {
      const auto& line = lines_[pos_];
      if (line.indent < expected_indent) {
        break;
      }
      if (line.indent != expected_indent) {
        break;
      }

      std::string_view content(line.content);
      if (content.size() < 2 || content[0] != '-' || content[1] != ' ') {
        // Not a sequence entry; we're done.
        break;
      }

      // Strip the "- " prefix.
      auto item_str = Trim(content.substr(2));

      if (!item_str.empty() && item_str[0] == '[') {
        // Flow sequence as an item.
        auto close = FindMatchingBrace(item_str, '[', ']');
        if (close == std::string_view::npos) {
          return base::ErrStatus("yaml:%u: unmatched '['", line.line_number);
        }
        auto inner = item_str.substr(1, close - 1);
        auto child = ParseFlowSequence(inner, line.line_number);
        if (!child.ok()) return child.status();
        node.push_back(std::move(*child));
        pos_++;
      } else if (!item_str.empty() && item_str[0] == '{') {
        // Flow mapping as an item.
        auto close = FindMatchingBrace(item_str, '{', '}');
        if (close == std::string_view::npos) {
          return base::ErrStatus("yaml:%u: unmatched '{'", line.line_number);
        }
        auto inner = item_str.substr(1, close - 1);
        auto child = ParseFlowMapping(inner, line.line_number);
        if (!child.ok()) return child.status();
        node.push_back(std::move(*child));
        pos_++;
      } else if (item_str.empty()) {
        // The item value is on the next line(s).
        pos_++;
        if (pos_ < lines_.size() && lines_[pos_].indent > expected_indent) {
          auto& next_line = lines_[pos_];
          if (next_line.content.size() >= 2 && next_line.content[0] == '-' &&
              next_line.content[1] == ' ') {
            auto child = ParseSequence(next_line.indent);
            if (!child.ok()) return child.status();
            node.push_back(std::move(*child));
          } else {
            auto child = ParseMapping(next_line.indent);
            if (!child.ok()) return child.status();
            node.push_back(std::move(*child));
          }
        } else {
          node.push_back(YamlNode());
        }
      } else {
        // Check if the item content contains a colon, meaning it's a
        // compact mapping entry like "- key: value".
        auto colon = FindBlockColon(item_str);
        if (colon != std::string_view::npos) {
          // This is a compact mapping notation: "- key: value".
          // We need to parse it as a mapping. The content after "- " is at
          // indent (expected_indent + 2).
          //
          // We handle this by creating a synthetic mapping. First, parse
          // this line's key:value, then parse any continuation lines that
          // are indented further than the dash.
          auto item_key = std::string(Trim(item_str.substr(0, colon)));
          auto after_colon = item_str.substr(colon + 1);
          auto item_val = Trim(after_colon);
          item_val = StripInlineComment(item_val);

          auto mapping = YamlNode::Mapping();

          if (!item_val.empty() && item_val[0] == '|') {
            pos_++;
            auto scalar = ParseBlockScalar(expected_indent + 2);
            mapping.insert(std::move(item_key),
                           YamlNode::Scalar(std::move(scalar)));
          } else if (!item_val.empty() && item_val[0] == '[') {
            auto close = FindMatchingBrace(item_val, '[', ']');
            if (close == std::string_view::npos) {
              return base::ErrStatus("yaml:%u: unmatched '['",
                                     line.line_number);
            }
            auto inner = item_val.substr(1, close - 1);
            auto child = ParseFlowSequence(inner, line.line_number);
            if (!child.ok()) return child.status();
            mapping.insert(std::move(item_key), std::move(*child));
            pos_++;
          } else if (!item_val.empty() && item_val[0] == '{') {
            auto close = FindMatchingBrace(item_val, '{', '}');
            if (close == std::string_view::npos) {
              return base::ErrStatus("yaml:%u: unmatched '{'",
                                     line.line_number);
            }
            auto inner = item_val.substr(1, close - 1);
            auto child = ParseFlowMapping(inner, line.line_number);
            if (!child.ok()) return child.status();
            mapping.insert(std::move(item_key), std::move(*child));
            pos_++;
          } else if (item_val.empty()) {
            pos_++;
            // Value is on subsequent indented lines.
            uint32_t item_indent = expected_indent + 2;
            if (pos_ < lines_.size() &&
                lines_[pos_].indent > item_indent) {
              // Check if it's further indented for the value.
              auto& nl = lines_[pos_];
              if (nl.content.size() >= 2 && nl.content[0] == '-' &&
                  nl.content[1] == ' ') {
                auto child = ParseSequence(nl.indent);
                if (!child.ok()) return child.status();
                mapping.insert(std::move(item_key), std::move(*child));
              } else {
                auto child = ParseMapping(nl.indent);
                if (!child.ok()) return child.status();
                mapping.insert(std::move(item_key), std::move(*child));
              }
            } else if (pos_ < lines_.size() &&
                       lines_[pos_].indent == item_indent) {
              // Could be a sub-mapping or sub-sequence at the item indent
              // level.
              auto& nl = lines_[pos_];
              if (nl.content.size() >= 2 && nl.content[0] == '-' &&
                  nl.content[1] == ' ') {
                auto child = ParseSequence(nl.indent);
                if (!child.ok()) return child.status();
                mapping.insert(std::move(item_key), std::move(*child));
              } else {
                auto child = ParseMapping(nl.indent);
                if (!child.ok()) return child.status();
                mapping.insert(std::move(item_key), std::move(*child));
              }
            } else {
              mapping.insert(std::move(item_key), YamlNode());
            }
          } else {
            mapping.insert(std::move(item_key),
                           YamlNode::Scalar(Unquote(item_val)));
            pos_++;
          }

          // Parse remaining keys at the same indentation (expected_indent+2).
          uint32_t item_indent = expected_indent + 2;
          while (pos_ < lines_.size() &&
                 lines_[pos_].indent >= item_indent) {
            if (lines_[pos_].indent != item_indent) {
              break;
            }
            // Parse as continuation of the mapping.
            auto& cl = lines_[pos_];
            auto cc = FindBlockColon(std::string_view(cl.content));
            if (cc == std::string_view::npos) {
              break;
            }
            auto ck = std::string(Trim(
                std::string_view(cl.content).substr(0, cc)));
            auto cv_str = Trim(
                std::string_view(cl.content).substr(cc + 1));
            cv_str = StripInlineComment(cv_str);

            if (!cv_str.empty() && cv_str[0] == '|') {
              pos_++;
              auto scalar = ParseBlockScalar(item_indent);
              mapping.insert(std::move(ck),
                             YamlNode::Scalar(std::move(scalar)));
            } else if (!cv_str.empty() && cv_str[0] == '[') {
              auto close = FindMatchingBrace(cv_str, '[', ']');
              if (close == std::string_view::npos) {
                return base::ErrStatus("yaml:%u: unmatched '['",
                                       cl.line_number);
              }
              auto inner = cv_str.substr(1, close - 1);
              auto child = ParseFlowSequence(inner, cl.line_number);
              if (!child.ok()) return child.status();
              mapping.insert(std::move(ck), std::move(*child));
              pos_++;
            } else if (!cv_str.empty() && cv_str[0] == '{') {
              auto close = FindMatchingBrace(cv_str, '{', '}');
              if (close == std::string_view::npos) {
                return base::ErrStatus("yaml:%u: unmatched '{'",
                                       cl.line_number);
              }
              auto inner = cv_str.substr(1, close - 1);
              auto child = ParseFlowMapping(inner, cl.line_number);
              if (!child.ok()) return child.status();
              mapping.insert(std::move(ck), std::move(*child));
              pos_++;
            } else if (cv_str.empty()) {
              pos_++;
              if (pos_ < lines_.size() &&
                  lines_[pos_].indent > item_indent) {
                auto& nl = lines_[pos_];
                if (nl.content.size() >= 2 && nl.content[0] == '-' &&
                    nl.content[1] == ' ') {
                  auto child = ParseSequence(nl.indent);
                  if (!child.ok()) return child.status();
                  mapping.insert(std::move(ck), std::move(*child));
                } else {
                  auto child = ParseMapping(nl.indent);
                  if (!child.ok()) return child.status();
                  mapping.insert(std::move(ck), std::move(*child));
                }
              } else {
                mapping.insert(std::move(ck), YamlNode());
              }
            } else {
              mapping.insert(std::move(ck),
                             YamlNode::Scalar(Unquote(cv_str)));
              pos_++;
            }
          }

          node.push_back(std::move(mapping));
        } else {
          // Simple scalar item.
          node.push_back(YamlNode::Scalar(Unquote(item_str)));
          pos_++;
        }
      }
    }
    return node;
  }

  std::vector<YamlLine> lines_;
  size_t pos_ = 0;
};

}  // namespace

int64_t YamlNode::as_int() const {
  return static_cast<int64_t>(strtoll(scalar_.c_str(), nullptr, 10));
}

double YamlNode::as_double() const {
  return strtod(scalar_.c_str(), nullptr);
}

bool YamlNode::as_bool() const {
  return scalar_ == "true" || scalar_ == "True" || scalar_ == "TRUE" ||
         scalar_ == "yes" || scalar_ == "Yes" || scalar_ == "YES" ||
         scalar_ == "on" || scalar_ == "On" || scalar_ == "ON";
}

size_t YamlNode::size() const {
  switch (type_) {
    case kSequence:
      return sequence_.size();
    case kMapping:
      return mapping_.size();
    case kScalar:
      return 1;
    case kNull:
      return 0;
  }
  PERFETTO_FATAL("Unexpected YamlNode type");
}

void YamlNode::insert(std::string key, YamlNode value) {
  mapping_.emplace_back(std::move(key), std::move(value));
}

const YamlNode* YamlNode::find(const std::string& key) const {
  for (const auto& entry : mapping_) {
    if (entry.first == key) {
      return &entry.second;
    }
  }
  return nullptr;
}

const YamlNode& YamlNode::operator[](const std::string& key) const {
  const YamlNode* result = find(key);
  if (result) {
    return *result;
  }
  return null_node();
}

const YamlNode& YamlNode::null_node() {
  static const YamlNode* kNull = new YamlNode();
  return *kNull;
}

base::StatusOr<YamlNode> ParseYaml(std::string_view input) {
  auto lines = Tokenize(input);
  YamlParser parser(std::move(lines));
  return parser.Parse();
}

}  // namespace perfetto::trace_processor::pfgraph
