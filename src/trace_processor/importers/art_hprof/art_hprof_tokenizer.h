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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_TOKENIZER_H_

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
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/art_hprof/art_hprof_event.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

namespace perfetto::trace_processor::art_hprof {

/**
 * Heap type enumeration from Android HPROF format.
 */
enum HprofHeapId {
  HPROF_HEAP_DEFAULT = 0,
  HPROF_HEAP_ZYGOTE = 1,
  HPROF_HEAP_APP = 2,
  HPROF_HEAP_IMAGE = 3,
  HPROF_HEAP_JIT = 4,
  HPROF_HEAP_APP_CACHE = 5,
  HPROF_HEAP_SYSTEM = 6
};

/**
 * Field type constants from HPROF format.
 */
enum FieldType {
  TYPE_OBJECT = 2,
  TYPE_BOOLEAN = 4,
  TYPE_CHAR = 5,
  TYPE_FLOAT = 6,
  TYPE_DOUBLE = 7,
  TYPE_BYTE = 8,
  TYPE_SHORT = 9,
  TYPE_INT = 10,
  TYPE_LONG = 11
};

/**
 * Tag constants from HPROF format.
 */
enum HprofTag {
  HPROF_UTF8 = 0x01,
  HPROF_LOAD_CLASS = 0x02,
  HPROF_UNLOAD_CLASS = 0x03,
  HPROF_FRAME = 0x04,
  HPROF_TRACE = 0x05,
  HPROF_ALLOC_SITES = 0x06,
  HPROF_HEAP_SUMMARY = 0x07,
  HPROF_START_THREAD = 0x0A,
  HPROF_END_THREAD = 0x0B,
  HPROF_HEAP_DUMP = 0x0C,
  HPROF_CPU_SAMPLES = 0x0D,
  HPROF_CONTROL_SETTINGS = 0x0E,
  HPROF_HEAP_DUMP_SEGMENT = 0x1C,
  HPROF_HEAP_DUMP_END = 0x2C
};

/**
 * Heap tag constants from HPROF format.
 */
enum HprofHeapTag {
  HPROF_ROOT_UNKNOWN = 0xFF,
  HPROF_ROOT_JNI_GLOBAL = 0x01,
  HPROF_ROOT_JNI_LOCAL = 0x02,
  HPROF_ROOT_JAVA_FRAME = 0x03,
  HPROF_ROOT_NATIVE_STACK = 0x04,
  HPROF_ROOT_STICKY_CLASS = 0x05,
  HPROF_ROOT_THREAD_BLOCK = 0x06,
  HPROF_ROOT_MONITOR_USED = 0x07,
  HPROF_ROOT_THREAD_OBJ = 0x08,
  HPROF_CLASS_DUMP = 0x20,
  HPROF_INSTANCE_DUMP = 0x21,
  HPROF_OBJ_ARRAY_DUMP = 0x22,
  HPROF_PRIM_ARRAY_DUMP = 0x23,
  HPROF_HEAP_DUMP_INFO = 0xFE,
  HPROF_ROOT_INTERNED_STRING = 0x89,
  HPROF_ROOT_FINALIZING = 0x8A,
  HPROF_ROOT_DEBUGGER = 0x8B,
  HPROF_ROOT_REFERENCE_CLEANUP = 0x8C,
  HPROF_ROOT_VM_INTERNAL = 0x8D,
  HPROF_ROOT_JNI_MONITOR = 0x8E
};

// Forward declarations
struct RootRecordData;
struct ClassDumpData;
struct InstanceDumpData;
struct ObjArrayDumpData;
struct PrimArrayDumpData;
struct HeapDumpInfoData;
struct HprofHeapRecord;
struct HprofRecord;
struct HeapDumpData;
struct Utf8StringData;
struct LoadClassData;

/**
 * Abstract interface for a byte iterator that reads from a data source.
 * This class provides an out-of-line virtual destructor to fix weak vtable
 * issues.
 */
class ByteIterator {
 public:
  virtual ~ByteIterator();

  /**
   * Read an unsigned 1-byte value.
   */
  virtual bool ReadU1(uint8_t& value) = 0;

  /**
   * Read an unsigned 2-byte value.
   */
  virtual bool ReadU2(uint16_t& value) = 0;

  /**
   * Read an unsigned 4-byte value.
   */
  virtual bool ReadU4(uint32_t& value) = 0;

