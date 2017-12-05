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

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "perfetto/ftrace_reader/format_parser.h"
#include "perfetto/ftrace_reader/ftrace_to_proto.h"

int main(int argc, const char** argv) {
  if (argc != 3) {
    printf("Usage: ./%s in.format out.proto\n", argv[0]);
    return 1;
  }

  const char* input_path = argv[1];
  const char* output_path = argv[2];

  std::ifstream fin(input_path, std::ios::in);
  if (!fin) {
    fprintf(stderr, "Failed to open %s\n", input_path);
    return 1;
  }
  std::ostringstream stream;
  stream << fin.rdbuf();
  fin.close();
  std::string contents = stream.str();

  perfetto::FtraceEvent format;
  if (!perfetto::ParseFtraceEvent(contents, &format)) {
    fprintf(stderr, "Could not parse file %s.\n", input_path);
    return 1;
  }

  perfetto::Proto proto;
  if (!perfetto::GenerateProto(format, &proto)) {
    fprintf(stderr, "Could not generate proto for file %s\n", input_path);
    return 1;
  }

  std::ofstream fout(output_path, std::ios::out);
  if (!fout) {
    fprintf(stderr, "Failed to open %s\n", output_path);
    return 1;
  }

  fout << proto.ToString();
  fout.close();
}
