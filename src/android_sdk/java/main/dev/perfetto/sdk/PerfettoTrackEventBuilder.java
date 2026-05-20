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
import java.util.function.Supplier;

import dev.perfetto.sdk.PerfettoNativeMemoryCleaner.AllocationStats;
import dev.perfetto.sdk.PerfettoTrace.Category;
import dev.perfetto.sdk.PerfettoTrackEventExtra.Arg;
import dev.perfetto.sdk.PerfettoTrackEventExtra.Counter;
import dev.perfetto.sdk.PerfettoTrackEventExtra.CounterTrack;
import dev.perfetto.sdk.PerfettoTrackEventExtra.Field;
import dev.perfetto.sdk.PerfettoTrackEventExtra.FieldContainer;
import dev.perfetto.sdk.PerfettoTrackEventExtra.FieldNested;
import dev.perfetto.sdk.PerfettoTrackEventExtra.Flow;
import dev.perfetto.sdk.PerfettoTrackEventExtra.NamedTrack;
import dev.perfetto.sdk.PerfettoTrackEventExtra.PerfettoPointer;
import dev.perfetto.sdk.PerfettoTrackEventExtra.Proto;

/** Builder for Perfetto track event extras. */
public final class PerfettoTrackEventBuilder {
  private static final int DEFAULT_EXTRA_CACHE_SIZE = 5;
  private static final int DEFAULT_PENDING_POINTERS_LIST_SIZE = 16;

  // When true, events are emitted via the Java-side path ({@link PerfettoEvent})
  // instead of the High Level ABI, as long as they use only migrated features
  // (name, category, debug args). An event that also uses a not-yet-migrated
  // extra (track, flow, counter, proto) falls back to the HL path. Defaults to
  // the "perfetto.use_java_emit" system property (off unless set).
  private static volatile boolean sUseJavaEmit =
      Boolean.getBoolean("perfetto.use_java_emit");

  private PerfettoTrackEventExtra mExtra;

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
    public final RingBuffer<Arg> mArgCache;

