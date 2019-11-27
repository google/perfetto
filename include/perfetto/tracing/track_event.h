/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_TRACING_TRACK_EVENT_H_
#define INCLUDE_PERFETTO_TRACING_TRACK_EVENT_H_

#include "perfetto/base/time.h"
#include "perfetto/tracing/internal/track_event_data_source.h"
#include "perfetto/tracing/internal/track_event_internal.h"
#include "perfetto/tracing/internal/track_event_macros.h"
#include "perfetto/tracing/track_event_category_registry.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

// This file contains a set of macros designed for instrumenting applications
// with track event trace points. While the underlying TrackEvent API can also
// be used directly, doing so efficiently requires some care (e.g., to avoid
// evaluating arguments while tracing is disabled). These types of optimizations
// are abstracted away by the macros below.
//
// ================
// Quickstart guide
// ================
//
//   To add track events to your application, first define your categories in,
//   e.g., my_tracing.h:
//
//       PERFETTO_DEFINE_CATEGORIES(
//           PERFETTO_CATEGORY(base),
//           PERFETTO_CATEGORY(v8),
//           PERFETTO_CATEGORY(cc));
//
//   Then in a single .cc file, e.g., my_tracing.cc:
//
//       #include "my_tracing.h"
//       PERFETTO_TRACK_EVENT_STATIC_STORAGE();
//
//   Finally, register track events at startup, after which you can record
//   events with the TRACE_EVENT macros:
//
//       #include "my_tracing.h"
//
//       int main() {
//         perfetto::TrackEvent::Register();
//
//         // A basic track event with just a name.
//         TRACE_EVENT("category", "MyEvent");
//
//         // A track event with (up to two) debug annotations.
//         TRACE_EVENT("category", "MyEvent", "parameter", 42);
//
//         // A track event with a strongly typed parameter.
//         TRACE_EVENT("category", "MyEvent", [](perfetto::EventContext ctx) {
//           ctx.event()->set_foo(42);
//           ctx.event()->set_bar(.5f);
//         });
//       }
//
// ====================
// Implementation notes
// ====================
//
// The track event library consists of the following layers and components. The
// classes the internal namespace shouldn't be considered part of the public
// API.
//                    .--------------------------------.
//               .----|  TRACE_EVENT                   |----.
//      write   |     |   - App instrumentation point  |     |  write
//      event   |     '--------------------------------'     |  arguments
//              V                                            V
//  .----------------------------------.    .-----------------------------.
//  | TrackEvent                       |    | EventContext                |
//  |  - Registry of event categories  |    |  - One track event instance |
//  '----------------------------------'    '-----------------------------'
//              |                                            |
//              |                                            | look up
//              | is                                         | interning ids
//              V                                            V
//  .----------------------------------.    .-----------------------------.
//  | internal::TrackEventDataSource   |    | TrackEventInternedDataIndex |
//  | - Perfetto data source           |    | - Corresponds to a field in |
//  | - Has TrackEventIncrementalState |    |   in interned_data.proto    |
//  '----------------------------------'    '-----------------------------'
//              |                  |                         ^
//              |                  |       owns (1:many)     |
//              | write event      '-------------------------'
//              V
//  .----------------------------------.
//  | internal::TrackEventInternal     |
//  | - Outlined code to serialize     |
//  |   one track event                |
//  '----------------------------------'
//

// Each compilation unit can be in exactly one track event namespace,
// allowing the overall program to use multiple track event data sources and
// category lists if necessary. Use this macro to select the namespace for the
// current compilation unit.
//
// If the program uses multiple track event namespaces, category & track event
// registration (see quickstart above) needs to happen for both namespaces
// separately.
#ifndef PERFETTO_TRACK_EVENT_NAMESPACE
#define PERFETTO_TRACK_EVENT_NAMESPACE perfetto
#endif

// A name for a single category. Wrapped in a macro in case we need to introduce
// more fields in the future.
#define PERFETTO_CATEGORY(name) \
  { #name }

// Register the set of available categories by passing a list of categories to
// this macro: PERFETTO_CATEGORY(cat1), PERFETTO_CATEGORY(cat2), ...
#define PERFETTO_DEFINE_CATEGORIES(...)                        \
  namespace PERFETTO_TRACK_EVENT_NAMESPACE {                   \
  /* The list of category names */                             \
  PERFETTO_INTERNAL_DECLARE_CATEGORIES(__VA_ARGS__);           \
  /* The track event data source for this set of categories */ \
  PERFETTO_INTERNAL_DECLARE_TRACK_EVENT_DATA_SOURCE();         \
  }  // namespace PERFETTO_TRACK_EVENT_NAMESPACE

// Allocate storage for each category by using this macro once per track event
// namespace.
#define PERFETTO_TRACK_EVENT_STATIC_STORAGE() \
  namespace PERFETTO_TRACK_EVENT_NAMESPACE {  \
  PERFETTO_INTERNAL_CATEGORY_STORAGE();       \
  }  // namespace PERFETTO_TRACK_EVENT_NAMESPACE

// Begin a thread-scoped slice under |category| with the title |name|. Both
// strings must be static constants. The track event is only recorded if
// |category| is enabled for a tracing session.
#define TRACE_EVENT_BEGIN(category, name, ...) \
  PERFETTO_INTERNAL_TRACK_EVENT(               \
      category, name,                          \
      ::perfetto::protos::pbzero::TrackEvent::TYPE_SLICE_BEGIN, ##__VA_ARGS__)

// End a thread-scoped slice under |category|.
#define TRACE_EVENT_END(category, ...) \
  PERFETTO_INTERNAL_TRACK_EVENT(       \
      category, nullptr,               \
      ::perfetto::protos::pbzero::TrackEvent::TYPE_SLICE_END, ##__VA_ARGS__)

// Begin a thread-scoped slice which gets automatically closed when going out of
// scope.
#define TRACE_EVENT(category, name, ...) \
  PERFETTO_INTERNAL_SCOPED_TRACK_EVENT(category, name, ##__VA_ARGS__)

// Emit a thread-scoped slice which has zero duration.
// TODO(skyostil): Add support for process-wide and global instant events.
#define TRACE_EVENT_INSTANT(category, name, ...)                            \
  PERFETTO_INTERNAL_TRACK_EVENT(                                            \
      category, name, ::perfetto::protos::pbzero::TrackEvent::TYPE_INSTANT, \
      ##__VA_ARGS__)

// TODO(skyostil): Add arguments.
// TODO(skyostil): Add async events.
// TODO(skyostil): Add flow events.
// TODO(skyostil): Add counters.

#endif  // INCLUDE_PERFETTO_TRACING_TRACK_EVENT_H_
