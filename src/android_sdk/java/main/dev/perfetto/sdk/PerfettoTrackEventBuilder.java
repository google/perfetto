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

import java.lang.ref.Reference;
import java.util.ArrayList;
import java.util.function.Supplier;

import dev.perfetto.sdk.PerfettoNativeMemoryCleaner.AllocationStats;
import dev.perfetto.sdk.PerfettoTrace.Category;
import dev.perfetto.sdk.PerfettoTrackEventExtra.Arg;
import dev.perfetto.sdk.PerfettoTrackEventExtra.CorrelationId;
import dev.perfetto.sdk.PerfettoTrackEventExtra.Counter;
import dev.perfetto.sdk.PerfettoTrackEventExtra.CounterTrack;
import dev.perfetto.sdk.PerfettoTrackEventExtra.Field;
import dev.perfetto.sdk.PerfettoTrackEventExtra.FieldContainer;
import dev.perfetto.sdk.PerfettoTrackEventExtra.FieldNested;
import dev.perfetto.sdk.PerfettoTrackEventExtra.Flow;
import dev.perfetto.sdk.PerfettoTrackEventExtra.NamedTrack;
import dev.perfetto.sdk.PerfettoTrackEventExtra.PerfettoPointer;
import dev.perfetto.sdk.PerfettoTrackEventExtra.StateTrack;
import dev.perfetto.sdk.PerfettoTrackEventExtra.Proto;

/** Builder for Perfetto track event extras. */
public final class PerfettoTrackEventBuilder {
  private static final int DEFAULT_EXTRA_CACHE_SIZE = 16;
  private static final int DEFAULT_PENDING_POINTERS_LIST_SIZE = 16;

  private static final int ENCODING_LEGACY = 1;
  private static final int ENCODING_ENCODED = 2;

  private PerfettoTrackEventExtra mExtra;
  private PerfettoEncodedEvent mEncoded;
  private int mEncodingModes;
  private PerfettoPointer mTrack;
  private boolean mIsProto;
  private boolean mIsNested;
  private int mNestedToken;

  private int mTraceType = -1;
  private Category mCategory = null;
  private String mEventName = null;
  private boolean mIsBuilt = false;
  private boolean mIsDebug = false;

  private PerfettoTrackEventBuilder mParent;
  private FieldContainer mCurrentContainer;

  private PerfettoNativeMemoryCleaner mNativeMemoryCleaner;
  private static final PerfettoNativeMemoryCleaner.AllocationStats sNativeAllocationStats =
      new AllocationStats();

  private static final class ObjectsPool {
    public final Pool<Field> mFieldPool;
    public final Pool<FieldNested> mFieldNestedPool;
    public final Pool<Proto> mProtoPool;
    public final Pool<Flow> mFlowPool;
    public final Pool<Flow> mTerminatingFlowPool;

    public ObjectsPool(int capacity) {
      mFieldPool = new Pool<>(capacity);
      mFieldNestedPool = new Pool<>(capacity);
      mProtoPool = new Pool<>(capacity);
      mFlowPool = new Pool<>(capacity);
      mTerminatingFlowPool = new Pool<>(capacity);
    }

    public void reset() {
      mFieldPool.reset();
      mFieldNestedPool.reset();
      mProtoPool.reset();
      mFlowPool.reset();
      mTerminatingFlowPool.reset();
    }
  }

  private static final class ObjectsCache {
    public final RingBuffer<NamedTrack> mNamedTrackCache;
    public final RingBuffer<CounterTrack> mCounterTrackCache;
    public final RingBuffer<StateTrack> mStateTrackCache;
    public final RingBuffer<Arg> mArgCache;

    public ObjectsCache(int capacity) {
      mNamedTrackCache = new RingBuffer<>(capacity);
      mCounterTrackCache = new RingBuffer<>(capacity);
      mStateTrackCache = new RingBuffer<>(capacity);
      mArgCache = new RingBuffer<>(capacity);
    }
  }

  private static final class LazyInitObjects {
    private Counter mCounter = null;
    private CorrelationId mCorrelationId = null;

