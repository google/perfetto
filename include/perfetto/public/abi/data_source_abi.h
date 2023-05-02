/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_PUBLIC_ABI_DATA_SOURCE_ABI_H_
#define INCLUDE_PERFETTO_PUBLIC_ABI_DATA_SOURCE_ABI_H_

#include <stdbool.h>
#include <stdint.h>

#include "perfetto/public/abi/atomic.h"
#include "perfetto/public/abi/export.h"
#include "perfetto/public/abi/stream_writer_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Internal representation of a data source type.
struct PerfettoDsImpl;

// Internal thread local state of a data source type.
struct PerfettoDsTlsImpl;

// Internal thread local state of a data source instance used for tracing.
struct PerfettoDsTracerImpl;

// A global atomic boolean that's always false.
extern PERFETTO_SDK_EXPORT PERFETTO_ATOMIC(bool) perfetto_atomic_false;

// There can be more than one data source instance for each data source type.
// This index identifies one of them.
typedef uint32_t PerfettoDsInstanceIndex;

// Creates a data source type.
//
// The data source type needs to be registered later with
// PerfettoDsImplRegister().
PERFETTO_SDK_EXPORT struct PerfettoDsImpl* PerfettoDsImplCreate(void);

// Called when a data source instance of a specific type is created. `ds_config`
// points to a serialized perfetto.protos.DataSourceConfig message,
// `ds_config_size` bytes long. `user_arg` is the value passed to
// PerfettoDsSetCbUserArg().
typedef void* (*PerfettoDsOnSetupCb)(PerfettoDsInstanceIndex inst_id,
                                     void* ds_config,
                                     size_t ds_config_size,
                                     void* user_arg);

// Called when tracing starts for a data source instance. `user_arg` is the
// value passed to PerfettoDsSetCbUserArg(). `inst_ctx` is the return
// value of PerfettoDsOnSetupCb.
typedef void (*PerfettoDsOnStartCb)(PerfettoDsInstanceIndex inst_id,
                                    void* user_arg,
                                    void* inst_ctx);

// Internal handle used to perform operations from the OnStop callback.
struct PerfettoDsOnStopArgs;

// Internal handle used to signal when the data source stop operation is
// complete.
struct PerfettoDsAsyncStopper;

// Tells the tracing service to postpone the stopping of a data source instance.
// The returned handle can be used to signal the tracing service when the data
// source instance can be stopped.
PERFETTO_SDK_EXPORT struct PerfettoDsAsyncStopper* PerfettoDsOnStopArgsPostpone(
    struct PerfettoDsOnStopArgs*);

// Tells the tracing service to stop a data source instance (whose stop
// operation was previously postponed with PerfettoDsOnStopArgsPostpone).
PERFETTO_SDK_EXPORT void PerfettoDsStopDone(struct PerfettoDsAsyncStopper*);

// Called when tracing stops for a data source instance. `user_arg` is the value
// passed to PerfettoDsSetCbUserArg(). `inst_ctx` is the return value of
// PerfettoDsOnSetupCb. `args` can be used to postpone stopping this data source
// instance.
typedef void (*PerfettoDsOnStopCb)(PerfettoDsInstanceIndex inst_id,
                                   void* user_arg,
                                   void* inst_ctx,
                                   struct PerfettoDsOnStopArgs* args);

// Creates custom state (either thread local state or incremental state) for
// instance `inst_id`. `user_arg` is the value passed to
// PerfettoDsSetCbUserArg().
typedef void* (*PerfettoDsOnCreateCustomState)(
    PerfettoDsInstanceIndex inst_id,
    struct PerfettoDsTracerImpl* tracer,
    void* user_arg);

// Deletes the previously created custom state `obj`.
typedef void (*PerfettoDsOnDeleteCustomState)(void* obj);

// Setters for callbacks: can not be called after PerfettoDsImplRegister().

