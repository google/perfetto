/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/protozero/filtering/filter_util.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/proto_utils.h"
#include "protos/perfetto/common/semantic_type.pbzero.h"
#include "src/protozero/filtering/filter_bytecode_generator.h"
#include "src/protozero/filtering/filter_bytecode_parser.h"

namespace protozero {

namespace {

class MultiFileErrorCollectorImpl
    : public google::protobuf::compiler::MultiFileErrorCollector {
 public:
  ~MultiFileErrorCollectorImpl() override = default;
#if GOOGLE_PROTOBUF_VERSION >= 4022000
  void RecordError(std::string_view filename,
                   int line,
                   int column,
                   std::string_view message) override {
    PERFETTO_ELOG("Error %.*s %d:%d: %.*s", static_cast<int>(filename.size()),
                  filename.data(), line, column,
                  static_cast<int>(message.size()), message.data());
  }
  void RecordWarning(std::string_view filename,
                     int line,
                     int column,
                     std::string_view message) override {
    PERFETTO_ELOG("Warning %.*s %d:%d: %.*s", static_cast<int>(filename.size()),
                  filename.data(), line, column,
                  static_cast<int>(message.size()), message.data());
  }
#else
  void AddError(const std::string& filename,
                int line,
                int column,
                const std::string& message) override {
    PERFETTO_ELOG("Error %s %d:%d: %s", filename.c_str(), line, column,
                  message.c_str());
  }
  void AddWarning(const std::string& filename,
                  int line,
                  int column,
                  const std::string& message) override {
    PERFETTO_ELOG("Warning %s %d:%d: %s", filename.c_str(), line, column,
                  message.c_str());
  }
#endif
};

}  // namespace

FilterUtil::FilterUtil() = default;
FilterUtil::~FilterUtil() = default;

bool FilterUtil::LoadMessageDefinition(const std::string& proto_file,
                                       const std::string& root_message,
                                       const std::string& proto_dir_path) {
  // The protobuf compiler doesn't like backslashes and prints an error like:
  // Error C:\it7mjanpw3\perfetto-a16500 -1:0: Backslashes, consecutive slashes,
  // ".", or ".." are not allowed in the virtual path.
  // Given that C:\foo\bar is a legit path on windows, fix it at this level
  // because the problem is really the protobuf compiler being too picky.
  static auto normalize_for_win = [](const std::string& path) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    return perfetto::base::ReplaceAll(path, "\\", "/");
#else
    return path;
#endif
  };

  google::protobuf::compiler::DiskSourceTree dst;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // If the path is absolute, maps "C:/" -> "C:/" (without hardcoding 'C').
  if (proto_file.size() > 3 && proto_file[1] == ':') {
    char win_drive[4]{proto_file[0], ':', '/', '\0'};
    dst.MapPath(win_drive, win_drive);
  }
#endif
  dst.MapPath("/", "/");  // We might still need this on Win under cygwin.
  dst.MapPath("", normalize_for_win(proto_dir_path));
  MultiFileErrorCollectorImpl mfe;
  google::protobuf::compiler::Importer importer(&dst, &mfe);
  const google::protobuf::FileDescriptor* root_file =
      importer.Import(normalize_for_win(proto_file));
  const google::protobuf::Descriptor* root_msg = nullptr;
  if (!root_message.empty()) {
    root_msg = importer.pool()->FindMessageTypeByName(root_message);
  } else if (root_file->message_type_count() > 0) {
    // The user didn't specify the root type. Pick the first type in the file,
    // most times it's the right guess.
    root_msg = root_file->message_type(0);
    if (root_msg)
      PERFETTO_LOG(
          "The guessed root message name is \"%.*s\". Pass -r com.MyName to "
          "override",
          int(root_msg->full_name().size()), root_msg->full_name().data());
  }

  if (!root_msg) {
    PERFETTO_ELOG("Could not find the root message \"%s\" in %s",
                  root_message.c_str(), proto_file.c_str());
    return false;
  }

  // |descriptors_by_full_name| is passed by argument rather than being a member
  // field so that we don't risk leaving it out of sync (and depending on it in
  // future without realizing) when performing the Dedupe() pass.
  DescriptorsByNameMap descriptors_by_full_name;
  ParseProtoDescriptor(root_msg, &descriptors_by_full_name, importer.pool());

  return true;
}

