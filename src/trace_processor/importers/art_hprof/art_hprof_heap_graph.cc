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

#include "src/trace_processor/importers/art_hprof/art_hprof_heap_graph.h"
#include "src/trace_processor/sorter/trace_sorter.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace perfetto::trace_processor::art_hprof {

// Constants
constexpr uint32_t kHprofHeaderMagic = 0x4A415641;  // "JAVA"
constexpr uint32_t kHprofHeaderLength = 20;

// ArtHprofTokenizer implementation
ArtHprofTokenizer::ArtHprofTokenizer(TraceProcessorContext* ctx)
    : context_(ctx), sub_parser_(Detect{this}) {}

ArtHprofTokenizer::~ArtHprofTokenizer() = default;

base::Status ArtHprofTokenizer::Parse(TraceBlobView blob) {
  reader_.PushBack(std::move(blob));

  if (std::holds_alternative<Detect>(sub_parser_)) {
    return std::get<Detect>(sub_parser_).Parse();
  } else if (std::holds_alternative<Streaming>(sub_parser_)) {
    return std::get<Streaming>(sub_parser_).Parse();
  } else {
    return std::get<NonStreaming>(sub_parser_).Parse();
  }
}

base::Status ArtHprofTokenizer::InitializeParserIfNeeded() {
  if (is_initialized_)
    return base::OkStatus();

  // Create the ByteIterator that wraps our reader
  byte_iterator_ = std::make_unique<TraceBlobViewIterator>(std::move(reader_));

  // Initialize the parser with our iterator
  parser_ = std::make_unique<HprofParser>(byte_iterator_.get());

  is_initialized_ = true;
  return base::OkStatus();
}

base::Status ArtHprofTokenizer::ProcessParsingResults() {
  // Initialize parser if needed
  auto status = InitializeParserIfNeeded();
  if (!status.ok())
    return status;

  if (!parser_result_) {
    parser_result_ = parser_->Parse();
  }

  if (!parser_result_) {
    return base::ErrStatus("Parsing failed");
  }

  if (parser_result_ && !ir_) {
    // Convert hprof to HeapGraph
    converter_ = std::make_unique<HeapGraphBuilder>();
    ir_ = converter_->Build(*parser_result_);

    // Check if HeapGraph conversion was successful
    if (!ir_) {
      return base::ErrStatus("Failed to convert hprof to heap graph");
    }

    // Log some information about the HeapGraph to help diagnose issues
    PERFETTO_DLOG("HeapGraph contains %zu classes, %zu objects, %zu references",
                  ir_->classes.size(), ir_->objects.size(),
                  ir_->references.size());

    // Create and push the event
    const ArtHprofEvent event(*ir_);
    context_->sorter->PushArtHprofEvent(0, event);
  }

  return base::OkStatus();
}

base::Status ArtHprofTokenizer::NotifyEndOfFile() {
  is_complete_ = true;

  if (std::holds_alternative<Detect>(sub_parser_)) {
    return std::get<Detect>(sub_parser_).NotifyEndOfFile();
  } else if (std::holds_alternative<Streaming>(sub_parser_)) {
    return std::get<Streaming>(sub_parser_).NotifyEndOfFile();
  } else {
    return std::get<NonStreaming>(sub_parser_).NotifyEndOfFile();
  }
}

// Detect implementation
base::Status ArtHprofTokenizer::Detect::Parse() {
  auto it = tokenizer_->reader_.GetIterator();

  // Try to read the magic number to detect format
  auto header = it.MaybeRead(4);
  if (!header) {
    return base::OkStatus();  // Not enough data yet
  }

  uint32_t magic = 0;
  memcpy(&magic, header->data(), 4);

  // Check the endianness and set the correct parser
  if (magic == kHprofHeaderMagic) {
    tokenizer_->sub_parser_ = NonStreaming{tokenizer_};
    return std::get<NonStreaming>(tokenizer_->sub_parser_).Parse();
  } else {
    // Try to read as streaming format or use another detection method
    tokenizer_->sub_parser_ = Streaming{tokenizer_};
    return std::get<Streaming>(tokenizer_->sub_parser_).Parse();
  }
}

base::Status ArtHprofTokenizer::Detect::NotifyEndOfFile() const {
  return base::ErrStatus("HPROF format detection incomplete");
}

