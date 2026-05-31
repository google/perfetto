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

import java.util.ArrayList;

import dev.perfetto.sdk.PerfettoNativeMemoryCleaner.AllocationStats;
import dev.perfetto.sdk.PerfettoTrace.Category;
import dev.perfetto.sdk.PerfettoTrackEventExtra.Counter;
import dev.perfetto.sdk.PerfettoTrackEventExtra.CounterTrack;
import dev.perfetto.sdk.PerfettoTrackEventExtra.NamedTrack;
import dev.perfetto.sdk.PerfettoTrackEventExtra.NestedTracks;
import dev.perfetto.sdk.PerfettoTrackEventExtra.PerfettoPointer;

/** Builder for Perfetto track event extras. */
public final class PerfettoTrackEventBuilder {
  private static final int DEFAULT_EXTRA_CACHE_SIZE = 16;
  private static final int DEFAULT_PENDING_POINTERS_LIST_SIZE = 16;

  private PerfettoTrackEventExtra mExtra;

  private int mTraceType = -1;
  private Category mCategory = null;
  private String mEventName = null;
  private boolean mIsBuilt = false;
  private boolean mIsDebug = false;

  private PerfettoNativeMemoryCleaner mNativeMemoryCleaner;
  private static final PerfettoNativeMemoryCleaner.AllocationStats sNativeAllocationStats =
      new AllocationStats();

  // Content cache for the flat track sugar (usingProcessNamedTrack /
  // usingCounterTrack etc.), which builds a fresh handle per emit. Keyed by
  // PerfettoTrack.flatCacheKey (folds in the counter flag, so a counter and a
  // same-named track never collide). The usingTrack(PerfettoTrack) path needs no
  // cache -- the handle owns its native track.
  private RingBuffer<NestedTracks> mTrackCache;
  private ObjectsCache mObjectsCache;

  private final boolean mIsCategoryEnabled;

  private ArrayList<PerfettoPointer> mPendingPointers;

  // Tracks whether the builder is between a beginProto() and its endProto(), so
  // the debug-only state checks can validate that field operations are scoped to
  // a proto block. Plain proto fields are written straight into the body; the
  // depth count gates endNested() back-fills.
  private boolean mInProto;
  private int mProtoDepth;

  // Whether this event carries any interned proto field. Interned strings can't
  // be Java-encoded -- their iids are assigned per-sequence by the native
  // interning tables -- so addFieldWithInterning() hands them to the RawBody
  // extra, which carries them as CSTR_INTERNED fields alongside the body. This
  // flag makes emit() register RawBody even when the body itself is empty.
  private boolean mHasInterned;

  // The TrackEvent body: debug annotations, flows, counter value and plain proto
  // fields are encoded here, then handed to native as one raw proto field. Owned
  // by the root builder (already thread-local), so the hot path does no per-call
  // ThreadLocal lookup. Reset at the start of each event.
  private ProtoWriter mBody;

  // Reused extra that copies the encoded body into its own native buffer and
  // carries it to native as one raw proto field.
  private PerfettoTrackEventExtra.RawBody mRawBody;