bool FilterUtil::LoadFromDescriptorSet(const uint8_t* file_descriptor_set_proto,
                                       size_t size,
                                       const std::string& root_message) {
  // Parse the binary FileDescriptorSet.
  google::protobuf::FileDescriptorSet fds;
  if (!fds.ParseFromArray(file_descriptor_set_proto, static_cast<int>(size))) {
    PERFETTO_ELOG("Failed to parse FileDescriptorSet");
    return false;
  }

  // Build a DescriptorPool from the FileDescriptorSet.
  // We use the generated_pool() as underlay so that extensions are properly
  // linked to compiled-in message types like FieldOptions.
  google::protobuf::DescriptorPool pool(
      google::protobuf::DescriptorPool::generated_pool());
  for (const auto& file : fds.file()) {
    // Skip files that are already in the generated pool (like
    // descriptor.proto).
    if (google::protobuf::DescriptorPool::generated_pool()->FindFileByName(
            file.name())) {
      continue;
    }
    if (!pool.BuildFile(file)) {
      PERFETTO_ELOG("Failed to build file descriptor: %s", file.name().c_str());
      return false;
    }
  }

  // Find the root message.
  const google::protobuf::Descriptor* root_msg =
      pool.FindMessageTypeByName(root_message);
  if (!root_msg) {
    PERFETTO_ELOG("Could not find root message: %s", root_message.c_str());
    return false;
  }

  // Parse the descriptor tree, reusing the same logic as LoadMessageDefinition.
  DescriptorsByNameMap descriptors_by_full_name;
  ParseProtoDescriptor(root_msg, &descriptors_by_full_name, &pool);

  return true;
}

namespace {

// Helper to read proto_filter annotation from a field using dynamic reflection.
// Returns true if the field has proto_filter annotation.
struct ProtoFilterOptions {
  uint32_t semantic_type = 0;
  bool filter_string = false;
  bool passthrough = false;
  bool add_to_v2 = false;
};

// Extension field number for perfetto.protos.proto_filter extension.
constexpr int kProtoFilterExtensionNumber = 73400001;

// Field numbers within ProtoFilterOptions message.
constexpr int kSemanticTypeFieldNumber = 1;
constexpr int kFilterStringFieldNumber = 2;
constexpr int kPassthroughFieldNumber = 3;
constexpr int kAddToV2FieldNumber = 4;

// Parse ProtoFilterOptions from raw bytes (length-delimited submessage).
void ParseProtoFilterOptionsFromBytes(std::string_view data,
                                      ProtoFilterOptions* opts) {
  // ProtoFilterOptions is a simple message with:
  // 1: semantic_type (enum/int32)
  // 2: filter_string (bool)
  // 3: passthrough (bool)
  // 4: add_to_v2 (bool)
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data.data());
  const uint8_t* end = ptr + data.size();

  while (ptr < end) {
    // Read tag (varint)
    uint64_t tag;
    const uint8_t* next = protozero::proto_utils::ParseVarInt(ptr, end, &tag);
    if (next == ptr)
      break;
    ptr = next;

    uint32_t field_number = static_cast<uint32_t>(tag >> 3);
    uint32_t wire_type = static_cast<uint32_t>(tag & 0x7);

    if (wire_type == 0) {  // Varint
      uint64_t value;
      next = protozero::proto_utils::ParseVarInt(ptr, end, &value);
      if (next == ptr)
        break;
      ptr = next;

      switch (field_number) {
        case kSemanticTypeFieldNumber:
          opts->semantic_type = static_cast<uint32_t>(value);
          break;
        case kFilterStringFieldNumber:
          opts->filter_string = (value != 0);
          break;
        case kPassthroughFieldNumber:
          opts->passthrough = (value != 0);
          break;
        case kAddToV2FieldNumber:
          opts->add_to_v2 = (value != 0);
          break;
      }
    } else {
      // Skip unknown wire types
      break;
    }
  }
}

