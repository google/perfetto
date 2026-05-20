/*
 * Copyright (C) 2025 The Android Open Source Project
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

import com.google.errorprone.annotations.CompileTimeConstant;

import java.nio.ByteBuffer;

import dev.perfetto.sdk.PerfettoTrace.Category;

/**
 * Builder for Perfetto track events.
 *
 * <p>An event is assembled allocation-free into two reused per-thread buffers:
 * the variable {@code TrackEvent} body (debug annotations, flows, counter value,
 * proto fields) is encoded with {@link ProtoWriter}; the framing the native side
 * needs (event name, track chain, interned-string fields) is recorded in plain
 * fields. {@link #emit} hands both to {@link PerfettoEvent}, which copies them
 * into one off-heap {@link EmitBuffer} and makes a single primitive-only native
 * call. Category / event-name interning, incremental-state resets and
 * per-instance fan-out happen natively (the Low Level track-event ABI owns
 * them).
 *
 * <p>The builder is obtained from {@link #newEvent}, which returns a reused
 * thread-local instance (or a shared no-op instance when the category is
 * disabled, so a disabled event does no work and allocates nothing).
 */
public final class PerfettoTrackEventBuilder {
  private int mTraceType = -1;
  private Category mCategory = null;
  private String mEventName = null;
  private boolean mIsBuilt = false;
  private boolean mIsDebug = false;

  private final boolean mIsCategoryEnabled;

  // Deepest track chain / most interned fields / proto nesting per event; keep
  // MAX_TRACK_LEVELS / MAX_INTERNED_FIELDS in sync with the C++ JNI.
  private static final int MAX_TRACK_LEVELS = 16;
  private static final int MAX_INTERNED_FIELDS = 16;
  private static final int MAX_NESTING_DEPTH = 16;

  // The variable TrackEvent body (debug annotations, flows, counter value,
  // non-interned proto fields). Reset at the start of each event.
  private ProtoWriter mBody;

  // Off-heap buffer the assembled event (body + frame) is copied into so emit
  // passes a single native pointer. See EmitBuffer and PerfettoEvent.
  private EmitBuffer mXfer;

  // The track the event attaches to, flattened root->leaf. The leaf uuid is set
  // on the event; each level's descriptor is emitted once per sequence natively.
  private long[] mTrackUuids;
  private long[] mTrackParentUuids;
  private String[] mTrackNames;
  private int mTrackCount;
  private boolean mHasTrack;
  private boolean mTrackNameStatic;
  private boolean mTrackIsCounter;
  private long mTrackLeafUuid;

  // Interned-string proto fields (addFieldWithInterning). Their iids are
  // per-sequence, so they are recorded here and interned natively at emit.
  private int[] mInternedFieldIds;
  private int[] mInternedTypeIds;
  private String[] mInternedStrings;
  private int mInternedFieldCount;

  // Proto nesting bookmarks for endNested (proto fields are written straight
  // into the body via ProtoWriter).
  private boolean mInProto;
  private int[] mProtoTokens;
  private int mProtoDepth;

  private static final PerfettoTrackEventBuilder NO_OP_BUILDER =
      new PerfettoTrackEventBuilder(/* isCategoryEnabled= */ false);

  public static final ThreadLocal<PerfettoTrackEventBuilder> sThreadLocalBuilder =
      ThreadLocal.withInitial(
          () -> new PerfettoTrackEventBuilder(/* isCategoryEnabled= */ true));

  public static PerfettoTrackEventBuilder newEvent(
      int traceType, Category category, boolean isDebug) {
    if (category.isRegistered() && category.isEnabled()) {
      return sThreadLocalBuilder.get().initNewEvent(traceType, category, isDebug);
    }
    return NO_OP_BUILDER;
  }

  private PerfettoTrackEventBuilder(boolean isCategoryEnabled) {
    mIsCategoryEnabled = isCategoryEnabled;
    if (!mIsCategoryEnabled) {
      // No fields of this builder will be used, no need to initialize them.
      return;
    }
    mTrackUuids = new long[MAX_TRACK_LEVELS];
    mTrackParentUuids = new long[MAX_TRACK_LEVELS];
    mTrackNames = new String[MAX_TRACK_LEVELS];
    mInternedFieldIds = new int[MAX_INTERNED_FIELDS];
    mInternedTypeIds = new int[MAX_INTERNED_FIELDS];
    mInternedStrings = new String[MAX_INTERNED_FIELDS];
    mProtoTokens = new int[MAX_NESTING_DEPTH];
    mBody = new ProtoWriter();
    mXfer = new EmitBuffer();
  }