PERFETTO_SDK_EXPORT void PerfettoDsSetOnSetupCallback(struct PerfettoDsImpl*,
                                                      PerfettoDsOnSetupCb);

PERFETTO_SDK_EXPORT void PerfettoDsSetOnStartCallback(struct PerfettoDsImpl*,
                                                      PerfettoDsOnStartCb);

PERFETTO_SDK_EXPORT void PerfettoDsSetOnStopCallback(struct PerfettoDsImpl*,
                                                     PerfettoDsOnStopCb);

// Callbacks for custom per instance thread local state.
PERFETTO_SDK_EXPORT void PerfettoDsSetOnCreateTls(
    struct PerfettoDsImpl*,
    PerfettoDsOnCreateCustomState);

PERFETTO_SDK_EXPORT void PerfettoDsSetOnDeleteTls(
    struct PerfettoDsImpl*,
    PerfettoDsOnDeleteCustomState);

// Callbacks for custom per instance thread local incremental state.
PERFETTO_SDK_EXPORT void PerfettoDsSetOnCreateIncr(
    struct PerfettoDsImpl*,
    PerfettoDsOnCreateCustomState);

PERFETTO_SDK_EXPORT void PerfettoDsSetOnDeleteIncr(
    struct PerfettoDsImpl*,
    PerfettoDsOnDeleteCustomState);

// Stores the `user_arg` that's going to be passed later to the callbacks for
// this data source type.
PERFETTO_SDK_EXPORT void PerfettoDsSetCbUserArg(struct PerfettoDsImpl*,
                                                void* user_arg);

// Registers the `*ds_impl` data source type.
//
// `ds_impl` must be obtained via a call to `PerfettoDsImplCreate()`.
//
// `**enabled_ptr` will be set to true when the data source type has been
// enabled.
//
// `descriptor` should point to a serialized
// perfetto.protos.DataSourceDescriptor message, `descriptor_size` bytes long.
//
// Returns `true` in case of success, `false` in case of failure (in which case
// `ds_impl is invalid`).
PERFETTO_SDK_EXPORT bool PerfettoDsImplRegister(struct PerfettoDsImpl* ds_impl,
                                                PERFETTO_ATOMIC(bool) *
                                                    *enabled_ptr,
                                                const void* descriptor,
                                                size_t descriptor_size);

// Updates the descriptor the `*ds_impl` data source type.
//
// `descriptor` should point to a serialized
// perfetto.protos.DataSourceDescriptor message, `descriptor_size` bytes long.
PERFETTO_SDK_EXPORT void PerfettoDsImplUpdateDescriptor(
    struct PerfettoDsImpl* ds_impl,
    const void* descriptor,
    size_t descriptor_size);

// Tries to get the `inst_ctx` returned by PerfettoDsOnSetupCb() for the
// instance with index `inst_id`.
//
// If successful, returns a non-null pointer and acquires a lock, which must be
// released with PerfettoDsImplReleaseInstanceLocked.
//
// If unsuccessful (because the instance was destroyed in the meantime) or if
// PerfettoDsOnSetupCb() returned a null value, returns null and does not
// acquire any lock.
PERFETTO_SDK_EXPORT void* PerfettoDsImplGetInstanceLocked(
    struct PerfettoDsImpl* ds_impl,
    PerfettoDsInstanceIndex inst_id);

// Releases a lock previouly acquired by a PerfettoDsImplGetInstanceLocked()
// call, which must have returned a non null value.
PERFETTO_SDK_EXPORT void PerfettoDsImplReleaseInstanceLocked(
    struct PerfettoDsImpl* ds_impl,
    PerfettoDsInstanceIndex inst_id);

// Gets the data source thread local instance custom state created by
// the callback passed to `PerfettoDsSetOnCreateTls`.
PERFETTO_SDK_EXPORT void* PerfettoDsImplGetCustomTls(
    struct PerfettoDsImpl* ds_impl,
    struct PerfettoDsTracerImpl* tracer,
    PerfettoDsInstanceIndex inst_id);

