/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PROCESSOR_C_H_
#define INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PROCESSOR_C_H_

// C API for Trace Processor, intended for use from other languages via FFI.
//
// This header is self-contained so it can be shipped alongside
// libtrace_processor_c without the rest of the Perfetto headers. The API is
// intentionally minimal to keep the ABI small: all objects are opaque and no
// structs cross the boundary.
//
// Conventions:
// - Destroy functions accept NULL.
// - Functions which can fail return a `struct PerfettoTpError*`: NULL on
//   success, otherwise an error owned by the caller which must be released
//   with PerfettoTpErrorDestroy().
// - Strings are NUL-terminated and UTF-8 encoded.
// - Objects are not thread-safe: a PerfettoTp and the iterators obtained from
//   it must only be used from one thread at a time. Different PerfettoTp
//   instances can be used concurrently on different threads.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(PERFETTO_TP_C_IMPLEMENTATION)
#ifdef _WIN32
#define PERFETTO_TP_C_EXPORT __declspec(dllexport)
#else
#define PERFETTO_TP_C_EXPORT __attribute__((visibility("default")))
#endif
#else
#define PERFETTO_TP_C_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct PerfettoTp;
struct PerfettoTpIterator;
struct PerfettoTpError;

// =====================================================================
// Errors
// =====================================================================

// Returns the error message. The string is owned by `err`.
PERFETTO_TP_C_EXPORT const char* PerfettoTpErrorMessage(
    const struct PerfettoTpError* err);

PERFETTO_TP_C_EXPORT void PerfettoTpErrorDestroy(struct PerfettoTpError* err);

// =====================================================================
// Trace Processor
// =====================================================================

// Creates a Trace Processor instance with the default config.
PERFETTO_TP_C_EXPORT struct PerfettoTp* PerfettoTpCreate(void);

// Destroys `tp`. All iterators obtained from `tp` must be destroyed first.
PERFETTO_TP_C_EXPORT void PerfettoTpDestroy(struct PerfettoTp* tp);

// Releases data passed to PerfettoTpParse().
typedef void (*PerfettoTpDeleter)(void* ctx);

// Pushes a chunk of trace data. The trace format is detected on the first
// call. Once an error is returned, all further calls fail.
//
// The data is not copied: `data` must stay valid and unmodified until
// `deleter` is invoked with `ctx`. `deleter` is invoked exactly once, when
// Trace Processor no longer references the data (at the latest during
// PerfettoTpDestroy()), even if an error is returned. `deleter` may be NULL if
// `data` outlives `tp`.
//
// To parse a large buffer (e.g. a memory-mapped file) in chunks, pass each
// chunk separately with a `ctx` holding a reference to the whole buffer.
PERFETTO_TP_C_EXPORT struct PerfettoTpError* PerfettoTpParse(
    struct PerfettoTp* tp,
    const uint8_t* data,
    size_t size,
    void* ctx,
    PerfettoTpDeleter deleter);

// Signals that all trace data has been pushed. Must be called exactly once
// after the last PerfettoTpParse().
PERFETTO_TP_C_EXPORT struct PerfettoTpError* PerfettoTpNotifyEndOfFile(
    struct PerfettoTp* tp);

// Executes `sql`, which can contain multiple semicolon-separated statements.
// All but the last statement are executed before returning; the rows of the
// last statement are returned by the iterator.
//
// Never returns NULL. Errors are reported by PerfettoTpIteratorStatus().
PERFETTO_TP_C_EXPORT struct PerfettoTpIterator* PerfettoTpExecuteQuery(
    struct PerfettoTp* tp,
    const char* sql);

// =====================================================================
// Query iterator
// =====================================================================

enum PerfettoTpValueType {
  PERFETTO_TP_VALUE_TYPE_NULL = 0,
  PERFETTO_TP_VALUE_TYPE_INT64 = 1,
  PERFETTO_TP_VALUE_TYPE_DOUBLE = 2,
  PERFETTO_TP_VALUE_TYPE_STRING = 3,
  PERFETTO_TP_VALUE_TYPE_BYTES = 4,
};

// Advances to the next row. Returns false when there are no more rows or an
// error occurred; call PerfettoTpIteratorStatus() to tell them apart.
PERFETTO_TP_C_EXPORT bool PerfettoTpIteratorNext(struct PerfettoTpIterator* it);

// Returns an error if the query failed, NULL otherwise.
PERFETTO_TP_C_EXPORT struct PerfettoTpError* PerfettoTpIteratorStatus(
    struct PerfettoTpIterator* it);

// Can be called before PerfettoTpIteratorNext().
PERFETTO_TP_C_EXPORT uint32_t
PerfettoTpIteratorColumnCount(struct PerfettoTpIterator* it);

// Returns the name of column `col`, which must be less than the column count.
// The string is valid until the next call to this function.
PERFETTO_TP_C_EXPORT const char* PerfettoTpIteratorColumnName(
    struct PerfettoTpIterator* it,
    uint32_t col);

// The getters below read column `col` of the current row. They must only be
// called after PerfettoTpIteratorNext() returned true, with `col` less than
// the column count. Calling a getter which doesn't match the value type
// returned by PerfettoTpIteratorGetType() aborts.

PERFETTO_TP_C_EXPORT enum PerfettoTpValueType PerfettoTpIteratorGetType(
    struct PerfettoTpIterator* it,
    uint32_t col);

PERFETTO_TP_C_EXPORT int64_t
PerfettoTpIteratorGetInt64(struct PerfettoTpIterator* it, uint32_t col);

PERFETTO_TP_C_EXPORT double PerfettoTpIteratorGetDouble(
    struct PerfettoTpIterator* it,
    uint32_t col);

// Returns a NUL-terminated string, valid until the next
// PerfettoTpIteratorNext() or PerfettoTpIteratorDestroy().
PERFETTO_TP_C_EXPORT const char* PerfettoTpIteratorGetString(
    struct PerfettoTpIterator* it,
    uint32_t col);

// Returns the bytes and stores their count in `*size`. The data is valid until
// the next PerfettoTpIteratorNext() or PerfettoTpIteratorDestroy().
PERFETTO_TP_C_EXPORT const uint8_t* PerfettoTpIteratorGetBytes(
    struct PerfettoTpIterator* it,
    uint32_t col,
    size_t* size);

PERFETTO_TP_C_EXPORT void PerfettoTpIteratorDestroy(
    struct PerfettoTpIterator* it);

#ifdef __cplusplus
}
#endif

#endif  // INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PROCESSOR_C_H_