// NonStreaming implementation
base::Status ArtHprofTokenizer::NonStreaming::Parse() {
  if (is_parsing_)
    return base::OkStatus();  // Already parsing

  is_parsing_ = true;

  auto status = tokenizer_->ProcessParsingResults();
  if (!status.ok())
    return status;

  is_parsing_ = false;
  return base::OkStatus();
}

base::Status ArtHprofTokenizer::NonStreaming::NotifyEndOfFile() const {
  return tokenizer_->ProcessParsingResults();
}

// Streaming implementation
base::Status ArtHprofTokenizer::Streaming::Parse() {
  // In streaming mode, we might need to handle chunked data
  if (!header_parsed_) {
    auto it = tokenizer_->reader_.GetIterator();
    PERFETTO_CHECK(it.MaybeAdvance(it_offset_));

    // Read and validate header
    auto header = it.MaybeRead(kHprofHeaderLength);
    if (!header) {
      return base::OkStatus();  // Not enough data yet
    }

    // Process header and update state
    header_parsed_ = true;
    it_offset_ = it.file_offset();
  }

  // Process the rest of the data
  return tokenizer_->ProcessParsingResults();
}

base::Status ArtHprofTokenizer::Streaming::NotifyEndOfFile() {
  return tokenizer_->ProcessParsingResults();
}

// TraceBlobViewIterator implementation
ArtHprofTokenizer::TraceBlobViewIterator::TraceBlobViewIterator(
    util::TraceBlobViewReader&& reader)
    : reader_(std::move(reader)), current_offset_(0) {}

