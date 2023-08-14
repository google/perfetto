/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_PUBLIC_TE_MACROS_H_
#define INCLUDE_PERFETTO_PUBLIC_TE_MACROS_H_

#include <assert.h>

#include "perfetto/public/abi/track_event_hl_abi.h"
#include "perfetto/public/track_event.h"

// This header defines the PERFETTO_TE macros and its possible params (at the
// end of the file). The rest of the file contains internal implementation
// details of the macros, which are subject to change at any time.
//
// The macro uses the High level ABI to emit track events.

#define PERFETTO_I_TE_STATIC_ASSERT_NUM_PARAMS_(                              \
    NAME_AND_TYPE1, NAME_AND_TYPE2, EXTRA1, EXTRA2, EXTRA3, EXTRA4, SENTINEL, \
    ...)                                                                      \
  static_assert((SENTINEL) == 0,                                              \
                "Too many arguments for PERFETTO_TE "                         \
                "macro")

// Fails to compile if there are too many params and they don't fit into
// PerfettoTeHlMacroParams.
#define PERFETTO_I_TE_STATIC_ASSERT_NUM_PARAMS(...) \
  PERFETTO_I_TE_STATIC_ASSERT_NUM_PARAMS_(__VA_ARGS__, 0, 0, 0, 0, 0, 0)

#define PERFETTO_I_TE_LIMIT_4__(NAME_AND_TYPE1, NAME_AND_TYPE2, EXTRA1, \
                                EXTRA2, EXTRA3, EXTRA4, ...)            \
  NAME_AND_TYPE1, NAME_AND_TYPE2, EXTRA1, EXTRA2, EXTRA3, EXTRA4
#define PERFETTO_I_TE_LIMIT_4_(MACRO, ARGS) MACRO ARGS
#define PERFETTO_I_TE_LIMIT_4(...) \
  PERFETTO_I_TE_LIMIT_4_(PERFETTO_I_TE_LIMIT_4__, (__VA_ARGS__))

// In C we have to use a compound literal. In C++ we can use a regular
// initializer.
#ifndef __cplusplus
#define PERFETTO_I_TE_HL_MACRO_PARAMS_PREAMBLE (struct PerfettoTeHlMacroParams)
#else
#define PERFETTO_I_TE_HL_MACRO_PARAMS_PREAMBLE
#endif

// Provides an initializer for `struct PerfettoTeHlMacroParams` and sets all the
// unused extra fields to PERFETTO_NULL.
#define PERFETTO_I_TE_HL_MACRO_PARAMS(...)                             \
  PERFETTO_I_TE_HL_MACRO_PARAMS_PREAMBLE {                             \
    PERFETTO_I_TE_LIMIT_4(__VA_ARGS__, PERFETTO_NULL, PERFETTO_NULL,   \
                          PERFETTO_NULL, PERFETTO_NULL, PERFETTO_NULL, \
                          PERFETTO_NULL)                               \
  }

#ifndef __cplusplus
#define PERFETTO_I_TE_COMPOUND_LITERAL_ADDR(STRUCT, ...) \
  &(struct STRUCT)__VA_ARGS__
#define PERFETTO_I_TE_EXTRA(STRUCT, ...)                           \
  ((struct PerfettoTeHlExtra*)PERFETTO_I_TE_COMPOUND_LITERAL_ADDR( \
      STRUCT, __VA_ARGS__))
#else
#define PERFETTO_I_TE_COMPOUND_LITERAL_ADDR(STRUCT, ...) \
  &(STRUCT{} = STRUCT __VA_ARGS__)
#define PERFETTO_I_TE_EXTRA(STRUCT, ...)       \
  reinterpret_cast<struct PerfettoTeHlExtra*>( \
      PERFETTO_I_TE_COMPOUND_LITERAL_ADDR(STRUCT, __VA_ARGS__))
#endif

struct PerfettoTeHlMacroNameAndType {
  const char* name;
  int32_t type;
};

