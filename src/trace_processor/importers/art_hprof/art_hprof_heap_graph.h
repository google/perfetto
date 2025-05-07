/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_HEAP_GRAPH_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_HEAP_GRAPH_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/art_hprof/art_hprof_event.h"
#include "src/trace_processor/importers/art_hprof/art_hprof_tokenizer.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

namespace perfetto::trace_processor::art_hprof {
class HeapGraphBuilder {
 public:
  HeapGraph Build(const HprofData& data);

 private:
  // Main conversion phases
  void ConvertClasses(const HprofData& data, HeapGraph& ir);
  void ConvertObjects(const HprofData& data, HeapGraph& ir);
  void ConvertReferences(const HprofData& data, HeapGraph& ir);

  // Helper methods
  std::string GetRootType(uint8_t root_type_id);
  std::string GetHeapType(uint8_t heap_id);
  // Generic template helper for creating objects
  template <typename T>
  void CreateObjectFromDump(const T& dump_data,
                            const HprofData& data,
                            HeapGraph& ir,
                            size_t& counter);

  // Tracking processed objects to avoid duplicates
  std::unordered_set<uint64_t> processed_object_ids_;
};
/**
 * Tokenizer for ART HPROF data that handles chunked input.
 */
class ArtHprofTokenizer : public ChunkedTraceReader {
 public:
  /**
   * Creates a new ArtHprofTokenizer with the given context.
   *
   * @param context Trace processor context
   */
  explicit ArtHprofTokenizer(TraceProcessorContext* context);

  /**
   * Destructor.
   */
  ~ArtHprofTokenizer() override;

  /**
   * Parse a chunk of HPROF data.
   *
   * @param blob The blob view containing the chunk
   * @return Status of the parsing operation
   */
  base::Status Parse(TraceBlobView blob) override;

  /**
   * Notifies that the end of the file has been reached.
   *
   * @return Status of the finalization
   */
  base::Status NotifyEndOfFile() override;

  /**
   * Sets the parser implementation.
   *
   * @param parser_impl The parser implementation to use
   */
  void SetParserImpl(ArtHprofParser* parser_impl) {
    parser_impl_ = parser_impl;
  }

 private:
  using Iterator = util::TraceBlobViewReader::Iterator;

  /**
   * ByteIterator implementation for TraceBlobView.
   */
  class TraceBlobViewIterator : public ByteIterator {
   public:
    explicit TraceBlobViewIterator(util::TraceBlobViewReader&& reader);
    ~TraceBlobViewIterator() override;

    bool ReadU1(uint8_t& value) override;
    bool ReadU2(uint16_t& value) override;
    bool ReadU4(uint32_t& value) override;
    bool ReadId(uint64_t& value, uint32_t id_size) override;
    bool ReadString(std::string& str, size_t length) override;
    bool ReadBytes(std::vector<uint8_t>& data, size_t length) override;
    bool SkipBytes(size_t count) override;
    size_t GetPosition() override;
    bool IsEof() const override;
    bool IsValid() const override;

   private:
    util::TraceBlobViewReader reader_;
    size_t current_offset_ = 0;
  };

  /**
   * Detection sub-parser.
   */
  struct Detect {
    base::Status Parse();
    base::Status NotifyEndOfFile() const;
    ArtHprofTokenizer* tokenizer_;
  };

  /**
   * Non-streaming sub-parser.
   */
  struct NonStreaming {
    base::Status Parse();
    base::Status NotifyEndOfFile() const;
    ArtHprofTokenizer* tokenizer_;
    bool is_parsing_ = false;
  };

  /**
   * Streaming sub-parser.
   */
  struct Streaming {
    base::Status Parse();
    base::Status NotifyEndOfFile();
    ArtHprofTokenizer* tokenizer_;
    size_t it_offset_ = 0;
    bool header_parsed_ = false;
  };

  using SubParser = std::variant<Detect, NonStreaming, Streaming>;

  /**
   * Initialize parsers if needed.
   *
   * @return Status of the initialization
   */
  base::Status InitializeParserIfNeeded();

  /**
   * Process parsing results and generate events.
   *
   * @return Status of the processing
   */
  base::Status ProcessParsingResults();

  TraceProcessorContext* const context_;
  util::TraceBlobViewReader reader_;
  SubParser sub_parser_ = Detect{this};
  ArtHprofParser* parser_impl_ = nullptr;

  // Parser components
  std::unique_ptr<ByteIterator> byte_iterator_;
  std::unique_ptr<HprofParser> parser_;
  std::optional<HprofData> parser_result_;
  std::unique_ptr<HeapGraphBuilder> converter_;
  std::optional<HeapGraph> ir_;

  bool is_initialized_ = false;
  bool is_complete_ = false;
};

}  // namespace perfetto::trace_processor::art_hprof

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_HEAP_GRAPH_H_
