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

#include "tools/ftrace_proto_gen/ftrace_proto_gen.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <fstream>
#include <regex>

#include "perfetto/base/file_utils.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/string_splitter.h"

namespace perfetto {

namespace {

bool StartsWith(const std::string& str, const std::string& prefix) {
  return str.compare(0, prefix.length(), prefix) == 0;
}

bool EndsWith(const std::string& str, const std::string& pattern) {
  return str.rfind(pattern) == str.size() - pattern.size();
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

int SetNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

std::string RunClangFmt(const std::string& input) {
  std::string output;
  pid_t pid;
  int input_pipes[2];
  int output_pipes[2];
  PERFETTO_CHECK(pipe(input_pipes) != -1);
  PERFETTO_CHECK(SetNonBlocking(input_pipes[0]) != -1);
  PERFETTO_CHECK(SetNonBlocking(input_pipes[1]) != -1);
  PERFETTO_CHECK(pipe(output_pipes) != -1);
  PERFETTO_CHECK(SetNonBlocking(output_pipes[0]) != -1);
  PERFETTO_CHECK(SetNonBlocking(output_pipes[1]) != -1);
  if ((pid = fork()) == 0) {
    // Child
    PERFETTO_CHECK(dup2(input_pipes[0], STDIN_FILENO) != -1);
    PERFETTO_CHECK(dup2(output_pipes[1], STDOUT_FILENO) != -1);
    close(input_pipes[1]);
    close(output_pipes[0]);
    PERFETTO_CHECK(execl("buildtools/linux64/clang-format", "clang-format",
                         nullptr) != -1);
  }
  PERFETTO_CHECK(pid > 0);
  // Parent
  size_t written = 0;
  size_t bytes_read = 0;
  close(input_pipes[0]);
  close(output_pipes[1]);
  // This cannot be left uninitialized because there's as continue statement
  // before the first assignment to this in the loop.
  ssize_t r = -1;
  do {
    if (written < input.size()) {
      ssize_t w =
          write(input_pipes[1], &(input[written]), input.size() - written);
      if (w == -1) {
        if (errno == EAGAIN || errno == EINTR)
          continue;
        PERFETTO_FATAL("write failed");
      }
      written += static_cast<size_t>(w);
      if (written == input.size())
        close(input_pipes[1]);
    }

    if (bytes_read + base::kPageSize > output.size())
      output.resize(output.size() + base::kPageSize);
    r = read(output_pipes[0], &(output[bytes_read]), base::kPageSize);
    if (r == -1) {
      if (errno == EAGAIN || errno == EINTR)
        continue;
      PERFETTO_FATAL("read failed");
    }
    if (r > 0)
      bytes_read += static_cast<size_t>(r);
  } while (r != 0);
  output.resize(bytes_read);

  int wstatus;
  waitpid(pid, &wstatus, 0);
  PERFETTO_CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);
  return output;
}

}  // namespace

VerifyStream::VerifyStream(std::string filename)
    : filename_(std::move(filename)) {
  PERFETTO_CHECK(base::ReadFile(filename_, &expected_));
}

VerifyStream::~VerifyStream() {
  std::string tidied = str();
  if (EndsWith(filename_, "cc") || EndsWith(filename_, "proto"))
    tidied = RunClangFmt(str());
  if (expected_ != tidied) {
    PERFETTO_FATAL("%s is out of date. Please run tools/run_ftrace_proto_gen.",
                   filename_.c_str());
  }
}

FtraceEventName::FtraceEventName(const std::string& full_name) {
  if (full_name == "removed") {
    valid_ = false;
    return;
  }
  name_ = full_name.substr(full_name.find('/') + 1, std::string::npos);
  group_ = full_name.substr(0, full_name.find('/'));
  valid_ = true;
}

bool FtraceEventName::valid() const {
  return valid_;
}

const std::string& FtraceEventName::name() const {
  PERFETTO_CHECK(valid_);
  return name_;
}

const std::string& FtraceEventName::group() const {
  PERFETTO_CHECK(valid_);
  return group_;
}

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

ProtoType ProtoType::GetSigned() const {
  PERFETTO_CHECK(type == NUMERIC);
  if (is_signed)
    return *this;

  if (size == 64) {
    return Numeric(64, true);
  }

  return Numeric(2 * size, true);
}

std::string ProtoType::ToString() const {
  switch (type) {
    case INVALID:
      PERFETTO_CHECK(false);
    case STRING:
      return "string";
    case NUMERIC: {
      std::string s;
      if (!is_signed)
        s += "u";
      s += "int";
      s += std::to_string(size);
      return s;
    }
  }
  PERFETTO_CHECK(false);  // for GCC.
}

// static
ProtoType ProtoType::String() {
  return {STRING, 0, false};
}

// static
ProtoType ProtoType::Invalid() {
  return {INVALID, 0, false};
}

// static
ProtoType ProtoType::Numeric(uint16_t size, bool is_signed) {
  PERFETTO_CHECK(size == 32 || size == 64);
  return {NUMERIC, size, is_signed};
}

// static
ProtoType ProtoType::FromDescriptor(
    google::protobuf::FieldDescriptor::Type type) {
  if (type == google::protobuf::FieldDescriptor::Type::TYPE_UINT64)
    return Numeric(64, false);

  if (type == google::protobuf::FieldDescriptor::Type::TYPE_INT64)
    return Numeric(64, true);

  if (type == google::protobuf::FieldDescriptor::Type::TYPE_UINT32)
    return Numeric(32, false);

  if (type == google::protobuf::FieldDescriptor::Type::TYPE_INT32)
    return Numeric(32, true);

  if (type == google::protobuf::FieldDescriptor::Type::TYPE_STRING)
    return String();

  return Invalid();
}

ProtoType GetCommon(ProtoType one, ProtoType other) {
  // Always need to prefer the LHS as it is the one already present
  // in the proto.
  if (one.type == ProtoType::STRING)
    return ProtoType::String();

  if (one.is_signed || other.is_signed) {
    one = one.GetSigned();
    other = other.GetSigned();
  }

  return ProtoType::Numeric(std::max(one.size, other.size), one.is_signed);
}

std::vector<FtraceEventName> ReadWhitelist(const std::string& filename) {
  std::string line;
  std::vector<FtraceEventName> lines;

  std::ifstream fin(filename, std::ios::in);
  if (!fin) {
    fprintf(stderr, "failed to open whitelist %s\n", filename.c_str());
    return lines;
  }
  while (std::getline(fin, line)) {
    if (!StartsWith(line, "#"))
      lines.emplace_back(FtraceEventName(line));
  }
  return lines;
}

ProtoType InferProtoType(const FtraceEvent::Field& field) {
  // Fixed length strings: "char foo[16]"
  if (std::regex_match(field.type_and_name, std::regex(R"(char \w+\[\d+\])")))
    return ProtoType::String();

  // String pointers: "__data_loc char[] foo" (as in
  // 'cpufreq_interactive_boost').
  if (Contains(field.type_and_name, "char[] "))
    return ProtoType::String();
  if (Contains(field.type_and_name, "char * "))
    return ProtoType::String();

  // Variable length strings: "char* foo"
  if (StartsWith(field.type_and_name, "char *"))
    return ProtoType::String();

  // Variable length strings: "char foo" + size: 0 (as in 'print').
  if (StartsWith(field.type_and_name, "char ") && field.size == 0)
    return ProtoType::String();

  // ino_t, i_ino and dev_t are 32bit on some devices 64bit on others. For the
  // protos we need to choose the largest possible size.
  if (StartsWith(field.type_and_name, "ino_t ") ||
      StartsWith(field.type_and_name, "i_ino ") ||
      StartsWith(field.type_and_name, "dev_t ")) {
    return ProtoType::Numeric(64, /* is_signed= */ false);
  }

  // Ints of various sizes:
  if (field.size <= 4)
    return ProtoType::Numeric(32, field.is_signed);
  if (field.size <= 8)
    return ProtoType::Numeric(64, field.is_signed);
  return ProtoType::Invalid();
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
  for (const auto& p : proto.fields) {
    const Proto::Field& field = p.second;
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
  proto_out->event_name = format.name;
  std::set<std::string> seen;
  // TODO(hjd): We should be cleverer about id assignment.
  uint32_t i = 1;
  for (const FtraceEvent::Field& field : format.fields) {
    std::string name = GetNameFromTypeAndName(field.type_and_name);
    // TODO(hjd): Handle dup names.
    if (name == "" || seen.count(name))
      continue;
    seen.insert(name);
    ProtoType type = InferProtoType(field);
    // Check we managed to infer a type.
    if (type.type == ProtoType::INVALID)
      continue;
    Proto::Field protofield{std::move(type), name, i};
    proto_out->AddField(std::move(protofield));
    i++;
  }

  return true;
}

void GenerateFtraceEventProto(const std::vector<FtraceEventName>& raw_whitelist,
                              std::ostream* fout) {
  *fout << "// Autogenerated by:\n";
  *fout << std::string("// ") + __FILE__ + "\n";
  *fout << "// Do not edit.\n\n";
  *fout << R"(syntax = "proto2";)"
        << "\n";
  *fout << "option optimize_for = LITE_RUNTIME;\n\n";
  for (const FtraceEventName& event : raw_whitelist) {
    if (!event.valid())
      continue;

    *fout << R"(import "perfetto/trace/ftrace/)" << event.name()
          << R"(.proto";)"
          << "\n";
  }