  // Single reused native counter extra, created on first use. A counter value
  // has no identity to cache, so one object per (thread-local) builder suffices
  // -- no pool or ring buffer. It stays native (not body-encoded) because a
  // CriticalNative value-set is cheaper than routing one tiny field through the
  // body. Flows differ: an event may carry several, so re-adding them would need
  // a Flow pool, and they stay body-encoded instead.
  private Counter mCounter;


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
    mNativeMemoryCleaner = new PerfettoNativeMemoryCleaner(null);
    mTrackCache = new RingBuffer<>(DEFAULT_EXTRA_CACHE_SIZE);
    mObjectsCache = new ObjectsCache(DEFAULT_EXTRA_CACHE_SIZE);
    mPendingPointers = new ArrayList<>(DEFAULT_PENDING_POINTERS_LIST_SIZE);
    mBody = new ProtoWriter();
    mRawBody = new PerfettoTrackEventExtra.RawBody(mNativeMemoryCleaner);
    mExtra = new PerfettoTrackEventExtra(mNativeMemoryCleaner);
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
    if (bodyLen > 0 || mHasInterned) {
      // mRawBody is a permanent field of this (thread-local) builder, so it can
      // never be GC'd mid-emit and does not need mPendingPointers tracking
      // (which exists only to keep transiently-referenced extras alive across
      // the native call). Register it on the extra directly. It also carries any
      // interned fields, so it must ride even when the body is empty.
      mExtra.addPerfettoPointer(mRawBody);
    }
    // One JNI crossing: native_emit copies the heap-encoded body into the
    // RawBody's native buffer (when bodyLen > 0) and emits in the same call.
    PerfettoTrackEventExtra.native_emit(
        mTraceType, mCategory.getPtr(), mEventName, mExtra.getPtr(),
        mRawBody.bodyPtr(), mBody.buffer(), bodyLen);
  }

  /** Initialize the builder for a new trace event. */
  private PerfettoTrackEventBuilder initNewEvent(
      int traceType, Category category, boolean isDebug) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    mIsBuilt = false;
    mIsDebug = isDebug;
    updateNativeMemoryCleanerForDebug(mIsDebug);
    mTraceType = traceType;
    mCategory = category;
    mEventName = "";

    mExtra.reset();
    mPendingPointers.clear();
    mInProto = false;
    mProtoDepth = 0;
    mHasInterned = false;
    mBody.reset();

    return this;
  }

  private void updateNativeMemoryCleanerForDebug(boolean enableDebug) {
    // In current implementation it is possible, that the 'PerfettoTrackEventBuilder' will be
    // used
    // with the 'isDebug' value that differs from the value the builder was created and/or
    // previously used.
    // To correctly handle this situation and to not allocate memory in the fast-path (when the
    // cached builder is used with the same 'isDebug' value), we check the state of the
    // previously
    // created cleaner and create the new one only if 'isDebug' value is updated.
    boolean debugIsAlreadyEnabled = mNativeMemoryCleaner.isReportAllocationStats();
    if (debugIsAlreadyEnabled == enableDebug) {
      return;
    }
    if (enableDebug) {
      mNativeMemoryCleaner = new PerfettoNativeMemoryCleaner(sNativeAllocationStats);
    } else {
      mNativeMemoryCleaner = new PerfettoNativeMemoryCleaner(null);
    }
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
    PerfettoTrackEventEncoder.addArg(mBody, name, val);
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
    PerfettoTrackEventEncoder.addArg(mBody, name, val);
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
    PerfettoTrackEventEncoder.addArg(mBody, name, val);
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
    PerfettoTrackEventEncoder.addArg(mBody, name, val);
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
    PerfettoTrackEventEncoder.addFlow(mBody, id);
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
    PerfettoTrackEventEncoder.addTerminatingFlow(mBody, id);
    return this;
  }

  /**
   * Emits this event on {@code track}, a (possibly nested) named track. The
   * descriptor for each level of the chain is emitted once per sequence. The
   * native side derives the per-level uuids; nothing is hardcoded here. The
   * {@link NestedTracks} wrapper is cached per {@link PerfettoTrack}, so this is
   * allocation-free after the first use of a given track.
   *
   * <p>This is the single named/nested track entry point. The flat {@code
   * usingProcessNamedTrack} / {@code usingThreadNamedTrack} helpers below are
   * thin sugar over it.
   */
  public PerfettoTrackEventBuilder usingTrack(PerfettoTrack track) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }

    // The handle owns its native track (built once, reused for its lifetime), so
    // there is no builder-side cache to consult -- just hand it over.
    addPerfettoPointerToExtra(track.nestedTracks());
    return this;
  }

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

  /**
   * Adds the events to a named track with a static name (populated in field 10 of
   * TrackDescriptor).
   */
  private PerfettoTrackEventBuilder usingNamedTrack(
          long id, String name, long parentUuid, boolean isNameStatic) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }

    NamedTrack track = mObjectsCache.mNamedTrackCache.get(name.hashCode());
    if (track == null || !track.getName().equals(name) || track.isNameStatic() != isNameStatic) {
      track = new NamedTrack(id, name, parentUuid, isNameStatic, mNativeMemoryCleaner);
      mObjectsCache.mNamedTrackCache.put(name.hashCode(), track);
    }
    addPerfettoPointerToExtra(track);
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

    CounterTrack track = mObjectsCache.mCounterTrackCache.get(name.hashCode());
    if (track == null || !track.getName().equals(name) || track.isNameStatic() != isNameStatic) {
      track = new CounterTrack(name, parentUuid, isNameStatic, mNativeMemoryCleaner);
      mObjectsCache.mCounterTrackCache.put(name.hashCode(), track);
    }
    addPerfettoPointerToExtra(track);
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
    if (mCounter == null) {
      mCounter = new Counter(mNativeMemoryCleaner);
    }
    mCounter.setValueInt64(val);
    addPerfettoPointerToExtra(mCounter);
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
    if (mCounter == null) {
      mCounter = new Counter(mNativeMemoryCleaner);
    }
    mCounter.setValueDouble(val);
    addPerfettoPointerToExtra(mCounter);
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
    PerfettoTrackEventEncoder.protoVarInt(mBody, (int) id, val);
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
    PerfettoTrackEventEncoder.protoDouble(mBody, (int) id, val);
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
    PerfettoTrackEventEncoder.protoString(mBody, (int) id, val);
    return this;
  }

  /**
   * Adds a proto field with field id {@code id} whose string {@code val} is
   * interned under {@code internedTypeId} (an InternedData field number). The
   * string is interned natively (its iid is assigned per-sequence), riding on the
   * event's raw-body extra alongside the encoded body. {@code internedTypeId}
   * must be non-zero.
   */
  public PerfettoTrackEventBuilder addFieldWithInterning(long id, String val, long internedTypeId) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkBuildingProto();
    }
    if (internedTypeId == 0) {
      return this;
    }
    mRawBody.addInterned(id, val, internedTypeId);
    mHasInterned = true;
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
    PerfettoTrackEventEncoder.protoBeginNested(mBody, (int) id);
    mProtoDepth++;
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
    mProtoDepth--;
    PerfettoTrackEventEncoder.protoEndNested(mBody);
    return this;
  }

  private void addPerfettoPointerToExtra(PerfettoPointer arg) {
    // Keep a reference to the Java object; the extra holds a native pointer into
    // the object.
    mPendingPointers.add(arg);
    mExtra.addPerfettoPointer(arg);
  }

  // Only used in tests.
  public static AllocationStats getNativeAllocationStats() {
    return sNativeAllocationStats;
  }

  /**
   * RingBuffer implemented on top of a SparseArray.
   *
   * <p>Bounds a SparseArray with a FIFO algorithm.
   */
  private static final class RingBuffer<T> {
    private final int mCapacity;
    private final int[] mKeyArray;
    private final T[] mValueArray;
    private int mWriteEnd = 0;

    RingBuffer(int capacity) {
      mCapacity = capacity;
      mKeyArray = new int[capacity];
      mValueArray = (T[]) new Object[capacity];
    }

    public void put(int key, T value) {
      mKeyArray[mWriteEnd] = key;
      mValueArray[mWriteEnd] = value;
      mWriteEnd = (mWriteEnd + 1) % mCapacity;
    }

    public T get(int key) {
      for (int i = 0; i < mCapacity; i++) {
        if (mKeyArray[i] == key) {
          return mValueArray[i];
        }
      }
      return null;
    }
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

  private static final class ObjectsCache {
    public final RingBuffer<NamedTrack> mNamedTrackCache;
    public final RingBuffer<CounterTrack> mCounterTrackCache;

    public ObjectsCache(int capacity) {
      mNamedTrackCache = new RingBuffer<>(capacity);
      mCounterTrackCache = new RingBuffer<>(capacity);
    }
  }
}