struct PerfettoTeHlMacroParams {
  struct PerfettoTeHlMacroNameAndType name_and_type;
  struct PerfettoTeHlExtra* extra1;
  struct PerfettoTeHlExtra* extra2;
  struct PerfettoTeHlExtra* extra3;
  struct PerfettoTeHlExtra* extra4;
};

static inline void PerfettoTeHlCall(struct PerfettoTeCategoryImpl* cat,
                                    struct PerfettoTeHlMacroParams params) {
  struct PerfettoTeHlExtra* perfetto_i_extra_data = PERFETTO_NULL;
  if (params.extra1) {
    params.extra1->next = perfetto_i_extra_data;
    perfetto_i_extra_data = params.extra1;
  }
  if (params.extra2) {
    params.extra2->next = perfetto_i_extra_data;
    perfetto_i_extra_data = params.extra2;
  }
  if (params.extra3) {
    params.extra3->next = perfetto_i_extra_data;
    perfetto_i_extra_data = params.extra3;
  }
  if (params.extra4) {
    params.extra4->next = perfetto_i_extra_data;
    perfetto_i_extra_data = params.extra4;
  }
  PerfettoTeHlEmitImpl(cat, params.name_and_type.type,
                       params.name_and_type.name, perfetto_i_extra_data);
}

// Instead of a previously registered category, this macro can be used to
// specify that the category will be provided dynamically as a param.
#define PERFETTO_TE_DYNAMIC_CATEGORY PerfettoTeRegisteredDynamicCategory()

// -------------------------------------------------
// Possible types of event for the PERFETTO_TE macro
// -------------------------------------------------

// Begins a slice named `const char* NAME` on a track.
#define PERFETTO_TE_SLICE_BEGIN(NAME) \
  { NAME, PERFETTO_TE_TYPE_SLICE_BEGIN }

// Ends the last slice opened on a track.
#define PERFETTO_TE_SLICE_END() \
  { PERFETTO_NULL, PERFETTO_TE_TYPE_SLICE_END }

// Reports an instant event named `const char* NAME`.
#define PERFETTO_TE_INSTANT(NAME) \
  { NAME, PERFETTO_TE_TYPE_INSTANT }

// Reports the value of a counter. The counter value must be specified
// separately on another param with PERFETTO_TE_INT_COUNTER() or
// PERFETTO_TE_DOUBLE_COUNTER().
#define PERFETTO_TE_COUNTER() \
  { PERFETTO_NULL, PERFETTO_TE_TYPE_COUNTER }

// -------------------------------------------------
// Possible types of event for the PERFETTO_TE macro
// -------------------------------------------------

// The value (`C`) of an integer counter. A separate parameter must describe the
// counter track this refers to. This should only be used for events with
// type PERFETTO_TE_COUNTER().
#define PERFETTO_TE_INT_COUNTER(C)   \
  PERFETTO_I_TE_EXTRA(               \
      PerfettoTeHlExtraCounterInt64, \
      {{PERFETTO_TE_HL_EXTRA_TYPE_COUNTER_INT64, PERFETTO_NULL}, C})

// The value (`C`) of a floating point. A separate parameter must describe the
// counter track this refers to. This should only be used for events with type
// PERFETTO_TE_COUNTER().
#define PERFETTO_TE_DOUBLE_COUNTER(C) \
  PERFETTO_I_TE_EXTRA(                \
      PerfettoTeHlExtraCounterDouble, \
      {{PERFETTO_TE_HL_EXTRA_TYPE_COUNTER_DOUBLE, PERFETTO_NULL}, C})

// Uses the timestamp `struct PerfettoTeTimestamp T` to report this event. If
// this is not specified, PERFETTO_TE() reads the current timestamp with
// PerfettoTeGetTimestamp().
#define PERFETTO_TE_TIMESTAMP(T)  \
  PERFETTO_I_TE_EXTRA(            \
      PerfettoTeHlExtraTimestamp, \
      {{PERFETTO_TE_HL_EXTRA_TYPE_TIMESTAMP, PERFETTO_NULL}, T})

