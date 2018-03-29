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

#include "ftrace_proto_gen.h"

#include <fstream>
#include <regex>
#include <set>
#include <string>

namespace perfetto {

namespace {

std::string ToCamelCase(const std::string& s) {
  std::string result;
  result.reserve(s.size());
  bool upperCaseNextChar = true;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '_') {
      upperCaseNextChar = true;
      continue;
    }
    if (upperCaseNextChar) {
      upperCaseNextChar = false;
      c = static_cast<char>(toupper(c));
    }
    result.push_back(c);
  }
  return result;
}

bool StartsWith(const std::string& str, const std::string& prefix) {
  return str.compare(0, prefix.length(), prefix) == 0;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

std::string InferProtoType(const FtraceEvent::Field& field) {
  // Fixed length strings: "char foo[16]"
  if (std::regex_match(field.type_and_name, std::regex(R"(char \w+\[\d+\])")))
    return "string";

  // String pointers: "__data_loc char[] foo" (as in
  // 'cpufreq_interactive_boost').
  if (Contains(field.type_and_name, "char[] "))
    return "string";
  if (Contains(field.type_and_name, "char * "))
    return "string";

  // Variable length strings: "char* foo"
  if (StartsWith(field.type_and_name, "char *"))
    return "string";

  // Variable length strings: "char foo" + size: 0 (as in 'print').
  if (StartsWith(field.type_and_name, "char ") && field.size == 0)
    return "string";

  // ino_t, i_ino and dev_t are 32bit on some devices 64bit on others. For the
  // protos we need to choose the largest possible size.
  if (StartsWith(field.type_and_name, "ino_t ") ||
      StartsWith(field.type_and_name, "i_ino ") ||
      StartsWith(field.type_and_name, "dev_t ")) {
    return "uint64";
  }

  // Ints of various sizes:
  if (field.size <= 4 && field.is_signed)
    return "int32";
  if (field.size <= 4 && !field.is_signed)
    return "uint32";
  if (field.size <= 8 && field.is_signed)
    return "int64";
  if (field.size <= 8 && !field.is_signed)
    return "uint64";
  return "";
}

void PrintFtraceEventProtoAdditions(const std::set<std::string>& events) {
  printf(
      "\nNumber appropriately and add output to "
      "protos/perfetto/trace/ftrace/ftrace_event.proto\n");
  for (auto event : events) {
    printf("%sFtraceEvent %s = ;\n", ToCamelCase(event).c_str(), event.c_str());
  }
}

void PrintEventFormatterMain(const std::set<std::string>& events) {
  printf(
      "\nAdd output to FormatEventText in "
      "tools/ftrace_proto_gen/ftrace_event_formatter.cc\n");
  for (auto event : events) {
    printf(
        "else if (event.has_%s()) {\nconst auto& inner = event.%s();\nline = "
        "Format%s(inner);\n} ",
        event.c_str(), event.c_str(), ToCamelCase(event).c_str());
  }
}

// Add output to ParseInode in ftrace_inode_handler
void PrintInodeHandlerMain(const std::string& event_name,
                           const perfetto::Proto& proto) {
  for (const auto& field : proto.fields) {
    if (Contains(field.name, "ino") && !Contains(field.name, "minor"))
      printf(
          "else if (event.has_%s() && event.%s().%s()) {\n*inode = "
          "static_cast<uint64_t>(event.%s().%s());\n return true;\n} ",
          event_name.c_str(), event_name.c_str(), field.name.c_str(),
          event_name.c_str(), field.name.c_str());
  }
}

void PrintEventFormatterUsingStatements(const std::set<std::string>& events) {
  printf("\nAdd output to tools/ftrace_proto_gen/ftrace_event_formatter.cc\n");
  for (auto event : events) {
    printf("using protos::%sFtraceEvent;\n", ToCamelCase(event).c_str());
  }
}

void PrintEventFormatterFunctions(const std::set<std::string>& events) {
  printf(
      "\nAdd output to tools/ftrace_proto_gen/ftrace_event_formatter.cc and "
      "then manually go through format files to match fields\n");
  for (auto event : events) {
    printf(
        "std::string Format%s(const %sFtraceEvent& event) {"
        "\nchar line[2048];"
        "\nsprintf(line,\"%s: );\nreturn std::string(line);\n}\n",
        ToCamelCase(event).c_str(), ToCamelCase(event).c_str(), event.c_str());
  }
}

bool GenerateProto(const FtraceEvent& format, Proto* proto_out) {
  proto_out->name = ToCamelCase(format.name) + "FtraceEvent";
  proto_out->fields.reserve(format.fields.size());
  std::set<std::string> seen;
  // TODO(hjd): We should be cleverer about id assignment.
  uint32_t i = 1;
  for (const FtraceEvent::Field& field : format.fields) {
    std::string name = GetNameFromTypeAndName(field.type_and_name);
    // TODO(hjd): Handle dup names.
    if (name == "" || seen.count(name))
      continue;
    seen.insert(name);
    std::string type = InferProtoType(field);
    // Check we managed to infer a type.
    if (type == "")
      continue;
    proto_out->fields.emplace_back(Proto::Field{type, name, i});
    i++;
  }

  return true;
}

std::set<std::string> GetWhitelistedEvents(const std::string& whitelist_path) {
  std::string line;
  std::set<std::string> whitelist;

  std::ifstream fin(whitelist_path, std::ios::in);
  if (!fin) {
    fprintf(stderr, "Failed to open whitelist %s\n", whitelist_path.c_str());
    return whitelist;
  }
  while (std::getline(fin, line)) {
    if (!StartsWith(line, "#")) {
      whitelist.insert(line);
    }
  }
  return whitelist;
}

// Generates section of event_info.cc for a single event.
std::string SingleEventInfo(perfetto::FtraceEvent format,
                            perfetto::Proto proto,
                            const std::string& group,
                            const std::string& proto_field_id) {
  std::string s = "";
  s += "    event->name = \"" + format.name + "\";\n";
  s += "    event->group = \"" + group + "\";\n";
  s += "    event->proto_field_id = " + proto_field_id + ";\n";

  for (const auto& field : proto.fields) {
    s += "    event->fields.push_back(MakeField(\"" + field.name + "\", " +
         std::to_string(field.number) + ", kProto" + ToCamelCase(field.type) +
         "));\n";
  }
  return s;
}

// This will generate the event_info.cc file for the whitelisted protos.
void GenerateEventInfo(const std::vector<std::string>& events_info) {
  std::string output_path = "src/ftrace_reader/event_info.cc";
  std::ofstream fout(output_path.c_str(), std::ios::out);
  if (!fout) {
    fprintf(stderr, "Failed to open %s\n", output_path.c_str());
    return;
  }

  std::string s = "// Autogenerated by:\n";
  s += std::string("// ") + __FILE__ + "\n";
  s += "// Do not edit.\n";
  s += R"(
#include "src/ftrace_reader/event_info.h"

namespace perfetto {

std::vector<Event> GetStaticEventInfo() {
  std::vector<Event> events;
)";

  for (const auto& event : events_info) {
    s += "\n";
    s += "  {\n";
    s += "    events.emplace_back(Event{});\n";
    s += "    Event* event = &events.back();\n";
    s += event;
    s += "  }\n";
  }

  s += R"(
  return events;
}

}  // namespace perfetto
)";

  fout << s;
  fout.close();
}

std::string Proto::ToString() {
  std::string s = "// Autogenerated by:\n";
  s += std::string("// ") + __FILE__ + "\n";
  s += "// Do not edit.\n";

  s += R"(
syntax = "proto2";
option optimize_for = LITE_RUNTIME;
package perfetto.protos;

)";

  s += "message " + name + " {\n";
  for (const Proto::Field& field : fields) {
    s += "  optional " + field.type + " " + field.name + " = " +
         std::to_string(field.number) + ";\n";
  }
  s += "}\n";
  return s;
}

}  // namespace perfetto
