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

#include "src/trace_processor/forwarding_trace_parser.h"
#include "src/trace_processor/util/gzip_utils.h"

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
using trace_processor::TraceType;
using trace_processor::util::GzipDecompressor;

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
  std::vector<uint8_t> whole_data = GzipDecompressor::DecompressFully(
      reinterpret_cast<const uint8_t*>(packets.data()), packets.size());
  protos::pbzero::Trace::Decoder decoder(whole_data.data(), whole_data.size());
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

// TracePacket descriptor and metadata, used to print a TracePacket proto as a
// text proto.
struct TracePacketProtoDescInfo {
  TracePacketProtoDescInfo();
  FileDescriptorSet desc_set;
  DescriptorPool pool;
  std::unique_ptr<DynamicMessageFactory> factory;
  const Descriptor* trace_descriptor;
  const Message* prototype;
  const FieldDescriptor* compressed_desc;
};

TracePacketProtoDescInfo::TracePacketProtoDescInfo() {
  desc_set.ParseFromArray(kTraceDescriptor.data(), kTraceDescriptor.size());
  for (const auto& desc : desc_set.file()) {
    pool.BuildFile(desc);
  }
  factory.reset(new DynamicMessageFactory(&pool));
  trace_descriptor = pool.FindMessageTypeByName("perfetto.protos.TracePacket");
  prototype = factory->GetPrototype(trace_descriptor);
  compressed_desc = trace_descriptor->FindFieldByNumber(
      protos::pbzero::TracePacket::kCompressedPacketsFieldNumber);
}

// Online algorithm to covert trace binary to text format.
// Usage:
//  - Feed the trace-binary in a sequence of memblock, and it will continue to
//    write the output in given std::ostream*.
class OnlineTraceToText {
 public:
  OnlineTraceToText(std::ostream* output)
      : zero_copy_out_stream_(output),
        msg_(pb_desc_info_.prototype->New()),
        compressed_packets_msg_(pb_desc_info_.prototype->New()),
        reflect_(msg_->GetReflection()) {
    printer_.SetInitialIndentLevel(1);
  }
  OnlineTraceToText(const OnlineTraceToText&) = delete;
  OnlineTraceToText& operator=(const OnlineTraceToText&) = delete;
  void Feed(const uint8_t* data, size_t len);
  bool ok() const { return ok_; }

 private:
  bool ok_ = true;
  OstreamOutputStream zero_copy_out_stream_;
  protozero::ProtoRingBuffer ring_buffer_;
  TextFormat::Printer printer_;
  TracePacketProtoDescInfo pb_desc_info_;
  std::unique_ptr<Message> msg_;
  std::unique_ptr<Message> compressed_packets_msg_;
  const Reflection* reflect_;
  std::string compressed_packets_;
  size_t bytes_processed_ = 0;
  size_t packet_ = 0;
};

void OnlineTraceToText::Feed(const uint8_t* data, size_t len) {
  ring_buffer_.Append(data, static_cast<size_t>(len));
  while (true) {
    auto token = ring_buffer_.ReadMessage();
    if (token.fatal_framing_error) {
      PERFETTO_ELOG("Failed to tokenize trace packet");
      ok_ = false;
      return;
    }
    if (!token.valid()) {
      // no need to set `ok_ = false` here because this just means
      // we've run out of packets in the ring buffer.
      break;
    }

    if (token.field_id != protos::pbzero::Trace::kPacketFieldNumber) {
      PERFETTO_ELOG("Skipping invalid field");
      continue;
    }
    if (!msg_->ParseFromArray(token.start, static_cast<int>(token.len))) {
      PERFETTO_ELOG("Skipping invalid packet");
      continue;
    }
    bytes_processed_ += token.len;
    if ((packet_++ & 0x3f) == 0) {
      fprintf(stderr, "Processing trace: %8zu KB%c", bytes_processed_ / 1024,
              kProgressChar);
      fflush(stderr);
    }
    if (reflect_->HasField(*msg_, pb_desc_info_.compressed_desc)) {
      // TODO(mohitms): GetStringReference ignores third argument. Why are we
      // passing ?
      compressed_packets_ = reflect_->GetStringReference(
          *msg_, pb_desc_info_.compressed_desc, &compressed_packets_);
      PrintCompressedPackets(compressed_packets_, compressed_packets_msg_.get(),
                             &zero_copy_out_stream_);
    } else {
      WriteToZeroCopyOutput(&zero_copy_out_stream_, kPacketPrefix,
                            sizeof(kPacketPrefix) - 1);
      printer_.Print(*msg_, &zero_copy_out_stream_);
      WriteToZeroCopyOutput(&zero_copy_out_stream_, kPacketSuffix,
                            sizeof(kPacketSuffix) - 1);
    }
  }
}

class InputReader {
 public:
  InputReader(std::istream* input) : input_(input) {}
  // Request the input-stream to read next |len_limit| bytes and load
  // it in |data|. It also updates the |len| with actual number of bytes loaded
  // in |data|. This can be less than requested |len_limit| if we have reached
  // at the end of the file.
  bool Read(uint8_t* data, uint32_t* len, uint32_t len_limit) {
    if (input_->eof())
      return false;
    input_->read(reinterpret_cast<char*>(data), std::streamsize(len_limit));
    if (input_->bad() || (input_->fail() && !input_->eof())) {
      PERFETTO_ELOG("Failed while reading trace");
      ok_ = false;
      return false;
    }
    *len = uint32_t(input_->gcount());
    return true;
  }
  bool ok() const { return ok_; }

 private:
  std::istream* input_;
  bool ok_ = true;
};

}  // namespace

bool TraceToText(std::istream* input, std::ostream* output) {
  constexpr size_t kMaxMsgSize = protozero::ProtoRingBuffer::kMaxMsgSize;
  std::unique_ptr<uint8_t[]> buffer(new uint8_t[kMaxMsgSize]);
  uint32_t buffer_len = 0;

  InputReader input_reader(input);
  OnlineTraceToText online_trace_to_text(output);

  input_reader.Read(buffer.get(), &buffer_len, kMaxMsgSize);
  TraceType type = trace_processor::GuessTraceType(buffer.get(), buffer_len);

  if (type == TraceType::kGzipTraceType) {
    GzipDecompressor decompressor;
    auto consumer = [&](const uint8_t* data, size_t len) {
      online_trace_to_text.Feed(data, len);
    };
    using ResultCode = GzipDecompressor::ResultCode;
    do {
      ResultCode code =
          decompressor.FeedAndExtract(buffer.get(), buffer_len, consumer);
      if (code == ResultCode::kError || !online_trace_to_text.ok())
        return false;
    } while (input_reader.Read(buffer.get(), &buffer_len, kMaxMsgSize));
    return input_reader.ok();
  } else if (type == TraceType::kProtoTraceType) {
    do {
      online_trace_to_text.Feed(buffer.get(), buffer_len);
      if (!online_trace_to_text.ok())
        return false;
    } while (input_reader.Read(buffer.get(), &buffer_len, kMaxMsgSize));
    return input_reader.ok();
  } else {
    PERFETTO_ELOG("Unrecognised file.");
    return false;
  }
}

}  // namespace trace_to_text
}  // namespace perfetto
