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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_EVENT_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_EVENT_H_

#include "perfetto/base/status.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/art_hprof/art_hprof_event.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace perfetto::trace_processor::art_hprof {

// HPROF format constants
constexpr uint32_t kHprofHeaderMagic = 0x4A415641;  // "JAVA" in ASCII
constexpr size_t kHprofHeaderLength = 20;           // Header size in bytes

constexpr const char* kJavaLangObject = "java.lang.Object";
constexpr const char* kUnknownClassKind = "[unknown class kind]";
constexpr const char* kUnknownString = "[unknown string]";

// Forward declarations
class ByteIterator;
class HeapGraph;

// HPROF format constants - use enum class for type safety
enum class HprofTag : uint8_t {
  UTF8 = 0x01,
  LOAD_CLASS = 0x02,
  FRAME = 0x04,
  TRACE = 0x05,
  HEAP_DUMP = 0x0C,
  HEAP_DUMP_SEGMENT = 0x1C,
  HEAP_DUMP_END = 0x2C
};

enum class HprofHeapRootTag : uint8_t {
  JNI_GLOBAL = 0x01,
  JNI_LOCAL = 0x02,
  JAVA_FRAME = 0x03,
  NATIVE_STACK = 0x04,
  STICKY_CLASS = 0x05,
  THREAD_BLOCK = 0x06,
  MONITOR_USED = 0x07,
  THREAD_OBJ = 0x08,
  INTERNED_STRING = 0x89,  // Android
  FINALIZING = 0x8A,       // Android
  DEBUGGER = 0x8B,         // Android
  VM_INTERNAL = 0x8D,      // Android
  JNI_MONITOR = 0x8E,      // Android
  UNKNOWN = 0xFF
};

enum class HprofHeapTag : uint8_t {
  CLASS_DUMP = 0x20,
  INSTANCE_DUMP = 0x21,
  OBJ_ARRAY_DUMP = 0x22,
  PRIM_ARRAY_DUMP = 0x23,
  HEAP_DUMP_INFO = 0xFE
};

enum class FieldType : uint8_t {
  OBJECT = 2,
  BOOLEAN = 4,
  CHAR = 5,
  FLOAT = 6,
  DOUBLE = 7,
  BYTE = 8,
  SHORT = 9,
  INT = 10,
  LONG = 11
};

enum class ObjectType : uint8_t {
  CLASS = 0,
  INSTANCE = 1,
  OBJECT_ARRAY = 2,
  PRIMITIVE_ARRAY = 3
};

// Field definition
class Field {
 public:
  Field(std::string name, FieldType type)
      : name_(std::move(name)), type_(type) {}

  const std::string& name() const { return name_; }
  FieldType type() const { return type_; }

  // Returns size in bytes based on type
  size_t GetSize() const {
    switch (type_) {
      case FieldType::BOOLEAN:
      case FieldType::BYTE:
        return 1;
      case FieldType::CHAR:
      case FieldType::SHORT:
        return 2;
      case FieldType::FLOAT:
      case FieldType::INT:
      case FieldType::OBJECT:  // ID/reference
        return 4;
      case FieldType::DOUBLE:
      case FieldType::LONG:
        return 8;
    }
  }

 private:
  std::string name_;
  FieldType type_;
};

struct Reference {
  uint64_t owner_id;
  uint64_t target_id;
  std::string field_name;
  uint64_t field_class_id;  // Store class ID instead of field type

  Reference(uint64_t owner,
            std::string_view name,
            uint64_t class_id,
            uint64_t target)
      : owner_id(owner),
        target_id(target),
        field_name(name),
        field_class_id(class_id) {}
};

// Byte reader interface
class ByteIterator {
 public:
  virtual ~ByteIterator();

  // Read operations - simple boolean return types
  virtual bool ReadU1(uint8_t& value) = 0;
  virtual bool ReadU2(uint16_t& value) = 0;
  virtual bool ReadU4(uint32_t& value) = 0;
  virtual bool ReadId(uint64_t& value, uint32_t id_size) = 0;
  virtual bool ReadString(std::string& str, size_t length) = 0;
  virtual bool ReadBytes(std::vector<uint8_t>& data, size_t length) = 0;
  virtual bool SkipBytes(size_t count) = 0;

