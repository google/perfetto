#include "src/trace_processor/util/protozero_to_text.h"

#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/proto_utils.h"
#include "protos/perfetto/common/descriptor.pbzero.h"
#include "src/trace_processor/util/descriptors.h"

// This is the highest level that this protozero to text supports.
#include "src/trace_processor/importers/proto/track_event.descriptor.h"

namespace perfetto {
namespace trace_processor {
namespace protozero_to_text {

namespace {

// Recursively determine the size of all the string like things passed in the
// parameter pack |rest|.
size_t SizeOfStr() {
  return 0;
}
template <typename T, typename... Rest>
size_t SizeOfStr(const T& first, Rest... rest) {
  return base::StringView(first).size() + SizeOfStr(rest...);
}

// Append |to_add| which is something string like to |out|.
template <typename T>
void StrAppendInternal(std::string* out, const T& to_add) {
  out->append(to_add);
}

template <typename T, typename... strings>
void StrAppendInternal(std::string* out, const T& first, strings... values) {
  StrAppendInternal(out, first);
  StrAppendInternal(out, values...);
}

// Append |to_add| which is something string like to |out|.
template <typename T>
void StrAppend(std::string* out, const T& to_add) {
  out->reserve(out->size() + base::StringView(to_add).size());
  out->append(to_add);
}

template <typename T, typename... strings>
void StrAppend(std::string* out, const T& first, strings... values) {
  out->reserve(out->size() + SizeOfStr(values...));
  StrAppendInternal(out, first);
  StrAppendInternal(out, values...);
}

void ConvertProtoTypeToFieldAndValueString(const FieldDescriptor& fd,
                                           const protozero::Field& field,
                                           const std::string& separator,
                                           const std::string& indent,
                                           const DescriptorPool& pool,
                                           std::string* out) {
  using FieldDescriptorProto = protos::pbzero::FieldDescriptorProto;
  switch (fd.type()) {
    case FieldDescriptorProto::TYPE_INT32:
    case FieldDescriptorProto::TYPE_SFIXED32:
    case FieldDescriptorProto::TYPE_FIXED32:
      StrAppend(out, separator, indent, fd.name(), ": ",
                std::to_string(field.as_int32()));
      return;
    case FieldDescriptorProto::TYPE_SINT32:
      StrAppend(out, separator, indent, fd.name(), ": ",
                std::to_string(field.as_sint32()));
      return;
    case FieldDescriptorProto::TYPE_INT64:
    case FieldDescriptorProto::TYPE_SFIXED64:
    case FieldDescriptorProto::TYPE_FIXED64:
      StrAppend(out, separator, indent, fd.name(), ": ",
                std::to_string(field.as_int64()));
      return;
    case FieldDescriptorProto::TYPE_SINT64:
      StrAppend(out, separator, indent, fd.name(), ": ",
                std::to_string(field.as_sint64()));
      return;
    case FieldDescriptorProto::TYPE_UINT32:
      StrAppend(out, separator, indent, fd.name(), ": ",
                std::to_string(field.as_uint32()));
      return;
    case FieldDescriptorProto::TYPE_UINT64:
      StrAppend(out, separator, indent, fd.name(), ": ",
                std::to_string(field.as_uint64()));
      return;
    case FieldDescriptorProto::TYPE_BOOL:
      StrAppend(out, separator, indent, fd.name(), ": ",
                field.as_bool() ? "true" : "false");
      return;
    case FieldDescriptorProto::TYPE_DOUBLE:
      StrAppend(out, separator, indent, fd.name(), ": ",
                std::to_string(field.as_double()));
      return;
    case FieldDescriptorProto::TYPE_FLOAT:
      StrAppend(out, separator, indent, fd.name(), ": ",
                std::to_string(field.as_float()));
      return;
    case FieldDescriptorProto::TYPE_STRING:
      StrAppend(out, separator, indent, fd.name(), ": ", field.as_std_string());
      return;
    case FieldDescriptorProto::TYPE_BYTES:
      // TODO(dproy): Write a bytes to hex function here when we need it.
      PERFETTO_FATAL("Bytes field cannot be converted to prototext.");
    case FieldDescriptorProto::TYPE_ENUM: {
      auto opt_enum_descriptor_idx =
          pool.FindDescriptorIdx(fd.resolved_type_name());
      PERFETTO_DCHECK(opt_enum_descriptor_idx);
      auto opt_enum_string =
          pool.descriptors()[*opt_enum_descriptor_idx].FindEnumString(
              field.as_int32());
      PERFETTO_DCHECK(opt_enum_string);
      StrAppend(out, separator, indent, fd.name(), ": ", *opt_enum_string);
      return;
    }
    default: {
      PERFETTO_FATAL(
          "Tried to write value of type field %s (in proto type "
          "%s) which has type enum %d",
          fd.name().c_str(), fd.resolved_type_name().c_str(), fd.type());
    }
  }
  return;
}

void IncreaseIndents(std::string* out) {
  StrAppend(out, "  ");
}

void DecreaseIndents(std::string* out) {
  PERFETTO_DCHECK(out->size() >= 2);
  out->erase(out->size() - 2);
}

// Recursive case function, Will parse |protobytes| assuming it is a proto of
// |type| and will use |pool| to look up the |type|. All output will be placed
// in |output| and between fields |separator| will be placed. When called for
// |indents| will be increased by 2 spaces to improve readability.
void ProtozeroToTextInternal(const std::string& type,
                             protozero::ConstBytes protobytes,
                             NewLinesMode new_lines_mode,
                             const DescriptorPool& pool,
                             std::string* indents,
                             std::string* output) {
  auto opt_proto_descriptor_idx = pool.FindDescriptorIdx(type);
  PERFETTO_DCHECK(opt_proto_descriptor_idx);
  auto& proto_descriptor = pool.descriptors()[*opt_proto_descriptor_idx];
  bool include_new_lines = new_lines_mode == kIncludeNewLines;

  protozero::ProtoDecoder decoder(protobytes.data, protobytes.size);
  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    auto opt_field_descriptor_idx =
        proto_descriptor.FindFieldIdxByTag(field.id());
    PERFETTO_DCHECK(opt_field_descriptor_idx);
    const auto& field_descriptor =
        proto_descriptor.fields()[*opt_field_descriptor_idx];

    if (field_descriptor.type() ==
        protos::pbzero::FieldDescriptorProto::TYPE_MESSAGE) {
      if (include_new_lines) {
        StrAppend(output, output->empty() ? "" : "\n", *indents,
                  field_descriptor.name(), ": {");
        IncreaseIndents(indents);
      } else {
        StrAppend(output, output->empty() ? "" : " ", field_descriptor.name(),
                  ": {");
      }
      ProtozeroToTextInternal(field_descriptor.resolved_type_name(),
                              field.as_bytes(), new_lines_mode, pool, indents,
                              output);
      if (include_new_lines) {
        DecreaseIndents(indents);
        StrAppend(output, "\n", *indents, "}");
      } else {
        StrAppend(output, " }");
      }
    } else {
      ConvertProtoTypeToFieldAndValueString(
          field_descriptor, field,
          output->empty() ? "" : include_new_lines ? "\n" : " ", *indents, pool,
          output);
    }
  }
  PERFETTO_DCHECK(decoder.bytes_left() == 0);
}

}  // namespace

std::string ProtozeroToText(const DescriptorPool& pool,
                            const std::string& type,
                            protozero::ConstBytes protobytes,
                            NewLinesMode new_lines_mode) {
  std::string indent = "";
  std::string final_result;
  ProtozeroToTextInternal(type, protobytes, new_lines_mode, pool, &indent,
                          &final_result);
  return final_result;
}

std::string DebugTrackEventProtozeroToText(const std::string& type,
                                           protozero::ConstBytes protobytes) {
  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTrackEventDescriptor.data(),
                                              kTrackEventDescriptor.size());
  PERFETTO_DCHECK(status.ok());
  return ProtozeroToText(pool, type, protobytes, kIncludeNewLines);
}
std::string ShortDebugTrackEventProtozeroToText(
    const std::string& type,
    protozero::ConstBytes protobytes) {
  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTrackEventDescriptor.data(),
                                              kTrackEventDescriptor.size());
  PERFETTO_DCHECK(status.ok());
  return ProtozeroToText(pool, type, protobytes, kSkipNewLines);
}

std::string ProtozeroEnumToText(const std::string& type, int32_t enum_value) {
  DescriptorPool pool;
  auto status = pool.AddFromFileDescriptorSet(kTrackEventDescriptor.data(),
                                              kTrackEventDescriptor.size());
  PERFETTO_DCHECK(status.ok());
  auto opt_enum_descriptor_idx = pool.FindDescriptorIdx(type);
  if (!opt_enum_descriptor_idx) {
    // Fall back to the integer representation of the field.
    return std::to_string(enum_value);
  }
  auto opt_enum_string =
      pool.descriptors()[*opt_enum_descriptor_idx].FindEnumString(enum_value);
  if (!opt_enum_string) {
    // Fall back to the integer representation of the field.
    return std::to_string(enum_value);
  }
  return *opt_enum_string;
}
}  // namespace protozero_to_text
}  // namespace trace_processor
}  // namespace perfetto
