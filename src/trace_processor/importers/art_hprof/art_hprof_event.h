/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_EVENT_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_EVENT_H_

#include "src/trace_processor/sorter/trace_sorter.h"
#include "perfetto/base/status.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/art_hprof/art_hprof_event.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace perfetto::trace_processor::art_hprof {
// HPROF format constants
constexpr uint32_t kHprofHeaderMagic = 0x4A415641; // "JAVA" in ASCII
constexpr size_t kHprofHeaderLength = 20; // Header size in bytes

// Forward declarations
class ByteIterator;
class HeapGraph;

// HPROF format constants
enum HprofTag : uint8_t {
  HPROF_TAG_UTF8 = 0x01,
  HPROF_TAG_LOAD_CLASS = 0x02,
  HPROF_TAG_FRAME = 0x04,
  HPROF_TAG_TRACE = 0x05,
  HPROF_TAG_HEAP_DUMP = 0x0C,
  HPROF_TAG_HEAP_DUMP_SEGMENT = 0x1C,
  HPROF_TAG_HEAP_DUMP_END = 0x2C
};

enum HprofHeapTag : uint8_t {
  HPROF_HEAP_TAG_ROOT_JNI_GLOBAL = 0x01,
  HPROF_HEAP_TAG_ROOT_JNI_LOCAL = 0x02,
  HPROF_HEAP_TAG_ROOT_JAVA_FRAME = 0x03,
  HPROF_HEAP_TAG_ROOT_NATIVE_STACK = 0x04,
  HPROF_HEAP_TAG_ROOT_STICKY_CLASS = 0x05,
  HPROF_HEAP_TAG_ROOT_THREAD_BLOCK = 0x06,
  HPROF_HEAP_TAG_ROOT_MONITOR_USED = 0x07,
  HPROF_HEAP_TAG_ROOT_THREAD_OBJ = 0x08,
  HPROF_HEAP_TAG_CLASS_DUMP = 0x20,
  HPROF_HEAP_TAG_INSTANCE_DUMP = 0x21,
  HPROF_HEAP_TAG_OBJ_ARRAY_DUMP = 0x22,
  HPROF_HEAP_TAG_PRIM_ARRAY_DUMP = 0x23,
  HPROF_HEAP_TAG_HEAP_DUMP_INFO = 0xFE,
  HPROF_HEAP_TAG_ROOT_INTERNED_STRING = 0x89,  // Android
  HPROF_HEAP_TAG_ROOT_FINALIZING = 0x8A,      // Android
  HPROF_HEAP_TAG_ROOT_DEBUGGER = 0x8B,        // Android
  HPROF_HEAP_TAG_ROOT_VM_INTERNAL = 0x8D,     // Android
  HPROF_HEAP_TAG_ROOT_JNI_MONITOR = 0x8E,     // Android
  HPROF_HEAP_TAG_ROOT_UNKNOWN = 0xFF
};

enum FieldType : uint8_t {
  FIELD_TYPE_OBJECT = 2,
  FIELD_TYPE_BOOLEAN = 4,
  FIELD_TYPE_CHAR = 5,
  FIELD_TYPE_FLOAT = 6,
  FIELD_TYPE_DOUBLE = 7,
  FIELD_TYPE_BYTE = 8,
  FIELD_TYPE_SHORT = 9,
  FIELD_TYPE_INT = 10,
  FIELD_TYPE_LONG = 11
};

enum HeapType : uint8_t {
  HEAP_TYPE_DEFAULT = 0,
  HEAP_TYPE_APP = 1,
  HEAP_TYPE_ZYGOTE = 2,
  HEAP_TYPE_IMAGE = 3,
  HEAP_TYPE_JIT = 4,
  HEAP_TYPE_APP_CACHE = 5,
  HEAP_TYPE_SYSTEM = 6
};

enum ObjectType : uint8_t {
  OBJECT_TYPE_CLASS = 0,
  OBJECT_TYPE_INSTANCE = 1,
  OBJECT_TYPE_OBJECT_ARRAY = 2,
  OBJECT_TYPE_PRIMITIVE_ARRAY = 3
};

struct Reference {
  uint64_t owner_id;
  uint64_t target_id;
  std::string field_name;
  uint64_t field_class_id;  // Store class ID instead of field type

  Reference(uint64_t owner, const std::string& name, uint64_t class_id, uint64_t target)
      : owner_id(owner), target_id(target), field_name(name), field_class_id(class_id) {}
};

// Field definition
class Field {
 public:
  Field(std::string name, FieldType type) : name_(std::move(name)), type_(type) {}

  const std::string& name() const { return name_; }
  FieldType type() const { return type_; }
  size_t GetSize() const;  // Returns size in bytes based on type