  /** Emits the track event. */
  public void emit() {
    if (!mIsCategoryEnabled) {
      return;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    mIsBuilt = true;

    int bodyLen = mBody.position();
    int needed = bodyLen
        + PerfettoEvent.frameSize(
            mEventName, mTrackCount, mTrackNames, mInternedFieldCount,
            mInternedStrings);
    mXfer.ensureCapacity(needed);
    ByteBuffer b = mXfer.buf;
    b.clear();
    b.put(mBody.buffer(), 0, bodyLen);
    int frameLen =
        PerfettoEvent.encodeFrame(
            b, mEventName, mHasTrack, mTrackLeafUuid, mTrackCount, mTrackUuids,
            mTrackParentUuids, mTrackNames, mTrackNameStatic, mTrackIsCounter,
            mInternedFieldCount, mInternedFieldIds, mInternedTypeIds,
            mInternedStrings);
    PerfettoEvent.native_emit(
        mTraceType, mCategory.getPtr(), mXfer.addr, bodyLen, frameLen);
  }

  /** Initialize the builder for a new trace event. */
  private PerfettoTrackEventBuilder initNewEvent(
      int traceType, Category category, boolean isDebug) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    mIsBuilt = false;
    mIsDebug = isDebug;
    mTraceType = traceType;
    mCategory = category;
    mEventName = "";

    mBody.reset();
    mHasTrack = false;
    mTrackCount = 0;
    mTrackIsCounter = false;
    mInProto = false;
    mProtoDepth = 0;
    mInternedFieldCount = 0;

    return this;
  }

  /** Sets the event name for the track event. */
  public PerfettoTrackEventBuilder setEventName(String eventName) {
    mEventName = eventName;
    return this;
  }

