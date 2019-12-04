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

#ifndef INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_MACROS_H_
#define INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_MACROS_H_

// This file contains underlying macros for the trace point track event
// implementation. Perfetto API users typically don't need to use anything here
// directly.

#include "perfetto/base/compiler.h"
#include "perfetto/tracing/internal/track_event_data_source.h"
#include "perfetto/tracing/track_event_category_registry.h"

// Defines data structures for backing a category registry.
//
// Each category has one enabled/disabled bit per possible data source instance.
// The bits are packed, i.e., each byte holds the state for instances. To
// improve cache locality, the bits for each instance are stored separately from
// the names of the categories:
//
//   byte 0                      byte 1
//   (inst0, inst1, ..., inst7), (inst0, inst1, ..., inst7)
//
#define PERFETTO_INTERNAL_DECLARE_CATEGORIES(...)                             \
  namespace internal {                                                        \
  constexpr ::perfetto::internal::TrackEventCategory kCategories[] = {        \
      __VA_ARGS__};                                                           \
  constexpr size_t kCategoryCount =                                           \
      sizeof(kCategories) / sizeof(kCategories[0]);                           \
  /* The per-instance enable/disable state per category */                    \
  extern std::atomic<uint8_t> g_category_state_storage[kCategoryCount];       \
  /* The category registry which mediates access to the above structures. */  \
  /* The registry is used for two purposes: */                                \
  /**/                                                                        \
  /*    1) For looking up categories at build (constexpr) time. */            \
  /*    2) For declaring the per-namespace TrackEvent data source. */         \
  /**/                                                                        \
  /* Because usage #1 requires a constexpr type and usage #2 requires an */   \
  /* extern type (to avoid declaring a type based on a translation-unit */    \
  /* variable), we need two separate copies of the registry with different */ \
  /* storage specifiers. */                                                   \
  /**/                                                                        \
  /* TODO(skyostil): Unify these using a C++17 inline constexpr variable. */  \
  constexpr ::perfetto::internal::TrackEventCategoryRegistry                  \
      kConstExprCategoryRegistry(kCategoryCount,                              \
                                 &kCategories[0],                             \
                                 &g_category_state_storage[0]);               \
  extern const ::perfetto::internal::TrackEventCategoryRegistry               \
      kCategoryRegistry;                                                      \
  static_assert(kConstExprCategoryRegistry.ValidateCategories(),              \
                "Invalid category names found");                              \
  }  // namespace internal

// In a .cc file, declares storage for each category's runtime state.
#define PERFETTO_INTERNAL_CATEGORY_STORAGE()                     \
  namespace internal {                                           \
  std::atomic<uint8_t> g_category_state_storage[kCategoryCount]; \
  constexpr ::perfetto::internal::TrackEventCategoryRegistry     \
      kCategoryRegistry(kCategoryCount,                          \
                        &kCategories[0],                         \
                        &g_category_state_storage[0]);           \
  }  // namespace internal

// Defines the TrackEvent data source for the current track event namespace.
#define PERFETTO_INTERNAL_DECLARE_TRACK_EVENT_DATA_SOURCE()              \
  struct TrackEvent : public ::perfetto::internal::TrackEventDataSource< \
                          TrackEvent, &internal::kCategoryRegistry> {}

// At compile time, turns a category name represented by a static string into an
// index into the current category registry. A build error will be generated if
// the category hasn't been registered. See PERFETTO_DEFINE_CATEGORIES.
#define PERFETTO_GET_CATEGORY_INDEX(category)                                \
  ::perfetto::internal::TrackEventCategoryRegistry::Validate<                \
      ::PERFETTO_TRACK_EVENT_NAMESPACE::internal::kConstExprCategoryRegistry \
          .Find(category)>()

// Efficiently determines whether tracing is enabled for the given category, and
// if so, emits one trace event with the given arguments.
#define PERFETTO_INTERNAL_TRACK_EVENT(category, name, ...)                    \
  ::PERFETTO_TRACK_EVENT_NAMESPACE::TrackEvent::CallIfCategoryEnabled<        \
      PERFETTO_GET_CATEGORY_INDEX(category)>([&](uint32_t instances) {        \
    /* Check that |name| is a static string. If this fails, you probably want \
     * to use "constexpr const char*" if the string is computed by an         \
     * expression, or something like this if the string is fully dynamic:     \
     *                                                                        \
     *   TRACE_EVENT("category", nullptr, [&](perfetto::EventContext ctx) {   \
     *     ctx.event()->set_name(dynamic_name);                               \
     *   }                                                                    \
     */                                                                       \
    static_assert(                                                            \
        (name) == nullptr || (name) != nullptr,                               \
        "Track event name must be a (constexpr) static string. Use a trace "  \
        "lambda if you need a dynamic name (see above).");                    \
    ::PERFETTO_TRACK_EVENT_NAMESPACE::TrackEvent::TraceForCategory<           \
        PERFETTO_GET_CATEGORY_INDEX(category)>(instances, name,               \
                                               ##__VA_ARGS__);                \
  })

// Generate a unique variable name with a given prefix.
#define PERFETTO_INTERNAL_CONCAT2(a, b) a##b
#define PERFETTO_INTERNAL_CONCAT(a, b) PERFETTO_INTERNAL_CONCAT2(a, b)
#define PERFETTO_INTERNAL_UID(prefix) PERFETTO_INTERNAL_CONCAT(prefix, __LINE__)

#define PERFETTO_INTERNAL_SCOPED_TRACK_EVENT(category, name, ...)             \
  struct {                                                                    \
    struct EventFinalizer {                                                   \
      /* The int parameter is an implementation detail. It allows the      */ \
      /* anonymous struct to use aggregate initialization to invoke the    */ \
      /* lambda (which emits the BEGIN event and returns an integer)       */ \
      /* with the proper reference capture for any                         */ \
      /* TrackEventArgumentFunction in |__VA_ARGS__|. This is required so  */ \
      /* that the scoped event is exactly ONE line and can't escape the    */ \
      /* scope if used in a single line if statement.                      */ \
      EventFinalizer(int) {}                                                  \
      ~EventFinalizer() { TRACE_EVENT_END(category); }                        \
    } finalizer;                                                              \
  } PERFETTO_INTERNAL_UID(scoped_event) {                                     \
    [&]() {                                                                   \
      TRACE_EVENT_BEGIN(category, name, ##__VA_ARGS__);                       \
      return 0;                                                               \
    }()                                                                       \
  }

#endif  // INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_MACROS_H_