 private:
  std::string name_;
  FieldType type_;
};

// Abstract byte reader interface
class ByteIterator {
 public:
  virtual ~ByteIterator();

  // Basic read operations
  virtual bool ReadU1(uint8_t& value) = 0;
  virtual bool ReadU2(uint16_t& value) = 0;
  virtual bool ReadU4(uint32_t& value) = 0;
  virtual bool ReadId(uint64_t& value, uint32_t id_size) = 0;
  virtual bool ReadString(std::string& str, size_t length) = 0;
  virtual bool ReadBytes(std::vector<uint8_t>& data, size_t length) = 0;
  virtual bool SkipBytes(size_t count) = 0;

  // Position and status
  virtual size_t GetPosition() = 0;
  virtual bool IsEof() const = 0;
  virtual bool IsValid() const = 0;
};

// Class definition
class ClassDefinition {
 public:
  ClassDefinition(uint64_t id, std::string name)
      : id_(id), name_(std::move(name)) {}

  ClassDefinition() = default;
  // Getters
  uint64_t id() const { return id_; }
  const std::string& name() const { return name_; }
  uint64_t super_class_id() const { return super_class_id_; }
  uint32_t instance_size() const { return instance_size_; }
  const std::vector<Field>& instance_fields() const { return instance_fields_; }

  // Setters
  void SetSuperClassId(uint64_t id) { super_class_id_ = id; }
  void SetInstanceSize(uint32_t size) { instance_size_ = size; }
  void SetInstanceFields(std::vector<Field> fields) {
    instance_fields_ = std::move(fields);
  }

  void AddInstanceField(Field field) {
    instance_fields_.push_back(std::move(field));
  }
  bool IsStringClass() const { return name_ == "java.lang.String"; }

 private:
  uint64_t id_;
  std::string name_;
  uint64_t super_class_id_ = 0;
  uint32_t instance_size_ = 0;
  std::vector<Field> instance_fields_;
};

// HPROF object representation
class HprofObject {
 public:
  HprofObject(uint64_t id,
              uint64_t class_id,
              HeapType heap,
              ObjectType type)
      : id_(id), class_id_(class_id), heap_(heap), type_(type) {}

  HprofObject() = default;

  // Core properties
  uint64_t id() const { return id_; }
  uint64_t class_id() const { return class_id_; }
  HeapType heap_type() const { return heap_; }
  ObjectType object_type() const { return type_; }

  // Root handling
  void SetRootType(HprofHeapTag root_type) {
    root_type_ = root_type;
    is_root_ = true;
  }

  bool is_root() const { return is_root_; }
  std::optional<HprofHeapTag> root_type() const { return root_type_; }

  // Instance-specific data
  void SetRawData(std::vector<uint8_t> data) { raw_data_ = std::move(data); }
  const std::vector<uint8_t>& raw_data() const { return raw_data_; }

  // References
  void AddReference(const std::string& field_name, uint64_t field_class_id, uint64_t target_id);

  const std::vector<Reference>& references() const { return references_; }

  // Array-specific data
  void SetArrayElements(std::vector<uint64_t> elements) {
    array_elements_ = std::move(elements);
  }

  void SetArrayElementType(FieldType type) { array_element_type_ = type; }

  const std::vector<uint64_t>& array_elements() const { return array_elements_; }
  FieldType array_element_type() const { return array_element_type_; }

  // Size calculation
  size_t GetSize() const {
    // For instances and primitive arrays, use raw data size
    if (type_ == ObjectType::OBJECT_TYPE_INSTANCE ||
        (type_ == ObjectType::OBJECT_TYPE_PRIMITIVE_ARRAY && !raw_data_.empty())) {
      return raw_data_.size();
    }

    // For object arrays, use element count * id size
    if (type_ == ObjectType::OBJECT_TYPE_OBJECT_ARRAY) {
      // Assuming id size is constant within an HPROF file and corresponds to
      // the size of object references in arrays. Use sizeof(uint64_t) for
      // safety if id_size_ from header is not directly available here, or pass
      // it in if needed.
      // NOTE: The original code used sizeof(uint64_t) which might be incorrect
      // if the HPROF's actual id_size is 4. We should ideally use the parser's
      // id_size. For now, keeping original logic.
      return array_elements_.size() * sizeof(uint64_t);
    }

    // Default size (e.g., for CLASS objects)
    return 0;
  }

 private:
  uint64_t id_;
  uint64_t class_id_;
  HeapType heap_;
  ObjectType type_;
  bool is_root_ = false;
  std::optional<HprofHeapTag> root_type_;