  /**
   * Read an ID of specified size.
   */
  virtual bool ReadId(uint64_t& value, uint32_t id_size) = 0;

  /**
   * Read a string of specified length.
   */
  virtual bool ReadString(std::string& str, size_t length) = 0;

  /**
   * Read bytes of specified length.
   */
  virtual bool ReadBytes(std::vector<uint8_t>& data, size_t length) = 0;

  /**
   * Skip specified number of bytes.
   */
  virtual bool SkipBytes(size_t count) = 0;

  /**
   * Get current position in the stream.
   */
  virtual std::streampos GetPosition() = 0;

  /**
   * Check if the end of the stream has been reached.
   */
  virtual bool IsEof() const = 0;

  /**
   * Check if the iterator is valid.
   */
  virtual bool IsValid() const = 0;
};

/**
 * Field information structure.
 */
struct FieldInfo {
  std::string name;
  uint8_t type = 0;
  uint64_t class_id = 0;
};

/**
 * Class information structure.
 */
struct ClassInfo {
  std::string name;
  uint64_t class_object_id = 0;
  uint64_t super_class_id = 0;
  uint32_t instance_size = 0;
  std::vector<FieldInfo> fields;
  bool is_string_class = false;
  bool use_string_compression = false;
};

/**
 * Object reference structure.
 */
struct ObjectReference {
  std::string field_name;
  uint64_t target_object_id = 0;
};

/**
 * Field value with type information.
 */
struct FieldValue {
  enum ValueType {
    NONE,
    BOOLEAN,
    BYTE,
    CHAR,
    SHORT,
    INT,
    FLOAT,
    LONG,
    DOUBLE,
    OBJECT_ID
  };

  ValueType type = NONE;

  union {
    bool bool_value;
    int8_t byte_value;
    char16_t char_value;
    int16_t short_value;
    int32_t int_value;
    float float_value;
    int64_t long_value;
    double double_value;
    uint64_t object_id_value;
  };

  // Default constructor
  FieldValue() = default;

  // Type-specific constructors
  explicit FieldValue(bool value) : type(BOOLEAN), bool_value(value) {}
  explicit FieldValue(int8_t value) : type(BYTE), byte_value(value) {}
  explicit FieldValue(char16_t value) : type(CHAR), char_value(value) {}
  explicit FieldValue(int16_t value) : type(SHORT), short_value(value) {}
  explicit FieldValue(int32_t value) : type(INT), int_value(value) {}
  explicit FieldValue(float value) : type(FLOAT), float_value(value) {}
  explicit FieldValue(int64_t value) : type(LONG), long_value(value) {}
  explicit FieldValue(double value) : type(DOUBLE), double_value(value) {}
  explicit FieldValue(uint64_t value)
      : type(OBJECT_ID), object_id_value(value) {}
};

/**
 * Field value record structure.
 */
struct FieldValueRecord {
  std::string field_name;
  FieldValue value;
};

/**
 * HPROF header structure.
 */
struct HprofHeader {
  std::string format;
  uint32_t identifier_size = 0;
  uint64_t timestamp = 0;
};

/**
 * UTF8 string data structure.
 */
struct Utf8StringData {
  uint64_t name_id = 0;
  std::string utf8_string;
};

/**
 * Load class data structure.
 */
struct LoadClassData {
  uint32_t class_serial_num = 0;
  uint64_t class_object_id = 0;
  uint32_t stack_trace_serial_num = 0;
  uint64_t class_name_id = 0;
  std::string class_name;
};

/**
 * Heap dump data structure.
 */
struct HeapDumpData {
  std::vector<HprofHeapRecord> records;
};

/**
 * Base record structure.
 */
struct HprofRecord {
  uint8_t tag = 0;
  uint32_t time = 0;
  uint32_t length = 0;