ProtoFilterOptions ReadProtoFilterAnnotation(
    const google::protobuf::FieldDescriptor* proto_field,
    const google::protobuf::DescriptorPool* pool) {
  ProtoFilterOptions opts;

  const auto& options = proto_field->options();
  const auto* reflection = options.GetReflection();

  // First, try to find the extension in recognized fields (works when using
  // compiler::Importer which inherits from generated_pool).
  std::vector<const google::protobuf::FieldDescriptor*> fields;
  reflection->ListFields(options, &fields);
  for (const auto* field : fields) {
    if (field->full_name() == "perfetto.protos.proto_filter") {
      // Found as recognized extension - parse using reflection.
      const auto& filter_opts = reflection->GetMessage(options, field);
      const auto* filter_opts_desc = filter_opts.GetDescriptor();
      const auto* filter_opts_refl = filter_opts.GetReflection();

      const auto* semantic_type_field =
          filter_opts_desc->FindFieldByName("semantic_type");
      if (semantic_type_field &&
          filter_opts_refl->HasField(filter_opts, semantic_type_field)) {
        opts.semantic_type = static_cast<uint32_t>(
            filter_opts_refl->GetEnumValue(filter_opts, semantic_type_field));
      }

      const auto* filter_string_field =
          filter_opts_desc->FindFieldByName("filter_string");
      if (filter_string_field &&
          filter_opts_refl->HasField(filter_opts, filter_string_field)) {
        opts.filter_string =
            filter_opts_refl->GetBool(filter_opts, filter_string_field);
      }

      const auto* passthrough_field =
          filter_opts_desc->FindFieldByName("passthrough");
      if (passthrough_field &&
          filter_opts_refl->HasField(filter_opts, passthrough_field)) {
        opts.passthrough =
            filter_opts_refl->GetBool(filter_opts, passthrough_field);
      }

      const auto* add_to_v2_field =
          filter_opts_desc->FindFieldByName("add_to_v2");
      if (add_to_v2_field &&
          filter_opts_refl->HasField(filter_opts, add_to_v2_field)) {
        opts.add_to_v2 =
            filter_opts_refl->GetBool(filter_opts, add_to_v2_field);
      }
      return opts;
    }
  }

  // If not found as recognized extension, check unknown fields.
  // This happens when loading from a binary FileDescriptorSet where the
  // extension wasn't compiled in.
  const auto& unknown = reflection->GetUnknownFields(options);
  for (int i = 0; i < unknown.field_count(); i++) {
    const auto& field = unknown.field(i);
    if (field.number() == kProtoFilterExtensionNumber &&
        field.type() == google::protobuf::UnknownField::TYPE_LENGTH_DELIMITED) {
      ParseProtoFilterOptionsFromBytes(field.length_delimited(), &opts);
      return opts;
    }
  }

  (void)pool;  // May be used in future for extension lookup.
  return opts;
}

}  // namespace

// Generates a Message object for the given libprotobuf message descriptor.
// Recurses as needed into nested fields.
FilterUtil::Message* FilterUtil::ParseProtoDescriptor(
    const google::protobuf::Descriptor* proto,
    DescriptorsByNameMap* descriptors_by_full_name,
    const google::protobuf::DescriptorPool* pool) {
  auto descr_it =
      descriptors_by_full_name->find(std::string(proto->full_name()));
  if (descr_it != descriptors_by_full_name->end())
    return descr_it->second;

  descriptors_.emplace_back();
  Message* msg = &descriptors_.back();
  msg->full_name = std::string(proto->full_name());
  (*descriptors_by_full_name)[msg->full_name] = msg;
  for (int i = 0; i < proto->field_count(); ++i) {
    const auto* proto_field = proto->field(i);
    const uint32_t field_id = static_cast<uint32_t>(proto_field->number());
    PERFETTO_CHECK(msg->fields.count(field_id) == 0);
    auto& field = msg->fields[field_id];
    field.name = proto_field->name();
    field.type = proto_field->type_name();

    // Read proto_filter annotation from the field.
    ProtoFilterOptions filter_opts =
        ReadProtoFilterAnnotation(proto_field, pool);
    bool passthrough = filter_opts.passthrough;

    if (passthrough) {
      field.type = "bytes";
    }

    // A field should be filtered if either:
    // - filter_string is explicitly set to true, or
    // - semantic_type is set (non-zero)
    if (filter_opts.filter_string || filter_opts.semantic_type != 0) {
      PERFETTO_CHECK(proto_field->type() ==
                     google::protobuf::FieldDescriptor::TYPE_STRING);
      field.filter_string = true;
      field.semantic_type = filter_opts.semantic_type;
      field.add_to_v2 = filter_opts.add_to_v2;
      msg->has_filter_string_fields = true;
    }

    if (proto_field->message_type() && !passthrough) {
      msg->has_nested_fields = true;
      // Recurse.
      field.nested_type = ParseProtoDescriptor(proto_field->message_type(),
                                               descriptors_by_full_name, pool);
    }
  }
  return msg;
}

