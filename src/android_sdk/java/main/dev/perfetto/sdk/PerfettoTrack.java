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

import com.google.errorprone.annotations.CompileTimeConstant;

/**
 * An immutable track an event can be emitted on, optionally nested under a
 * parent track to form an arbitrary hierarchy.
 *
 * <p>Build a track once and reuse it; the uuid (and the whole parent chain) is
 * precomputed at construction, so attaching an event to it via {@link
 * PerfettoTrackEventBuilder#usingTrack} is allocation-free. The uuid is derived
 * exactly as the C SDK derives it ({@code parentUuid ^ fnv1a(name) ^ id} for a
 * named track, {@code kCounterMagic ^ parentUuid ^ fnv1a(name)} for a counter),
 * so a track is identical whether it originates here or in C++.
 *
 * <p>Example:
 *
 * <pre>{@code
 * static final PerfettoTrack RENDER = PerfettoTrack.process("Render");
 * static final PerfettoTrack GPU = RENDER.child("GPU");
 * ...
 * PerfettoTrace.instant(cat, "frame").usingTrack(GPU).emit();
 * }</pre>
 *
 * Names are compile-time constants ({@code static_name} in the descriptor); for
 * one-off dynamic names use the {@code usingNamedTrackWithDynamicName} builder
 * methods. Counter tracks are leaves (set values with {@code setCounter}).
 */
public final class PerfettoTrack {
  // Counter track uuid magic, matching kCounterMagic in the C SDK.
  private static final long COUNTER_TRACK_MAGIC = 0xb1a4a67d7970839eL;

  /**
   * How a track's children are ordered in the UI. Values match
   * {@code TrackDescriptor.ChildTracksOrdering}.
   */
  public enum ChildOrdering {
    LEXICOGRAPHIC(1),
    CHRONOLOGICAL(2),
    EXPLICIT(3);

    final int value;

    ChildOrdering(int value) {
      this.value = value;
    }
  }

  /** Display unit for a counter track. Values match {@code CounterDescriptor.Unit}. */
  public enum CounterUnit {
    TIME_NS(1),
    COUNT(2),
    SIZE_BYTES(3);

    final int value;

    CounterUnit(int value) {
      this.value = value;
    }
  }

  // Counter-track display metadata (unit / unit name / multiplier / incremental),
  // written into the leaf CounterDescriptor. null when unset.
  static final class CounterConfig {
    static final CounterConfig EMPTY = new CounterConfig(0, null, 0, false);

    final int unit; // 0 = unset, else CounterUnit.value
    final String unitName;
    final long unitMultiplier; // 0 = unset
    final boolean isIncremental;

    CounterConfig(int unit, String unitName, long unitMultiplier, boolean isIncremental) {
      this.unit = unit;
      this.unitName = unitName;
      this.unitMultiplier = unitMultiplier;
      this.isIncremental = isIncremental;
    }
  }

  final long mUuid;
  final long mParentUuid;
  final String mName;
  final boolean mIsCounter;
  final PerfettoTrack mParent; // null when rooted at a non-PerfettoTrack uuid
  final int mDepth; // number of levels whose descriptor this track owns
  // How this track's children are ordered (0 = unset); and this track's own rank
  // among its siblings when the parent orders children EXPLICIT.
  final int mChildOrdering;
  final int mSiblingOrderRank;
  final CounterConfig mCounterConfig; // null unless set via withUnit/... on a counter

  private PerfettoTrack(
      long uuid,
      long parentUuid,
      String name,
      boolean isCounter,
      PerfettoTrack parent,
      int childOrdering,
      int siblingOrderRank,
      CounterConfig counterConfig) {
    mUuid = uuid;
    mParentUuid = parentUuid;
    mName = name;
    mIsCounter = isCounter;
    mParent = parent;
    mDepth = (parent == null) ? 1 : parent.mDepth + 1;
    mChildOrdering = childOrdering;
    mSiblingOrderRank = siblingOrderRank;
    mCounterConfig = counterConfig;
  }

  /** Returns a copy of this track that orders its children by {@code ordering}. */
  public PerfettoTrack withChildOrdering(ChildOrdering ordering) {
    return new PerfettoTrack(
        mUuid, mParentUuid, mName, mIsCounter, mParent, ordering.value, mSiblingOrderRank,
        mCounterConfig);
  }

  /**
   * Returns a copy of this track with sibling rank {@code rank}, used to position
   * it among its siblings when the parent orders children
   * {@link ChildOrdering#EXPLICIT}.
   */
  public PerfettoTrack withSiblingOrderRank(int rank) {
    return new PerfettoTrack(
        mUuid, mParentUuid, mName, mIsCounter, mParent, mChildOrdering, rank, mCounterConfig);
  }

  /** Returns a copy of this counter track displayed with {@code unit}. */
  public PerfettoTrack withUnit(CounterUnit unit) {
    CounterConfig c = mCounterConfig != null ? mCounterConfig : CounterConfig.EMPTY;
    return withCounterConfig(
        new CounterConfig(unit.value, c.unitName, c.unitMultiplier, c.isIncremental));
  }