  *fout << "\n";
  *fout << "package perfetto.protos;\n\n";
  *fout << R"(message FtraceEvent {
  // Nanoseconds since an epoch.
  // Epoch is configurable by writing into trace_clock.
  // By default this timestamp is CPU local.
  // TODO: Figure out a story for reconciling the various clocks.
  optional uint64 timestamp = 1;

  optional uint32 pid = 2;

  oneof event {
)";

  int i = 3;
  for (const FtraceEventName& event : raw_whitelist) {
    if (!event.valid()) {
      *fout << "    // removed field with id " << i << ";\n";
      ++i;
      continue;
    }

    *fout << "    " << ToCamelCase(event.name()) << "FtraceEvent "
          << event.name() << " = " << i << ";\n";
    ++i;
  }
  *fout << "  }\n";
  *fout << "}\n";
}

// Generates section of event_info.cc for a single event.
std::string SingleEventInfo(perfetto::Proto proto,
                            const std::string& group,
                            const uint32_t proto_field_id) {
  std::string s = "";
  s += "    event->name = \"" + proto.event_name + "\";\n";
  s += "    event->group = \"" + group + "\";\n";
  s += "    event->proto_field_id = " + std::to_string(proto_field_id) + ";\n";

  for (const auto& field : proto.SortedFields()) {
    s += "    event->fields.push_back(MakeField(\"" + field->name + "\", " +
         std::to_string(field->number) + ", kProto" +
         ToCamelCase(field->type.ToString()) + "));\n";
  }
  return s;
}