void FilterUtil::Dedupe() {
  std::map<std::string /*identity*/, Message*> index;

  std::map<Message*, Message*> dupe_graph;  // K,V: K shall be duped against V.

  // As a first pass, generate an |identity| string for each leaf message. The
  // identity is simply the comma-separated stringification of its field ids.
  // If another message with the same identity exists, add an edge to the graph.
  const size_t initial_count = descriptors_.size();
  size_t field_count = 0;
  for (auto& descr : descriptors_) {
    // Dedupe only leaf messages without nested or string filter fields.
    if (descr.has_nested_fields || descr.has_filter_string_fields)
      continue;
    std::string identity;
    for (const auto& id_and_field : descr.fields)
      identity.append(std::to_string(id_and_field.first) + ",");
    auto it_and_inserted = index.emplace(identity, &descr);
    if (!it_and_inserted.second) {
      // insertion failed, a dupe exists already.
      Message* dupe_against = it_and_inserted.first->second;
      dupe_graph.emplace(&descr, dupe_against);
    }
  }

  // Now apply de-duplications by re-directing the nested_type pointer to the
  // equivalent descriptors that have the same set of allowed field ids.
  std::set<Message*> referenced_descriptors;
  referenced_descriptors.emplace(&descriptors_.front());  // The root.
  for (auto& descr : descriptors_) {
    for (auto& id_and_field : descr.fields) {
      Message* target = id_and_field.second.nested_type;
      if (!target)
        continue;  // Only try to dedupe nested types.
      auto it = dupe_graph.find(target);
      if (it == dupe_graph.end()) {
        referenced_descriptors.emplace(target);
        continue;
      }
      ++field_count;
      // Replace with the dupe.
      id_and_field.second.nested_type = it->second;
    }  // for (nested_fields).
  }  // for (descriptors_).

  // Remove unreferenced descriptors. We should much rather crash in the case of
  // a logic bug rather than trying to use them but don't emit them.
  size_t removed_count = 0;
  for (auto it = descriptors_.begin(); it != descriptors_.end();) {
    if (referenced_descriptors.count(&*it)) {
      ++it;
    } else {
      ++removed_count;
      it = descriptors_.erase(it);
    }
  }
  PERFETTO_LOG(
      "Deduplication removed %zu duped descriptors out of %zu descriptors from "
      "%zu fields",
      removed_count, initial_count, field_count);
}

// Prints the list of messages and fields in a diff-friendly text format.
void FilterUtil::PrintAsText(std::optional<std::string> filter_bytecode) {
  using perfetto::base::StripPrefix;
  const std::string& root_name = descriptors_.front().full_name;
  std::string root_prefix = root_name.substr(0, root_name.rfind('.'));
  if (!root_prefix.empty())
    root_prefix.append(".");

  FilterBytecodeParser parser;
  if (filter_bytecode) {
    PERFETTO_CHECK(
        parser.Load(filter_bytecode->data(), filter_bytecode->size()));
  }

  // <Filter msg_index, Message>
  std::deque<std::pair<uint32_t, const Message*>> queue;
  std::set<const Message*> seen_msgs{&descriptors_.front()};
  queue.emplace_back(0u, &descriptors_.front());

  while (!queue.empty()) {
    auto index_and_descr = queue.front();
    queue.pop_front();
    uint32_t msg_index = index_and_descr.first;
    const auto& descr = *index_and_descr.second;

    for (const auto& id_and_field : descr.fields) {
      const uint32_t field_id = id_and_field.first;
      const auto& field = id_and_field.second;

      FilterBytecodeParser::QueryResult result{false, 0, 0};
      if (filter_bytecode) {
        result = parser.Query(msg_index, field_id);
        if (!result.allowed) {
          continue;
        }
      }

      const Message* nested_type = id_and_field.second.nested_type;
      bool passthrough = false;
      if (nested_type) {
        // result.simple_field might be true if the generated bytecode is
        // passing through a whole submessage without recursing.
        passthrough = result.allowed && result.simple_field();
        auto [_, msg_inserted] = seen_msgs.insert(nested_type);
        if (msg_inserted) {
          queue.emplace_back(result.nested_msg_index, nested_type);
        }
      } else {  // simple field
        PERFETTO_CHECK(result.simple_field() || result.filter_string_field() ||
                       !filter_bytecode);
        PERFETTO_CHECK(result.filter_string_field() == field.filter_string ||
                       !filter_bytecode);
      }

      auto stripped_name = StripPrefix(descr.full_name, root_prefix);
      std::string stripped_nested =
          nested_type ? " " + StripPrefix(nested_type->full_name, root_prefix)
                      : "";
      if (passthrough)
        stripped_nested += "  # PASSTHROUGH";
      if (field.filter_string)
        stripped_nested += "  # FILTER STRING";
      using SemanticType = perfetto::protos::pbzero::SemanticType;
      if (field.semantic_type) {
        stripped_nested +=
            std::string("  # SEMANTIC TYPE ") +
            SemanticType_Name(static_cast<SemanticType>(field.semantic_type));
      }
      fprintf(print_stream_, "%-60s %3u %-8s %-32s%s\n", stripped_name.c_str(),
              field_id, field.type.c_str(), field.name.c_str(),
              stripped_nested.c_str());
    }
  }
}