  // Position and status
  virtual size_t GetPosition() const = 0;
  virtual bool IsEof() const = 0;
  virtual bool IsValid() const = 0;
};

// Class definition
class ClassDefinition {
 public:
  ClassDefinition(uint64_t id, std::string name)
      : id_(id), name_(std::move(name)) {}

  ClassDefinition() = default;

  // Default copy/move operations
  ClassDefinition(const ClassDefinition&) = default;
  ClassDefinition& operator=(const ClassDefinition&) = default;
  ClassDefinition(ClassDefinition&&) = default;
  ClassDefinition& operator=(ClassDefinition&&) = default;
  ~ClassDefinition() = default;

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

 private:
  uint64_t id_ = 0;
  std::string name_;
  uint64_t super_class_id_ = 0;
  uint32_t instance_size_ = 0;
  std::vector<Field> instance_fields_;
};

// HPROF object representation
class HprofObject {
 public:
  HprofObject(uint64_t id, uint64_t class_id, std::string heap, ObjectType type)
      : id_(id),
        class_id_(class_id),
        type_(type),
        heap_type_(std::move(heap)) {}

  HprofObject() = default;

  // Default copy/move operations
  HprofObject(const HprofObject&) = default;
  HprofObject& operator=(const HprofObject&) = default;
  HprofObject(HprofObject&&) = default;
  HprofObject& operator=(HprofObject&&) = default;
  ~HprofObject() = default;

  // Core properties
  uint64_t id() const { return id_; }
  uint64_t class_id() const { return class_id_; }
  const std::string& heap_type() const { return heap_type_; }
  ObjectType object_type() const { return type_; }

  // Root handling
  void SetRootType(HprofHeapRootTag root_type) {
    root_type_ = root_type;
    is_root_ = true;
  }

  void SetHeapType(std::string heap_type) { heap_type_ = std::move(heap_type); }

  bool is_root() const { return is_root_; }
  std::optional<HprofHeapRootTag> root_type() const { return root_type_; }

  // Instance-specific data
  void SetRawData(std::vector<uint8_t> data) { raw_data_ = std::move(data); }

  const std::vector<uint8_t>& raw_data() const { return raw_data_; }

  // References
  void AddReference(std::string_view field_name,
                    uint64_t field_class_id,
                    uint64_t target_id) {
    references_.emplace_back(id_, field_name, field_class_id, target_id);
  }

  const std::vector<Reference>& references() const { return references_; }

  // Array-specific data
  void SetArrayElements(std::vector<uint64_t> elements) {
    array_elements_ = std::move(elements);
  }

  void SetArrayElementType(FieldType type) { array_element_type_ = type; }

  const std::vector<uint64_t>& array_elements() const {
    return array_elements_;
  }

  FieldType array_element_type() const { return array_element_type_; }

  // Size calculation with id size parameter to avoid assumptions
  size_t GetSize(uint32_t id_size = sizeof(uint64_t)) const {
    // For instances and primitive arrays, use raw data size
    if (type_ == ObjectType::INSTANCE ||
        (type_ == ObjectType::PRIMITIVE_ARRAY && !raw_data_.empty())) {
      return raw_data_.size();
    }

    // For object arrays, use element count * id size
    if (type_ == ObjectType::OBJECT_ARRAY) {
      return array_elements_.size() * id_size;
    }

    // Default size (e.g., for CLASS objects)
    return 0;
  }

 private:
  uint64_t id_ = 0;
  uint64_t class_id_ = 0;
  ObjectType type_ = ObjectType::INSTANCE;
  bool is_root_ = false;
  std::optional<HprofHeapRootTag> root_type_;
  std::string heap_type_;

  // Data storage - used differently based on object type
  std::vector<uint8_t> raw_data_;
  std::vector<Reference> references_;
  std::vector<uint64_t> array_elements_;
  FieldType array_element_type_ = FieldType::OBJECT;
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
  uint32_t id_size_ = 4;  // Default ID size
  uint64_t timestamp_ = 0;
};