    private final PerfettoNativeMemoryCleaner mNativeMemoryCleaner;

    private LazyInitObjects(PerfettoNativeMemoryCleaner memoryCleaner) {
      this.mNativeMemoryCleaner = memoryCleaner;
    }

    public Counter getCounter() {
      if (mCounter == null) {
        mCounter = new Counter(mNativeMemoryCleaner);
      }
      return mCounter;
    }

    public CorrelationId getCorrelationId() {
      if (mCorrelationId == null) {
        mCorrelationId = new CorrelationId(mNativeMemoryCleaner);
      }
      return mCorrelationId;
    }
  }

  private Pool<PerfettoTrackEventBuilder> mChildBuildersCache;
  private ObjectsPool mObjectsPool;
  private ObjectsCache mObjectsCache;
  private LazyInitObjects mLazyInitObjects;

  private final boolean mIsCategoryEnabled;

  private ArrayList<PerfettoPointer> mPendingPointers;

  private final Supplier<PerfettoTrackEventBuilder> perfettoTrackEventBuilderSupplier =
      () -> new PerfettoTrackEventBuilder(true, this);
  private final Supplier<FieldNested> fieldNestedSupplier =
      () -> new FieldNested(mNativeMemoryCleaner);
  private final Supplier<Proto> protoSupplier = () -> new Proto(mNativeMemoryCleaner);
  private final Supplier<Field> fieldSupplier = () -> new Field(mNativeMemoryCleaner);
  private final Supplier<Flow> flowSupplier = () -> new Flow(mNativeMemoryCleaner);

  private static final PerfettoTrackEventBuilder NO_OP_BUILDER =
      new PerfettoTrackEventBuilder(/* isCategoryEnabled= */ false, /* parent= */ null);

  private static final int MAX_POOLED_BUILDERS = 2;

  private static final ThreadLocal<PerfettoTrackEventBuilder[]> sThreadLocalBuilder =
      ThreadLocal.withInitial(() -> new PerfettoTrackEventBuilder[MAX_POOLED_BUILDERS]);

  public static PerfettoTrackEventBuilder newEvent(
      int traceType, Category category, boolean isDebug) {
    int modes = category.getEncodingModes();
    if (modes == 0) {
      return NO_OP_BUILDER;
    }
    PerfettoTrackEventBuilder[] pool = sThreadLocalBuilder.get();
    for (int i = MAX_POOLED_BUILDERS - 1; i >= 0; i--) {
      PerfettoTrackEventBuilder builder = pool[i];
      if (builder != null) {
        pool[i] = null;
        return builder.initNewEvent(traceType, category, isDebug, modes);
      }
    }
    return new PerfettoTrackEventBuilder(/* isCategoryEnabled= */ true, /* parent= */ null)
        .initNewEvent(traceType, category, isDebug, modes);
  }

  private PerfettoTrackEventBuilder(boolean isCategoryEnabled, PerfettoTrackEventBuilder parent) {
    mIsCategoryEnabled = isCategoryEnabled;
    if (!mIsCategoryEnabled) {
      // No fields of this builder will be used, no need to initialize them.
      return;
    }
    if (parent == null) {
      // We are creating a root builder which will be saved in thread local storage.
      mParent = null;
      mNativeMemoryCleaner = new PerfettoNativeMemoryCleaner(null);
      mChildBuildersCache = new Pool<>(DEFAULT_EXTRA_CACHE_SIZE);
      mObjectsPool = new ObjectsPool(DEFAULT_EXTRA_CACHE_SIZE);
      mObjectsCache = new ObjectsCache(DEFAULT_EXTRA_CACHE_SIZE);
      mLazyInitObjects = new LazyInitObjects(mNativeMemoryCleaner);
      mPendingPointers = new ArrayList<>(DEFAULT_PENDING_POINTERS_LIST_SIZE);
    } else {
      // We are create a child builder for proto fields, read all cache fields from the parent.
      mParent = parent;
      mNativeMemoryCleaner = parent.mNativeMemoryCleaner;
      readAllCacheFieldsFromParent(parent);
    }

    // Native extras are allocated lazily when a LEGACY session needs them.
  }

