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

// Main entry point for converting HPROF data to HeapGraph
HeapGraph HeapGraphBuilder::Build(const HprofData& data) {
  PERFETTO_DLOG("Converting HPROF data to HeapGraph");

  HeapGraph result;
  processed_object_ids_.clear();

  ConvertClasses(data, result);
  ConvertObjects(data, result);
  ConvertReferences(data, result);

  return result;
}

// Convert class definitions to HeapGraph classes
void HeapGraphBuilder::ConvertClasses(const HprofData& data, HeapGraph& ir) {
  PERFETTO_DLOG("Converting classes to HeapGraph");

  for (const auto& [class_id, class_info] : data.classes) {
    HeapGraphClass hg_class;
    hg_class.name = class_info.name;
    hg_class.class_object_id = class_id;
    hg_class.kind = "unknown";  // Simplified for now

    if (class_info.super_class_id != 0) {
      hg_class.superclass_id = class_info.super_class_id;
    }

    ir.classes.push_back(std::move(hg_class));
  }

  PERFETTO_DLOG("Converted %zu classes to HeapGraph", ir.classes.size());
}

void HeapGraphBuilder::ConvertObjects(const HprofData& data, HeapGraph& ir) {
  PERFETTO_DLOG("Converting objects from HPROF records");

  size_t instance_count = 0;
  size_t class_obj_count = 0;
  size_t array_count = 0;
  size_t prim_array_count = 0;

  // Process each record to create objects
  for (const auto& record : data.records) {
    // Skip non-heap dump records
    if (record.tag != HPROF_HEAP_DUMP &&
        record.tag != HPROF_HEAP_DUMP_SEGMENT) {
      continue;
    }

    if (!std::holds_alternative<HeapDumpData>(record.data)) {
      continue;
    }

    const auto& heap_dump = std::get<HeapDumpData>(record.data);

    // Process each sub-record to create objects
    for (const auto& sub_record : heap_dump.records) {
      // Process instance dumps
      if (sub_record.tag == HPROF_INSTANCE_DUMP &&
          std::holds_alternative<InstanceDumpData>(sub_record.data)) {
        const auto& instance_data = std::get<InstanceDumpData>(sub_record.data);
        CreateObjectFromDump(instance_data, data, ir, instance_count);
      }
      // Process class dumps
      else if (sub_record.tag == HPROF_CLASS_DUMP &&
               std::holds_alternative<ClassDumpData>(sub_record.data)) {
        const auto& class_data = std::get<ClassDumpData>(sub_record.data);
        CreateObjectFromDump(class_data, data, ir, class_obj_count);
      }
      // Process object array dumps
      else if (sub_record.tag == HPROF_OBJ_ARRAY_DUMP &&
               std::holds_alternative<ObjArrayDumpData>(sub_record.data)) {
        const auto& array_data = std::get<ObjArrayDumpData>(sub_record.data);
        CreateObjectFromDump(array_data, data, ir, array_count);
      }
      // Process primitive array dumps
      else if (sub_record.tag == HPROF_PRIM_ARRAY_DUMP &&
               std::holds_alternative<PrimArrayDumpData>(sub_record.data)) {
        const auto& array_data = std::get<PrimArrayDumpData>(sub_record.data);
        CreateObjectFromDump(array_data, data, ir, prim_array_count);
      }
    }
  }

  PERFETTO_LOG(
      "Converted %zu objects to HeapGraph (%zu instances, %zu class objects, "
      "%zu object arrays, %zu primitive arrays)",
      ir.objects.size(), instance_count, class_obj_count, array_count,
      prim_array_count);
}