// This will generate the event_info.cc file for the whitelisted protos.
void GenerateEventInfo(const std::vector<std::string>& events_info,
                       std::ostream* fout) {
  std::string s = "// Autogenerated by:\n";
  s += std::string("// ") + __FILE__ + "\n";
  s += "// Do not edit.\n";
  s += R"(
#include "src/traced/probes/ftrace/event_info.h"

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

  *fout << s;
}

Proto::Proto(std::string evt_name, const google::protobuf::Descriptor& desc)
    : name(desc.name()), event_name(evt_name) {
  for (int i = 0; i < desc.field_count(); ++i) {
    const google::protobuf::FieldDescriptor* field = desc.field(i);
    PERFETTO_CHECK(field);
    AddField(Field{ProtoType::FromDescriptor(field->type()), field->name(),
                   uint32_t(field->number())});
  }
}

std::vector<const Proto::Field*> Proto::SortedFields() {
  std::vector<const Proto::Field*> sorted_fields;

  for (const auto& p : fields) {
    sorted_fields.emplace_back(&p.second);
  }
  std::sort(sorted_fields.begin(), sorted_fields.end(),
            [](const Proto::Field* a, const Proto::Field* b) {
              return a->number < b->number;
            });
  return sorted_fields;
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
  for (const auto field : SortedFields()) {
    s += "  optional " + field->type.ToString() + " " + field->name + " = " +
         std::to_string(field->number) + ";\n";
  }
  s += "}\n";
  return s;
}

void Proto::MergeFrom(const Proto& other) {
  // Always keep number from the left hand side.
  PERFETTO_CHECK(name == other.name);
  for (const auto& p : other.fields) {
    auto it = fields.find(p.first);
    if (it == fields.end()) {
      Proto::Field field = p.second;
      field.number = ++max_id;
      AddField(std::move(field));
    } else {
      it->second.type = GetCommon(it->second.type, p.second.type);
    }
  }
}

void Proto::AddField(Proto::Field other) {
  max_id = std::max(max_id, other.number);
  fields.emplace(other.name, std::move(other));
}

}  // namespace perfetto