  // Payload for different record types
  std::variant<std::monostate, Utf8StringData, LoadClassData, HeapDumpData>
      data;
};

/**
 * Root record data structure.
 */
struct RootRecordData {
  uint8_t root_type = 0;
  uint64_t object_id = 0;
  uint32_t thread_id = 0;
  uint32_t frame_number = 0;
};

/**
 * Class dump data structure.
 */
struct ClassDumpData {
  uint64_t class_object_id = 0;
  uint32_t stack_trace_serial_num = 0;
  uint64_t super_class_object_id = 0;
  uint64_t class_loader_object_id = 0;
  uint64_t signers_object_id = 0;
  uint64_t protection_domain_object_id = 0;
  uint32_t instance_size = 0;
  std::vector<FieldInfo> static_fields;
  std::vector<FieldInfo> instance_fields;
  bool is_string_class = false;
};

/**
 * Instance dump data structure.
 */
struct InstanceDumpData {
  uint64_t object_id = 0;
  uint32_t stack_trace_serial_num = 0;
  uint64_t class_object_id = 0;
  std::vector<uint8_t> raw_instance_data;
  std::vector<ObjectReference> references;
  std::vector<FieldValueRecord> field_values;
  HprofHeapId heap_id = HPROF_HEAP_DEFAULT;
};

/**
 * Object array dump data structure.
 */
struct ObjArrayDumpData {
  uint64_t array_object_id = 0;
  uint32_t stack_trace_serial_num = 0;
  uint64_t array_class_object_id = 0;
  std::vector<uint64_t> elements;
  HprofHeapId heap_id = HPROF_HEAP_DEFAULT;
};

/**
 * Primitive array dump data structure.
 */
struct PrimArrayDumpData {
  uint64_t array_object_id = 0;
  uint32_t stack_trace_serial_num = 0;
  uint8_t element_type = 0;
  std::vector<uint8_t> elements;
  HprofHeapId heap_id = HPROF_HEAP_DEFAULT;
  bool is_compressed = false;
};

/**
 * Heap dump info data structure.
 */
struct HeapDumpInfoData {
  uint32_t heap_id = 0;
  uint64_t heap_name_string_id = 0;
  std::string heap_name;
};

/**
 * Heap record variant structure.
 */
struct HprofHeapRecord {
  HprofHeapTag tag;

  std::variant<std::monostate,
               RootRecordData,
               ClassDumpData,
               InstanceDumpData,
               ObjArrayDumpData,
               PrimArrayDumpData,
               HeapDumpInfoData>
      data;
};

/**
 * AST structure for parsed HPROF data.
 */
struct HprofAst {
  HprofHeader header;
  std::vector<HprofRecord> records;

  // Maps for resolving references
  std::unordered_map<uint64_t, std::string> id_to_string_map;
  std::unordered_map<uint32_t, uint64_t> class_serial_to_id;
  std::unordered_map<uint64_t, ClassInfo> classes;
  std::unordered_map<uint64_t, uint64_t> object_to_class;
  std::unordered_map<uint64_t, std::vector<ObjectReference>> owner_to_owned;

  // Structure to track Android heap stats
  struct AndroidHeapStats {
    size_t object_count = 0;
    size_t total_bytes = 0;

    void AddObject(size_t size);
  };

  std::unordered_map<HprofHeapId, AndroidHeapStats> android_heap_stats;
  std::unordered_map<uint64_t, uint8_t> root_objects;

  // Counters for summary
  size_t string_count = 0;
  size_t class_count = 0;
  size_t heap_dump_count = 0;
  size_t class_instance_count = 0;
  size_t object_array_count = 0;
  size_t primitive_array_count = 0;
  size_t root_count = 0;
  size_t field_reference_count = 0;
  size_t heap_info_count = 0;

  // Track Android-specific information
  bool use_string_compression = false;
};

/**
 * Parser for HPROF heap dumps.
 */
class HprofParser {
 public:
  /**
   * Creates a new HprofParser with the given byte iterator.
   *
   * @param iterator Byte iterator for reading HPROF data
   * @param detect_string_classes Whether to detect string classes
   */
  explicit HprofParser(ByteIterator* iterator,
                       bool detect_string_classes = true)
      : byte_iterator_(iterator),
        identifier_size_(0),
        current_heap_(HPROF_HEAP_DEFAULT),
        detect_string_class_(detect_string_classes) {}

  /**
   * Destructor.
   */
  virtual ~HprofParser();

  /**
   * Parse the HPROF data into an AST.
   *
   * @return The parsed AST
   */
  HprofAst Parse();

 private:
  ByteIterator* byte_iterator_;
  uint32_t identifier_size_;
  HprofAst ast_;
  HprofHeapId current_heap_;
  bool detect_string_class_;

