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

#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "src/protozero/proto_ring_buffer.h"
#include "tools/trace_to_text/proto_full_utils.h"
#include "tools/trace_to_text/trace.descriptor.h"
#include "tools/trace_to_text/utils.h"

#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
#include <zlib.h>
#endif

namespace perfetto {
namespace trace_to_text {
namespace {

using google::protobuf::Descriptor;
using google::protobuf::DescriptorPool;
using google::protobuf::DynamicMessageFactory;
using google::protobuf::FieldDescriptor;
using google::protobuf::FileDescriptor;
using google::protobuf::FileDescriptorSet;
using google::protobuf::Message;
using google::protobuf::Reflection;
using google::protobuf::TextFormat;
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

constexpr char kCompressedPacketsPrefix[] = "compressed_packets {\n";
constexpr char kCompressedPacketsSuffix[] = "}\n";

constexpr char kIndentedPacketPrefix[] = "  packet {\n";
constexpr char kIndentedPacketSuffix[] = "  }\n";

constexpr char kPacketPrefix[] = "packet {\n";
constexpr char kPacketSuffix[] = "}\n";

void PrintCompressedPackets(const std::string& packets,
                            Message* compressed_msg_scratch,
                            ZeroCopyOutputStream* output) {
#if PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
  uint8_t out[4096];
  std::vector<uint8_t> data;

  z_stream stream{};
  stream.next_in =
      const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(packets.data()));
  stream.avail_in = static_cast<unsigned int>(packets.length());

  if (inflateInit(&stream) != Z_OK) {
    PERFETTO_ELOG("Error when initiliazing zlib to decompress packets");
    return;
  }

  int ret;
  do {
    stream.next_out = out;
    stream.avail_out = sizeof(out);
    ret = inflate(&stream, Z_NO_FLUSH);
    if (ret != Z_STREAM_END && ret != Z_OK) {
      PERFETTO_ELOG("Error when decompressing packets: %s",
                    (stream.msg ? stream.msg : ""));
      return;
    }
    data.insert(data.end(), out, out + (sizeof(out) - stream.avail_out));
  } while (ret != Z_STREAM_END);
  inflateEnd(&stream);

  protos::pbzero::Trace::Decoder decoder(data.data(), data.size());
  WriteToZeroCopyOutput(output, kCompressedPacketsPrefix,
                        sizeof(kCompressedPacketsPrefix) - 1);
  TextFormat::Printer printer;
  printer.SetInitialIndentLevel(2);
  for (auto it = decoder.packet(); it; ++it) {
    protozero::ConstBytes cb = *it;
    compressed_msg_scratch->ParseFromArray(cb.data, static_cast<int>(cb.size));
    WriteToZeroCopyOutput(output, kIndentedPacketPrefix,
                          sizeof(kIndentedPacketPrefix) - 1);
    printer.Print(*compressed_msg_scratch, output);
    WriteToZeroCopyOutput(output, kIndentedPacketSuffix,
                          sizeof(kIndentedPacketSuffix) - 1);
  }
  WriteToZeroCopyOutput(output, kCompressedPacketsSuffix,
                        sizeof(kCompressedPacketsSuffix) - 1);
#else
  base::ignore_result(packets);
  base::ignore_result(compressed_msg_scratch);
  base::ignore_result(kIndentedPacketPrefix);
  base::ignore_result(kIndentedPacketSuffix);
  WriteToZeroCopyOutput(output, kCompressedPacketsPrefix,
                        sizeof(kCompressedPacketsPrefix) - 1);
  static const char kErrMsg[] =
      "Cannot decode compressed packets. zlib not enabled in the build config";
  WriteToZeroCopyOutput(output, kErrMsg, sizeof(kErrMsg) - 1);
  WriteToZeroCopyOutput(output, kCompressedPacketsSuffix,
                        sizeof(kCompressedPacketsSuffix) - 1);
  static bool log_once = [] {
    PERFETTO_ELOG("%s", kErrMsg);
    return true;
  }();
  base::ignore_result(log_once);
#endif  // PERFETTO_BUILDFLAG(PERFETTO_ZLIB)
}

}  // namespace

int TraceToText(std::istream* input, std::ostream* output) {
  DescriptorPool pool;
  FileDescriptorSet desc_set;
  desc_set.ParseFromArray(kTraceDescriptor.data(), kTraceDescriptor.size());
  for (const auto& desc : desc_set.file()) {
    pool.BuildFile(desc);
  }

  DynamicMessageFactory factory(&pool);
  const Descriptor* trace_descriptor =
      pool.FindMessageTypeByName("perfetto.protos.TracePacket");
  const Message* prototype = factory.GetPrototype(trace_descriptor);
  std::unique_ptr<Message> msg(prototype->New());

  OstreamOutputStream zero_copy_output(output);
  OstreamOutputStream* zero_copy_output_ptr = &zero_copy_output;

  constexpr uint32_t kCompressedPacketFieldDescriptor = 50;
  const Reflection* reflect = msg->GetReflection();
  const FieldDescriptor* compressed_desc =
      trace_descriptor->FindFieldByNumber(kCompressedPacketFieldDescriptor);

  std::unique_ptr<Message> compressed_packets_msg(prototype->New());
  std::string compressed_packets;

  TextFormat::Printer printer;
  printer.SetInitialIndentLevel(1);

  static constexpr size_t kMaxMsgSize = protozero::ProtoRingBuffer::kMaxMsgSize;
  std::unique_ptr<char> data(new char[kMaxMsgSize]);
  protozero::ProtoRingBuffer ring_buffer;

  uint32_t packet = 0;
  size_t bytes_processed = 0;
  while (!input->eof()) {
    input->read(data.get(), kMaxMsgSize);
    if (input->bad() || (input->fail() && !input->eof())) {
      PERFETTO_ELOG("Failed while reading trace");
      return 1;
    }
    ring_buffer.Append(data.get(), static_cast<size_t>(input->gcount()));

    for (;;) {
      auto token = ring_buffer.ReadMessage();
      if (token.fatal_framing_error) {
        PERFETTO_ELOG("Failed to tokenize trace packet");
        return 1;
      }
      if (!token.valid())
        break;
      bytes_processed += token.len;

      if (token.field_id != 1) {
        PERFETTO_ELOG("Skipping invalid field");
        continue;
      }

      if ((packet++ & 0x3f) == 0) {
        fprintf(stderr, "Processing trace: %8zu KB%c", bytes_processed / 1024,
                kProgressChar);
        fflush(stderr);
      }

      if (!msg->ParseFromArray(token.start, static_cast<int>(token.len))) {
        PERFETTO_ELOG("Skipping invalid packet");
        continue;
      }

      if (reflect->HasField(*msg, compressed_desc)) {
        compressed_packets = reflect->GetStringReference(*msg, compressed_desc,
                                                         &compressed_packets);
        PrintCompressedPackets(compressed_packets, compressed_packets_msg.get(),
                               zero_copy_output_ptr);
      } else {
        WriteToZeroCopyOutput(zero_copy_output_ptr, kPacketPrefix,
                              sizeof(kPacketPrefix) - 1);
        printer.Print(*msg, zero_copy_output_ptr);
        WriteToZeroCopyOutput(zero_copy_output_ptr, kPacketSuffix,
                              sizeof(kPacketSuffix) - 1);
      }
    }
  }
  return 0;
}

}  // namespace trace_to_text
}  // namespace perfetto