  // Data storage - used differently based on object type
  std::vector<uint8_t> raw_data_;
  std::vector<Reference> references_;
  std::vector<uint64_t> array_elements_;
  FieldType array_element_type_ = FIELD_TYPE_OBJECT;
};

// HPROF file header
class HprofHeader {
 public:
  HprofHeader() = default;

  void SetFormat(std::string format) { format_ = std::move(format); }
  void SetIdSize(uint32_t size) { id_size_ = size; }
  void SetTimestamp(uint64_t timestamp) { timestamp_ = timestamp; }

  const std::string& format() const { return format_; }
  uint32_t id_size() const { return id_size_; }
  uint64_t timestamp() const { return timestamp_; }

 private:
  std::string format_;
  uint32_t id_size_ = 4;
  uint64_t timestamp_ = 0;
};

// Main HPROF parser class
class HprofParser {
 public:
  explicit HprofParser(std::unique_ptr<ByteIterator> iterator);
  ~HprofParser();

  // Parse the HPROF file and build heap graph
  HeapGraph Parse();
 private:
  // Parsing helper methods
  bool ParseHeader();
  bool ParseRecord();
  bool ParseHeapDump(size_t length);
  bool ParseHeapDumpRecord();

  // Record type handlers
  bool HandleUtf8Record(uint32_t length);
  bool HandleLoadClassRecord();
  bool HandleHeapDumpInfoRecord();
  bool HandleClassDumpRecord();
  bool HandleInstanceDumpRecord();
  bool HandleObjectArrayDumpRecord();
  bool HandlePrimitiveArrayDumpRecord();
  bool HandleRootRecord(uint8_t tag);

  // Reference extraction
  bool ExtractReferences(HprofObject& obj, const ClassDefinition& cls);

  // Utility methods
  std::vector<Field> GetFieldsForClassHierarchy(uint64_t class_id);
  size_t GetFieldTypeSize(FieldType type) const;
  std::string GetString(uint64_t id) const;
  std::string GetHeapName(HeapType type) const;

  // Heap graph building
  HeapGraph BuildHeapGraph();

  void FixupObjectReferencesAndRoots();

  // Data members
  std::unique_ptr<ByteIterator> iterator_;
  HprofHeader header_;
  std::vector<std::string> errors_;

  // Current parsing state
  HeapType current_heap_ = HeapType::HEAP_TYPE_DEFAULT;

  // Data collections
  std::unordered_map<uint64_t, std::string> strings_;
  std::unordered_map<uint64_t, ClassDefinition> classes_;
  std::unordered_map<uint64_t, HprofObject> objects_;
  std::unordered_map<HeapType, std::string> heap_names_;

  // Stats
  size_t string_count_ = 0;
  size_t class_count_ = 0;
  size_t heap_dump_count_ = 0;
  size_t instance_count_ = 0;
  size_t object_array_count_ = 0;
  size_t primitive_array_count_ = 0;
  size_t root_count_ = 0;
  size_t reference_count_ = 0;
};

// Heap graph implementation
class HeapGraph {
public:
    HeapGraph() = default;

    void AddObject(HprofObject object);
    void AddClass(ClassDefinition cls);
    void AddString(uint64_t id, std::string string);
    std::string GetString(uint64_t id) const;

    const std::unordered_map<uint64_t, HprofObject>& GetObjects() const;
    const std::unordered_map<uint64_t, ClassDefinition>& GetClasses() const;

    size_t GetObjectCount() const;
    size_t GetClassCount() const;
    size_t GetStringCount() const;
    static std::string GetRootType(uint8_t root_type_id);
    static std::string GetHeapType(uint8_t heap_id);

    // Analysis methods
    void PrintObjectTypeDistribution() const;
    void PrintRootDistribution() const;
    void PrintTopClasses(size_t top_n = 10) const;
    bool ValidateReferences() const;
    void PrintReferenceStats() const;
    void PrintDetailedStats() const;

private:
    std::unordered_map<uint64_t, HprofObject> objects_;
    std::unordered_map<uint64_t, ClassDefinition> classes_;
    std::unordered_map<uint64_t, std::string> strings_;
};

/**
 * Event structure for HPROF data from the Android Runtime.
 */
struct ArtHprofEvent {
  // Process ID if applicable
  uint32_t pid = 0;

  // The parsed heap graph data
  HeapGraph data;

  // Constructor
  explicit ArtHprofEvent(HeapGraph&& ir) : data(std::move(ir)) {}
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
  std::optional<HeapGraph> parser_result_;

  bool is_initialized_ = false;
  bool is_complete_ = false;
};

}  // namespace perfetto::trace_processor::art_hprof

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_EVENT_H_
