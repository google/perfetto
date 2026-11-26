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
import java.util.concurrent.atomic.AtomicBoolean;
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
import dev.perfetto.sdk.PerfettoTrackEventExtra.NestedTracks;
import dev.perfetto.sdk.PerfettoTrackEventExtra.PerfettoPointer;
import dev.perfetto.sdk.PerfettoTrackEventExtra.StateTrack;
import dev.perfetto.sdk.PerfettoTrackEventExtra.Proto;

/** Builder for Perfetto track event extras. */
public final class PerfettoTrackEventBuilder {
  private static final int DEFAULT_EXTRA_CACHE_SIZE = 16;
  private static final int DEFAULT_PENDING_POINTERS_LIST_SIZE = 16;

  // Event types. Must match enum PerfettoTeType in track_event_abi.h.
  static final int PERFETTO_TE_TYPE_SLICE_BEGIN = 1;
  static final int PERFETTO_TE_TYPE_SLICE_END = 2;
  static final int PERFETTO_TE_TYPE_INSTANT = 3;
  static final int PERFETTO_TE_TYPE_COUNTER = 4;
  static final int PERFETTO_TE_TYPE_STATE = 5;

  // Must match PERFETTO_TE_EVENT_FORMAT_* in track_event_abi.h.
  // Both bits can be set if multiple sessions running concurrently chose different formats, so
  // setters check buildsRawProtoEvent() and buildsExtrasEvent() independently.
  private static final int EVENT_FORMAT_EXTRAS = 1;
  private static final int EVENT_FORMAT_RAW_PROTO = 2;

  private static final int SCOPE_EVENT = 0;
  private static final int SCOPE_PROTO = 1; // Between beginProto() and endProto().
  private static final int SCOPE_NESTED = 2; // Between beginNested() and endNested().

  private PerfettoTrackEventExtra mExtra;
  // Shared with child builders for RAW_PROTO events.
  private PerfettoRawProtoEvent mRawProto;
  private int mEventFormats;
  private PerfettoPointer mTrack;
  // Native pointer of mTrack (0 for default track). mTrack keeps it alive.
  private long mTrackPtr;
  private int mScope = SCOPE_EVENT;
  // 0 for SCOPE_EVENT / SCOPE_PROTO, and 1..MAX_NESTING_DEPTH for SCOPE_NESTED.
  private int mNestedDepth;

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
  private static final AtomicBoolean sLoggedDroppedInternedField = new AtomicBoolean(false);

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
    int formats = category.getEventFormats();
    if (formats == 0) {
      return NO_OP_BUILDER;
    }
    PerfettoTrackEventBuilder[] pool = sThreadLocalBuilder.get();
    for (int i = MAX_POOLED_BUILDERS - 1; i >= 0; i--) {
      PerfettoTrackEventBuilder builder = pool[i];
      if (builder != null) {
        pool[i] = null;
        return builder.initNewEvent(traceType, category, isDebug, formats);
      }
    }
    return new PerfettoTrackEventBuilder(/* isCategoryEnabled= */ true, /* parent= */ null)
        .initNewEvent(traceType, category, isDebug, formats);
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

