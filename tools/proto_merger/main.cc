/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include <stdio.h>
#include <string>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/getopt.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/version.h"
#include "tools/proto_merger/proto_file.h"

namespace perfetto {
namespace proto_merger {
namespace {

class MultiFileErrorCollectorImpl
    : public google::protobuf::compiler::MultiFileErrorCollector {
 public:
  ~MultiFileErrorCollectorImpl() override;
  void AddError(const std::string&, int, int, const std::string&) override;
  void AddWarning(const std::string&, int, int, const std::string&) override;
};

MultiFileErrorCollectorImpl::~MultiFileErrorCollectorImpl() = default;

void MultiFileErrorCollectorImpl::AddError(const std::string& filename,
                                           int line,
                                           int column,
                                           const std::string& message) {
  PERFETTO_ELOG("Error %s %d:%d: %s", filename.c_str(), line, column,
                message.c_str());
}

void MultiFileErrorCollectorImpl::AddWarning(const std::string& filename,
                                             int line,
                                             int column,
                                             const std::string& message) {
  PERFETTO_ELOG("Warning %s %d:%d: %s", filename.c_str(), line, column,
                message.c_str());
}

struct ImportResult {
  std::unique_ptr<google::protobuf::compiler::Importer> importer;
  const google::protobuf::FileDescriptor* file_descriptor;
};

ImportResult ImportProto(const std::string& proto_file,
                         const std::string& proto_dir_path) {
  MultiFileErrorCollectorImpl mfe;

  google::protobuf::compiler::DiskSourceTree dst;
  dst.MapPath("", proto_dir_path);

  ImportResult result;
  result.importer.reset(new google::protobuf::compiler::Importer(&dst, &mfe));
  result.file_descriptor = result.importer->Import(proto_file);
  return result;
}

// TODO(lalitm): add example here once all the arguments are included.
const char kUsage[] =
    R"(Usage: proto_merger [-i input proto] [-I import dir]
  -i --input:            Path to the input .proto file (relative to
                         --input-include directory). The contents of this
                         file will be updated using the source of truth
                         proto.
  -I --input-include:    Root directory from which includes for --input
                         proto should be searched.
  -u --upstream:         Path to the upstream .proto file;
                         the contents of this file will be used to update
                         the input proto.
  -U --upstream-include: Root directory from which includes for --upstream
                         proto should be searched.
)";

int Main(int argc, char** argv) {
  static const option long_options[] = {
      {"help", no_argument, nullptr, 'h'},
      {"version", no_argument, nullptr, 'v'},
      {"input", required_argument, nullptr, 'i'},
      {"input-include", required_argument, nullptr, 'I'},
      {"upstream", required_argument, nullptr, 'u'},
      {"upstream-include", required_argument, nullptr, 'U'},
      {nullptr, 0, nullptr, 0}};

  std::string input;
  std::string input_include;
  std::string upstream;
  std::string upstream_include;

  for (;;) {
    int option = getopt_long(argc, argv, "hvi:I:u:U:", long_options, nullptr);

    if (option == -1)
      break;  // EOF.

    if (option == 'v') {
      printf("%s\n", base::GetVersionString());
      return 0;
    }

    if (option == 'i') {
      input = optarg;
      continue;
    }

    if (option == 'I') {
      input_include = optarg;
      continue;
    }

    if (option == 'u') {
      upstream = optarg;
      continue;
    }

    if (option == 'U') {
      upstream_include = optarg;
      continue;
    }

    if (option == 'h') {
      fprintf(stdout, kUsage);
      return 0;
    }

    fprintf(stderr, kUsage);
    return 1;
  }

  if (input.empty()) {
    PERFETTO_ELOG("Input proto (--input) should be specified");
    return 1;
  }

  if (input_include.empty()) {
    PERFETTO_ELOG(
        "Input include directory (--input-include) should be specified");
    return 1;
  }

  if (upstream.empty()) {
    PERFETTO_ELOG("Upstream proto (--upstream) should be specified");
    return 1;
  }

  if (upstream_include.empty()) {
    PERFETTO_ELOG(
        "Upstream include directory (--upstream-include) should be specified");
    return 1;
  }

  ImportResult input_proto = ImportProto(input, input_include);
  ProtoFile input_file = ProtoFileFromDescriptor(*input_proto.file_descriptor);

  ImportResult upstream_proto = ImportProto(upstream, upstream_include);
  ProtoFile upstream_file =
      ProtoFileFromDescriptor(*upstream_proto.file_descriptor);

  // TODO(lalitm): actually make use of the ProtoFiles in a followup CL.
  base::ignore_result(input_file);
  base::ignore_result(upstream_file);

  return 0;
}

}  // namespace
}  // namespace proto_merger
}  // namespace perfetto

int main(int argc, char** argv) {
  return perfetto::proto_merger::Main(argc, argv);
}