  /** Adds a debug arg with key {@code name} and value {@code val}. */
  public PerfettoTrackEventBuilder addArg(String name, long val) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    PerfettoEvent.addArg(mBody, name, val);
    return this;
  }

  /** Adds a debug arg with key {@code name} and value {@code val}. */
  public PerfettoTrackEventBuilder addArg(String name, boolean val) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    PerfettoEvent.addArg(mBody, name, val);
    return this;
  }

  /** Adds a debug arg with key {@code name} and value {@code val}. */
  public PerfettoTrackEventBuilder addArg(String name, double val) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    PerfettoEvent.addArg(mBody, name, val);
    return this;
  }

  /** Adds a debug arg with key {@code name} and value {@code val}. */
  public PerfettoTrackEventBuilder addArg(String name, String val) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    PerfettoEvent.addArg(mBody, name, val);
    return this;
  }

  /** Deprecated: use {@link #addFlow} */
  public PerfettoTrackEventBuilder setFlow(long id) {
    return addFlow(id);
  }

  /** Adds a flow with {@code id}. */
  public PerfettoTrackEventBuilder addFlow(long id) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    PerfettoEvent.addFlow(mBody, id);
    return this;
  }

  /** Deprecated: use {@link #addTerminatingFlow} */
  public PerfettoTrackEventBuilder setTerminatingFlow(long id) {
    return addTerminatingFlow(id);
  }

  /** Adds a terminating flow with {@code id}. */
  public PerfettoTrackEventBuilder addTerminatingFlow(long id) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    PerfettoEvent.addTerminatingFlow(mBody, id);
    return this;
  }

  /** Adds the events to a named track instead of the thread track where the event occurred. */
  public PerfettoTrackEventBuilder usingNamedTrack(
          long id, @CompileTimeConstant String name, long parentUuid) {
      return usingNamedTrack(id, name, parentUuid, /* isNameStatic = */ true);
  }

  /**
   * Adds the events to a named track with a dynamic name (populated in field 10 of
   * TrackDescriptor).
   */
  public PerfettoTrackEventBuilder usingNamedTrackWithDynamicName(
      long id, String name, long parentUuid) {
    return usingNamedTrack(id, name, parentUuid, /* isNameStatic = */ false);
  }

  private PerfettoTrackEventBuilder usingNamedTrack(
          long id, String name, long parentUuid, boolean isNameStatic) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    // uuid derived exactly as native (see PerfettoTrack), so a track is identical
    // whether emitted from here or from C++.
    long uuid = PerfettoTrack.namedUuid(parentUuid, id, name);
    mHasTrack = true;
    mTrackIsCounter = false;
    mTrackLeafUuid = uuid;
    mTrackNameStatic = isNameStatic;
    mTrackCount = 1;
    mTrackUuids[0] = uuid;
    mTrackParentUuids[0] = parentUuid;
    mTrackNames[0] = name;
    return this;
  }

  /**
   * Adds the events to {@code track}, emitting a TrackDescriptor for every level
   * of its hierarchy not yet seen on a sequence. The event attaches to the leaf.
   */
  public PerfettoTrackEventBuilder usingTrack(PerfettoTrack track) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    mHasTrack = true;
    mTrackLeafUuid = track.mUuid;
    mTrackIsCounter = track.mIsCounter;
    mTrackNameStatic = true; // PerfettoTrack names are compile-time constants
    int n = Math.min(track.mDepth, MAX_TRACK_LEVELS);
    mTrackCount = n;
    // Flatten root->leaf: walk leaf->root filling from the last slot back.
    PerfettoTrack level = track;
    for (int i = n - 1; i >= 0; i--) {
      mTrackUuids[i] = level.mUuid;
      mTrackParentUuids[i] = level.mParentUuid;
      mTrackNames[i] = level.mName;
      level = level.mParent;
    }
    return this;
  }

  /**
   * Adds the events to a process scoped named track instead of the thread track where the event
   * occurred.
   */
  public PerfettoTrackEventBuilder usingProcessNamedTrack(
          long id, @CompileTimeConstant String name) {
      if (!mIsCategoryEnabled) {
          return this;
      }
      return usingNamedTrack(id, name, PerfettoTrace.getProcessTrackUuid());
  }

  /**
   * Adds the events to a process scoped named track with a dynamic name instead of the thread track
   * where the event occurred.
   */
  public PerfettoTrackEventBuilder usingProcessNamedTrackWithDynamicName(
      long id,  String name) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    return usingNamedTrackWithDynamicName(id, name, PerfettoTrace.getProcessTrackUuid());
  }

  /**
   * Adds the events to a thread scoped named track instead of the thread track where the event
   * occurred.
   */
  public PerfettoTrackEventBuilder usingThreadNamedTrack(
          long id, @CompileTimeConstant String name, long tid) {
      if (!mIsCategoryEnabled) {
          return this;
      }
      return usingNamedTrack(id, name, PerfettoTrace.getThreadTrackUuid(tid));
  }

  /**
   * Adds the events to a thread scoped named track with a dynamic name instead of the thread track
   * where the event occurred.
   */
  public PerfettoTrackEventBuilder usingThreadNamedTrackWithDynamicName(
      long id, String name, long tid) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    return usingNamedTrackWithDynamicName(id, name, PerfettoTrace.getThreadTrackUuid(tid));
  }

  /** Adds the events to a counter track instead. This is required for setting counter values. */
  public PerfettoTrackEventBuilder usingCounterTrack(
          long parentUuid, @CompileTimeConstant String name) {
      return usingCounterTrack(parentUuid, name, /* isNameStatic = */ true);
  }

  /**
   * Adds the events to a counter track with a static name instead. This is required for setting
   * counter values.
   */
  public PerfettoTrackEventBuilder usingCounterTrackWithDynamicName(
      long parentUuid,  String name) {
    return usingCounterTrack(parentUuid, name, /* isNameStatic = */ false);
  }

  private PerfettoTrackEventBuilder usingCounterTrack(
      long parentUuid, String name, boolean isNameStatic) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    // Counter tracks are keyed by name + parent only (see PerfettoTrack).
    long uuid = PerfettoTrack.counterUuid(parentUuid, name);
    mHasTrack = true;
    mTrackIsCounter = true;
    mTrackLeafUuid = uuid;
    mTrackNameStatic = isNameStatic;
    mTrackCount = 1;
    mTrackUuids[0] = uuid;
    mTrackParentUuids[0] = parentUuid;
    mTrackNames[0] = name;
    return this;
  }

  /**
   * Adds the events to a process scoped counter track instead. This is required for setting counter
   * values.
   */
  public PerfettoTrackEventBuilder usingProcessCounterTrack(@CompileTimeConstant String name) {
      if (!mIsCategoryEnabled) {
          return this;
      }
      return usingCounterTrack(PerfettoTrace.getProcessTrackUuid(), name);
  }

  /**
   * Adds the events to a process scoped counter track with a static name instead. This is required
   * for setting counter values.
   */
  public PerfettoTrackEventBuilder usingProcessCounterTrackWithDynamicName(
       String name) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    return usingCounterTrackWithDynamicName(PerfettoTrace.getProcessTrackUuid(), name);
  }

  /**
   * Adds the events to a thread scoped counter track instead. This is required for setting counter
   * values.
   */
  public PerfettoTrackEventBuilder usingThreadCounterTrack(
          long tid, @CompileTimeConstant String name) {
      if (!mIsCategoryEnabled) {
          return this;
      }
      return usingCounterTrack(PerfettoTrace.getThreadTrackUuid(tid), name);
  }

  /**
   * Adds the events to a thread scoped counter track with a static name instead. This is required for
   * setting counter values.
   */
  public PerfettoTrackEventBuilder usingThreadCounterTrackWithDynamicName(
      long tid,  String name) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    return usingCounterTrackWithDynamicName(PerfettoTrace.getThreadTrackUuid(tid), name);
  }

  /** Sets a long counter value on the event. */
  public PerfettoTrackEventBuilder setCounter(long val) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    PerfettoEvent.setCounter(mBody, val);
    return this;
  }

  /** Sets a double counter value on the event. */
  public PerfettoTrackEventBuilder setCounter(double val) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    PerfettoEvent.setCounter(mBody, val);
    return this;
  }

  /** Adds a proto field with field id {@code id} and value {@code val}. */
  public PerfettoTrackEventBuilder addField(long id, long val) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkBuildingProto();
    }
    PerfettoEvent.protoVarInt(mBody, (int) id, val);
    return this;
  }

  /** Adds a proto field with field id {@code id} and value {@code val}. */
  public PerfettoTrackEventBuilder addField(long id, double val) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkBuildingProto();
    }
    PerfettoEvent.protoDouble(mBody, (int) id, val);
    return this;
  }

  /** Adds a proto field with field id {@code id} and value {@code val}. */
  public PerfettoTrackEventBuilder addField(long id, String val) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkBuildingProto();
    }
    PerfettoEvent.protoString(mBody, (int) id, val);
    return this;
  }

  /**
   * Adds a proto field with field id {@code id} whose string {@code val} is
   * interned under {@code internedTypeId} (an InternedData field number). The
   * field is recorded and interned natively (its iid is per-sequence).
   * {@code internedTypeId} must be non-zero.
   */
  public PerfettoTrackEventBuilder addFieldWithInterning(long id, String val, long internedTypeId) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkBuildingProto();
    }
    if (internedTypeId != 0 && mInternedFieldCount < MAX_INTERNED_FIELDS) {
      mInternedFieldIds[mInternedFieldCount] = (int) id;
      mInternedTypeIds[mInternedFieldCount] = (int) internedTypeId;
      mInternedStrings[mInternedFieldCount] = val;
      mInternedFieldCount++;
    }
    return this;
  }

  /**
   * Begins a proto field. Fields can be added from this point and there must be a corresponding
   * {@link #endProto}.
   *
   * <p>The proto field is a singleton and all proto fields get added inside the one {@code
   * beginProto} and {@code endProto} within the {@link PerfettoTrackEventBuilder}.
   */
  public PerfettoTrackEventBuilder beginProto() {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    // Proto fields are written straight into the body; no child builder.
    mInProto = true;
    return this;
  }

  /** Ends a proto field. */
  public PerfettoTrackEventBuilder endProto() {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkMatchingBeginProto();
    }
    mInProto = false;
    return this;
  }

  /**
   * Begins a nested proto field with field id {@code id}. Fields can be added from this point and
   * there must be a corresponding {@link #endNested}.
   */
  public PerfettoTrackEventBuilder beginNested(long id) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkBuildingProto();
    }
    mProtoTokens[mProtoDepth++] = PerfettoEvent.protoBeginNested(mBody, (int) id);
    return this;
  }

  /** Ends a nested proto field. */
  public PerfettoTrackEventBuilder endNested() {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkMatchingBeginNested();
    }
    PerfettoEvent.protoEndNested(mBody, mProtoTokens[--mProtoDepth]);
    return this;
  }

  private void checkState() {
    if (mIsBuilt) throwStateError();
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwStateError() {
    throw new IllegalStateException(
        "This builder has already been used. Create a new builder for another event.");
  }

  private void checkNotBuildingProto() {
    checkState();
    if (mInProto) throwNotBuildingProtoError();
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwNotBuildingProtoError() {
    throw new IllegalStateException("Operation not supported for proto.");
  }

  private void checkBuildingProto() {
    checkState();
    if (!mInProto) throwBuildingProtoError();
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwBuildingProtoError() {
    throw new IllegalStateException("Field operations must be within beginProto/endProto block.");
  }

  private void checkMatchingBeginNested() {
    checkState();
    if (mProtoDepth == 0) throwMatchingBeginNestedError();
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwMatchingBeginNestedError() {
    throw new IllegalStateException("No matching beginNested call.");
  }

  private void checkMatchingBeginProto() {
    checkState();
    if (!mInProto || mProtoDepth > 0) throwMatchingBeginProtoError();
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwMatchingBeginProtoError() {
    throw new IllegalStateException("No matching beginProto call.");
  }
}