  /** Emits the track event. */
  public void emit() {
    if (!mIsCategoryEnabled) {
      return;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    if (mIsBuilt) {
      return;
    }

    mIsBuilt = true;
    try {
      boolean encoded = hasEncoded();
      if (encoded) mEncoded.finish(mTraceType == 2 || mTraceType == 4 ? "" : mEventName);
      long legacyExtra = hasLegacy() ? mExtra.getPtr() : 0;
      long track = mTrack == null ? 0 : mTrack.getPtr();
      if (encoded && mEncoded.bufferAddress() != 0) {
        PerfettoTrackEventExtra.native_emit_java_direct(
            mTraceType, mCategory.getPtr(), mEncodingModes, legacyExtra, track,
            mEncoded.buffer(), mEncoded.bufferAddress(), mEncoded.size(),
            mEncoded.fieldsSize(), mEncoded.nameSize(), mEncoded.slotsOffset(),
            mEncoded.slotCount());
      } else {
        PerfettoTrackEventExtra.native_emit_java(
            mTraceType, mCategory.getPtr(), mEncodingModes, legacyExtra, track,
            encoded ? null : mEventName, encoded ? mEncoded.buffer() : null,
            encoded ? mEncoded.size() : 0, encoded ? mEncoded.fieldsSize() : 0,
            encoded ? mEncoded.nameSize() : 0, encoded ? mEncoded.slotsOffset() : 0,
            encoded ? mEncoded.slotCount() : 0);
      }
    } finally {
      // Native extras and tracks borrow their Java owners' allocations.
      Reference.reachabilityFence(mCategory);
      Reference.reachabilityFence(mExtra);
      Reference.reachabilityFence(mPendingPointers);
      Reference.reachabilityFence(mTrack);
      if (hasEncoded()) mEncoded.release();
      mPendingPointers.clear();
      mTrack = null;
      if (mParent == null) {
        PerfettoTrackEventBuilder[] pool = sThreadLocalBuilder.get();
        for (int i = 0; i < MAX_POOLED_BUILDERS; i++) {
          if (pool[i] == null) {
            pool[i] = this;
            break;
          }
        }
      }
    }
  }

  /** Initialize the builder for a new trace event. */
  private PerfettoTrackEventBuilder initNewEvent(
      int traceType, Category category, boolean isDebug, int modes) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    mIsBuilt = false;
    mParent = null;
    mIsDebug = isDebug;
    updateNativeMemoryCleanerForDebug(mIsDebug);
    mTraceType = traceType;
    mCategory = category;
    mEventName = "";
    mEncodingModes = modes;
    mTrack = null;
    mIsProto = false;
    mIsNested = false;
    if (hasLegacy()) {
      if (mExtra == null) mExtra = new PerfettoTrackEventExtra(mNativeMemoryCleaner);
      mExtra.reset();
    }
    if (hasEncoded()) {
      if (mEncoded == null) mEncoded = new PerfettoEncodedEvent();
      mEncoded.reset();
    }
    mChildBuildersCache.reset();
    mObjectsPool.reset();
    mPendingPointers.clear();
    mCurrentContainer = null;

    return this;
  }

