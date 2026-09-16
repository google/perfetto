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

#include <algorithm>
#include <cstring>
#include <string>
#include <variant>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/art_hprof/art_heap_graph_builder.h"
#include "src/trace_processor/importers/common/stats_tracker.h"

namespace perfetto::trace_processor::art_hprof {

HeapGraphResolver::HeapGraphResolver(
    TraceProcessorContext* context,
    HprofHeader& header,
    ObjectStore& objects,
    base::FlatHashMap<uint64_t, ClassDefinition>& classes,
    base::FlatHashMap<uint64_t, HprofHeapRootTag>& roots,
    ClassFieldLayouts& class_fields,
    uint64_t string_class_id,
    DebugStats& stats)
    : context_(context),
      header_(header),
      objects_(objects),
      roots_(roots),
      classes_(classes),
      class_fields_(class_fields),
      stats_(stats),
      referent_field_id_(
          context->storage->InternString("java.lang.ref.Reference.referent")),
      string_value_field_id_(
          context->storage->InternString("java.lang.String.value")),
      string_offset_field_id_(
          context->storage->InternString("java.lang.String.offset")),
      string_count_field_id_(
          context->storage->InternString("java.lang.String.count")),
      cleaner_thunk_field_id_(
          context->storage->InternString("sun.misc.Cleaner.thunk")),
      cleaner_thunk_outer_field_id_(context->storage->InternString(
          "libcore.util.NativeAllocationRegistry$CleanerThunk.this$0")),
      native_registry_size_field_id_(context->storage->InternString(
          "libcore.util.NativeAllocationRegistry.size")),
      string_class_id_(string_class_id) {}

void HeapGraphResolver::ResolveGraph() {
  BuildClassFieldLayouts();
  ExtractAllObjectData();
  DecodeJavaStrings();
  MarkReachableObjects();
  CalculateNativeSizes();
}

void HeapGraphResolver::ExtractAllObjectData() {
  // Match ahat's self_size computation:
  //   Instances: CLASS_DUMP.instanceSize (not INSTANCE_DUMP.data_length).
  //   Class objects: java.lang.Class.instanceSize + staticFieldsSize.
  //   Arrays: CLASS_DUMP.instanceSize (header) + element_count * element_size.
  uint32_t java_lang_class_size = 0;
  uint64_t cleaner_class_id = 0;
  for (auto it = classes_.GetIterator(); it; ++it) {
    if (it.value().GetName() == "java.lang.Class") {
      java_lang_class_size = it.value().GetInstanceSize();
    } else if (it.value().GetName() == kSunMiscCleaner) {
      cleaner_class_id = it.key();
    }
  }

  const uint32_t id_size = header_.GetIdSize();
  for (ObjectIndex i = 0; i < objects_.size(); ++i) {
    auto& obj = objects_.at(i);
    auto* cls = classes_.Find(obj.GetClassId());
    switch (obj.GetObjectType()) {
      case ObjectType::kInstance:
      case ObjectType::kClass: {
        if (cls) {
          ExtractObjectReferences(obj, *cls);
        }
        if (obj.GetObjectType() == ObjectType::kClass) {
          size_t size = java_lang_class_size;
          for (const auto& field : obj.GetFields()) {
            size += GetFieldTypeSize(field.GetType(), id_size);
          }
          obj.SetSelfSizeOverride(size);
        } else if (cls) {
          obj.SetSelfSizeOverride(cls->GetInstanceSize());
        }
        // Collect string objects for DecodeJavaStrings().
        if (string_class_id_ != 0 && obj.GetClassId() == string_class_id_) {
          string_object_indices_.push_back(i);
        }
        // Collect sun.misc.Cleaner objects for CalculateNativeSizes().
        if (cleaner_class_id != 0 && obj.GetClassId() == cleaner_class_id) {
          ObjectIndex referent = kInvalidObjectIndex;
          ObjectIndex thunk = kInvalidObjectIndex;
          for (const auto& ref : obj.GetReferences()) {
            if (ref.field_name == referent_field_id_) {
              referent = ref.target_index;
            } else if (ref.field_name == cleaner_thunk_field_id_) {
              thunk = ref.target_index;
            }
          }
          if (referent != kInvalidObjectIndex && thunk != kInvalidObjectIndex) {
            cleaners_.emplace_back(referent, thunk);
          }
        }
        break;
      }
      case ObjectType::kObjectArray:
        ExtractArrayElementReferences(obj);
        if (cls) {
          obj.SetSelfSizeOverride(cls->GetInstanceSize() +
                                  obj.GetArrayElementCount() * id_size);
        }
        break;
      case ObjectType::kPrimitiveArray:
        if (cls) {
          obj.SetSelfSizeOverride(cls->GetInstanceSize() +
                                  obj.GetArrayDataBytes());
        }
        break;
    }

    uint64_t obj_id = obj.GetId();
    auto pending = roots_.Find(obj_id);
    if (pending) {
      obj.SetRootType(*pending);
      roots_.Erase(obj_id);
    }
  }
}

void HeapGraphResolver::MarkReachableObjects() {
  // BFS from roots to mark reachability and compute shortest-path
  // root_distance. Edges flagged as weak/phantom/finalizer referents during
  // extraction are not followed.
  std::vector<ObjectIndex> queue;

  for (ObjectIndex i = 0; i < objects_.size(); ++i) {
    auto& obj = objects_.at(i);
    if (obj.IsRoot()) {
      queue.push_back(i);
      obj.SetReachable();
      obj.SetRootDistance(0);
    }
  }

  for (size_t head = 0; head < queue.size(); ++head) {
    Object& obj = objects_.at(queue[head]);
    int32_t next_distance = obj.GetRootDistance() + 1;
    for (const auto& ref : obj.GetReferences()) {
      if (ref.target_index == kInvalidObjectIndex || ref.is_weak_referent) {
        continue;
      }
      Object& target = objects_.at(ref.target_index);
      if (!target.IsReachable()) {
        target.SetReachable();
        target.SetRootDistance(next_distance);
        queue.push_back(ref.target_index);
      }
    }
  }
}

void HeapGraphResolver::ExtractArrayElementReferences(Object& obj) {
  const uint8_t* elements = obj.GetRawData();
  size_t elements_size = obj.GetRawDataSize();
  size_t count = obj.GetArrayElementCount();
  uint32_t id_size = header_.GetIdSize();
  char buf[24];
  obj.ReserveReferences(count);
  for (size_t i = 0; i < count; ++i) {
    uint64_t element_id = ReadBigEndian<uint64_t>(
        context_, elements, elements_size, i * id_size, id_size);
    if (element_id != 0) {
      ObjectIndex target = objects_.FindIndex(element_id);
      if (target != kInvalidObjectIndex) {
        size_t len = base::SprintfTrunc(buf, sizeof(buf), "[%zu]", i);
        obj.AddReference(
            context_->storage->InternString(base::StringView(buf, len)),
            target);
        stats_.reference_count++;
      }
    }
  }
  // The element ids are no longer needed once references have been created.
  obj.ClearRawData();
}

bool HeapGraphResolver::ExtractObjectReferences(Object& obj,
                                                const ClassDefinition& cls) {
  // Resolve pending static field references, qualifying names as
  // "ClassName.fieldName" to match the proto heap graph format.
  for (const auto& ref : obj.GetPendingReferences()) {
    ObjectIndex target = objects_.FindIndex(ref.target_id);
    if (target != kInvalidObjectIndex) {
      std::string qualified =
          cls.GetName() + "." +
          context_->storage->GetString(ref.field_name).ToStdString();
      obj.AddReference(context_->storage->InternString(qualified), target);
      stats_.reference_count++;
    }
  }

  const uint8_t* data = obj.GetRawData();
  size_t data_size = obj.GetRawDataSize();
  if (data_size == 0) {
    return true;
  }

  const auto* layout = class_fields_.Find(cls.GetId());
  if (!layout) {
    return true;
  }

  const uint32_t id_size = header_.GetIdSize();
  obj.ReserveReferences(obj.GetReferences().size() +
                        layout->object_fields.size());
  for (const auto& field : layout->object_fields) {
    if (field.offset >= data_size) {
      break;
    }
    if (field.offset + id_size > data_size) {
      context_->stats_tracker->IncrementStats(stats::hprof_reference_errors);
      break;
    }
    uint64_t target_id = ReadBigEndian<uint64_t>(context_, data, data_size,
                                                 field.offset, id_size);
    if (target_id != 0) {
      obj.AddReference(field.name, objects_.FindIndex(target_id),
                       field.is_weak_referent);
      stats_.reference_count++;
    }
  }

  return true;
}

std::optional<std::string> HeapGraphResolver::DecodeJavaString(
    const Object& string_obj) {
  auto cls = classes_.Find(string_obj.GetClassId());
  if (!cls || cls->GetName() != kJavaLangString)
    return std::nullopt;

  uint64_t value_array_id = 0;
  std::optional<int32_t> offset_opt;
  std::optional<int32_t> count_opt;

  const auto* layout = class_fields_.Find(string_obj.GetClassId());
  if (!layout)
    return std::nullopt;

  ForEachFieldValue(context_, layout->fields, string_obj.GetRawData(),
                    string_obj.GetRawDataSize(), header_.GetIdSize(),
                    [&](const Field& f) {
                      if (f.GetName() == string_value_field_id_) {
                        if (auto v = f.GetValue<uint64_t>())
                          value_array_id = *v;
                      } else if (f.GetName() == string_offset_field_id_) {
                        offset_opt = f.GetValue<int32_t>();
                      } else if (f.GetName() == string_count_field_id_) {
                        count_opt = f.GetValue<int32_t>();
                      }
                    });

  if (value_array_id == 0)
    return std::nullopt;

  const Object* array = objects_.Find(value_array_id);
  if (!array)
    return std::nullopt;

  size_t array_len = array->GetArrayElementCount();
  int32_t offset = offset_opt.value_or(0);
  int32_t count = count_opt.value_or(static_cast<int32_t>(array_len) - offset);

  if (offset < 0 || count < 0 ||
      static_cast<size_t>(offset) + static_cast<size_t>(count) > array_len)
    return std::nullopt;

  std::string result;
  result.reserve(static_cast<size_t>(count));

  auto append_utf8_from_utf16 = [&](uint16_t ch) {
    if (ch < 0x80) {
      result.push_back(static_cast<char>(ch));
    } else if (ch < 0x800) {
      result.push_back(static_cast<char>(0xC0 | (ch >> 6)));
      result.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else {
      result.push_back(static_cast<char>(0xE0 | (ch >> 12)));
      result.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    }
  };

  const uint8_t* array_data = array->GetArrayData().data();

  if (array->GetArrayElementType() == FieldType::kByte) {
    for (int32_t i = 0; i < count; ++i)
      result.push_back(static_cast<char>(array_data[offset + i]));
    return result;
  }
  if (array->GetArrayElementType() == FieldType::kChar) {
    for (int32_t i = 0; i < count; ++i) {
      uint16_t ch;
      memcpy(&ch,
             array_data + static_cast<size_t>(offset + i) * sizeof(uint16_t),
             sizeof(ch));
      append_utf8_from_utf16(ch);
    }
    return result;
  }

  return std::nullopt;
}

void HeapGraphResolver::DecodeJavaStrings() {
  if (string_class_id_ == 0)
    return;

  for (ObjectIndex idx : string_object_indices_) {
    Object& obj = objects_.at(idx);
    auto decoded = DecodeJavaString(obj);
    if (decoded) {
      obj.SetDecodedString(context_->storage->InternString(*decoded));
    }
  }
}

void HeapGraphResolver::BuildClassFieldLayouts() {
  // Reference subclasses whose "referent" edge must not be followed when
  // computing reachability. Matches ahat's retained=SOFT behavior: soft
  // referent edges are followed.
  base::FlatHashMap<uint64_t, bool> weak_ref_classes;
  {
    base::FlatHashMap<uint64_t, bool> base_refs;
    for (auto it = classes_.GetIterator(); it; ++it) {
      const auto& name = it.value().GetName();
      if (name == "java.lang.ref.WeakReference" ||
          name == "java.lang.ref.PhantomReference" ||
          name == "java.lang.ref.FinalizerReference") {
        base_refs[it.key()] = true;
      }
    }
    for (auto it = classes_.GetIterator(); it; ++it) {
      uint64_t current = it.key();
      for (int depth = 0; depth < 100 && current != 0; ++depth) {
        if (base_refs.Find(current)) {
          weak_ref_classes[it.key()] = true;
          break;
        }
        auto* cls = classes_.Find(current);
        if (!cls)
          break;
        current = cls->GetSuperClassId();
      }
    }
  }

  // HPROF instance data is laid out derived-class-first: the most-derived
  // class's fields come first, then its superclass fields, etc.
  // Field names are qualified as "ClassName.fieldName" to match the proto
  // heap graph format and allow MarkReachableObjects to recognize
  // "java.lang.ref.Reference.referent".
  uint32_t id_size = header_.GetIdSize();
  for (auto it = classes_.GetIterator(); it; ++it) {
    ClassFieldLayout layout;
    bool is_weak_ref = weak_ref_classes.Find(it.key()) != nullptr;
    uint32_t offset = 0;
    uint64_t current_class_id = it.key();
    while (current_class_id != 0) {
      auto cls = classes_.Find(current_class_id);
      if (!cls) {
        break;
      }
      for (const auto& f : cls->GetInstanceFields()) {
        std::string qualified =
            cls->GetName() + "." +
            context_->storage->GetString(f.GetName()).ToStdString();
        StringId name = context_->storage->InternString(qualified);
        layout.fields.emplace_back(name, f.GetType());
        if (f.GetType() == FieldType::kObject) {
          layout.object_fields.push_back(
              {name, offset, is_weak_ref && name == referent_field_id_});
        }
        offset += static_cast<uint32_t>(GetFieldTypeSize(f.GetType(), id_size));
      }
      current_class_id = cls->GetSuperClassId();
    }
    class_fields_[it.key()] = std::move(layout);
  }
}

// Attribute native_size to objects via the Cleaner→CleanerThunk→Registry chain:
//
//             +-------------------------------+  .referent   +--------+
//             |       sun.misc.Cleaner        | -----------> | Object |
//             +-------------------------------+              +--------+
//                |
//                | .thunk
//                v
// +----------------------------------------------------+
// | libcore.util.NativeAllocationRegistry$CleanerThunk |
// +----------------------------------------------------+
//   |
//   | .this$0
//   v
// +----------------------------------------------------+
// |       libcore.util.NativeAllocationRegistry        |
// |                       .size                        |
// +----------------------------------------------------+
//
// Registry.size is attributed as the native_size of Object.
// Matches ahat's AhatClassInstance.asRegisteredNativeAllocation().
void HeapGraphResolver::CalculateNativeSizes() {
  // Traverse cleaner chains to find NativeAllocationRegistry and attribute
  // size. The cleaners themselves were collected by ExtractAllObjectData().
  for (const auto& [referent_idx, thunk_idx] : cleaners_) {
    const Object& thunk = objects_.at(thunk_idx);

    // Verify thunk is a CleanerThunk (matches ahat check)
    auto thunk_cls = classes_.Find(thunk.GetClassId());
    if (!thunk_cls ||
        thunk_cls->GetName() != kNativeAllocationRegistryCleanerThunk) {
      continue;
    }

    ObjectIndex registry_idx = kInvalidObjectIndex;
    for (const auto& ref : thunk.GetReferences()) {
      if (ref.field_name == cleaner_thunk_outer_field_id_) {
        registry_idx = ref.target_index;
        break;
      }
    }

    if (registry_idx == kInvalidObjectIndex) {
      continue;
    }

    const Object& registry = objects_.at(registry_idx);

    // Verify registry is a NativeAllocationRegistry (matches ahat check)
    auto registry_cls = classes_.Find(registry.GetClassId());
    if (!registry_cls || registry_cls->GetName() != kNativeAllocationRegistry) {
      continue;
    }

    const auto* layout = class_fields_.Find(registry.GetClassId());
    if (!layout) {
      continue;
    }

    int64_t native_size = 0;
    ForEachFieldValue(context_, layout->fields, registry.GetRawData(),
                      registry.GetRawDataSize(), header_.GetIdSize(),
                      [&](const Field& f) {
                        if (f.GetName() == native_registry_size_field_id_) {
                          native_size = f.GetNumericValue();
                        }
                      });
    if (native_size <= 0) {
      continue;
    }

    objects_.at(referent_idx).AddNativeSize(native_size);
  }
}
}  // namespace perfetto::trace_processor::art_hprof