ArtHprofTokenizer::TraceBlobViewIterator::~TraceBlobViewIterator() = default;

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadU1(uint8_t& value) {
  auto slice = reader_.SliceOff(current_offset_, 1);
  if (!slice)
    return false;
  value = *slice->data();
  current_offset_ += 1;
  return true;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadU2(uint16_t& value) {
  uint8_t b1, b2;
  if (!ReadU1(b1) || !ReadU1(b2))
    return false;
  value = static_cast<uint16_t>((static_cast<uint16_t>(b1) << 8) |
                                static_cast<uint16_t>(b2));
  return true;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadU4(uint32_t& value) {
  uint8_t b1, b2, b3, b4;
  if (!ReadU1(b1) || !ReadU1(b2) || !ReadU1(b3) || !ReadU1(b4))
    return false;
  value = (static_cast<uint32_t>(b1) << 24) |
          (static_cast<uint32_t>(b2) << 16) | (static_cast<uint32_t>(b3) << 8) |
          static_cast<uint32_t>(b4);
  return true;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadId(uint64_t& value,
                                                      uint32_t id_size) {
  if (id_size == 4) {
    uint32_t id;
    if (!ReadU4(id))
      return false;
    value = id;
    return true;
  } else if (id_size == 8) {
    uint32_t high, low;
    if (!ReadU4(high) || !ReadU4(low))
      return false;
    value = (static_cast<uint64_t>(high) << 32) | low;
    return true;
  }
  return false;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadString(std::string& str,
                                                          size_t length) {
  auto slice = reader_.SliceOff(current_offset_, length);
  if (!slice)
    return false;

  str.resize(length);
  std::memcpy(&str[0], slice->data(), length);
  current_offset_ += length;
  return true;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::ReadBytes(
    std::vector<uint8_t>& data,
    size_t length) {
  auto slice = reader_.SliceOff(current_offset_, length);
  if (!slice)
    return false;

  data.resize(length);
  std::memcpy(data.data(), slice->data(), length);
  current_offset_ += length;
  return true;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::SkipBytes(size_t count) {
  auto slice = reader_.SliceOff(current_offset_, count);
  if (!slice)
    return false;

  current_offset_ += count;
  return true;
}

size_t ArtHprofTokenizer::TraceBlobViewIterator::GetPosition() {
  return current_offset_;
}

bool ArtHprofTokenizer::TraceBlobViewIterator::IsEof() const {
  return !reader_.SliceOff(current_offset_, 1);
}

bool ArtHprofTokenizer::TraceBlobViewIterator::IsValid() const {
  return true;
}

// HeapGraphBuilder implementation
HeapGraph HeapGraphBuilder::Build(const HprofData& data) {
  PERFETTO_DLOG("Converting hprof to HeapGraph HeapGraph");

  HeapGraph ir;

  // Reset diagnostics
  diagnostics_ = ConversionDiagnostics{};

  // Conversion steps with detailed tracking
  ToClasses(data, ir);
  ToObjects(data, ir);
  ToReferences(data, ir);

  // Print detailed diagnostics
  PrintConversionDiagnostics();

  return ir;
}

void HeapGraphBuilder::ToClasses(const HprofData& data, HeapGraph& ir) {
  PERFETTO_DLOG("Converting classes to HeapGraph");

  std::unordered_set<uint64_t> processed_class_ids;

  for (const auto& [class_id, class_info] : data.classes) {
    diagnostics_.total_processed_classes++;

    // Prevent duplicate class processing
    if (processed_class_ids.count(class_id)) {
      continue;
    }

    processed_class_ids.insert(class_id);
    diagnostics_.unique_classes_processed++;

    // Track class kind
    std::string kind = DetermineClassKind(class_info.name);
    diagnostics_.class_kind_counts[kind]++;

    PERFETTO_DLOG("Converting class: id=%" PRIu64 ", name='%s', kind='%s'",
                  class_id, class_info.name.c_str(), kind.c_str());

    // Create HeapGraphClass and add to HeapGraph
    HeapGraphClass hg_class;
    hg_class.name = class_info.name;
    hg_class.class_object_id = class_id;
    hg_class.kind = kind;

    // Add superclass reference if exists
    if (class_info.super_class_id != 0) {
      hg_class.superclass_id = class_info.super_class_id;
      PERFETTO_DLOG("  With superclass: %" PRIu64, class_info.super_class_id);
    }

    ir.classes.push_back(std::move(hg_class));
  }

  PERFETTO_DLOG("Converted %zu classes to HeapGraph", ir.classes.size());
}

std::string HeapGraphBuilder::RootTypeToString(uint8_t root_type) {
  switch (root_type) {
    case HPROF_ROOT_JNI_GLOBAL:
      return "jni_global";
    case HPROF_ROOT_JNI_LOCAL:
      return "jni_local";
    case HPROF_ROOT_JAVA_FRAME:
      return "java_frame";
    case HPROF_ROOT_NATIVE_STACK:
      return "native_stack";
    case HPROF_ROOT_STICKY_CLASS:
      return "sticky_class";
    case HPROF_ROOT_THREAD_BLOCK:
      return "thread_block";
    case HPROF_ROOT_MONITOR_USED:
      return "monitor_used";
    case HPROF_ROOT_THREAD_OBJ:
      return "thread_object";
    case HPROF_ROOT_INTERNED_STRING:
      return "interned_string";
    case HPROF_ROOT_FINALIZING:
      return "finalizing";
    case HPROF_ROOT_DEBUGGER:
      return "debugger";
    case HPROF_ROOT_VM_INTERNAL:
      return "vm_internal";
    case HPROF_ROOT_JNI_MONITOR:
      return "jni_monitor";
    case HPROF_ROOT_UNKNOWN:
    default:
      return "unknown";
  }
}

// Fix GetHeapTypeFromId in HeapGraphBuilder
std::optional<std::string> HeapGraphBuilder::GetHeapTypeFromId(
    uint8_t heap_id) {
  // Fallback to the standard heap types if no explicit mapping
  switch (heap_id) {
    case HPROF_HEAP_APP:
      return "app";
    case HPROF_HEAP_ZYGOTE:
      return "zygote";
    case HPROF_HEAP_IMAGE:
      return "image";
    case HPROF_HEAP_JIT:
      return "jit";
    case HPROF_HEAP_APP_CACHE:
      return "app-cache";
    case HPROF_HEAP_SYSTEM:
      return "system";
    case HPROF_HEAP_DEFAULT:
      return "default";
    default:
      return "unknown";
  }
}

// Process an instance dump record
HeapGraphObject HeapGraphBuilder::ProcessInstanceDump(
    const InstanceDumpData& instance_data) {
  uint64_t object_id = instance_data.object_id;
  uint64_t type_id = instance_data.class_object_id;

  // Create HeapGraph object
  HeapGraphObject hg_object;
  hg_object.object_id = object_id;
  hg_object.type_id = type_id;
  hg_object.self_size =
      static_cast<int64_t>(instance_data.raw_instance_data.size());
  hg_object.heap_type = GetHeapTypeFromId(instance_data.heap_id);

  return hg_object;
}

// Main ToObjects implementation
void HeapGraphBuilder::ToObjects(const HprofData& data, HeapGraph& ir) {
  PERFETTO_DLOG("Converting objects from hprof to HeapGraph");

  size_t instance_objects = 0;
  size_t obj_array_objects = 0;
  size_t prim_array_objects = 0;
  size_t root_objects = 0;
  size_t skipped_objects = 0;

  // Track which object IDs have been processed to avoid duplicates
  std::unordered_set<uint64_t> processed_object_ids;

  // Process all records in the hprof for objects
  for (const auto& record : data.records) {
    // We're only interested in heap dump records
    if (record.tag != HPROF_HEAP_DUMP &&
        record.tag != HPROF_HEAP_DUMP_SEGMENT) {
      continue;
    }

    // Process heap dump records
    if (std::holds_alternative<HeapDumpData>(record.data)) {
      const auto& heap_dump_data = std::get<HeapDumpData>(record.data);

      for (const auto& sub_record : heap_dump_data.records) {
        uint64_t object_id = 0;
        uint64_t type_id = 0;
        uint32_t ref_set_id = 0;
        HeapGraphObject hg_object;
        bool skip_object = false;

        // Process based on record type
        if (sub_record.tag == HPROF_INSTANCE_DUMP &&
            std::holds_alternative<InstanceDumpData>(sub_record.data)) {
          // Handle instance dump
          const auto& instance_data =
              std::get<InstanceDumpData>(sub_record.data);
          object_id = instance_data.object_id;
          type_id = instance_data.class_object_id;

          // Check if class ID exists in the hprof classes
          if (type_id == 0 ||
              data.classes.find(type_id) == data.classes.end()) {
            PERFETTO_DLOG(
                "Skipping instance with missing class: object_id=%" PRIu64
                ", class_id=%" PRIu64,
                object_id, type_id);
            skipped_objects++;
            continue;
          }

          // Create HeapGraph object
          hg_object.object_id = object_id;
          hg_object.type_id = type_id;
          hg_object.self_size =
              static_cast<int64_t>(instance_data.raw_instance_data.size());

          // Use the heap ID from the instance data
          auto heap_name = GetHeapTypeFromId(instance_data.heap_id);
          if (heap_name) {
            hg_object.heap_type = *heap_name;
          } else {
            // Fallback to "default" if no heap info available
            hg_object.heap_type = "default";
          }

          instance_objects++;
        } else if (sub_record.tag == HPROF_OBJ_ARRAY_DUMP &&
                   std::holds_alternative<ObjArrayDumpData>(sub_record.data)) {
        } else if (sub_record.tag == HPROF_PRIM_ARRAY_DUMP &&
                   std::holds_alternative<PrimArrayDumpData>(sub_record.data)) {
        } else {
          // Skip other record types
          continue;
        }

        // Skip if object ID is 0 or already processed
        if (object_id == 0 || processed_object_ids.count(object_id) > 0) {
          continue;
        }

        // Mark as processed
        processed_object_ids.insert(object_id);

        // Skip if we're supposed to skip this object (due to missing class ID)
        if (skip_object) {
          continue;
        }

        // Check if this object is a root and add root type
        auto root_it = data.root_objects.find(object_id);
        if (root_it != data.root_objects.end()) {
          std::string root_type = RootTypeToString(root_it->second);
          hg_object.root_type = root_type;

          root_objects++;

          // Log root objects (limited to avoid spam)
          if (root_objects <= 10 || root_objects % 1000 == 0) {
            PERFETTO_DLOG("Found root object: ID=%" PRIu64 ", type=%s",
                          object_id, hg_object.root_type->c_str());
          }
        }

        // Generate reference set ID
        ref_set_id = next_reference_set_id_++;
        object_to_reference_set_id_[object_id] = ref_set_id;
        hg_object.reference_set_id = ref_set_id;

        // Log sample object conversions
        if (ir.objects.size() < 10 || ir.objects.size() % 10000 == 0) {
          PERFETTO_DLOG("Converting object to HeapGraph: ID=%" PRIu64
                        ", type=%" PRIu64 ", size=%" PRId64 "%s",
                        object_id, type_id, hg_object.self_size,
                        hg_object.root_type.has_value()
                            ? (", root_type=" + *hg_object.root_type).c_str()
                            : "");
        }

        ir.objects.push_back(std::move(hg_object));
      }
    }
  }

  // Add class objects as well
  for (const auto& [class_id, class_info] : data.classes) {
    // Skip if already processed (could happen if class objects were already in
    // objects)
    if (processed_object_ids.count(class_id) > 0) {
      continue;
    }

    HeapGraphObject hg_object;
    hg_object.object_id = class_id;

    // Class objects are instances of java.lang.Class
    if (data.java_lang_class_object_id != 0) {
      hg_object.type_id = data.java_lang_class_object_id;
    } else {
      // If java.lang.Class wasn't found, use the class ID itself
      hg_object.type_id = class_id;
    }

    // Since classes are often in the zygote or system heap
    hg_object.heap_type = "system";

    // Size is difficult to determine for class objects - using a constant size
    hg_object.self_size = 64;  // Placeholder size

    // Generate reference set ID
    uint32_t ref_set_id = next_reference_set_id_++;
    object_to_reference_set_id_[class_id] = ref_set_id;
    hg_object.reference_set_id = ref_set_id;

    // Check if this class object is also a root
    auto root_it = data.root_objects.find(class_id);
    if (root_it != data.root_objects.end()) {
      std::string root_type = RootTypeToString(root_it->second);
      hg_object.root_type = root_type;
    }

    ir.objects.push_back(std::move(hg_object));
  }

  PERFETTO_DLOG(
      "Converted %zu objects to HeapGraph (%zu instances, %zu obj arrays, %zu "
      "prim "
      "arrays, %zu roots, %zu skipped)",
      ir.objects.size(), instance_objects, obj_array_objects,
      prim_array_objects, root_objects, skipped_objects);
}

// Build a set of objects in the IR for fast lookup
std::unordered_set<uint64_t> HeapGraphBuilder::BuildObjectsInIrSet(
    const HeapGraph& ir) {
  std::unordered_set<uint64_t> objects_in_ir;
  for (const auto& obj : ir.objects) {
    objects_in_ir.insert(obj.object_id);
  }
  return objects_in_ir;
}

// Check if an object is an array
bool HeapGraphBuilder::IsArrayObject(uint64_t owner_id, const HprofData& data) {
  uint64_t class_id = 0;
  auto owner_class_it = data.object_to_class.find(owner_id);
  if (owner_class_it != data.object_to_class.end()) {
    class_id = owner_class_it->second;
    auto class_info_it = data.classes.find(class_id);
    if (class_info_it != data.classes.end()) {
      // Check if class name indicates an array
      const std::string& class_name = class_info_it->second.name;
      return (!class_name.empty() &&
              class_name[0] ==
                  '[') ||  // Handles "[I", "[Ljava.lang.String;" etc.
             (class_name.find("[]") !=
              std::string::npos);  // Handles "int[]", "java.lang.String[]" etc.
    }
  }
  return false;
}

// Create a HeapGraphReference from an ObjectReference
HeapGraphReference HeapGraphBuilder::CreateHeapGraphReference(
    uint32_t reference_set_id,
    uint64_t owner_id,
    const ObjectReference& owned_ref,
    bool is_array,
    uint64_t class_id,
    const HprofData& data,
    const std::unordered_set<uint64_t>& objects_in_ir) {
  HeapGraphReference hg_ref;
  hg_ref.reference_set_id = reference_set_id;
  hg_ref.owner_id = owner_id;

  // Set the field name properly based on whether this is an array or a regular
  // object
  if (is_array) {
    // Keep the array index format for arrays
    hg_ref.field_name = owned_ref.field_name;
  } else {
    // For regular objects, make sure we don't have array index format
    std::string field_name = owned_ref.field_name;
    if (field_name.size() >= 2 && field_name[0] == '[' &&
        field_name[field_name.size() - 1] == ']') {
      // This looks like an array index but the owner is not an array
      // This is probably a bug in the parsing phase
      PERFETTO_DLOG(
          "Warning: Found array index field name '%s' for non-array object "
          "%" PRIu64,
          field_name.c_str(), owner_id);
    }
    hg_ref.field_name = field_name;
  }

  // Set the owned ID if valid
  uint64_t target_id = owned_ref.target_object_id;
  if (target_id != 0) {
    if (objects_in_ir.find(target_id) != objects_in_ir.end()) {
      hg_ref.owned_id = target_id;
    }
  }

  // Set field type name
  std::string owner_class_name;
  if (class_id != 0) {
    auto class_info_it = data.classes.find(class_id);
    if (class_info_it != data.classes.end()) {
      owner_class_name = class_info_it->second.name;
    }
  }

  // Try to determine field type name from owned object
  if (target_id != 0) {
    auto type_it = data.object_to_class.find(target_id);
    if (type_it != data.object_to_class.end()) {
      auto class_it = data.classes.find(type_it->second);
      if (class_it != data.classes.end()) {
        hg_ref.field_type_name = class_it->second.name;
      }
    }
  }

  // If field type is still empty, use owner class name as fallback
  if (hg_ref.field_type_name.empty() && !owner_class_name.empty()) {
    hg_ref.field_type_name = owner_class_name;
  } else if (hg_ref.field_type_name.empty()) {
    // If still empty, use a default type name
    hg_ref.field_type_name = "java.lang.Object";
  }

  return hg_ref;
}

// Create references for a single owner
std::vector<HeapGraphReference> HeapGraphBuilder::CreateReferencesForOwner(
    uint64_t owner_id,
    const std::vector<ObjectReference>& owned_list,
    uint32_t reference_set_id,
    bool is_array,
    const HprofData& data,
    const std::unordered_set<uint64_t>& objects_in_ir) {
  std::vector<HeapGraphReference> references;

  uint64_t class_id = 0;
  auto owner_class_it = data.object_to_class.find(owner_id);
  if (owner_class_it != data.object_to_class.end()) {
    class_id = owner_class_it->second;
  }

  // Process all references from this owner
  for (const auto& owned_ref : owned_list) {
    HeapGraphReference hg_ref =
        CreateHeapGraphReference(reference_set_id, owner_id, owned_ref,
                                 is_array, class_id, data, objects_in_ir);

    references.push_back(std::move(hg_ref));
  }

  return references;
}

// Main ToReferences implementation
void HeapGraphBuilder::ToReferences(const HprofData& data, HeapGraph& ir) {
  PERFETTO_DLOG("Converting %zu reference owner-to-owned entries to HeapGraph",
                data.owner_to_owned.size());

  // Track reference conversion statistics
  size_t total_references = 0;
  size_t refs_with_valid_owner = 0;
  size_t refs_with_valid_owned = 0;

  // Build a set of objects in IR for fast lookup
  std::unordered_set<uint64_t> objects_in_ir = BuildObjectsInIrSet(ir);
  PERFETTO_DLOG("Found %zu objects in HeapGraph", objects_in_ir.size());

  // Process each owner and its references
  for (const auto& [owner_id, owned_list] : data.owner_to_owned) {
    // Check if owner exists in HeapGraph objects
    if (objects_in_ir.find(owner_id) == objects_in_ir.end()) {
      if (total_references < 10 || total_references % 10000 == 0) {
        PERFETTO_DLOG("Owner ID %" PRIu64
                      " from hprof not found in HeapGraph objects",
                      owner_id);
      }
      continue;
    }

    refs_with_valid_owner++;

    // Find the reference set ID for the owner
    uint32_t reference_set_id = 0;
    auto ref_set_id_it = object_to_reference_set_id_.find(owner_id);
    if (ref_set_id_it != object_to_reference_set_id_.end()) {
      reference_set_id = ref_set_id_it->second;
    } else {
      PERFETTO_DLOG("No reference set ID found for owner %" PRIu64, owner_id);
      continue;
    }

    // Determine if the owner is an array
    bool is_array = IsArrayObject(owner_id, data);

    // Create references for this owner
    std::vector<HeapGraphReference> owner_references = CreateReferencesForOwner(
        owner_id, owned_list, reference_set_id, is_array, data, objects_in_ir);

    // Update statistics and add references to IR
    for (auto& ref : owner_references) {
      total_references++;

      if (ref.owned_id.has_value()) {
        refs_with_valid_owned++;
      }

      // Log sample of references for debugging
      if (total_references < 10 || total_references % 10000 == 0) {
        PERFETTO_DLOG(
            "Added reference: owner=%" PRIu64 " (%s), owned=%s, field=%s",
            owner_id, is_array ? "array" : "object",
            ref.owned_id.has_value() ? std::to_string(*ref.owned_id).c_str()
                                     : "null",
            ref.field_name.c_str());
      }

      // Add the reference to HeapGraph
      ir.references.push_back(std::move(ref));
    }
  }

  PERFETTO_DLOG(
      "Converted %zu references: %zu with valid owner, %zu with valid owned",
      total_references, refs_with_valid_owner, refs_with_valid_owned);
}

HeapGraphValue HeapGraphBuilder::ConvertFieldValue(const FieldValue& value) {
  PERFETTO_DLOG("Converting field value of type %d",
                static_cast<int>(value.type));
  HeapGraphValue hg_value;

  switch (value.type) {
    case FieldValue::ValueType::BOOLEAN:
      hg_value.type = HeapGraphValue::Type::BOOLEAN;
      hg_value.primitive_value = std::get<bool>(value.value);
      break;
    case FieldValue::ValueType::BYTE:
      hg_value.type = HeapGraphValue::Type::BYTE;
      hg_value.primitive_value = std::get<int8_t>(value.value);
      break;
    case FieldValue::ValueType::CHAR:
      hg_value.type = HeapGraphValue::Type::CHAR;
      hg_value.primitive_value = std::get<char16_t>(value.value);
      break;
    case FieldValue::ValueType::SHORT:
      hg_value.type = HeapGraphValue::Type::SHORT;
      hg_value.primitive_value = std::get<int16_t>(value.value);
      break;
    case FieldValue::ValueType::INT:
      hg_value.type = HeapGraphValue::Type::INT;
      hg_value.primitive_value = std::get<int32_t>(value.value);
      break;
    case FieldValue::ValueType::FLOAT:
      hg_value.type = HeapGraphValue::Type::FLOAT;
      hg_value.primitive_value = std::get<float>(value.value);
      break;
    case FieldValue::ValueType::LONG:
      hg_value.type = HeapGraphValue::Type::LONG;
      hg_value.primitive_value = std::get<int64_t>(value.value);
      break;
    case FieldValue::ValueType::DOUBLE:
      hg_value.type = HeapGraphValue::Type::DOUBLE;
      hg_value.primitive_value = std::get<double>(value.value);
      break;
    case FieldValue::ValueType::OBJECT_ID:
      hg_value.type = HeapGraphValue::Type::OBJECT_ID;
      hg_value.primitive_value = std::get<uint64_t>(value.value);
      break;
    case FieldValue::ValueType::NONE:
      hg_value.type = HeapGraphValue::Type::NONE;
      hg_value.primitive_value = std::monostate{};
      break;
  }

  return hg_value;
}

std::string HeapGraphBuilder::DetermineClassKind(
    const std::string& class_name) const {
  PERFETTO_DLOG("Determining class kind for: %s", class_name.c_str());

  // Refined kind determination
  if (class_name.find("java.lang.") == 0)
    return "system";
  if (class_name.find("java.util.") == 0)
    return "system";
  if (class_name.find("java.concurrent.") == 0)
    return "system";
  if (class_name.find("jdk.internal.") == 0)
    return "system";
  if (class_name.find("sun.") == 0)
    return "system";
  if (class_name.find("com.sun.") == 0)
    return "system";
  if (class_name.find("android.") == 0)
    return "framework";
  if (class_name.find("com.android.") == 0)
    return "framework";
  if (class_name.find("androidx.") == 0)
    return "framework";

  return "app";
}

void HeapGraphBuilder::PrintConversionDiagnostics() {
  PERFETTO_DLOG("\nConversion Diagnostics:");
  PERFETTO_DLOG("----------------------");

  PERFETTO_DLOG("Total Classes Processed: %zu",
                diagnostics_.total_processed_classes);
  PERFETTO_DLOG("Unique Classes Processed: %zu",
                diagnostics_.unique_classes_processed);

  PERFETTO_DLOG("\nClass Kind Distribution:");
  for (const auto& [kind, count] : diagnostics_.class_kind_counts) {
    PERFETTO_DLOG("  %s: %zu", kind.c_str(), count);
  }

  PERFETTO_DLOG("\nSuperclass Chain Lengths:");
  for (const auto& [length, count] : diagnostics_.superclass_chain_lengths) {
    PERFETTO_DLOG("  %s: %zu", length.c_str(), count);
  }

  PERFETTO_DLOG("\nReferences:");
  PERFETTO_DLOG("  Generated References: %zu",
                diagnostics_.references_generated);
}

}  // namespace perfetto::trace_processor::art_hprof
