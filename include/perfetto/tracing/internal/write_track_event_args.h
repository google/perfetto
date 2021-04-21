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

#ifndef INCLUDE_PERFETTO_TRACING_INTERNAL_WRITE_TRACK_EVENT_ARGS_H_
#define INCLUDE_PERFETTO_TRACING_INTERNAL_WRITE_TRACK_EVENT_ARGS_H_

#include "perfetto/base/compiler.h"
#include "perfetto/tracing/event_context.h"
#include "perfetto/tracing/traced_proto.h"

namespace perfetto {
namespace internal {

// Helper function handling filling provided |EventContext| from the provided
// arguments, which include:
// - Lambda functions,
// - Debug annotations.
//
// TRACE_EVENT parameters which do not translate to directly writing something
// into TrackEvent proto (like tracks and timestamps are _not_ covered by this
// function).
template <typename... Args, typename TypeCheck = void>
void WriteTrackEventArgs(EventContext event_context, Args&&... args);

// No arguments means that we don't have to write anything.
template <>
PERFETTO_ALWAYS_INLINE inline void WriteTrackEventArgs(EventContext) {}

namespace {

// A template helper for determining whether a type can be used as a track event
// lambda, i.e., it has the signature "void(EventContext)". This is achieved by
// checking that we can pass an EventContext value (the inner declval) into a T
// instance (the outer declval). If this is a valid expression, the result
// evaluates to sizeof(0), i.e., true.
// TODO(skyostil): Replace this with std::is_convertible<std::function<...>>
// once we have C++14.
template <typename T>
static constexpr bool IsValidTraceLambdaImpl(
    typename std::enable_if<static_cast<bool>(
        sizeof(std::declval<T>()(std::declval<EventContext>()), 0))>::type* =
        nullptr) {
  return true;
}

template <typename T>
static constexpr bool IsValidTraceLambdaImpl(...) {
  return false;
}

template <typename T>
static constexpr bool IsValidTraceLambda() {
  return IsValidTraceLambdaImpl<T>(nullptr);
}

}  // namespace

// Write a lambda.
// TODO(altimin): At the moment lambda takes EventContext, which is
// non-copyable, so only one lambda is supported and it has to be the last
// argument.
template <typename ArgumentFunction,
          typename ArgFunctionCheck = typename std::enable_if<
              IsValidTraceLambda<ArgumentFunction>()>::type>
PERFETTO_ALWAYS_INLINE void WriteTrackEventArgs(EventContext event_ctx,
                                                ArgumentFunction arg_function) {
  arg_function(std::move(event_ctx));
}

// Write one debug annotation and recursively write the rest of the arguments.
template <typename ArgValue, typename... Args>
PERFETTO_ALWAYS_INLINE void WriteTrackEventArgs(EventContext event_ctx,
                                                const char* arg_name,
                                                ArgValue&& arg_value,
                                                Args&&... args) {
  TrackEventInternal::AddDebugAnnotation(&event_ctx, arg_name,
                                         std::forward<ArgValue>(arg_value));
  WriteTrackEventArgs(std::move(event_ctx), std::forward<Args>(args)...);
}

// Write one typed message and recursively write the rest of the arguments.
template <typename FieldMetadataType,
          typename ArgValue,
          typename... Args,
          typename Check = base::enable_if_t<
              std::is_base_of<protozero::proto_utils::FieldMetadataBase,
                              FieldMetadataType>::value>>
PERFETTO_ALWAYS_INLINE void WriteTrackEventArgs(
    EventContext event_ctx,
    protozero::proto_utils::internal::FieldMetadataHelper<FieldMetadataType>
        field_name,
    ArgValue&& arg_value,
    Args&&... args) {
  static_assert(
      std::is_base_of<protos::pbzero::TrackEvent,
                      typename FieldMetadataType::message_type>::value,
      "Only fields of TrackEvent (and TrackEvent's extensions) can "
      "be passed to TRACE_EVENT");
  WriteIntoTracedProto(
      event_ctx.Wrap(
          event_ctx.event<typename FieldMetadataType::message_type>()),
      field_name, std::forward<ArgValue>(arg_value));
  WriteTrackEventArgs(std::move(event_ctx), std::forward<Args>(args)...);
}

}  // namespace internal
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_INTERNAL_WRITE_TRACK_EVENT_ARGS_H_
