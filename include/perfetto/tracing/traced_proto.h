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

#ifndef INCLUDE_PERFETTO_TRACING_TRACED_PROTO_H_
#define INCLUDE_PERFETTO_TRACING_TRACED_PROTO_H_

#include "perfetto/base/template_util.h"
#include "perfetto/protozero/field_writer.h"
#include "perfetto/protozero/proto_utils.h"

namespace perfetto {
class EventContext;

// A Wrapper around a protozero message to allow C++ classes to specify how it
// should be serialised into the trace:
//
// class Foo {
//  public:
//   void WriteIntoTrace(perfetto::TracedProto<pbzero::Foo> message) {
//     message->set_int_field(int_field_);
//   }
// };
//
// This class also exposes EventContext, e.g. to enable data interning.
//
// NOTE: the functionality below is not ready yet.
// TODO(altimin): Make the interop below possible.
// TracedProto also provides a seamless integration with writing untyped
// values via TracedValue / TracedDictionary / TracedArray:
//
// - TracedValue can be converted to a TracedProto, either by calling
//   TracedValue::WriteProto<T>() or implicitly.
// - If a proto message has a repeating DebugAnnotation debug_annotations
//   field, it can be filled using the TracedDictionary obtained from
//   TracedProto::WriteDebugAnnotations.
template <typename MessageType>
class TracedProto {
 public:
  TracedProto(const TracedProto&) = delete;
  TracedProto& operator=(const TracedProto&) = delete;
  TracedProto& operator=(TracedProto&&) = delete;
  TracedProto(TracedProto&&) = default;
  ~TracedProto() = default;

  MessageType* operator->() const { return message_; }

  MessageType* message() { return message_; }

  EventContext& context() const { return context_; }

 private:
  friend class EventContext;

  TracedProto(MessageType* message, EventContext& context)
      : message_(message), context_(context) {}

  MessageType* const message_;
  EventContext& context_;
};

namespace internal {

// TypedProtoWriter takes the protozero message (TracedProto<MessageType>),
// field description (FieldMetadata) and value and writes the given value
// into the given field of the given protozero message.
//
// This is primarily used for inline writing of typed messages:
// TRACE_EVENT(..., pbzero::Message:kField, value);
//
// Ideally we would use a function here and not a struct, but passing template
// arguments directly to the function (e.g. foo<void>()) isn't supported until
// C++20, so we have to use a helper struct here.
template <typename FieldMetadata>
struct TypedProtoWriter {
 private:
  using ProtoSchemaType = protozero::proto_utils::ProtoSchemaType;
  using RepetitionType = protozero::proto_utils::RepetitionType;

  static_assert(FieldMetadata::kRepetitionType !=
                    RepetitionType::kRepeatedPacked,
                "writing packed fields isn't supported yet");

 public:
  // Implementation note: typename Check=void is used to ensure that SFINAE
  // kicks in and the methods which do not match FieldMetadata do not fail
  // to compile. std::is_same<Check,void> prevents early evaluation of the
  // first enable_if_t argument.

  // Simple non-repeated field.
  template <typename Proto, typename ValueType, typename Check = void>
  static typename base::enable_if_t<
      FieldMetadata::kProtoFieldType != ProtoSchemaType::kMessage &&
      FieldMetadata::kRepetitionType == RepetitionType::kNotRepeated &&
      std::is_same<Check, void>::value>
  Write(TracedProto<Proto> context, ValueType&& value) {
    protozero::internal::FieldWriter<FieldMetadata::kProtoFieldType>::Append(
        *context.message(), FieldMetadata::kFieldId, value);
  }

  // Simple repeated non-packed field.
  template <typename Proto, typename ValueType, typename Check = void>
  static typename base::enable_if_t<
      FieldMetadata::kProtoFieldType != ProtoSchemaType::kMessage &&
      FieldMetadata::kRepetitionType == RepetitionType::kRepeatedNotPacked &&
      std::is_same<Check, void>::value>
  Write(TracedProto<Proto> context, ValueType&& value) {
    for (auto&& item : value) {
      protozero::internal::FieldWriter<FieldMetadata::kProtoFieldType>::Append(
          *context.message(), FieldMetadata::kFieldId, item);
    }
  }

  // Nested non-repeated field.
  template <typename Proto, typename ValueType, typename Check = void>
  static typename base::enable_if_t<
      FieldMetadata::kProtoFieldType == ProtoSchemaType::kMessage &&
      FieldMetadata::kRepetitionType == RepetitionType::kNotRepeated &&
      std::is_same<Check, void>::value>
  Write(TracedProto<Proto> context, ValueType&& value) {
    // TODO(altimin): support TraceFormatTraits here.
    value.WriteIntoTrace(
        context.context().Wrap(context.message()
                                   ->template BeginNestedMessage<
                                       typename FieldMetadata::cpp_field_type>(
                                       FieldMetadata::kFieldId)));
  }

  // Nested repeated non-packed field.
  template <typename Proto, typename ValueType, typename Check = void>
  static typename base::enable_if_t<
      FieldMetadata::kProtoFieldType == ProtoSchemaType::kMessage &&
      FieldMetadata::kRepetitionType == RepetitionType::kRepeatedNotPacked &&
      std::is_same<Check, void>::value>
  Write(TracedProto<Proto> context, ValueType&& value) {
    // TODO(altimin): support TraceFormatTraits here.
    for (auto&& item : value) {
      item.WriteIntoTrace(context.context().Wrap(
          context.message()
              ->template BeginNestedMessage<
                  typename FieldMetadata::cpp_field_type>(
                  FieldMetadata::kFieldId)));
    }
  }
};

}  // namespace internal

template <typename MessageType, typename FieldMetadataType, typename ValueType>
void WriteIntoTracedProto(
    TracedProto<MessageType> message,
    protozero::proto_utils::internal::FieldMetadataHelper<FieldMetadataType>,
    ValueType&& value) {
  static_assert(
      std::is_base_of<protozero::proto_utils::FieldMetadataBase,
                      FieldMetadataType>::value,
      "Field name should be a protozero::internal::FieldMetadata<...>");
  static_assert(
      std::is_base_of<MessageType,
                      typename FieldMetadataType::message_type>::value,
      "Field's parent type should match the context.");

  internal::TypedProtoWriter<FieldMetadataType>::Write(
      std::move(message), std::forward<ValueType>(value));
}

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_TRACED_PROTO_H_