  // ----------- Helper methods -----------
  bool IsStringClass(const std::string& class_name) const;
  size_t GetFieldTypeSize(uint8_t type) const;
  int8_t ReadByteValue(const std::vector<uint8_t>& data, size_t offset) const;
  bool ReadBooleanValue(const std::vector<uint8_t>& data, size_t offset) const;
  int16_t ReadShortValue(const std::vector<uint8_t>& data, size_t offset) const;
  char16_t ReadCharValue(const std::vector<uint8_t>& data, size_t offset) const;
  int32_t ReadIntValue(const std::vector<uint8_t>& data, size_t offset) const;
  float ReadFloatValue(const std::vector<uint8_t>& data, size_t offset) const;
  int64_t ReadLongValue(const std::vector<uint8_t>& data, size_t offset) const;
  double ReadDoubleValue(const std::vector<uint8_t>& data, size_t offset) const;
  uint64_t ReadObjectIDValue(const std::vector<uint8_t>& data,
                             size_t offset,
                             uint32_t id_size) const;
  FieldValue ExtractFieldValue(const std::vector<uint8_t>& data,
                               size_t offset,
                               uint8_t field_type);
  void ExtractInstanceFields(InstanceDumpData& instance_data,
                             const ClassInfo& class_info);
  void ExtractStringInstance(InstanceDumpData& instance_data,
                             const ClassInfo& class_info);
  void UpdateHeapStats(HprofHeapId heap_id, size_t object_size);
  void SkipUnknownSubRecord(uint8_t sub_tag, std::streampos end_pos);
  std::vector<FieldInfo> GetFieldsForClassHierarchy(uint64_t class_object_id);

  // ----------- Main parsing methods -----------
  bool ParseHeader();
  void ParseRecords();
  void ParseRecord(uint8_t tag, uint32_t time, uint32_t length);

  // ----------- Record type specific parsing methods -----------
  void ParseUtf8Record(HprofRecord& record);
  void ParseLoadClassRecord(HprofRecord& record);
  void ParseHeapDumpRecord(HprofRecord& record);
  bool ParseHeapSubRecord(uint8_t sub_tag,
                          std::vector<HprofHeapRecord>& sub_records);

  // ----------- Heap sub-record parsing methods -----------
  void ParseRootJniGlobal(HprofHeapRecord& record);
  void ParseRootWithThread(HprofHeapRecord& record);
  void ParseSimpleRoot(HprofHeapRecord& record);
  void ParseThreadObjectRoot(HprofHeapRecord& record);
  void ParseHeapDumpInfo(HprofHeapRecord& record);
  void ParseClassDump(HprofHeapRecord& record);
  void ParseInstanceDump(HprofHeapRecord& record);
  void ParseObjectArrayDump(HprofHeapRecord& record);
  void ParsePrimitiveArrayDump(HprofHeapRecord& record);
};

/**
 * Converter from HPROF AST to heap graph IR.
 */
class HprofAstConverter {
 public:
  /**
   * Convert a HPROF AST to heap graph IR.
   *
   * @param ast The HPROF AST to convert
   * @return The converted heap graph IR
   */
  HeapGraphIR ConvertToIR(const HprofAst& ast);

 private:
  /**
   * Diagnostic tracking structure.
   */
  struct ConversionDiagnostics {
    std::unordered_map<std::string, size_t> class_kind_counts;
    std::unordered_map<std::string, size_t> superclass_chain_lengths;
    size_t total_processed_classes = 0;
    size_t unique_classes_processed = 0;
    size_t references_generated = 0;
  };

  ConversionDiagnostics diagnostics_;
  uint32_t next_reference_set_id_ = 1;
  std::unordered_map<uint64_t, uint32_t> object_to_reference_set_id_;

  void ConvertClasses(const HprofAst& ast, HeapGraphIR& ir);
  void ConvertObjects(const HprofAst& ast, HeapGraphIR& ir);
  void ConvertReferences(const HprofAst& ast, HeapGraphIR& ir);
  HeapGraphValue ConvertFieldValue(const FieldValue& value);
  std::string DetermineClassKind(const std::string& class_name) const;
  void PrintConversionDiagnostics();
  std::string RootTypeToString(uint8_t root_type);
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
    std::streampos GetPosition() override;
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
  std::optional<HprofAst> parser_result_;
  std::unique_ptr<HprofAstConverter> converter_;
  std::optional<HeapGraphIR> ir_;

  bool is_initialized_ = false;
  bool is_complete_ = false;
};

}  // namespace perfetto::trace_processor::art_hprof

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_TOKENIZER_H_