  /** Returns a copy of this counter track displayed with custom unit {@code unitName}. */
  public PerfettoTrack withUnitName(@CompileTimeConstant String unitName) {
    CounterConfig c = mCounterConfig != null ? mCounterConfig : CounterConfig.EMPTY;
    return withCounterConfig(
        new CounterConfig(c.unit, unitName, c.unitMultiplier, c.isIncremental));
  }

  /** Returns a copy of this counter track whose raw values are scaled by {@code multiplier}. */
  public PerfettoTrack withUnitMultiplier(long multiplier) {
    CounterConfig c = mCounterConfig != null ? mCounterConfig : CounterConfig.EMPTY;
    return withCounterConfig(
        new CounterConfig(c.unit, c.unitName, multiplier, c.isIncremental));
  }

  /** Returns a copy of this counter track whose values are deltas to accumulate. */
  public PerfettoTrack withIsIncremental(boolean isIncremental) {
    CounterConfig c = mCounterConfig != null ? mCounterConfig : CounterConfig.EMPTY;
    return withCounterConfig(
        new CounterConfig(c.unit, c.unitName, c.unitMultiplier, isIncremental));
  }

  private PerfettoTrack withCounterConfig(CounterConfig counterConfig) {
    return new PerfettoTrack(
        mUuid, mParentUuid, mName, mIsCounter, mParent, mChildOrdering, mSiblingOrderRank,
        counterConfig);
  }

  /** A named track scoped to the current process. */
  public static PerfettoTrack process(@CompileTimeConstant String name) {
    return root(PerfettoTrace.getProcessTrackUuid(), 0, name, /* isCounter= */ false);
  }

  /** A named track scoped to thread {@code tid}. */
  public static PerfettoTrack thread(long tid, @CompileTimeConstant String name) {
    return root(PerfettoTrace.getThreadTrackUuid(tid), 0, name, /* isCounter= */ false);
  }

  /** A named track at the global (root) scope, shared across processes. */
  public static PerfettoTrack global(@CompileTimeConstant String name) {
    return root(PerfettoTrace.getGlobalTrackUuid(), 0, name, /* isCounter= */ false);
  }

  /**
   * A named track rooted at an arbitrary existing track {@code parentUuid} (whose
   * descriptor is assumed to be emitted elsewhere, e.g. a process/thread track).
   */
  public static PerfettoTrack named(@CompileTimeConstant String name, long parentUuid) {
    return root(parentUuid, 0, name, /* isCounter= */ false);
  }

  /** A counter track scoped to the current process. */
  public static PerfettoTrack processCounter(@CompileTimeConstant String name) {
    return root(PerfettoTrace.getProcessTrackUuid(), 0, name, /* isCounter= */ true);
  }

  /** A counter track scoped to thread {@code tid}. */
  public static PerfettoTrack threadCounter(long tid, @CompileTimeConstant String name) {
    return root(PerfettoTrace.getThreadTrackUuid(tid), 0, name, /* isCounter= */ true);
  }

  /** A counter track at the global (root) scope, shared across processes. */
  public static PerfettoTrack globalCounter(@CompileTimeConstant String name) {
    return root(PerfettoTrace.getGlobalTrackUuid(), 0, name, /* isCounter= */ true);
  }

  /** A counter track rooted at an arbitrary existing track {@code parentUuid}. */
  public static PerfettoTrack counter(@CompileTimeConstant String name, long parentUuid) {
    return root(parentUuid, 0, name, /* isCounter= */ true);
  }

  /** A named child track nested under this one. */
  public PerfettoTrack child(@CompileTimeConstant String name) {
    return child(0, name);
  }

  /** A named child track nested under this one, with {@code id} disambiguating siblings. */
  public PerfettoTrack child(long id, @CompileTimeConstant String name) {
    return new PerfettoTrack(
        namedUuid(mUuid, id, name), mUuid, name, /* isCounter= */ false, this, 0, 0, null);
  }

  /** A counter child track nested under this one. */
  public PerfettoTrack counterChild(@CompileTimeConstant String name) {
    return new PerfettoTrack(
        counterUuid(mUuid, name), mUuid, name, /* isCounter= */ true, this, 0, 0, null);
  }

  /** The track uuid (e.g. to root another track or reference it elsewhere). */
  public long getUuid() {
    return mUuid;
  }

  private static PerfettoTrack root(long parentUuid, long id, String name, boolean isCounter) {
    long uuid = isCounter ? counterUuid(parentUuid, name) : namedUuid(parentUuid, id, name);
    return new PerfettoTrack(uuid, parentUuid, name, isCounter, /* parent= */ null, 0, 0, null);
  }

  static long namedUuid(long parentUuid, long id, String name) {
    return parentUuid ^ fnv1a(name) ^ id;
  }

  static long counterUuid(long parentUuid, String name) {
    return COUNTER_TRACK_MAGIC ^ parentUuid ^ fnv1a(name);
  }

  // FNV-1a over the name bytes, matching PerfettoFnv1a / PerfettoTeNamedTrackUuid
  // in the C SDK (chars above 0x7F fold to '?', the same ASCII fold the emit
  // frame uses) so Java-derived track uuids match native ones.
  static long fnv1a(String s) {
    long h = 0xcbf29ce484222325L;
    for (int i = 0; i < s.length(); i++) {
      char c = s.charAt(i);
      h ^= (c <= 0x7F) ? c : '?';
      h *= 0x100000001b3L;
    }
    return h;
  }
}