// Gets the data source thread local instance incremental state created by
// the callback passed to `PerfettoDsSetOnCreateIncr`.
PERFETTO_SDK_EXPORT void* PerfettoDsImplGetIncrementalState(
    struct PerfettoDsImpl* ds_impl,
    struct PerfettoDsTracerImpl* tracer,
    PerfettoDsInstanceIndex inst_id);

// Iterator for all the active instances (on this thread) of a data source type.
struct PerfettoDsImplTracerIterator {
  // Instance id.
  PerfettoDsInstanceIndex inst_id;
  // Caches a pointer to the internal thread local state of the data source
  // type.
  struct PerfettoDsTlsImpl* tls;
  // Pointer to the object used to output trace packets. When nullptr, the
  // iteration is over.
  struct PerfettoDsTracerImpl* tracer;
};

// Start iterating over all the active instances of the data source type
// (`ds_impl`).
//
// If the returned tracer is not nullptr, the user must continue the iteration
// with PerfettoDsImplTraceIterateNext(), until it is. The iteration can
// only be interrupted early by calling PerfettoDsImplTraceIterateBreak().
PERFETTO_SDK_EXPORT struct PerfettoDsImplTracerIterator
PerfettoDsImplTraceIterateBegin(struct PerfettoDsImpl* ds_impl);

// Advances the iterator to the next active instance of the data source type
// (`ds_impl`).
//
// The user must call PerfettoDsImplTraceIterateNext(), until it returns a
// nullptr tracer. The iteration can only be interrupted early by calling
// PerfettoDsImplTraceIterateBreak().
PERFETTO_SDK_EXPORT void PerfettoDsImplTraceIterateNext(
    struct PerfettoDsImpl* ds_impl,
    struct PerfettoDsImplTracerIterator* iterator);

// Prematurely interrupts iteration over all the active instances of the data
// source type (`ds_impl`).
PERFETTO_SDK_EXPORT void PerfettoDsImplTraceIterateBreak(
    struct PerfettoDsImpl* ds_impl,
    struct PerfettoDsImplTracerIterator* iterator);

// Creates a new trace packet on `tracer`. Returns a stream writer that can be
// used to write data to the packet. The caller must use
// PerfettoDsTracerImplPacketEnd() when done.
PERFETTO_SDK_EXPORT struct PerfettoStreamWriter PerfettoDsTracerImplPacketBegin(
    struct PerfettoDsTracerImpl* tracer);

// Signals that the trace packets created previously on `tracer` with
// PerfettoDsTracerImplBeginPacket(), has been fully written.
//
// `writer` should point to the writer returned by
// PerfettoDsTracerImplBeginPacket() and cannot be used anymore after this call.
PERFETTO_SDK_EXPORT void PerfettoDsTracerImplPacketEnd(
    struct PerfettoDsTracerImpl* tracer,
    struct PerfettoStreamWriter* writer);

// Called when a flush request is complete.
typedef void (*PerfettoDsTracerOnFlushCb)(void* user_arg);

// Forces a commit of the thread-local tracing data written so far to the
// service.
//
// If `cb` is not NULL, it is called on a dedicated internal thread (with
// `user_arg`), when flushing is complete. It may never be called (e.g. if the
// tracing service disconnects).
//
// This is almost never required (tracing data is periodically committed as
// trace pages are filled up) and has a non-negligible performance hit.
PERFETTO_SDK_EXPORT void PerfettoDsTracerImplFlush(
    struct PerfettoDsTracerImpl* tracer,
    PerfettoDsTracerOnFlushCb cb,
    void* user_arg);

#ifdef __cplusplus
}
#endif

#endif  // INCLUDE_PERFETTO_PUBLIC_ABI_DATA_SOURCE_ABI_H_