// Specifies that the current track for this event is
// `struct PerfettoTeRegisteredTrack* T`, which must have been previously
// registered.
#define PERFETTO_TE_REGISTERED_TRACK(T)                             \
  PERFETTO_I_TE_EXTRA(                                              \
      PerfettoTeHlExtraRegisteredTrack,                             \
      {{PERFETTO_TE_HL_EXTRA_TYPE_REGISTERED_TRACK, PERFETTO_NULL}, \
       &(T)->impl})

// Specifies that the current track for this event is a track named `const char
// *NAME`, child of a track whose uuid is `PARENT_UUID`. `NAME`, `uint64_t ID`
// and `PARENT_UUID` uniquely identify a track. Common values for `PARENT_UUID`
// include PerfettoTeProcessTrackUuid(), PerfettoTeThreadTrackUuid() or
// PerfettoTeGlobalTrackUuid().
#define PERFETTO_TE_NAMED_TRACK(NAME, ID, PARENT_UUID)                         \
  PERFETTO_I_TE_EXTRA(PerfettoTeHlExtraNamedTrack,                             \
                      {{PERFETTO_TE_HL_EXTRA_TYPE_NAMED_TRACK, PERFETTO_NULL}, \
                       NAME,                                                   \
                       ID,                                                     \
                       PARENT_UUID})

// When PERFETTO_TE_DYNAMIC_CATEGORY is used, this is used to specify `const
// char* S` as a category name.
#define PERFETTO_TE_DYNAMIC_CATEGORY_STRING(S)                      \
  PERFETTO_I_TE_EXTRA(                                              \
      PerfettoTeHlExtraDynamicCategory,                             \
      {{PERFETTO_TE_HL_EXTRA_TYPE_DYNAMIC_CATEGORY, PERFETTO_NULL}, \
       PERFETTO_I_TE_COMPOUND_LITERAL_ADDR(                         \
           PerfettoTeCategoryDescriptor,                            \
           {S, PERFETTO_NULL, PERFETTO_NULL, 0})})

// Adds the debug annotation named `const char * NAME` with value `bool VALUE`.
#define PERFETTO_TE_ARG_BOOL(NAME, VALUE)                         \
  PERFETTO_I_TE_EXTRA(                                            \
      PerfettoTeHlExtraDebugArgBool,                              \
      {{PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_BOOL, PERFETTO_NULL}, \
       NAME,                                                      \
       VALUE})

// Adds the debug annotation named `const char * NAME` with value `uint64_t
// VALUE`.
#define PERFETTO_TE_ARG_UINT64(NAME, VALUE)                         \
  PERFETTO_I_TE_EXTRA(                                              \
      PerfettoTeHlExtraDebugArgUint64,                              \
      {{PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_UINT64, PERFETTO_NULL}, \
       NAME,                                                        \
       VALUE})

// Adds the debug annotation named `const char * NAME` with value `int64_t
// VALUE`.
#define PERFETTO_TE_ARG_INT64(NAME, VALUE)                         \
  PERFETTO_I_TE_EXTRA(                                             \
      PerfettoTeHlExtraDebugArgInt64,                              \
      {{PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_INT64, PERFETTO_NULL}, \
       NAME,                                                       \
       VALUE})

// Adds the debug annotation named `const char * NAME` with value `double
// VALUE`.
#define PERFETTO_TE_ARG_DOUBLE(NAME, VALUE)                         \
  PERFETTO_I_TE_EXTRA(                                              \
      PerfettoTeHlExtraDebugArgDouble,                              \
      {{PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_DOUBLE, PERFETTO_NULL}, \
       NAME,                                                        \
       VALUE})

// Adds the debug annotation named `const char * NAME` with value `const char*
// VALUE`.
#define PERFETTO_TE_ARG_STRING(NAME, VALUE)                         \
  PERFETTO_I_TE_EXTRA(                                              \
      PerfettoTeHlExtraDebugArgString,                              \
      {{PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_STRING, PERFETTO_NULL}, \
       NAME,                                                        \
       VALUE})

// Adds the debug annotation named `const char * NAME` with value `void* VALUE`.
#define PERFETTO_TE_ARG_POINTER(NAME, VALUE)                         \
  PERFETTO_I_TE_EXTRA(                                               \
      PerfettoTeHlExtraDebugArgPointer,                              \
      {{PERFETTO_TE_HL_EXTRA_TYPE_DEBUG_ARG_POINTER, PERFETTO_NULL}, \
       NAME,                                                         \
       VALUE})

