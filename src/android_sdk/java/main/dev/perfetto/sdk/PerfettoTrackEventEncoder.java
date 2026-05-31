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

package dev.perfetto.sdk;

/**
 * Encoders for the variable part of a {@code TrackEvent} -- its "body": debug
 * annotations, flows, the counter value and proto fields.
 *
 * <p>Each method writes the corresponding protobuf field(s) into the caller's
 * reused {@link ProtoWriter}. The encoded body is later handed to native as one
 * verbatim raw proto field, spliced into the {@code track_event} submessage.
 * Encoding the body in Java keeps these fields off the per-field native crossing
 * the High Level extras would otherwise need.
 *
 * @hide
 */
public final class PerfettoTrackEventEncoder {
  // TrackEvent field numbers.
  private static final int TE_DEBUG_ANNOTATIONS = 4;
  private static final int TE_FLOW_IDS = 47;
  private static final int TE_TERMINATING_FLOW_IDS = 48;

  // DebugAnnotation field numbers.
  private static final int DA_BOOL_VALUE = 2;
  private static final int DA_INT_VALUE = 4;
  private static final int DA_DOUBLE_VALUE = 5;
  private static final int DA_STRING_VALUE = 6;
  private static final int DA_NAME = 10;

  // Process track uuid, cached on first use; flows are xor-folded with it,
  // matching PerfettoTeProcessScopedFlow in the C SDK.
  private static volatile long sProcessTrackUuid;
  private static volatile boolean sProcessTrackUuidValid;

  private PerfettoTrackEventEncoder() {}

  private static long processTrackUuid() {
    if (!sProcessTrackUuidValid) {
      sProcessTrackUuid = PerfettoTrace.getProcessTrackUuid();
      sProcessTrackUuidValid = true;
    }
    return sProcessTrackUuid;
  }

  // All encode methods take the caller's ProtoWriter `b` (owned by the thread-
  // local PerfettoTrackEventBuilder) so the hot path does no ThreadLocal lookup.

  /** Appends an int64 debug annotation to the body. */
  static void addArg(ProtoWriter b, String name, long value) {
    b.beginNested(TE_DEBUG_ANNOTATIONS);
    b.writeString(DA_NAME, name);
    b.writeVarInt(DA_INT_VALUE, value);
    b.endNested();
  }

  /** Appends a bool debug annotation to the body. */
  static void addArg(ProtoWriter b, String name, boolean value) {
    b.beginNested(TE_DEBUG_ANNOTATIONS);
    b.writeString(DA_NAME, name);
    b.writeBool(DA_BOOL_VALUE, value);
    b.endNested();
  }

  /** Appends a double debug annotation to the body. */
  static void addArg(ProtoWriter b, String name, double value) {
    b.beginNested(TE_DEBUG_ANNOTATIONS);
    b.writeString(DA_NAME, name);
    b.writeDouble(DA_DOUBLE_VALUE, value);
    b.endNested();
  }

  /** Appends a string debug annotation to the body. */
  static void addArg(ProtoWriter b, String name, String value) {
    b.beginNested(TE_DEBUG_ANNOTATIONS);
    b.writeString(DA_NAME, name);
    b.writeString(DA_STRING_VALUE, value);
    b.endNested();
  }

  /** Appends a (process-scoped) flow id to the body. */
  static void addFlow(ProtoWriter b, long id) {
    b.writeFixed64(TE_FLOW_IDS, id ^ processTrackUuid());
  }

  /** Appends a (process-scoped) terminating flow id to the body. */
  static void addTerminatingFlow(ProtoWriter b, long id) {
    b.writeFixed64(TE_TERMINATING_FLOW_IDS, id ^ processTrackUuid());
  }

  /** Appends a varint proto field to the body (for beginProto/addField). */
  static void protoVarInt(ProtoWriter b, int fieldId, long value) {
    b.writeVarInt(fieldId, value);
  }

  /** Appends a double proto field to the body. */
  static void protoDouble(ProtoWriter b, int fieldId, double value) {
    b.writeDouble(fieldId, value);
  }

  /** Appends a string proto field to the body. */
  static void protoString(ProtoWriter b, int fieldId, String value) {
    b.writeString(fieldId, value);
  }

  /** Begins a nested proto message in the body. */
  static void protoBeginNested(ProtoWriter b, int fieldId) {
    b.beginNested(fieldId);
  }

  /** Ends the innermost nested proto message started with {@link #protoBeginNested}. */
  static void protoEndNested(ProtoWriter b) {
    b.endNested();
  }
}