    public ObjectsCache(int capacity) {
      mNamedTrackCache = new RingBuffer<>(capacity);
      mCounterTrackCache = new RingBuffer<>(capacity);
      mArgCache = new RingBuffer<>(capacity);
    }
  }

  private static final class LazyInitObjects {
    private Counter mCounter = null;

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
  }

  private Pool<PerfettoTrackEventBuilder> mChildBuildersCache;
  private ObjectsPool mObjectsPool;
  private ObjectsCache mObjectsCache;
  private LazyInitObjects mLazyInitObjects;

  private final boolean mIsCategoryEnabled;

  private ArrayList<PerfettoPointer> mPendingPointers;

  // Debug args buffered for the Java emit path. While sUseJavaEmit is on, args
  // are stashed here (allocation-free, reused arrays) instead of being built as
  // High Level Arg extras, then either encoded into the Java event body (if the
  // event uses no not-yet-migrated extra) or replayed as HL args (if it does).
  // This keeps the common args-only event allocation-free without losing data
  // when an arg is combined with, say, a track. Only used by the root builder.
  private static final int ARG_KIND_LONG = 0;
  private static final int ARG_KIND_BOOL = 1;
  private static final int ARG_KIND_DOUBLE = 2;
  private static final int ARG_KIND_STRING = 3;
  private static final int MAX_BUFFERED_ARGS = 32;
  private String[] mArgNames;
  private int[] mArgKinds;
  private long[] mArgLongs;
  private double[] mArgDoubles;
  private String[] mArgStrings;
  private int mArgCount;
  // Set when the buffer overflowed and the buffered args were already replayed
  // as HL args; further args on this event go straight to HL.
  private boolean mArgsSpilled;

  // The nested track set by usingTrack(), flattened root->leaf into reused
  // arrays (uuids precomputed in PerfettoTrack). The leaf is the track the event
  // attaches to; each level's descriptor is emitted once per sequence. ids are
  // kept only for the HL fallback (when the event also uses a not-yet-migrated
  // extra). Only used by the root builder.
  private static final int MAX_TRACK_LEVELS = 16; // keep in sync with C++ JNI
  private long[] mTrackUuids;
  private long[] mTrackParentUuids;
  private long[] mTrackIds;
  private String[] mTrackNames;
  private int mTrackCount;
  private boolean mHasTrack;
  private boolean mTrackNameStatic;
  private boolean mTrackIsCounter;
  private long mTrackLeafUuid;

  // Counter track uuid magic, matching kCounterMagic in the C SDK.
  private static final long COUNTER_TRACK_MAGIC = 0xb1a4a67d7970839eL;

  // Counter value set by setCounter() for the Java emit path.
  private boolean mHasCounter;
  private boolean mCounterIsDouble;
  private long mCounterLong;
  private double mCounterDouble;

  // Flows buffered for the Java emit path (raw ids; folded with the process
  // track uuid at encode time). Same rationale as the arg buffer. Root only.
  private static final int MAX_BUFFERED_FLOWS = 16;
  private long[] mFlowIds;
  private boolean[] mFlowTerminating;
  private int mFlowCount;

  // Proto-field state for the Java emit path. Non-interned fields are written
  // straight into the body; nesting tokens are tracked here for endNested.
  // Interned-string fields can't be Java-encoded (their iids need native, per-
  // sequence interning), so they're recorded and the native layer interns them.
  private static final int MAX_NESTING_DEPTH = 16;
  private static final int MAX_INTERNED_FIELDS = 16;
  private boolean mInProto;
  private int[] mProtoTokens;
  private int mProtoDepth;
  private int[] mInternedFieldIds;
  private int[] mInternedTypeIds;
  private String[] mInternedStrings;
  private int mInternedFieldCount;

  private final Supplier<PerfettoTrackEventBuilder> perfettoTrackEventBuilderSupplier =
      () -> new PerfettoTrackEventBuilder(true, this);
  private final Supplier<FieldNested> fieldNestedSupplier =
      () -> new FieldNested(mNativeMemoryCleaner);
  private final Supplier<Proto> protoSupplier = () -> new Proto(mNativeMemoryCleaner);
  private final Supplier<Field> fieldSupplier = () -> new Field(mNativeMemoryCleaner);
  private final Supplier<Flow> flowSupplier = () -> new Flow(mNativeMemoryCleaner);

  private static final PerfettoTrackEventBuilder NO_OP_BUILDER =
      new PerfettoTrackEventBuilder(/* isCategoryEnabled= */ false, /* parent= */ null);

  public static final ThreadLocal<PerfettoTrackEventBuilder> sThreadLocalBuilder =
      ThreadLocal.withInitial(
          () -> new PerfettoTrackEventBuilder(/* isCategoryEnabled= */ true, /* parent= */ null));

  public static PerfettoTrackEventBuilder newEvent(
      int traceType, Category category, boolean isDebug) {
    if (category.isRegistered() && category.isEnabled()) {
      return sThreadLocalBuilder.get().initNewEvent(traceType, category, isDebug);
    }
    return NO_OP_BUILDER;
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
      mArgNames = new String[MAX_BUFFERED_ARGS];
      mArgKinds = new int[MAX_BUFFERED_ARGS];
      mArgLongs = new long[MAX_BUFFERED_ARGS];
      mArgDoubles = new double[MAX_BUFFERED_ARGS];
      mArgStrings = new String[MAX_BUFFERED_ARGS];
      mTrackUuids = new long[MAX_TRACK_LEVELS];
      mTrackParentUuids = new long[MAX_TRACK_LEVELS];
      mTrackIds = new long[MAX_TRACK_LEVELS];
      mTrackNames = new String[MAX_TRACK_LEVELS];
      mFlowIds = new long[MAX_BUFFERED_FLOWS];
      mFlowTerminating = new boolean[MAX_BUFFERED_FLOWS];
      mProtoTokens = new int[MAX_NESTING_DEPTH];
      mInternedFieldIds = new int[MAX_INTERNED_FIELDS];
      mInternedTypeIds = new int[MAX_INTERNED_FIELDS];
      mInternedStrings = new String[MAX_INTERNED_FIELDS];
    } else {
      // We are create a child builder for proto fields, read all cache fields from the parent.
      mParent = parent;
      mNativeMemoryCleaner = parent.mNativeMemoryCleaner;
      readAllCacheFieldsFromParent(parent);
    }

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
    // Java-side path: taken when the flag is on and the event carries only
    // migrated extras (debug args and tracks, both held off mPendingPointers).
    // Any not-yet-migrated extra (flow, counter, proto) lands in mPendingPointers
    // and forces the High Level path; buffered args and the track are then
    // replayed as HL extras so nothing is lost.
    if (sUseJavaEmit && !mArgsSpilled && mPendingPointers.isEmpty()) {
      // Body was reset at newEvent; proto fields are already in it. Append the
      // buffered args / flows / counter, then emit.
      writeArgs();
      writeFlows();
      if (mHasCounter) {
        if (mCounterIsDouble) {
          PerfettoEvent.setCounter(mCounterDouble);
        } else {
          PerfettoEvent.setCounter(mCounterLong);
        }
      }
      if (mHasTrack || mInternedFieldCount > 0) {
        PerfettoEvent.emitWithExtras(
            mTraceType, mCategory.getPtr(), mEventName, mHasTrack, mTrackLeafUuid,
            mTrackCount, mTrackUuids, mTrackParentUuids, mTrackNames,
            mTrackNameStatic, mTrackIsCounter, mInternedFieldCount,
            mInternedFieldIds, mInternedTypeIds, mInternedStrings);
      } else {
        PerfettoEvent.emit(mTraceType, mCategory.getPtr(), mEventName);
      }
      return;
    }
    if (mArgCount > 0) {
      flushArgsToHl();
    }
    if (mFlowCount > 0) {
      flushFlowsToHl();
    }
    if (mHasCounter) {
      flushCounterToHl();
    }
    if (mHasTrack) {
      flushTrackToHl();
    }
    PerfettoTrackEventExtra.native_emit(
        mTraceType, mCategory.getPtr(), mEventName, mExtra.getPtr());
  }

  /**
   * Enables or disables routing extra-free events through the Java-side Low
   * Level emit path. Visible for tests and benchmarks; production code controls
   * this via the {@code perfetto.use_java_emit} system property.
   */
  public static void setUseJavaEmit(boolean enabled) {
    sUseJavaEmit = enabled;
  }

  /** Whether extra-free events are routed through the Java-side emit path. */
  public static boolean getUseJavaEmit() {
    return sUseJavaEmit;
  }

  /** Initialize the builder for a new trace event. */
  private PerfettoTrackEventBuilder initNewEvent(
      int traceType, Category category, boolean isDebug) {
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

    mExtra.reset();
    mChildBuildersCache.reset();
    mObjectsPool.reset();
    mPendingPointers.clear();
    mCurrentContainer = null;
    mArgCount = 0;
    mArgsSpilled = false;
    mHasTrack = false;
    mTrackCount = 0;
    mTrackIsCounter = false;
    mHasCounter = false;
    mFlowCount = 0;
    mInProto = false;
    mProtoDepth = 0;
    mInternedFieldCount = 0;
    if (sUseJavaEmit) {
      // Reset the body here so proto fields written during the event (which go
      // straight into the body) accumulate; args/flows/counter are appended at
      // emit.
      PerfettoEvent.beginBody();
    }

    return this;
  }

  private PerfettoTrackEventBuilder initChildBuilderForProto(
      PerfettoTrackEventBuilder parent, FieldContainer fieldContainer) {
    mIsBuilt = false;
    mParent = parent;
    mIsDebug = parent.mIsDebug;
    updateNativeMemoryCleanerForDebug(mIsDebug);

    readAllCacheFieldsFromParent(parent);

    mCurrentContainer = fieldContainer;
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
    if (stashArg(name, ARG_KIND_LONG)) {
      mArgLongs[mArgCount - 1] = val;
    } else {
      addArgHl(name).setValueInt64(val);
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
    if (stashArg(name, ARG_KIND_BOOL)) {
      mArgLongs[mArgCount - 1] = val ? 1 : 0;
    } else {
      addArgHl(name).setValueBool(val);
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
    if (stashArg(name, ARG_KIND_DOUBLE)) {
      mArgDoubles[mArgCount - 1] = val;
    } else {
      addArgHl(name).setValueDouble(val);
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
    if (stashArg(name, ARG_KIND_STRING)) {
      mArgStrings[mArgCount - 1] = val;
    } else {
      addArgHl(name).setValueString(val);
    }
    return this;
  }

  // Buffers an arg slot for the Java emit path; the caller fills the value in the
  // returned slot (mArgCount-1). Returns false if the event is not on the Java
  // path (flag off, or buffer spilled to HL), in which case the caller uses the
  // HL path via addArgHl(). On overflow the buffered args are spilled to HL.
  private boolean stashArg(String name, int kind) {
    if (!sUseJavaEmit || mArgsSpilled) {
      return false;
    }
    if (mArgCount == MAX_BUFFERED_ARGS) {
      flushArgsToHl();
      mArgsSpilled = true;
      return false;
    }
    mArgNames[mArgCount] = name;
    mArgKinds[mArgCount] = kind;
    mArgCount++;
    return true;
  }

  // Returns a (cached) HL Arg for `name`, registered as an extra. Holds the HL
  // arg-creation logic shared by the direct HL path and the buffer spill/replay.
  private Arg addArgHl(String name) {
    Arg arg = mObjectsCache.mArgCache.get(name.hashCode());
    if (arg == null || !arg.getName().equals(name)) {
      arg = new Arg(name, mNativeMemoryCleaner);
      mObjectsCache.mArgCache.put(name.hashCode(), arg);
    }
    addPerfettoPointerToExtra(arg);
    return arg;
  }

  // Replays the buffered args as HL extras (used when the event also carries a
  // not-yet-migrated extra, so the whole event must take the HL path).
  private void flushArgsToHl() {
    for (int i = 0; i < mArgCount; i++) {
      Arg arg = addArgHl(mArgNames[i]);
      switch (mArgKinds[i]) {
        case ARG_KIND_LONG:
          arg.setValueInt64(mArgLongs[i]);
          break;
        case ARG_KIND_BOOL:
          arg.setValueBool(mArgLongs[i] != 0);
          break;
        case ARG_KIND_DOUBLE:
          arg.setValueDouble(mArgDoubles[i]);
          break;
        case ARG_KIND_STRING:
          arg.setValueString(mArgStrings[i]);
          break;
        default: // unreachable
      }
      mArgNames[i] = null;
      mArgStrings[i] = null;
    }
    mArgCount = 0;
  }

  // Encodes the buffered args into the Java event body (PerfettoEvent).
  private void writeArgs() {
    for (int i = 0; i < mArgCount; i++) {
      String name = mArgNames[i];
      switch (mArgKinds[i]) {
        case ARG_KIND_LONG:
          PerfettoEvent.addArg(name, mArgLongs[i]);
          break;
        case ARG_KIND_BOOL:
          PerfettoEvent.addArg(name, mArgLongs[i] != 0);
          break;
        case ARG_KIND_DOUBLE:
          PerfettoEvent.addArg(name, mArgDoubles[i]);
          break;
        case ARG_KIND_STRING:
          PerfettoEvent.addArg(name, mArgStrings[i]);
          break;
        default: // unreachable
      }
      mArgNames[i] = null;
      mArgStrings[i] = null;
    }
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
    if (stashFlow(id, /* terminating= */ false)) {
      return this;
    }
    addFlowHl(id, /* terminating= */ false);
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
    if (stashFlow(id, /* terminating= */ true)) {
      return this;
    }
    addFlowHl(id, /* terminating= */ true);
    return this;
  }

  // Buffers a flow for the Java emit path (raw id; the process-scoped fold
  // happens at encode time). Returns false (use the HL path) when the flag is
  // off or the buffer is full.
  private boolean stashFlow(long id, boolean terminating) {
    if (!sUseJavaEmit || mFlowCount == MAX_BUFFERED_FLOWS) {
      return false;
    }
    mFlowIds[mFlowCount] = id;
    mFlowTerminating[mFlowCount] = terminating;
    mFlowCount++;
    return true;
  }

  // High Level flow extra (flag off, or HL fallback replay).
  private void addFlowHl(long id, boolean terminating) {
    Flow flow =
        (terminating ? mObjectsPool.mTerminatingFlowPool : mObjectsPool.mFlowPool)
            .get(flowSupplier);
    if (terminating) {
      flow.setProcessTerminatingFlow(id);
    } else {
      flow.setProcessFlow(id);
    }
    addPerfettoPointerToExtra(flow);
  }

  private void writeFlows() {
    for (int i = 0; i < mFlowCount; i++) {
      if (mFlowTerminating[i]) {
        PerfettoEvent.addTerminatingFlow(mFlowIds[i]);
      } else {
        PerfettoEvent.addFlow(mFlowIds[i]);
      }
    }
  }

  private void flushFlowsToHl() {
    for (int i = 0; i < mFlowCount; i++) {
      addFlowHl(mFlowIds[i], mFlowTerminating[i]);
    }
  }

  // Replays the pending track as an HL extra (used when the event also carries a
  // not-yet-migrated extra, forcing the HL path). Same name/id/parent, so HL
  // recomputes the identical uuid.
  private void flushTrackToHl() {
    if (mTrackIsCounter) {
      usingCounterTrackHl(mTrackParentUuids[0], mTrackNames[0], mTrackNameStatic);
      return;
    }
    for (int i = 0; i < mTrackCount; i++) {
      usingNamedTrackHl(mTrackIds[i], mTrackNames[i], mTrackParentUuids[i],
          mTrackNameStatic);
    }
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

  // Routes a named track to the Java emit path (records it for emit), or to the
  // High Level path when the flag is off.
  private PerfettoTrackEventBuilder usingNamedTrack(
          long id, String name, long parentUuid, boolean isNameStatic) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    if (sUseJavaEmit) {
      setTrack(id, name, parentUuid, isNameStatic);
    } else {
      usingNamedTrackHl(id, name, parentUuid, isNameStatic);
    }
    return this;
  }

  // Records a single named track for the Java emit path. The uuid is derived the
  // same way as native (parentUuid ^ fnv1a(name) ^ id), so a track is identical
  // whether emitted from here, the HL path, or C++.
  private void setTrack(long id, String name, long parentUuid, boolean isNameStatic) {
    long uuid = parentUuid ^ fnv1a(name) ^ id;
    mHasTrack = true;
    mTrackIsCounter = false;
    mTrackLeafUuid = uuid;
    mTrackNameStatic = isNameStatic;
    mTrackCount = 1;
    mTrackUuids[0] = uuid;
    mTrackParentUuids[0] = parentUuid;
    mTrackIds[0] = id;
    mTrackNames[0] = name;
  }

  // Records a counter track for the Java emit path. uuid uses the counter magic
  // (kCounterMagic ^ parentUuid ^ fnv1a(name)), matching PerfettoTeCounterTrack
  // Uuid; no id, since counter tracks are keyed by name + parent only.
  private void setCounterTrack(long parentUuid, String name, boolean isNameStatic) {
    long uuid = COUNTER_TRACK_MAGIC ^ parentUuid ^ fnv1a(name);
    mHasTrack = true;
    mTrackIsCounter = true;
    mTrackLeafUuid = uuid;
    mTrackNameStatic = isNameStatic;
    mTrackCount = 1;
    mTrackUuids[0] = uuid;
    mTrackParentUuids[0] = parentUuid;
    mTrackIds[0] = 0;
    mTrackNames[0] = name;
  }

  // High Level named track: builds a NamedTrack extra (flag off, or HL fallback).
  private void usingNamedTrackHl(long id, String name, long parentUuid, boolean isNameStatic) {
    NamedTrack track = mObjectsCache.mNamedTrackCache.get(name.hashCode());
    if (track == null || !track.getName().equals(name) || track.isNameStatic() != isNameStatic) {
      track = new NamedTrack(id, name, parentUuid, isNameStatic, mNativeMemoryCleaner);
      mObjectsCache.mNamedTrackCache.put(name.hashCode(), track);
    }
    addPerfettoPointerToExtra(track);
  }

  // FNV-1a over the name bytes, matching PerfettoFnv1a / PerfettoTeNamedTrackUuid
  // in the C SDK (chars above 0x7F fold to '?', the JNI's ASCII conversion) so
  // Java-derived track uuids match native ones.
  private static long fnv1a(String s) {
    long h = 0xcbf29ce484222325L;
    for (int i = 0; i < s.length(); i++) {
      char c = s.charAt(i);
      h ^= (c <= 0x7F) ? c : '?';
      h *= 0x100000001b3L;
    }
    return h;
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

  // Routes a counter track to the Java emit path, or to HL when the flag is off.
  private PerfettoTrackEventBuilder usingCounterTrack(
      long parentUuid, String name, boolean isNameStatic) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkNotBuildingProto();
    }
    if (sUseJavaEmit) {
      setCounterTrack(parentUuid, name, isNameStatic);
    } else {
      usingCounterTrackHl(parentUuid, name, isNameStatic);
    }
    return this;
  }

  // High Level counter track (flag off, or HL fallback).
  private void usingCounterTrackHl(long parentUuid, String name, boolean isNameStatic) {
    CounterTrack track = mObjectsCache.mCounterTrackCache.get(name.hashCode());
    if (track == null || !track.getName().equals(name) || track.isNameStatic() != isNameStatic) {
      track = new CounterTrack(name, parentUuid, isNameStatic, mNativeMemoryCleaner);
      mObjectsCache.mCounterTrackCache.put(name.hashCode(), track);
    }
    addPerfettoPointerToExtra(track);
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
    if (sUseJavaEmit) {
      mHasCounter = true;
      mCounterIsDouble = false;
      mCounterLong = val;
    } else {
      mLazyInitObjects.getCounter().setValueInt64(val);
      addPerfettoPointerToExtra(mLazyInitObjects.getCounter());
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
    if (sUseJavaEmit) {
      mHasCounter = true;
      mCounterIsDouble = true;
      mCounterDouble = val;
    } else {
      mLazyInitObjects.getCounter().setValueDouble(val);
      addPerfettoPointerToExtra(mLazyInitObjects.getCounter());
    }
    return this;
  }

  // Replays the pending counter value as an HL Counter extra (HL fallback).
  private void flushCounterToHl() {
    Counter counter = mLazyInitObjects.getCounter();
    if (mCounterIsDouble) {
      counter.setValueDouble(mCounterDouble);
    } else {
      counter.setValueInt64(mCounterLong);
    }
    addPerfettoPointerToExtra(counter);
  }

  /** Adds a proto field with field id {@code id} and value {@code val}. */
  public PerfettoTrackEventBuilder addField(long id, long val) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkBuildingProto();
    }
    if (sUseJavaEmit) {
      PerfettoEvent.protoVarInt((int) id, val);
    } else {
      Field field = mObjectsPool.mFieldPool.get(fieldSupplier);
      field.setValueInt64(id, val);
      addFieldToContainer(field);
    }
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
    if (sUseJavaEmit) {
      PerfettoEvent.protoDouble((int) id, val);
    } else {
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
    if (sUseJavaEmit) {
      PerfettoEvent.protoString((int) id, val);
    } else {
      Field field = mObjectsPool.mFieldPool.get(fieldSupplier);
      field.setValueString(id, val);
      addFieldToContainer(field);
    }
    return this;
  }

  /**
   * Adds a proto field with field id {@code id} whose string {@code val} is
   * interned under {@code internedTypeId} (an InternedData field number). On the
   * Java path the field is recorded and interned natively (its iid is per-
   * sequence). {@code internedTypeId} must be non-zero.
   */
  public PerfettoTrackEventBuilder addFieldWithInterning(long id, String val, long internedTypeId) {
    if (!mIsCategoryEnabled) {
      return this;
    }
    if (mIsDebug) {
      checkBuildingProto();
    }
    if (sUseJavaEmit) {
      if (internedTypeId != 0 && mInternedFieldCount < MAX_INTERNED_FIELDS) {
        mInternedFieldIds[mInternedFieldCount] = (int) id;
        mInternedTypeIds[mInternedFieldCount] = (int) internedTypeId;
        mInternedStrings[mInternedFieldCount] = val;
        mInternedFieldCount++;
      }
    } else {
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
    if (sUseJavaEmit) {
      // Proto fields are written straight into the body; no child builder.
      mInProto = true;
      return this;
    }
    Proto proto = mObjectsPool.mProtoPool.get(protoSupplier);
    proto.clearFields();
    addPerfettoPointerToExtra(proto);
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
    if (sUseJavaEmit) {
      mInProto = false;
      return this;
    }
    return mParent;
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
    if (sUseJavaEmit) {
      mProtoTokens[mProtoDepth++] = PerfettoEvent.protoBeginNested((int) id);
      return this;
    }
    FieldNested field = mObjectsPool.mFieldNestedPool.get(fieldNestedSupplier);
    field.setId(id);
    addFieldToContainer(field);
    return mChildBuildersCache
        .get(perfettoTrackEventBuilderSupplier)
        .initChildBuilderForProto(this, field);
  }

  /** Ends a nested proto field. */
  public PerfettoTrackEventBuilder endNested() {
    if (!mIsCategoryEnabled) {
      return this;
    }

    if (mIsDebug) {
      checkMatchingBeginNested();
    }

    if (sUseJavaEmit) {
      PerfettoEvent.protoEndNested(mProtoTokens[--mProtoDepth]);
      return this;
    }

    return mParent;
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
    return mParent == null && mCurrentContainer == null;
  }

  private boolean isBuildingProto() {
    return (mParent != null && mParent.isBuildingTopLevelExtra()) && mCurrentContainer != null;
  }

  private boolean isBuildingNestedProto() {
    return (mParent != null && (mParent.isBuildingProtoOrNestedProto()))
        && mCurrentContainer != null;
  }

  private boolean isBuildingProtoOrNestedProto() {
    return isBuildingProto() || isBuildingNestedProto();
  }

  private void checkNotBuildingProto() {
    checkState();
    if (sUseJavaEmit) {
      if (mInProto) throwNotBuildingProtoError();
      return;
    }
    if (isBuildingProtoOrNestedProto()) throwNotBuildingProtoError();
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwNotBuildingProtoError() {
    throw new IllegalStateException("Operation not supported for proto.");
  }

  private void checkBuildingProto() {
    checkState();
    if (sUseJavaEmit) {
      if (!mInProto) throwBuildingProtoError();
      return;
    }
    if (isBuildingTopLevelExtra()) throwBuildingProtoError();
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwBuildingProtoError() {
    throw new IllegalStateException("Field operations must be within beginProto/endProto block.");
  }

  private void checkMatchingBeginNested() {
    checkState();
    if (sUseJavaEmit) {
      if (mProtoDepth == 0) throwMatchingBeginNestedError();
      return;
    }
    if (!isBuildingNestedProto()) throwMatchingBeginNestedError();
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwMatchingBeginNestedError() {
    throw new IllegalStateException("No matching beginNested call.");
  }

  private void checkMatchingBeginProto() {
    checkState();
    if (sUseJavaEmit) {
      if (!mInProto || mProtoDepth > 0) throwMatchingBeginProtoError();
      return;
    }
    if (!isBuildingProto()) throwMatchingBeginProtoError();
  }

  /** Outlined to keep the caller method small and more likely to be inlined. */
  private static void throwMatchingBeginProtoError() {
    throw new IllegalStateException("No matching beginProto call.");
  }
}