// Specifies that this event is part (or starts) a "flow" (i.e. a link among
// different events). The flow is identified by `struct PerfettoTeFlow VALUE`.
#define PERFETTO_TE_FLOW(VALUE) \
  PERFETTO_I_TE_EXTRA(          \
      PerfettoTeHlExtraFlow,    \
      {{PERFETTO_TE_HL_EXTRA_TYPE_FLOW, PERFETTO_NULL}, (VALUE).id})

// Specifies that this event terminates a "flow" (i.e. a link among different
// events). The flow is identified by `struct PerfettoTeFlow VALUE`.
#define PERFETTO_TE_TERMINATING_FLOW(VALUE)                         \
  PERFETTO_I_TE_EXTRA(                                              \
      PerfettoTeHlExtraFlow,                                        \
      {{PERFETTO_TE_HL_EXTRA_TYPE_TERMINATING_FLOW, PERFETTO_NULL}, \
       (VALUE).id})

// Flushes the shared memory buffer and makes sure that all the previous events
// emitted by this thread are visibile in the central tracing buffer.
#define PERFETTO_TE_FLUSH()              \
  PERFETTO_I_TE_EXTRA(PerfettoTeHlExtra, \
                      {PERFETTO_TE_HL_EXTRA_TYPE_FLUSH, PERFETTO_NULL})

// Turns off interning for event names.
#define PERFETTO_TE_NO_INTERN()          \
  PERFETTO_I_TE_EXTRA(PerfettoTeHlExtra, \
                      {PERFETTO_TE_HL_EXTRA_TYPE_NO_INTERN, PERFETTO_NULL})

// ----------------------------------
// The main PERFETTO_TE tracing macro
// ----------------------------------
//
// If tracing is active and the passed tracing category is enabled, adds an
// entry in the tracing stream of the perfetto track event data source.
// Parameters:
// * `CAT`: The tracing category (it should be a struct
//   PerfettoTeCategory object). It can be
//   PERFETTO_TE_DYNAMIC_CATEGORY for dynamic categories (the dynamic category
//   name should be passed later with)
// * The type of the event. It can be one of:
//   * PERFETTO_TE_SLICE_BEGIN(name)
//   * PERFETTO_TE_SLICE_END()
//   * PERFETTO_TE_INSTANT()
//   * PERFETTO_TE_COUNTER()
// * `...`: One or more (up to 4) macro parameters from the above list that
//   specify the data to be traced.
//
// Examples:
//
// PERFETTO_TE(category, PERFETTO_TE_SLICE_BEGIN("name"),
//             PERFETTO_TE_ARG_UINT64("extra_arg", 42));
// PERFETTO_TE(category, PERFETTO_TE_SLICE_END());
// PERFETTO_TE(category, PERFETTO_TE_COUNTER(),
//             PERFETTO_TE_REGISTERED_TRACK(&mycounter),
//             PERFETTO_TE_INT_COUNTER(79));
// PERFETTO_TE(PERFETTO_TE_DYNAMIC_CATEGORY, PERFETTO_TE_INSTANT("instant"),
//             PERFETTO_TE_DYNAMIC_CATEGORY_STRING("category"));
//
#define PERFETTO_TE(CAT, ...)                                       \
  do {                                                              \
    if (PERFETTO_UNLIKELY(PERFETTO_ATOMIC_LOAD_EXPLICIT(            \
            (CAT).enabled, PERFETTO_MEMORY_ORDER_RELAXED))) {       \
      PERFETTO_I_TE_STATIC_ASSERT_NUM_PARAMS(__VA_ARGS__);          \
      PerfettoTeHlCall((CAT).impl,                                  \
                       PERFETTO_I_TE_HL_MACRO_PARAMS(__VA_ARGS__)); \
    }                                                               \
  } while (0)

#endif  // INCLUDE_PERFETTO_PUBLIC_TE_MACROS_H_
