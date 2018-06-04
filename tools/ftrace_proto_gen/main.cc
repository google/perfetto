/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include <getopt.h>
#include <sys/stat.h>
#include <fstream>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>

#include "perfetto/base/file_utils.h"
#include "perfetto/base/logging.h"
#include "src/traced/probes/ftrace/format_parser.h"
#include "tools/ftrace_proto_gen/ftrace_proto_gen.h"

std::unique_ptr<std::ostream> MakeOFStream(const std::string& filename);
std::unique_ptr<std::ostream> MakeOFStream(const std::string& filename) {
  return std::unique_ptr<std::ostream>(new std::ofstream(filename));
}

std::unique_ptr<std::ostream> MakeVerifyStream(const std::string& filename);
std::unique_ptr<std::ostream> MakeVerifyStream(const std::string& filename) {
  return std::unique_ptr<std::ostream>(new perfetto::VerifyStream(filename));
}

int main(int argc, char** argv) {
  static struct option long_options[] = {
      {"whitelist_path", required_argument, nullptr, 'w'},
      {"output_dir", required_argument, nullptr, 'o'},
      {"proto_descriptor", required_argument, nullptr, 'd'},
      {"update_build_files", no_argument, nullptr, 'b'},
      {"check_only", no_argument, nullptr, 'c'},
  };

  int option_index;
  int c;

  std::string whitelist_path;
  std::string output_dir;
  std::string proto_descriptor;
  bool update_build_files = false;

  std::unique_ptr<std::ostream> (*ostream_factory)(const std::string&) =
      &MakeOFStream;

  while ((c = getopt_long(argc, argv, "", long_options, &option_index)) != -1) {
    switch (c) {
      case 'w':
        whitelist_path = optarg;
        break;
      case 'o':
        output_dir = optarg;
        break;
      case 'd':
        proto_descriptor = optarg;
        break;
      case 'b':
        update_build_files = true;
        break;
      case 'c':
        ostream_factory = &MakeVerifyStream;
    }
  }

  PERFETTO_CHECK(!whitelist_path.empty());
  PERFETTO_CHECK(!output_dir.empty());
  PERFETTO_CHECK(!proto_descriptor.empty());

  if (optind >= argc) {
    fprintf(stderr,
            "Usage: ./%s -w whitelist_dir -o output_dir -d proto_descriptor "
            "[--check_only] input_dir...\n",
            argv[0]);
    return 1;
  }

  std::vector<perfetto::FtraceEventName> whitelist =
      perfetto::ReadWhitelist(whitelist_path);
  std::vector<std::string> events_info;

  google::protobuf::DescriptorPool descriptor_pool;
  descriptor_pool.AllowUnknownDependencies();
  {
    google::protobuf::FileDescriptorSet file_descriptor_set;
    std::string descriptor_bytes;
    if (!perfetto::base::ReadFile(proto_descriptor, &descriptor_bytes)) {
      fprintf(stderr, "Failed to open %s\n", proto_descriptor.c_str());
      return 1;
    }
    file_descriptor_set.ParseFromString(descriptor_bytes);

    for (const auto& d : file_descriptor_set.file()) {
      PERFETTO_CHECK(descriptor_pool.BuildFile(d));
    }
  }

  {
    std::unique_ptr<std::ostream> out =
        ostream_factory(output_dir + "/ftrace_event.proto");
    perfetto::GenerateFtraceEventProto(whitelist, out.get());
  }

  std::set<std::string> new_events;
  for (const auto& event : whitelist) {
    if (!event.valid())
      continue;
    struct stat buf;
    if (stat(
            ("protos/perfetto/trace/ftrace/" + event.name() + ".proto").c_str(),
            &buf) == -1) {
      new_events.insert(event.name());
    }
  }

  if (!new_events.empty()) {
    perfetto::PrintEventFormatterMain(new_events);
    perfetto::PrintEventFormatterUsingStatements(new_events);
    perfetto::PrintEventFormatterFunctions(new_events);
    printf(
        "\nAdd output to ParseInode in "
        "tools/ftrace_proto_gen/ftrace_inode_handler.cc\n");
  }

  // The first id used for events in FtraceEvent proto is 3.
  // Because we increment before the loop, this is 2.
  uint32_t proto_field_id = 2;
  for (auto event : whitelist) {
    ++proto_field_id;
    if (!event.valid())
      continue;
    std::string proto_file_name = event.name() + ".proto";
    std::string output_path = output_dir + std::string("/") + proto_file_name;

    std::string proto_name =
        perfetto::ToCamelCase(event.name()) + "FtraceEvent";
    perfetto::Proto proto;
    proto.name = proto_name;
    proto.event_name = event.name();
    const google::protobuf::Descriptor* d =
        descriptor_pool.FindMessageTypeByName("perfetto.protos." + proto_name);
    if (d)
      proto = perfetto::Proto(event.name(), *d);
    else
      PERFETTO_LOG("Did not find %s", proto_name.c_str());
    for (int i = optind; i < argc; ++i) {
      std::string input_dir = argv[i];
      std::string input_path = input_dir + event.group() + "/" + event.name() +
                               std::string("/format");

      std::string contents;
      if (!perfetto::base::ReadFile(input_path, &contents)) {
        fprintf(stderr, "Failed to open %s\n", input_path.c_str());
        continue;
      }

      perfetto::FtraceEvent format;
      if (!perfetto::ParseFtraceEvent(contents, &format)) {
        fprintf(stderr, "Could not parse file %s.\n", input_path.c_str());
        return 1;
      }

      perfetto::Proto event_proto;
      if (!perfetto::GenerateProto(format, &event_proto)) {
        fprintf(stderr, "Could not generate proto for file %s\n",
                input_path.c_str());
        return 1;
      }
      proto.MergeFrom(event_proto);
    }

    if (!new_events.empty())
      PrintInodeHandlerMain(proto.name, proto);

    events_info.push_back(
        perfetto::SingleEventInfo(proto, event.group(), proto_field_id));

    std::unique_ptr<std::ostream> fout = ostream_factory(output_path);
    if (!fout) {
      fprintf(stderr, "Failed to open %s\n", output_path.c_str());
      return 1;
    }

    *fout << proto.ToString();
    PERFETTO_CHECK(!fout->fail());
  }

  {
    std::unique_ptr<std::ostream> out =
        ostream_factory("src/traced/probes/ftrace/event_info.cc");
    perfetto::GenerateEventInfo(events_info, out.get());
    PERFETTO_CHECK(!out->fail());
  }

  if (update_build_files) {
    std::unique_ptr<std::ostream> f =
        ostream_factory(output_dir + "/all_protos.gni");

    *f << R"(# Copyright (C) 2018 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Autogenerated by ftrace_proto_gen.

ftrace_proto_names = [
  "ftrace_event.proto",
  "ftrace_event_bundle.proto",
  "ftrace_stats.proto",
  "test_bundle_wrapper.proto",
)";
    for (const perfetto::FtraceEventName& event : whitelist) {
      if (!event.valid())
        continue;
      *f << "  \"" << event.name() << ".proto\",\n";
    }
    *f << "]\n";
    PERFETTO_CHECK(!f->fail());
  }
}