  private PerfettoTrackEventBuilder initChildBuilderForProto(
      PerfettoTrackEventBuilder parent, FieldContainer fieldContainer, boolean nested, int payload) {
    mIsBuilt = false;
    mParent = parent;
    mIsDebug = parent.mIsDebug;
    updateNativeMemoryCleanerForDebug(mIsDebug);

    readAllCacheFieldsFromParent(parent);

    mCurrentContainer = fieldContainer;
    mEncodingModes = parent.mEncodingModes;
    mEncoded = parent.mEncoded;
    mIsProto = true;
    mIsNested = nested;
    mNestedToken = payload;
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

  private void readAllCacheFieldsFromParent(PerfettoTrackEventBuilder parent) {
    mChildBuildersCache = parent.mChildBuildersCache;
    mObjectsPool = parent.mObjectsPool;
    mObjectsCache = parent.mObjectsCache;
    mLazyInitObjects = parent.mLazyInitObjects;
    mPendingPointers = parent.mPendingPointers;
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
    if (hasEncoded()) {
      mEncoded.addArg(name, val);
    }
    if (hasLegacy()) {
      Arg arg = mObjectsCache.mArgCache.get(name.hashCode());
      if (arg == null || !arg.getName().equals(name)) {
        arg = new Arg(name, mNativeMemoryCleaner);
        mObjectsCache.mArgCache.put(name.hashCode(), arg);
      }
      arg.setValueInt64(val);
      addPerfettoPointerToExtra(arg);
    }
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
    if (hasEncoded()) {
      mEncoded.addArg(name, val);
    }
    if (hasLegacy()) {
      Arg arg = mObjectsCache.mArgCache.get(name.hashCode());
      if (arg == null || !arg.getName().equals(name)) {
        arg = new Arg(name, mNativeMemoryCleaner);
        mObjectsCache.mArgCache.put(name.hashCode(), arg);
      }
      arg.setValueBool(val);
      addPerfettoPointerToExtra(arg);
    }
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
    if (hasEncoded()) {
      mEncoded.addArg(name, val);
    }
    if (hasLegacy()) {
      Arg arg = mObjectsCache.mArgCache.get(name.hashCode());
      if (arg == null || !arg.getName().equals(name)) {
        arg = new Arg(name, mNativeMemoryCleaner);
        mObjectsCache.mArgCache.put(name.hashCode(), arg);
      }
      arg.setValueDouble(val);
      addPerfettoPointerToExtra(arg);
    }
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
    if (hasEncoded()) {
      mEncoded.addArg(name, val);
    }
    if (hasLegacy()) {
      Arg arg = mObjectsCache.mArgCache.get(name.hashCode());
      if (arg == null || !arg.getName().equals(name)) {
        arg = new Arg(name, mNativeMemoryCleaner);
        mObjectsCache.mArgCache.put(name.hashCode(), arg);
      }
      arg.setValueString(val);
      addPerfettoPointerToExtra(arg);
    }
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
    if (hasEncoded()) {
      mEncoded.addBuiltinFixed64(47, id ^ PerfettoTrace.getProcessTrackUuid());
    }
    if (hasLegacy()) {
      Flow flow = mObjectsPool.mFlowPool.get(flowSupplier);
      flow.setProcessFlow(id);
      addPerfettoPointerToExtra(flow);
    }
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
    if (hasEncoded()) {
      mEncoded.addBuiltinFixed64(48, id ^ PerfettoTrace.getProcessTrackUuid());
    }
    if (hasLegacy()) {
      Flow terminatingFlow = mObjectsPool.mTerminatingFlowPool.get(flowSupplier);
      terminatingFlow.setProcessTerminatingFlow(id);
      addPerfettoPointerToExtra(terminatingFlow);
    }
    return this;
  }

  /**
   * Emits this event on {@code track}, a (possibly nested) named track. The
   * descriptor for each level of the chain is emitted once per sequence; the
   * native side derives the per-level uuids. The handle owns its native track
   * (built once on first use, reused for its lifetime), so a reused {@code track}
   * -- the intended {@code static final} usage -- is allocation-free.
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
    setNativeTrack(track.nestedTracks());
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

    int key = trackKey(id, name, parentUuid, isNameStatic);
    NamedTrack track = mObjectsCache.mNamedTrackCache.get(key);
    if (track == null
        || track.getId() != id
        || track.getParentUuid() != parentUuid
        || track.isNameStatic() != isNameStatic
        || !track.getName().equals(name)) {
      track = new NamedTrack(id, name, parentUuid, isNameStatic, mNativeMemoryCleaner);
      mObjectsCache.mNamedTrackCache.put(key, track);
    }
    setNativeTrack(track);
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

    int key = trackKey(0, name, parentUuid, isNameStatic);
    CounterTrack track = mObjectsCache.mCounterTrackCache.get(key);
    if (track == null
        || track.getParentUuid() != parentUuid
        || track.isNameStatic() != isNameStatic
        || !track.getName().equals(name)) {
      track = new CounterTrack(name, parentUuid, isNameStatic, mNativeMemoryCleaner);
      mObjectsCache.mCounterTrackCache.put(key, track);
    }
    setNativeTrack(track);
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

  /** Adds the event to a state track instead. Required for setting state values. */
  public PerfettoTrackEventBuilder usingStateTrack(
      long parentUuid, @CompileTimeConstant String name) {
    return usingStateTrack(parentUuid, name, /* isNameStatic = */ true);
  }

  /** Adds the event to a state track with a dynamic name instead. */
  public PerfettoTrackEventBuilder usingStateTrackWithDynamicName(
      long parentUuid, String name) {
    return usingStateTrack(parentUuid, name, /* isNameStatic = */ false);
  }

  private PerfettoTrackEventBuilder usingStateTrack(
      long parentUuid, String name, boolean isNameStatic) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }

