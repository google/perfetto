/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "tools/trace_to_text/trace_to_hprof.h"

#include <endian.h>
#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "perfetto/base/logging.h"
#include "tools/trace_to_text/utils.h"

// Spec
// http://hg.openjdk.java.net/jdk6/jdk6/jdk/raw-file/tip/src/share/demo/jvmti/hprof/manual.html#Basic_Type
// Parser
// https://cs.android.com/android/platform/superproject/+/master:art/tools/ahat/src/main/com/android/ahat/heapdump/Parser.java

namespace perfetto {
namespace trace_to_text {

namespace {
constexpr char HEADER[] = "PERFETTO_JAVA_HEAP";
constexpr uint32_t ID_SZ = 8;

class BigEndianBuffer {
 public:
  void WriteId(uint64_t val) { WriteU8(val); }

  void WriteU8(uint64_t val) {
    val = htobe64(val);
    Write(reinterpret_cast<char*>(&val), sizeof(uint64_t));
  }

  void WriteU4(uint32_t val) {
    val = htobe32(val);
    Write(reinterpret_cast<char*>(&val), sizeof(uint32_t));
  }

  void SetU4(uint32_t val, size_t pos) {
    val = htobe32(val);
    memcpy(buf_.data() + pos, &val, sizeof(uint32_t));
  }

  // Uncomment when needed
  // void WriteU2(uint16_t val) {
  //   val = htobe16(val);
  //   Write(reinterpret_cast<char*>(&val), sizeof(uint16_t));
  // }

  void WriteByte(uint8_t val) { buf_.emplace_back(val); }

  void Write(const char* val, uint32_t sz) {
    const char* end = val + sz;
    while (val < end) {
      WriteByte(static_cast<uint8_t>(*val));
      val++;
    }
  }

  size_t written() const { return buf_.size(); }

  void Flush(std::ostream* out) const {
    out->write(buf_.data(), static_cast<std::streamsize>(buf_.size()));
  }

 private:
  std::vector<char> buf_;
};

class HprofWriter {
 public:
  HprofWriter(std::ostream* output) : output_(output) {}

  void WriteBuffer(const BigEndianBuffer& buf) { buf.Flush(output_); }

  void WriteRecord(const uint8_t type,
                   const std::function<void(BigEndianBuffer*)>&& writer) {
    BigEndianBuffer buf;
    buf.WriteByte(type);
    // ts offset
    buf.WriteU4(0);
    // size placeholder
    buf.WriteU4(0);
    writer(&buf);
    uint32_t record_sz = static_cast<uint32_t>(buf.written() - 9);
    buf.SetU4(record_sz, 5);
    WriteBuffer(buf);
  }

 private:
  std::ostream* output_;
};

// TODO: sample code really, rewrite this
std::unordered_map<std::string, uint32_t> WriteStrings(
    trace_processor::TraceProcessor* tp,
    HprofWriter* writer) {
  auto it = tp->ExecuteQuery(R"(
      SELECT DISTINCT str FROM (
        SELECT CASE
          WHEN str LIKE 'java.lang.Class<%' THEN rtrim(substr(str, 17), '>')
          ELSE str
        END str
        FROM (SELECT IFNULL(deobfuscated_name, name) str FROM heap_graph_class)
        UNION ALL
        SELECT IFNULL(deobfuscated_field_name, field_name) str
        FROM heap_graph_reference
      ))");

  std::unordered_map<std::string, uint32_t> strings;
  uint32_t id = 1;
  while (it.Next()) {
    std::string name(it.Get(0).AsString());
    strings[name] = id;

    // Size of record is the id + the string length
    writer->WriteRecord(0x01, [id, &name](BigEndianBuffer* buf) {
      buf->WriteId(id);
      buf->Write(name.c_str(), static_cast<uint32_t>(name.length()));
    });

    ++id;
  }
  return strings;
}
}  // namespace

int TraceToHprof(trace_processor::TraceProcessor* tp,
                 std::ostream* output,
                 uint64_t pid,
                 uint64_t ts) {
  PERFETTO_DCHECK(tp != nullptr && pid != 0 && ts != 0);
  HprofWriter hprof(output);
  BigEndianBuffer header;
  header.Write(HEADER, sizeof(HEADER));
  // Identifier size
  header.WriteU4(ID_SZ);
  // walltime high (unused)
  header.WriteU4(0);
  // walltime low (unused)
  header.WriteU4(0);
  hprof.WriteBuffer(header);

  const auto interned = WriteStrings(tp, &hprof);
  // Add placeholder stack trace (required by the format).
  hprof.WriteRecord(0x05, [](BigEndianBuffer* buf) {
    buf->WriteU4(0);
    buf->WriteU4(0);
    buf->WriteU4(0);
  });
  return 0;
}

int TraceToHprof(std::istream* input,
                 std::ostream* output,
                 uint64_t pid,
                 std::vector<uint64_t> timestamps) {
  // TODO: Simplify this for cmdline users. For example, if there is a single
  // heap graph, use this, and only fail when there is ambiguity.
  if (pid == 0) {
    PERFETTO_ELOG("Must specify pid");
    return -1;
  }
  if (timestamps.size() != 1) {
    PERFETTO_ELOG("Must specify single timestamp");
    return -1;
  }
  trace_processor::Config config;
  std::unique_ptr<trace_processor::TraceProcessor> tp =
      trace_processor::TraceProcessor::CreateInstance(config);
  if (!ReadTrace(tp.get(), input))
    return false;
  tp->NotifyEndOfFile();
  return TraceToHprof(tp.get(), output, pid, timestamps[0]);
}

}  // namespace trace_to_text
}  // namespace perfetto