// Main HPROF parser class
class HprofParser {
 public:
  explicit HprofParser(std::unique_ptr<ByteIterator> iterator);
  ~HprofParser();

  // Parse the HPROF file and build heap graph
  bool Parse();

  // Build and return the final heap graph
  HeapGraph BuildGraph();

 private:
  // Parsing helper methods - keep simple boolean return types
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
  bool HandleRootRecord(HprofHeapRootTag tag);

  // Reference extraction
  bool ExtractReferences(HprofObject& obj, const ClassDefinition& cls);

  // Utility methods
  std::vector<Field> GetFieldsForClassHierarchy(uint64_t class_id);
  size_t GetFieldTypeSize(FieldType type) const;
  std::string GetString(uint64_t id) const;
  static std::string NormalizeClassName(const std::string& name);

  // Heap graph building
  void FixupObjectReferencesAndRoots();

  // Data members
  std::unique_ptr<ByteIterator> iterator_;
  HprofHeader header_;

  // Current parsing state
  std::string current_heap_;

  // Data collections
  std::unordered_map<uint64_t, std::string> strings_;
  std::unordered_map<uint64_t, ClassDefinition> classes_;
  std::unordered_map<uint64_t, HprofObject> objects_;
  std::array<uint64_t, 12> prim_array_class_ids_ = {};
  std::unordered_map<uint64_t, HprofHeapRootTag> pending_roots_;

  // Stats for diagnostics
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
  HeapGraph(uint64_t timestamp) : timestamp_(timestamp) {}

  // Basic copy/move operations - disable copy, enable move
  HeapGraph(const HeapGraph&) = delete;
  HeapGraph& operator=(const HeapGraph&) = delete;
  HeapGraph(HeapGraph&&) = default;
  HeapGraph& operator=(HeapGraph&&) = default;
  ~HeapGraph() = default;

  void AddObject(HprofObject object);
  void AddClass(ClassDefinition cls);
  void AddString(uint64_t id, std::string string);
  std::string GetString(uint64_t id) const;

  const std::unordered_map<uint64_t, HprofObject>& GetObjects() const {
    return objects_;
  }

  const std::unordered_map<uint64_t, ClassDefinition>& GetClasses() const {
    return classes_;
  }

  size_t GetObjectCount() const { return objects_.size(); }
  size_t GetClassCount() const { return classes_.size(); }
  size_t GetStringCount() const { return strings_.size(); }
  uint64_t GetTimestamp() const { return timestamp_; }

  static std::string_view GetRootTypeName(HprofHeapRootTag root_type_id);

  // Statistics and validation
  void PrintStats() const;
  bool ValidateReferences() const;

 private:
  std::unordered_map<uint64_t, HprofObject> objects_;
  std::unordered_map<uint64_t, ClassDefinition> classes_;
  std::unordered_map<uint64_t, std::string> strings_;
  std::unordered_map<uint32_t, std::string> heap_id_to_name_;
  uint64_t timestamp_;
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
  explicit ArtHprofEvent(HeapGraph&& graph) : data(std::move(graph)) {}
};

/**
 * Tokenizer for ART HPROF data that handles chunked input.
 */
class ArtHprofTokenizer : public ChunkedTraceReader {
 public:
  explicit ArtHprofTokenizer(TraceProcessorContext* context);
  ~ArtHprofTokenizer() override;

  base::Status Parse(TraceBlobView blob) override;
  base::Status NotifyEndOfFile() override;

 private:
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
    size_t GetPosition() const override;
    bool IsEof() const override;
    bool IsValid() const override;

   private:
    util::TraceBlobViewReader reader_;
    size_t current_offset_ = 0;
  };

  TraceProcessorContext* const context_;
  util::TraceBlobViewReader reader_;

  // Parser components
  std::unique_ptr<ByteIterator> byte_iterator_;
  std::unique_ptr<HprofParser> parser_;
};

}  // namespace perfetto::trace_processor::art_hprof

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_EVENT_H_