    int key = trackKey(0, name, parentUuid, isNameStatic);
    StateTrack track = mObjectsCache.mStateTrackCache.get(key);
    if (track == null
        || track.getParentUuid() != parentUuid
        || track.isNameStatic() != isNameStatic
        || !track.getName().equals(name)) {
      track = new StateTrack(name, parentUuid, isNameStatic, mNativeMemoryCleaner);
      mObjectsCache.mStateTrackCache.put(key, track);
    }
    setNativeTrack(track);
    return this;
  }

  /** Adds the event to a process scoped state track. Required for setting state values. */
  public PerfettoTrackEventBuilder usingProcessStateTrack(@CompileTimeConstant String name) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    return usingStateTrack(PerfettoTrace.getProcessTrackUuid(), name);
  }

  /** Adds the event to a process scoped state track with a dynamic name. */
  public PerfettoTrackEventBuilder usingProcessStateTrackWithDynamicName(String name) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    return usingStateTrackWithDynamicName(PerfettoTrace.getProcessTrackUuid(), name);
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
    if (hasEncoded()) {
      if (mTraceType == 4) mEncoded.addBuiltinVarint(30, val);
    }
    if (hasLegacy()) {
      Counter counter = mLazyInitObjects.getCounter();
      counter.setValueInt64(val);
      addPerfettoPointerToExtra(counter);
    }
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
    if (hasEncoded()) {
      if (mTraceType == 4) mEncoded.addBuiltinFixed64(44, Double.doubleToRawLongBits(val));
    }
    if (hasLegacy()) {
      Counter counter = mLazyInitObjects.getCounter();
      counter.setValueDouble(val);
      addPerfettoPointerToExtra(counter);
    }
    return this;
  }

  /**
   * Sets an integer correlation id on the event (TrackEvent's {@code correlation_id}), an opaque
   * id linking this event with other events that are part of the same logical operation, even if
   * they are not causally connected. For causally connected events, prefer {@link #addFlow}.
   */
  public PerfettoTrackEventBuilder setCorrelationId(long id) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    if (hasEncoded()) {
      mEncoded.addBuiltinVarint(52, id);
    }
    if (hasLegacy()) {
      CorrelationId correlationId = mLazyInitObjects.getCorrelationId();
      correlationId.setValueInt64(id);
      addPerfettoPointerToExtra(correlationId);
    }
    return this;
  }

  /**
   * Sets a string correlation id on the event (TrackEvent's {@code correlation_id_str}), an
   * opaque id linking this event with other events that are part of the same logical operation,
   * even if they are not causally connected. For causally connected events, prefer {@link
   * #addFlow}.
   */
  public PerfettoTrackEventBuilder setCorrelationId(String id) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    if (hasEncoded()) {
      mEncoded.addBuiltinString(53, id);
    }
    if (hasLegacy()) {
      CorrelationId correlationId = mLazyInitObjects.getCorrelationId();
      correlationId.setValueString(id);
      addPerfettoPointerToExtra(correlationId);
    }
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
    if (hasEncoded()) {
      mEncoded.checkNestedDepth(mIsNested ? mNestedToken + 1 : 0);
      mEncoded.addVarint(id, val);
    }
    if (hasLegacy()) {
      Field field = mObjectsPool.mFieldPool.get(fieldSupplier);
      field.setValueInt64(id, val);
      addFieldToContainer(field);
    }
    return this;
  }

  /**
   * Adds a proto boolean field with field id {@code id}, encoded as the varint 1
   * ({@code true}) or 0 ({@code false}).
   */
  public PerfettoTrackEventBuilder addField(long id, boolean val) {
    return addField(id, val ? 1L : 0L);
  }

  /** Adds a proto field with field id {@code id} and value {@code val}. */
  public PerfettoTrackEventBuilder addField(long id, double val) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkBuildingProto();
    }
    if (hasEncoded()) {
      mEncoded.checkNestedDepth(mIsNested ? mNestedToken + 1 : 0);
      mEncoded.addFixed64(id, Double.doubleToRawLongBits(val));
    }
    if (hasLegacy()) {
      Field field = mObjectsPool.mFieldPool.get(fieldSupplier);
      field.setValueDouble(id, val);
      addFieldToContainer(field);
    }
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
    if (hasEncoded()) {
      mEncoded.checkNestedDepth(mIsNested ? mNestedToken + 1 : 0);
      mEncoded.addString(id, val);
    }
    if (hasLegacy()) {
      addLegacyStringField(id, val);
    }
    return this;
  }

  // The callstack path has both pieces already. Only LEGACY needs the joined string.
  PerfettoTrackEventBuilder addField(long id, String first, char separator, String second) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkBuildingProto();
    }
    if (hasEncoded()) {
      mEncoded.checkNestedDepth(mIsNested ? mNestedToken + 1 : 0);
      mEncoded.addString(id, first, separator, second);
    }
    if (hasLegacy()) {
      addLegacyStringField(id, first + separator + second);
    }
    return this;
  }

  private void addLegacyStringField(long id, String value) {
    Field field = mObjectsPool.mFieldPool.get(fieldSupplier);
    field.setValueString(id, value);
    addFieldToContainer(field);
  }

  /**
   * Adds a proto field with field id { @code id} and value { @code val}.
   * { @code internedTypeId} must be non-zero, in which case the string { @code val} will be interned
   * with the given type ID. If { @code internedTypeId} is zero, the string is dropped silently.
   */
  public PerfettoTrackEventBuilder addFieldWithInterning(long id, String val, long internedTypeId) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkBuildingProto();
    }
    if (hasEncoded()) {
      mEncoded.checkNestedDepth(mIsNested ? mNestedToken + 1 : 0);
      mEncoded.addInternedString(id, val, internedTypeId);
    }
    if (hasLegacy()) {
      Field field = mObjectsPool.mFieldPool.get(fieldSupplier);
      field.setValueWithInterning(id, val, internedTypeId);
      addFieldToContainer(field);
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
    if (hasEncoded()) mEncoded.beginProto();
    Proto proto = null;
    if (hasLegacy()) {
      proto = mObjectsPool.mProtoPool.get(protoSupplier);
      proto.clearFields();
      addPerfettoPointerToExtra(proto);
    }
    return mChildBuildersCache
        .get(perfettoTrackEventBuilderSupplier)
        .initChildBuilderForProto(this, proto, false, 0);
  }

  /** Ends a proto field. */
  public PerfettoTrackEventBuilder endProto() {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkMatchingBeginProto();
    }
    if (hasEncoded()) mEncoded.checkNestedDepth(0);
    return mParent;
  }

  /**
   * Begins a nested proto field with field id {@code id}. Fields can be added from this point and
   * there must be a corresponding {@link #endNested}.
   *
   * <p>Finish the nested field before adding more proto fields to its parent. The encoded mode
   * rejects writes to a parent while its nested field is open. Root event setters may still be
   * used while building a nested proto field. Encoded events support up to 64 nested messages.
   */
  public PerfettoTrackEventBuilder beginNested(long id) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkBuildingProto();
    }
    int payload = 0;
    if (hasEncoded()) {
      mEncoded.checkNestedDepth(mIsNested ? mNestedToken + 1 : 0);
      payload = mEncoded.beginNested(id);
    }
    FieldNested field = null;
    if (hasLegacy()) {
      field = mObjectsPool.mFieldNestedPool.get(fieldNestedSupplier);
      field.setId(id);
      addFieldToContainer(field);
    }
    return mChildBuildersCache
        .get(perfettoTrackEventBuilderSupplier)
        .initChildBuilderForProto(this, field, true, payload);
  }

  /** Ends a nested proto field. */
  public PerfettoTrackEventBuilder endNested() {
    if (!mIsCategoryEnabled) {
      return this;
    }

    if (mIsDebug) {
      checkMatchingBeginNested();
    }

    if (hasEncoded()) mEncoded.endNested(mNestedToken);
    return mParent;
  }

  private boolean hasLegacy() {
    return (mEncodingModes & ENCODING_LEGACY) != 0;
  }

  private boolean hasEncoded() {
    return (mEncodingModes & ENCODING_ENCODED) != 0;
  }

  private void setNativeTrack(PerfettoPointer track) {
    mTrack = track;
    if (hasLegacy()) addPerfettoPointerToExtra(track);
  }

  private void addFieldToContainer(PerfettoPointer field) {
    // Keep reference to the java object, `mCurrentContainer` uses a native part of the field
    // object.
    mPendingPointers.add(field);
    mCurrentContainer.addField(field);
  }

  private void addPerfettoPointerToExtra(PerfettoPointer arg) {
    // Keep reference to the java object, `mCurrentContainer` uses a native part of the field
    // object.
    mPendingPointers.add(arg);
    mExtra.addPerfettoPointer(arg);
  }

  // Only used in tests.
  public static AllocationStats getNativeAllocationStats() {
    return sNativeAllocationStats;
  }

  // Hash collisions are possible, so cache hits must compare all track inputs.
  private static int trackKey(long id, String name, long parentUuid, boolean isNameStatic) {
    int key = name.hashCode();
    key = 31 * key + Long.hashCode(id);
    key = 31 * key + Long.hashCode(parentUuid);
    return 31 * key + (isNameStatic ? 1 : 0);
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

  private static final class Pool<T> {
    private final int mCapacity;
    private final T[] mValueArray;
    private int mIdx = 0;

    Pool(int capacity) {
      mCapacity = capacity;
      mValueArray = (T[]) new Object[capacity];
    }

    public void reset() {
      mIdx = 0;
    }

    public T get(Supplier<T> supplier) {
      if (mIdx >= mCapacity) {
        return supplier.get();
      }
      if (mValueArray[mIdx] == null) {
        mValueArray[mIdx] = supplier.get();
      }
      return mValueArray[mIdx++];
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

  private boolean isBuildingTopLevelExtra() {
    return !mIsProto;
  }

  private boolean isBuildingProto() {
    return mIsProto && !mIsNested;
  }

  private boolean isBuildingNestedProto() {
    return mIsNested;
  }

  private boolean isBuildingProtoOrNestedProto() {
    return isBuildingProto() || isBuildingNestedProto();
  }

  private void checkNotBuildingProto() {
    checkState();
    if (isBuildingProtoOrNestedProto()) throwNotBuildingProtoError();
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwNotBuildingProtoError() {
    throw new IllegalStateException("Operation not supported for proto.");
  }

  private void checkBuildingProto() {
    checkState();
    if (isBuildingTopLevelExtra()) throwBuildingProtoError();
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwBuildingProtoError() {
    throw new IllegalStateException("Field operations must be within beginProto/endProto block.");
  }

  private void checkMatchingBeginNested() {
    checkState();
    if (!isBuildingNestedProto()) throwMatchingBeginNestedError();
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwMatchingBeginNestedError() {
    throw new IllegalStateException("No matching beginNested call.");
  }

  private void checkMatchingBeginProto() {
    checkState();
    if (!isBuildingProto()) throwMatchingBeginProtoError();
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwMatchingBeginProtoError() {
    throw new IllegalStateException("No matching beginProto call.");
  }
}