    // Native extras are allocated lazily when an EXTRAS session needs them.
  }

  /** Emits the track event. */
  public void emit() {
    if (!mIsCategoryEnabled || !isEventBuilder() || mIsBuilt) {
      return;
    }
    mIsBuilt = true;
    // Always reset and return to the pool, even if debug checks throw.
    try {
      switch (mEventFormats) {
        case EVENT_FORMAT_EXTRAS:
          PerfettoTrackEventExtra.native_emit(
              mTraceType, mCategory.getPtr(), mEventName, mExtra.getPtr());
          break;
        case EVENT_FORMAT_RAW_PROTO:
          emitRawProtoOnly();
          break;
        default: // EVENT_FORMAT_EXTRAS | EVENT_FORMAT_RAW_PROTO
          emitRawProtoAndExtras();
          break;
      }
    } finally {
      releaseAfterEmit();
    }
  }

  private void releaseAfterEmit() {
    if (buildsRawProtoEvent()) {
      mRawProto.release();
    }
    // Release references to native extras so their Java owners can be collected.
    if (buildsExtrasEvent()) {
      mPendingPointers.clear();
    }
    mTrack = null;
    mTrackPtr = 0;
    mEventName = null;
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

  // Finishes serialization and validates bounds before passing the buffer to native code.
  private boolean prepareRawProtoForEmit() {
    PerfettoRawProtoEvent event = mRawProto;
    event.finish(mEventName);
    if (event.failed()) {
      dropInvalidEvent("Raw-proto event construction failed");
      return false;
    }

    // Native code reads a non-movable array by raw address without its length.
    if (event.bufferAddress() != 0 && event.size() > event.buffer().length) {
      dropInvalidEvent("Raw-proto event exceeds its array");
      return false;
    }
    return true;
  }

  private void emitRawProtoOnly() {
    if (!prepareRawProtoForEmit()) {
      return;
    }
    PerfettoRawProtoEvent event = mRawProto;
    byte[] buffer = event.buffer();
    long address = event.bufferAddress();

    boolean ok = true;
    if (address != 0) {
      // Non-movable buffer fast path (@CriticalNative, no JNI copy).
      PerfettoTrackEventExtra.native_emit_raw_proto(mTraceType, mCategory.getPtr(), mTrackPtr,
          address, event.fieldsSize(), event.nameSize());
    } else {
      ok = PerfettoTrackEventExtra.native_emit_with_formats(mTraceType, mCategory.getPtr(),
          EVENT_FORMAT_RAW_PROTO, /* extras= */ 0, mTrackPtr, buffer, /* address= */ 0,
          event.fieldsSize(), event.nameSize(), event.size());
    }

    // Keep `buffer` reachable while native code reads it by raw address.
    Reference.reachabilityFence(buffer);
    if (!ok) {
      dropInvalidEvent("Invalid raw-proto event");
    }
  }

  private void emitRawProtoAndExtras() {
    if (!prepareRawProtoForEmit()) {
      // Fall back to native_emit so EXTRAS sessions still receive the event.
      PerfettoTrackEventExtra.native_emit(
          mTraceType, mCategory.getPtr(), mEventName, mExtra.getPtr());
      return;
    }
    PerfettoRawProtoEvent event = mRawProto;
    byte[] buffer = event.buffer();

    boolean ok = PerfettoTrackEventExtra.native_emit_with_formats(mTraceType, mCategory.getPtr(),
        EVENT_FORMAT_EXTRAS | EVENT_FORMAT_RAW_PROTO, mExtra.getPtr(), mTrackPtr, buffer,
        event.bufferAddress(), event.fieldsSize(), event.nameSize(), event.size());

    Reference.reachabilityFence(buffer);
    if (!ok) {
      dropInvalidEvent("Invalid raw-proto event");
    }
  }

  private void dropInvalidEvent(String message) {
    if (mIsDebug) {
      throw new IllegalArgumentException(message);
    }
  }

  /** Initialize the builder for a new trace event. */
  private PerfettoTrackEventBuilder initNewEvent(
      int traceType, Category category, boolean isDebug, int formats) {
    mIsBuilt = false;
    mParent = null;
    mIsDebug = isDebug;
    updateNativeMemoryCleanerForDebug(mIsDebug);
    mTraceType = traceType;
    mCategory = category;
    mEventName = "";
    mEventFormats = formats;
    mScope = SCOPE_EVENT;
    mNestedDepth = 0;
    if (buildsExtrasEvent()) {
      prepareExtrasStorage();
    }
    if (buildsRawProtoEvent()) {
      // Already reset by release() after the previous emit().
      if (mRawProto == null) {
        mRawProto = new PerfettoRawProtoEvent();
      }
      mRawProto.setThrowOnError(isDebug);
    }
    mChildBuildersCache.reset();
    mCurrentContainer = null;

    return this;
  }

  private void prepareExtrasStorage() {
    if (mExtra == null) {
      mExtra = new PerfettoTrackEventExtra(mNativeMemoryCleaner);
    } else {
      mExtra.reset();
    }
    mObjectsPool.reset();
  }

  private PerfettoTrackEventBuilder initChildBuilderForProto(
      PerfettoTrackEventBuilder parent, Proto proto) {
    return initChildBuilder(parent, proto, SCOPE_PROTO, /* nestedDepth= */ 0);
  }

  private PerfettoTrackEventBuilder initChildBuilderForNested(
      PerfettoTrackEventBuilder parent, FieldNested field) {
    return initChildBuilder(parent, field, SCOPE_NESTED, parent.mNestedDepth + 1);
  }

  private PerfettoTrackEventBuilder initChildBuilder(PerfettoTrackEventBuilder parent,
      FieldContainer fieldContainer, int scope, int nestedDepth) {
    mIsBuilt = false;
    mParent = parent;
    mIsDebug = parent.mIsDebug;
    updateNativeMemoryCleanerForDebug(mIsDebug);

    readAllCacheFieldsFromParent(parent);

    mCurrentContainer = fieldContainer;
    mEventFormats = parent.mEventFormats;
    mRawProto = parent.mRawProto;
    mScope = scope;
    mNestedDepth = nestedDepth;
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
    if (mTraceType == PERFETTO_TE_TYPE_COUNTER || mTraceType == PERFETTO_TE_TYPE_SLICE_END) {
      return this;
    }
    mEventName = eventName;
    return this;
  }

  /**
   * Forces this event to use the EXTRAS format for every session, including RAW_PROTO sessions.
   * Call before adding args, fields or counter values.
   *
   * <p>Required by callers that use {@link #addFieldWithInterning} (such as JobScheduler), because
   * RAW_PROTO cannot intern strings across sessions.
   */
  // TODO(b/587941572): Remove once callers no longer need per-field interning.
  public PerfettoTrackEventBuilder useExtrasFormat() {
    if (!mIsCategoryEnabled || !isEventBuilder() || !buildsRawProtoEvent()) {
      return this;
    }
    if (!mRawProto.isEmpty()) {
      if (mIsDebug) {
        throw new IllegalStateException(
            "useExtrasFormat() must come before args, fields and counter values");
      }
      // Too late to switch without dropping already-serialized fields.
      return this;
    }
    if (!buildsExtrasEvent()) {
      // Lazily initialize EXTRAS storage skipped in initNewEvent() for RAW_PROTO-only events.
      prepareExtrasStorage();
      if (mTrack != null) {
        addPerfettoPointerToExtra(mTrack);
      }
    }
    mEventFormats = EVENT_FORMAT_EXTRAS;
    return this;
  }

  /** Adds a debug arg with key {@code name} and value {@code val}. */
  public PerfettoTrackEventBuilder addArg(String name, long val) {
    if (!mIsCategoryEnabled || !isEventBuilder()) {
      return this;
    }
    if (buildsRawProtoEvent()) {
      mRawProto.addArg(name, val);
    }
    if (buildsExtrasEvent()) {
      Arg arg = getOrCreateArg(name);
      arg.setValueInt64(val);
      addPerfettoPointerToExtra(arg);
    }
    return this;
  }

  /** Adds a debug arg with key {@code name} and value {@code val}. */
  public PerfettoTrackEventBuilder addArg(String name, boolean val) {
    if (!mIsCategoryEnabled || !isEventBuilder()) {
      return this;
    }
    if (buildsRawProtoEvent()) {
      mRawProto.addArg(name, val);
    }
    if (buildsExtrasEvent()) {
      Arg arg = getOrCreateArg(name);
      arg.setValueBool(val);
      addPerfettoPointerToExtra(arg);
    }
    return this;
  }

  /** Adds a debug arg with key {@code name} and value {@code val}. */
  public PerfettoTrackEventBuilder addArg(String name, double val) {
    if (!mIsCategoryEnabled || !isEventBuilder()) {
      return this;
    }
    if (buildsRawProtoEvent()) {
      mRawProto.addArg(name, val);
    }
    if (buildsExtrasEvent()) {
      Arg arg = getOrCreateArg(name);
      arg.setValueDouble(val);
      addPerfettoPointerToExtra(arg);
    }
    return this;
  }

  /** Adds a debug arg with key {@code name} and value {@code val}. */
  public PerfettoTrackEventBuilder addArg(String name, String val) {
    if (!mIsCategoryEnabled || !isEventBuilder()) {
      return this;
    }
    if (buildsRawProtoEvent()) {
      mRawProto.addArg(name, val);
    }
    if (buildsExtrasEvent()) {
      Arg arg = getOrCreateArg(name);
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
    if (!mIsCategoryEnabled || !isEventBuilder()) {
      return this;
    }
    if (buildsRawProtoEvent()) {
      mRawProto.addFlowId(id ^ PerfettoTrace.getProcessTrackUuid());
    }
    if (buildsExtrasEvent()) {
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
    if (!mIsCategoryEnabled || !isEventBuilder()) {
      return this;
    }
    if (buildsRawProtoEvent()) {
      mRawProto.addTerminatingFlowId(id ^ PerfettoTrace.getProcessTrackUuid());
    }
    if (buildsExtrasEvent()) {
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
    if (!mIsCategoryEnabled || !isEventBuilder()) {
      return this;
    }

    // The handle owns its native track (built once, reused for its lifetime), so
    // there is no builder-side cache to consult -- just hand it over.
    NestedTracks nestedTracks = track.nestedTracks();
    setTrack(nestedTracks, nestedTracks.getPtr());
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
    if (!mIsCategoryEnabled || !isEventBuilder()) {
      return this;
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
    setTrack(track, track.getPtr());
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
    if (!mIsCategoryEnabled || !isEventBuilder()) {
      return this;
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
    setTrack(track, track.getPtr());
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
    if (!mIsCategoryEnabled || !isEventBuilder()) {
      return this;
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
    setTrack(track, track.getPtr());
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
    if (!mIsCategoryEnabled || !isEventBuilder() || mTraceType != PERFETTO_TE_TYPE_COUNTER) {
      return this;
    }
    if (buildsRawProtoEvent()) {
      mRawProto.setCounterValue(val);
    }
    if (buildsExtrasEvent()) {
      Counter counter = mLazyInitObjects.getCounter();
      counter.setValueInt64(val);
      addPerfettoPointerToExtra(counter);
    }
    return this;
  }

  /** Sets a double counter value on the event. */
  public PerfettoTrackEventBuilder setCounter(double val) {
    if (!mIsCategoryEnabled || !isEventBuilder() || mTraceType != PERFETTO_TE_TYPE_COUNTER) {
      return this;
    }
    if (buildsRawProtoEvent()) {
      mRawProto.setCounterValue(val);
    }
    if (buildsExtrasEvent()) {
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
    if (!mIsCategoryEnabled || !isEventBuilder()) {
      return this;
    }
    if (buildsRawProtoEvent()) {
      mRawProto.setCorrelationId(id);
    }
    if (buildsExtrasEvent()) {
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
    if (!mIsCategoryEnabled || !isEventBuilder()) {
      return this;
    }
    if (buildsRawProtoEvent()) {
      mRawProto.setCorrelationId(id);
    }
    if (buildsExtrasEvent()) {
      CorrelationId correlationId = mLazyInitObjects.getCorrelationId();
      correlationId.setValueString(id);
      addPerfettoPointerToExtra(correlationId);
    }
    return this;
  }

  /** Adds a proto field with field id {@code id} and value {@code val}. */
  public PerfettoTrackEventBuilder addField(long id, long val) {
    if (!mIsCategoryEnabled || !isProtoBuilder()) {
      return this;
    }
    if (buildsRawProtoEvent()) {
      checkNoOpenNestedField();
      mRawProto.addVarint(id, val);
    }
    if (buildsExtrasEvent()) {
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
    if (!mIsCategoryEnabled || !isProtoBuilder()) {
      return this;
    }
    if (buildsRawProtoEvent()) {
      checkNoOpenNestedField();
      mRawProto.addFixed64(id, Double.doubleToRawLongBits(val));
    }
    if (buildsExtrasEvent()) {
      Field field = mObjectsPool.mFieldPool.get(fieldSupplier);
      field.setValueDouble(id, val);
      addFieldToContainer(field);
    }
    return this;
  }

  /** Adds a proto field with field id {@code id} and value {@code val}. */
  public PerfettoTrackEventBuilder addField(long id, String val) {
    if (!mIsCategoryEnabled || !isProtoBuilder()) {
      return this;
    }
    if (buildsRawProtoEvent()) {
      checkNoOpenNestedField();
      mRawProto.addString(id, val);
    }
    if (buildsExtrasEvent()) {
      Field field = mObjectsPool.mFieldPool.get(fieldSupplier);
      field.setValueString(id, val);
      addFieldToContainer(field);
    }
    return this;
  }

  /**
   * Adds a proto field with field id {@code id} and value {@code val}. {@code internedTypeId} must
   * be non-zero to intern {@code val} under that type ID (0 drops the field silently). Call {@link
   * #useExtrasFormat} before adding fields when interning is needed, otherwise RAW_PROTO sessions
   * omit this field.
   */
  public PerfettoTrackEventBuilder addFieldWithInterning(long id, String val, long internedTypeId) {
    if (!mIsCategoryEnabled || !isProtoBuilder()) {
      return this;
    }
    if (buildsRawProtoEvent() && internedTypeId != 0 && !sLoggedDroppedInternedField.get()
        && sLoggedDroppedInternedField.compareAndSet(false, true)) {
      System.err.println(
          "PerfettoTrackEventBuilder: addFieldWithInterning() requires useExtrasFormat() "
              + "for RAW_PROTO sessions, dropping interned field " + id);
    }
    if (!buildsExtrasEvent()) {
      return this;
    }
    Field field = mObjectsPool.mFieldPool.get(fieldSupplier);
    field.setValueWithInterning(id, val, internedTypeId);
    addFieldToContainer(field);
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
    if (!mIsCategoryEnabled || !isEventBuilder()) {
      return this;
    }
    Proto proto = null;
    if (buildsExtrasEvent()) {
      proto = mObjectsPool.mProtoPool.get(protoSupplier);
      proto.clearFields();
      addPerfettoPointerToExtra(proto);
    }
    return mChildBuildersCache
        .get(perfettoTrackEventBuilderSupplier)
        .initChildBuilderForProto(this, proto);
  }

  /** Ends a proto field. */
  public PerfettoTrackEventBuilder endProto() {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkMatchingBeginProto();
    }
    if (buildsRawProtoEvent()) {
      mRawProto.checkNestedDepth(0);
    }
    return mParent;
  }

  /**
   * Begins a nested proto field with field id {@code id}. Fields can be added from this point and
   * there must be a corresponding {@link #endNested}.
   */
  public PerfettoTrackEventBuilder beginNested(long id) {
    if (!mIsCategoryEnabled || !isProtoBuilder()) {
      return this;
    }
    if (buildsRawProtoEvent()) {
      // RAW_PROTO streams sequentially, so reject writing to a parent with an open child (max depth 64).
      checkNoOpenNestedField();
      mRawProto.beginNested(id);
    }
    FieldNested field = null;
    if (buildsExtrasEvent()) {
      field = mObjectsPool.mFieldNestedPool.get(fieldNestedSupplier);
      field.setId(id);
      addFieldToContainer(field);
    }
    return mChildBuildersCache
        .get(perfettoTrackEventBuilderSupplier)
        .initChildBuilderForNested(this, field);
  }

  /** Ends a nested proto field. */
  public PerfettoTrackEventBuilder endNested() {
    if (!mIsCategoryEnabled) {
      return this;
    }

    if (mIsDebug) {
      checkMatchingBeginNested();
    }
    if (buildsRawProtoEvent()) {
      mRawProto.endNested(mNestedDepth);
    }
    return mParent;
  }

  // True if this event is being built in the EXTRAS format (native extras).
  private boolean buildsExtrasEvent() {
    return (mEventFormats & EVENT_FORMAT_EXTRAS) != 0;
  }

  // True if this event is being built in the RAW_PROTO format (Java protobuf bytes).
  private boolean buildsRawProtoEvent() {
    return (mEventFormats & EVENT_FORMAT_RAW_PROTO) != 0;
  }

  // Fails the raw-proto event if a child nested field is still open.
  private void checkNoOpenNestedField() {
    mRawProto.checkNestedDepth(mNestedDepth);
  }

  private Arg getOrCreateArg(String name) {
    Arg arg = mObjectsCache.mArgCache.get(name.hashCode());
    if (arg == null || !arg.getName().equals(name)) {
      arg = new Arg(name, mNativeMemoryCleaner);
      mObjectsCache.mArgCache.put(name.hashCode(), arg);
    }
    return arg;
  }

  // Tracks are passed natively in both formats. Callers pass getPtr() via the concrete class.
  private void setTrack(PerfettoPointer track, long trackPtr) {
    mTrack = track;
    mTrackPtr = trackPtr;
    if (buildsExtrasEvent()) {
      addPerfettoPointerToExtra(track);
    }
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

  // True for the root event builder (SCOPE_EVENT). Throws in debug mode when misused on a proto
  // child builder.
  private boolean isEventBuilder() {
    if (mIsDebug) {
      checkState();
      if (mScope != SCOPE_EVENT) throwNotBuildingProtoError();
    }
    return mScope == SCOPE_EVENT;
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwNotBuildingProtoError() {
    throw new IllegalStateException("Operation not supported for proto.");
  }

  // True for proto child builders (SCOPE_PROTO / SCOPE_NESTED). Throws in debug mode when misused
  // on the root event builder.
  private boolean isProtoBuilder() {
    if (mIsDebug) {
      checkState();
      if (mScope == SCOPE_EVENT) throwBuildingProtoError();
    }
    return mScope != SCOPE_EVENT;
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwBuildingProtoError() {
    throw new IllegalStateException("Field operations must be within beginProto/endProto block.");
  }

  private void checkMatchingBeginNested() {
    checkState();
    if (mScope != SCOPE_NESTED) throwMatchingBeginNestedError();
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwMatchingBeginNestedError() {
    throw new IllegalStateException("No matching beginNested call.");
  }

  private void checkMatchingBeginProto() {
    checkState();
    if (mScope != SCOPE_PROTO) throwMatchingBeginProtoError();
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwMatchingBeginProtoError() {
    throw new IllegalStateException("No matching beginProto call.");
  }
}
