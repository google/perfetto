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

#include "tools/trace_to_text/trace_to_text.h"

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>

#include "perfetto/base/logging.h"
#include "tools/trace_to_text/proto_full_utils.h"
#include "tools/trace_to_text/utils.h"

namespace perfetto {
namespace trace_to_text {

namespace {
using google::protobuf::Descriptor;
using google::protobuf::DynamicMessageFactory;
using google::protobuf::FileDescriptor;
using google::protobuf::Message;
using google::protobuf::TextFormat;
using google::protobuf::compiler::DiskSourceTree;
using google::protobuf::compiler::Importer;
using google::protobuf::io::OstreamOutputStream;
using google::protobuf::io::ZeroCopyOutputStream;

inline void WriteToZeroCopyOutput(ZeroCopyOutputStream* output,
                                  const char* str,
                                  size_t length) {
  if (length == 0)
    return;

  void* data;
  int size = 0;
  size_t bytes_to_copy = 0;
  while (length) {
    output->Next(&data, &size);
    bytes_to_copy = std::min(length, static_cast<size_t>(size));
    memcpy(data, str, bytes_to_copy);
    length -= bytes_to_copy;
    str += bytes_to_copy;
  }
  output->BackUp(size - static_cast<int>(bytes_to_copy));
}

constexpr char kPacketPrefix[] = "packet {\n";
constexpr char kPacketSuffix[] = "}\n";

}  // namespace

int TraceToText(std::istream* input, std::ostream* output) {
  DiskSourceTree dst;
  dst.MapPath("perfetto", "protos/perfetto");
  MultiFileErrorCollectorImpl mfe;
  Importer importer(&dst, &mfe);
  const FileDescriptor* parsed_file =
      importer.Import("perfetto/trace/trace_packet.proto");

  DynamicMessageFactory dmf;
  const Descriptor* trace_descriptor = parsed_file->message_type(0);
  const Message* root = dmf.GetPrototype(trace_descriptor);
  OstreamOutputStream zero_copy_output(output);
  OstreamOutputStream* zero_copy_output_ptr = &zero_copy_output;
  Message* msg = root->New();

  ForEachPacketBlobInTrace(
      input,
      [msg, zero_copy_output_ptr](std::unique_ptr<char[]> buf, size_t size) {
        if (!msg->ParseFromArray(buf.get(), static_cast<int>(size))) {
          PERFETTO_ELOG("Skipping invalid packet");
          return;
        }
        WriteToZeroCopyOutput(zero_copy_output_ptr, kPacketPrefix,
                              sizeof(kPacketPrefix) - 1);
        TextFormat::Print(*msg, zero_copy_output_ptr);
        WriteToZeroCopyOutput(zero_copy_output_ptr, kPacketSuffix,
                              sizeof(kPacketSuffix) - 1);
      });
  return 0;
}

}  // namespace trace_to_text
}  // namespace perfetto