FilterBytecodeGenerator::SerializeResult FilterUtil::GenerateFilterBytecode(
    FilterBytecodeGenerator::BytecodeVersion min_version) {
  protozero::FilterBytecodeGenerator bytecode_gen(min_version);

  // Assign indexes to descriptors, simply by counting them in order;
  std::map<Message*, uint32_t> descr_to_idx;
  for (auto& descr : descriptors_)
    descr_to_idx[&descr] = static_cast<uint32_t>(descr_to_idx.size());

  for (auto& descr : descriptors_) {
    for (auto it = descr.fields.begin(); it != descr.fields.end();) {
      uint32_t field_id = it->first;
      const Message::Field& field = it->second;
      if (field.nested_type) {
        // Append the index of the target submessage.
        PERFETTO_CHECK(descr_to_idx.count(field.nested_type));
        uint32_t nested_msg_index = descr_to_idx[field.nested_type];
        bytecode_gen.AddNestedField(field_id, nested_msg_index);
        ++it;
        continue;
      }
      if (field.filter_string) {
        if (field.semantic_type != 0) {
          bytecode_gen.AddFilterStringFieldWithType(
              field_id, field.semantic_type, field.add_to_v2);
        } else {
          bytecode_gen.AddFilterStringField(field_id);
        }
        ++it;
        continue;
      }
      // Simple field. Lookahead to see if we have a range of contiguous simple
      // fields.
      for (uint32_t range_len = 1;; ++range_len) {
        ++it;
        if (it != descr.fields.end() && it->first == field_id + range_len &&
            it->second.is_simple()) {
          continue;
        }
        // At this point it points to either the end() of the vector or a
        // non-contiguous or non-simple field (which will be picked up by the
        // next iteration).
        if (range_len == 1) {
          bytecode_gen.AddSimpleField(field_id);
        } else {
          bytecode_gen.AddSimpleFieldRange(field_id, range_len);
        }
        break;
      }  // for (range_len)
    }  // for (descr.fields)
    bytecode_gen.EndMessage();
  }  // for (descriptors)
  return bytecode_gen.Serialize();
}

std::string FilterUtil::LookupField(const std::string& varint_encoded_path) {
  const uint8_t* ptr =
      reinterpret_cast<const uint8_t*>(varint_encoded_path.data());
  const uint8_t* const end = ptr + varint_encoded_path.size();

  std::vector<uint32_t> fields;
  while (ptr < end) {
    uint64_t varint;
    const uint8_t* next = proto_utils::ParseVarInt(ptr, end, &varint);
    PERFETTO_CHECK(next != ptr);
    fields.emplace_back(static_cast<uint32_t>(varint));
    ptr = next;
  }
  return LookupField(fields.data(), fields.size());
}

std::string FilterUtil::LookupField(const uint32_t* field_ids,
                                    size_t num_fields) {
  const Message* msg = descriptors_.empty() ? nullptr : &descriptors_.front();
  std::string res;
  for (size_t i = 0; i < num_fields; ++i) {
    const uint32_t field_id = field_ids[i];
    const Message::Field* field = nullptr;
    if (msg) {
      auto it = msg->fields.find(field_id);
      field = it == msg->fields.end() ? nullptr : &it->second;
    }
    res.append(".");
    if (field) {
      res.append(field->name);
      msg = field->nested_type;
    } else {
      res.append(std::to_string(field_id));
    }
  }
  return res;
}

}  // namespace protozero
