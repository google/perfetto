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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_MODEL_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_MODEL_H_

#include "src/trace_processor/importers/art_hprof/art_hprof_types.h"
#include "src/trace_processor/storage/trace_storage.h"

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/trace_processor/trace_blob_view.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace perfetto::trace_processor::art_hprof {

// Field class with value storage using std::variant
class Field {
 public:
  using ValueType = std::variant<std::monostate,  // For no value
                                 bool,            // BOOLEAN
                                 uint8_t,         // BYTE
                                 uint16_t,        // CHAR
                                 int16_t,         // SHORT
                                 int32_t,         // INT
                                 int64_t,         // LONG
                                 float,           // FLOAT
                                 double,          // DOUBLE
                                 uint64_t         // OBJECT (reference ID)
                                 >;

  Field(StringId name, FieldType type) : name_(name), type_(type) {}

  template <typename T>
  Field(StringId name, FieldType type, T value)
      : name_(name), type_(type), value_(value) {}

  Field(const Field&) = default;
  Field& operator=(const Field&) = default;
  Field(Field&&) = default;
  Field& operator=(Field&&) = default;
  ~Field() = default;

  StringId GetName() const { return name_; }
  FieldType GetType() const { return type_; }
  bool HasValue() const {
    return !std::holds_alternative<std::monostate>(value_);
  }

  // Template setter for all supported types
  template <typename T>
  void SetValue(T value) {
    value_ = value;
  }

  // Type-safe getter template
  template <typename T>
  std::optional<T> GetValue() const {
    if (const T* ptr = std::get_if<T>(&value_)) {
      return *ptr;
    }
    return std::nullopt;
  }

  // Get numeric value as int64_t (useful for sizes)
  int64_t GetNumericValue() const {
    return std::visit(
        [](auto&& val) -> int64_t {
          using T = std::decay_t<decltype(val)>;
          if constexpr (std::is_same_v<T, std::monostate>)
            return 0;
          else if constexpr (std::is_same_v<T, bool>)
            return val ? 1 : 0;
          else
            return static_cast<int64_t>(val);
        },
        value_);
  }

 private:
  StringId name_;
  FieldType type_;
  ValueType value_ = std::monostate{};
};

// Index of an object in the ObjectStore. Kept as a 32 bit index rather than
// an hprof object id so that following a reference is an array lookup instead
// of a hash map probe.
using ObjectIndex = uint32_t;
constexpr ObjectIndex kInvalidObjectIndex =
    std::numeric_limits<ObjectIndex>::max();

struct Reference {
  StringId field_name;
  // Index of the referred-to object, or kInvalidObjectIndex if the target is
  // not present in the dump.
  ObjectIndex target_index;
  // Referent of a weak/phantom/finalizer reference: not followed when
  // computing reachability.
  bool is_weak_referent;

  Reference(StringId name, ObjectIndex target, bool weak_referent = false)
      : field_name(name),
        target_index(target),
        is_weak_referent(weak_referent) {}
};

// A static field reference seen while parsing, before the objects it points at
// have been read.
struct PendingReference {
  StringId field_name;
  uint64_t target_id;

  PendingReference(StringId name, uint64_t target)
      : field_name(name), target_id(target) {}
};

class ClassDefinition {
 public:
  ClassDefinition(uint64_t id, std::string name)
      : id_(id), name_(std::move(name)) {}

  ClassDefinition() = default;

  ClassDefinition(const ClassDefinition&) = default;
  ClassDefinition& operator=(const ClassDefinition&) = default;
  ClassDefinition(ClassDefinition&&) = default;
  ClassDefinition& operator=(ClassDefinition&&) = default;
  ~ClassDefinition() = default;

  uint64_t GetId() const { return id_; }
  const std::string& GetName() const { return name_; }
  uint64_t GetSuperClassId() const { return super_class_id_; }
  uint32_t GetInstanceSize() const { return instance_size_; }
  const std::vector<Field>& GetInstanceFields() const {
    return instance_fields_;
  }

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

class Object {
 public:
  Object(uint64_t id, uint64_t class_id, StringId heap, ObjectType type)
      : id_(id), class_id_(class_id), type_(type), heap_type_(heap) {}

  Object() = default;

  Object(const Object&) = delete;
  Object& operator=(const Object&) = delete;
  Object(Object&&) = default;
  Object& operator=(Object&&) = default;
  ~Object() = default;

  uint64_t GetId() const { return id_; }
  uint64_t GetClassId() const { return class_id_; }
  StringId GetHeapType() const { return heap_type_; }
  ObjectType GetObjectType() const { return type_; }

  void SetRootType(HprofHeapRootTag root_type) {
    root_type_ = root_type;
    is_root_ = true;
  }

  void SetReachable() { is_reachable_ = true; }

  void SetHeapType(StringId heap_type) { heap_type_ = heap_type; }

  bool IsRoot() const { return is_root_; }
  bool IsReachable() const { return is_reachable_; }
  std::optional<HprofHeapRootTag> GetRootType() const { return root_type_; }

  void SetRootDistance(int32_t d) { root_distance_ = d; }
  int32_t GetRootDistance() const { return root_distance_; }

  // Instance data / object array element ids. This is a view onto the trace
  // bytes themselves: on native builds, where the trace is mmapped, no copy
  // of the data is ever made. It is released when the graph is torn down at
  // the end of the import, so it never pins trace chunks beyond that.
  void SetRawData(TraceBlobView data) { raw_data_ = std::move(data); }

  const uint8_t* GetRawData() const { return raw_data_.data(); }
  size_t GetRawDataSize() const { return raw_data_.size(); }
  const TraceBlobView& GetRawDataView() const { return raw_data_; }
  void ClearRawData() { raw_data_ = TraceBlobView(); }

  void AddReference(StringId field_name,
                    ObjectIndex target_index,
                    bool is_weak_referent = false) {
    references_.emplace_back(field_name, target_index, is_weak_referent);
  }

  void ReserveReferences(size_t count) { references_.reserve(count); }

  void AddPendingReference(StringId field_name, uint64_t target_id) {
    pending_references_.emplace_back(field_name, target_id);
  }

  const std::vector<Reference>& GetReferences() const { return references_; }

  const std::vector<PendingReference>& GetPendingReferences() const {
    return pending_references_;
  }

  // Array-specific data
  void SetArrayElementCount(uint32_t count) { array_element_count_ = count; }

  void SetArrayElementType(FieldType type) { array_element_type_ = type; }

  // Size in bytes of the array payload as it appeared in the dump. Kept
  // separately so the raw bytes can be dropped once they have been decoded.
  void SetArrayDataBytes(uint32_t bytes) { array_data_bytes_ = bytes; }
  uint32_t GetArrayDataBytes() const { return array_data_bytes_; }

  FieldType GetArrayElementType() const { return array_element_type_; }

  void AddField(Field field) { fields_.push_back(field); }

  void ReserveFields(size_t count) { fields_.reserve(count); }

  const std::vector<Field>& GetFields() const { return fields_; }

  int64_t GetNativeSize() const { return native_size_; }

  void AddNativeSize(int64_t size) { native_size_ += size; }

  void SetDecodedString(StringId str) { decoded_string_ = str; }
  std::optional<StringId> GetDecodedString() const {
    return decoded_string_.is_null() ? std::nullopt
                                     : std::make_optional(decoded_string_);
  }

  void SetSelfSizeOverride(size_t size) { self_size_override_ = size; }
  std::optional<size_t> GetSelfSizeOverride() const {
    return self_size_override_;
  }

  // Primitive array payload, decoded to native endianness at parse time. This
  // is the only copy of the data: it is handed to the storage as-is.
  void SetArrayData(TraceBlobView data, uint32_t element_count) {
    array_data_ = std::move(data);
    array_element_count_ = element_count;
  }

  bool HasArrayData() const { return array_data_.size() > 0; }

  const TraceBlobView& GetArrayData() const { return array_data_; }

  size_t GetArrayElementCount() const { return array_element_count_; }

 private:
  uint64_t id_ = 0;
  uint64_t class_id_ = 0;
  ObjectType type_ = ObjectType::kInstance;
  bool is_root_ = false;
  bool is_reachable_ = false;
  int32_t root_distance_ = -1;
  std::optional<HprofHeapRootTag> root_type_;
  StringId heap_type_;

  // Data storage - used differently based on object type
  TraceBlobView raw_data_;
  std::vector<Reference> references_;
  std::vector<PendingReference> pending_references_;
  uint32_t array_element_count_ = 0;
  uint32_t array_data_bytes_ = 0;
  FieldType array_element_type_ = FieldType::kObject;

  int64_t native_size_ = 0;
  std::optional<size_t> self_size_override_;
  StringId decoded_string_ = StringId::Null();

  // Field values
  std::vector<Field> fields_;
  TraceBlobView array_data_;
};

// An object-typed field, at its fixed offset in the instance data.
struct ObjectFieldRef {
  StringId name;
  uint32_t offset;
  bool is_weak_referent;
};

// Fully qualified instance field layout of a class hierarchy. Computed once
// per class, never per object.
struct ClassFieldLayout {
  // All fields, in the order they appear in the instance data.
  std::vector<Field> fields;
  // Just the object-typed fields, with their offsets precomputed so that
  // reference extraction does not have to walk over the primitive ones.
  std::vector<ObjectFieldRef> object_fields;
};

using ClassFieldLayouts = base::FlatHashMap<uint64_t, ClassFieldLayout>;

// Stores all the objects in a dump contiguously, with a side map from hprof
// object id to index. Keeping the (large) Object payloads out of the hash map
// keeps probes cache friendly and avoids moving objects around on rehash.
class ObjectStore {
 public:
  Object* Find(uint64_t id) {
    ObjectIndex* idx = index_.Find(id);
    return idx ? &objects_[*idx] : nullptr;
  }

  const Object* Find(uint64_t id) const {
    const ObjectIndex* idx = index_.Find(id);
    return idx ? &objects_[*idx] : nullptr;
  }

  ObjectIndex FindIndex(uint64_t id) const {
    const ObjectIndex* idx = index_.Find(id);
    return idx ? *idx : kInvalidObjectIndex;
  }

  // Returns the object with `id`, default constructing it if it does not
  // exist yet.
  Object& operator[](uint64_t id) {
    auto res = index_.Insert(id, static_cast<ObjectIndex>(objects_.size()));
    if (res.second) {
      objects_.emplace_back();
    }
    return objects_[*res.first];
  }

  Object& at(ObjectIndex index) { return objects_[index]; }
  const Object& at(ObjectIndex index) const { return objects_[index]; }

  size_t size() const { return objects_.size(); }

  void Clear() {
    index_.Clear();
    objects_.clear();
    objects_.shrink_to_fit();
  }

 private:
  base::FlatHashMap<uint64_t, ObjectIndex> index_;
  std::vector<Object> objects_;
};

}  // namespace perfetto::trace_processor::art_hprof

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_ART_HPROF_ART_HPROF_MODEL_H_