void HeapGraphBuilder::ConvertReferences(const HprofData& data, HeapGraph& ir) {
  PERFETTO_DLOG("Converting references from object relationships");

  size_t total_refs = 0;
  size_t skipped_owner_refs = 0;
  size_t skipped_target_refs = 0;

  // New diagnostics
  std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, std::string>>>
      missing_by_owner;
  std::unordered_map<std::string, size_t> field_names_for_missing;
  std::unordered_map<uint64_t, size_t> owner_classes_with_missing;
  std::unordered_set<uint64_t> sample_missing_ids;

  // Track ID ranges for missing objects
  uint64_t min_missing_id = UINT64_MAX;
  uint64_t max_missing_id = 0;

  // Track ID ranges for processed objects
  uint64_t min_processed_id = UINT64_MAX;
  uint64_t max_processed_id = 0;

  // Update processed ID ranges
  for (uint64_t id : processed_object_ids_) {
    min_processed_id = std::min(min_processed_id, id);
    max_processed_id = std::max(max_processed_id, id);
  }

  // Now process references
  for (const auto& [owner_id, owned_list] : data.owner_to_owned) {
    if (!processed_object_ids_.count(owner_id)) {
      skipped_owner_refs += owned_list.size();
      continue;
    }

    // Get owner class info for context
    std::string owner_class_name = "Unknown";
    uint64_t owner_class_id = 0;
    auto owner_class_it = data.object_to_class.find(owner_id);
    if (owner_class_it != data.object_to_class.end()) {
      owner_class_id = owner_class_it->second;
      auto class_it = data.classes.find(owner_class_id);
      if (class_it != data.classes.end()) {
        owner_class_name = class_it->second.name;
      }
    }

    // Process each reference from this owner
    for (const auto& owned_ref : owned_list) {
      uint64_t target_id = owned_ref.target_object_id;

      // Skip if target is null
      if (target_id == 0) {
        continue;
      }

      // Skip if target doesn't exist in our object set
      if (!processed_object_ids_.count(target_id)) {
        skipped_target_refs++;

        // Update missing ID ranges
        min_missing_id = std::min(min_missing_id, target_id);
        max_missing_id = std::max(max_missing_id, target_id);

        // Collect sample of missing IDs (limit to 100)
        if (sample_missing_ids.size() < 100) {
          sample_missing_ids.insert(target_id);
        }

        // Track which fields are missing
        field_names_for_missing[owned_ref.field_name]++;

        // Group by owner
        if (missing_by_owner[owner_id].size() < 10) {  // Limit to 10 per owner
          missing_by_owner[owner_id].push_back(
              {target_id, owned_ref.field_name});
        }

        // Track owner classes with missing references
        owner_classes_with_missing[owner_class_id]++;

        continue;
      }

      // Create reference
      HeapGraphReference ref;
      ref.reference_set_id = 0;  // Will be set later
      ref.owner_id = owner_id;
      ref.owned_id = target_id;
      ref.field_name = owned_ref.field_name;

      // Set field type name if possible
      if (auto type_it = data.object_to_class.find(target_id);
          type_it != data.object_to_class.end()) {
        if (auto class_it = data.classes.find(type_it->second);
            class_it != data.classes.end()) {
          ref.field_type_name = class_it->second.name;
        }
      }

      // Use default type name if not determined
      if (ref.field_type_name.empty()) {
        ref.field_type_name = "java.lang.Object";
      }

      // Add to IR
      ir.references.push_back(std::move(ref));
      total_refs++;
    }
  }

  // Sort owner classes by number of missing refs (descending)
  std::vector<std::pair<uint64_t, size_t>> sorted_owner_classes(
      owner_classes_with_missing.begin(), owner_classes_with_missing.end());
  std::sort(sorted_owner_classes.begin(), sorted_owner_classes.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  // Log owner classes with most missing refs
  PERFETTO_LOG("=== Top Owner Classes with Missing References ===");
  for (size_t i = 0; i < std::min(sorted_owner_classes.size(), size_t(10));
       i++) {
    uint64_t class_id = sorted_owner_classes[i].first;
    size_t count = sorted_owner_classes[i].second;
    std::string class_name = "Unknown";
    if (class_id != 0) {
      auto it = data.classes.find(class_id);
      if (it != data.classes.end()) {
        class_name = it->second.name;
      }
    }
    PERFETTO_LOG("  Class '%s': %zu missing reference targets",
                 class_name.c_str(), count);
  }

  // Sort field names by frequency (descending)
  std::vector<std::pair<std::string, size_t>> sorted_fields(
      field_names_for_missing.begin(), field_names_for_missing.end());
  std::sort(sorted_fields.begin(), sorted_fields.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  // Log most common field names with missing refs
  PERFETTO_LOG("=== Top Field Names with Missing References ===");
  for (size_t i = 0; i < std::min(sorted_fields.size(), size_t(10)); i++) {
    PERFETTO_LOG("  Field '%s': %zu missing references",
                 sorted_fields[i].first.c_str(), sorted_fields[i].second);
  }

  // Log ID ranges
  PERFETTO_LOG("=== ID Range Analysis ===");
  PERFETTO_LOG("  Processed objects ID range: %" PRIu64 " to %" PRIu64,
               min_processed_id, max_processed_id);
  PERFETTO_LOG("  Missing objects ID range: %" PRIu64 " to %" PRIu64,
               min_missing_id, max_missing_id);

  // Distribution of missing IDs (10 buckets)
  if (min_missing_id != UINT64_MAX && max_missing_id > min_missing_id) {
    uint64_t range = max_missing_id - min_missing_id;
    uint64_t bucket_size = range / 10 + 1;
    std::vector<size_t> distribution(10, 0);

    for (uint64_t id : sample_missing_ids) {
      size_t bucket = (id - min_missing_id) / bucket_size;
      if (bucket < 10) {
        distribution[bucket]++;
      }
    }

    PERFETTO_LOG("  Distribution of missing IDs (sample):");
    for (size_t i = 0; i < 10; i++) {
      uint64_t start = min_missing_id + i * bucket_size;
      uint64_t end = min_missing_id + (i + 1) * bucket_size - 1;
      PERFETTO_LOG("    Range %" PRIu64 "-%" PRIu64 ": %zu objects", start, end,
                   distribution[i]);
    }
  }

  // Get a sampling of owner-missing target pairs for detailed inspection
  PERFETTO_LOG("=== Sample of Owner-Missing Target Pairs ===");
  size_t pairs_logged = 0;
  for (const auto& [owner_id, targets] : missing_by_owner) {
    if (pairs_logged >= 10)
      break;

    std::string owner_class = "Unknown";
    auto owner_class_it = data.object_to_class.find(owner_id);
    if (owner_class_it != data.object_to_class.end()) {
      auto class_it = data.classes.find(owner_class_it->second);
      if (class_it != data.classes.end()) {
        owner_class = class_it->second.name;
      }
    }

    for (const auto& [target_id, field_name] : targets) {
      PERFETTO_LOG("  Owner %" PRIu64 " (Class '%s') -> Missing Target %" PRIu64
                   " (Field: %s)",
                   owner_id, owner_class.c_str(), target_id,
                   field_name.c_str());
      pairs_logged++;
      if (pairs_logged >= 20)
        break;
    }
  }

  PERFETTO_LOG("Converted %zu object references to HeapGraph", total_refs);
  PERFETTO_LOG("Skipped %zu references due to missing owner",
               skipped_owner_refs);
  PERFETTO_LOG("Skipped %zu references due to missing target",
               skipped_target_refs);
}

// Convert root type ID to string
std::string HeapGraphBuilder::GetRootType(uint8_t root_type_id) {
  switch (root_type_id) {
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
    default:
      return "unknown";
  }
}

// Convert heap type ID to string
std::string HeapGraphBuilder::GetHeapType(uint8_t heap_id) {
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

// Generic helper for creating objects from different record types
template <typename T>
void HeapGraphBuilder::CreateObjectFromDump(const T& dump_data,
                                            const HprofData& data,
                                            HeapGraph& ir,
                                            size_t& counter) {
  // Extract object ID, type ID and other fields based on dump type
  uint64_t object_id;
  uint64_t type_id;
  int64_t size;
  HprofHeapId heap_id;

  // Set values based on the specific dump type
  if constexpr (std::is_same_v<T, InstanceDumpData>) {
    object_id = dump_data.object_id;
    type_id = dump_data.class_object_id;
    size = static_cast<int64_t>(dump_data.raw_instance_data.size());
    heap_id = dump_data.heap_id;
  } else if constexpr (std::is_same_v<T, ClassDumpData>) {
    object_id = dump_data.class_object_id;
    type_id = data.java_lang_class_object_id != 0
                  ? data.java_lang_class_object_id
                  : dump_data.class_object_id;
    size = dump_data.instance_size;
    heap_id = dump_data.heap_id;
  } else if constexpr (std::is_same_v<T, ObjArrayDumpData>) {
    object_id = dump_data.array_object_id;
    type_id = dump_data.array_class_object_id;
    size = static_cast<int64_t>(
        dump_data.elements.size() *
        (data.header.identifier_size == 0 ? 8 : data.header.identifier_size));
    heap_id = dump_data.heap_id;

    for (const auto& element_id : dump_data.elements) {
      if (element_id != 0) {
        processed_object_ids_.insert(element_id);
      }
    }
  } else if constexpr (std::is_same_v<T, PrimArrayDumpData>) {
    object_id = dump_data.array_object_id;
    type_id = 0;  // Primitive arrays don't have a specific class
    size = static_cast<int64_t>(dump_data.elements.size());
    heap_id = dump_data.heap_id;
  } else {
    // This should never happen with proper usage
    return;
  }

  // Skip if already processed
  if (processed_object_ids_.count(object_id)) {
    return;
  }

  // Create object
  HeapGraphObject object;
  object.object_id = object_id;
  object.type_id = type_id;
  object.self_size = size;
  object.heap_type = GetHeapType(heap_id);

  // Check if it's a root object
  auto root_it = data.root_objects.find(object_id);
  if (root_it != data.root_objects.end()) {
    object.root_type = GetRootType(root_it->second);
  }

  // Add to heap graph
  ir.objects.push_back(std::move(object));
  processed_object_ids_.insert(object_id);
  counter++;
}

}  // namespace perfetto::trace_processor::art_hprof
