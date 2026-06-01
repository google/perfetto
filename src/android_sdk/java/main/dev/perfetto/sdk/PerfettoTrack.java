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

import dev.perfetto.sdk.PerfettoTrackEventExtra.NestedTracks;

/**
 * An immutable, reusable handle to a (possibly nested) named track.
 *
 * <p>Build a track once — typically a {@code static final} — and pass it to
 * {@link PerfettoTrackEventBuilder#usingTrack}. A track is rooted at the process,
 * the current thread, or the global scope, and can be nested arbitrarily deep with
 * {@link #child}:
 *
 * <pre>
 *   static final PerfettoTrack RENDER = PerfettoTrack.process("Render");
 *   static final PerfettoTrack GPU = RENDER.child("GPU");
 *   ...
 *   PerfettoTrace.instant(CAT, "frame").usingTrack(GPU).emit();
 * </pre>
 *
 * <p>Emitting on {@code GPU} emits the {@code TrackDescriptor}s for the whole
 * chain (Render under the process, GPU under Render) once per sequence, matching
 * the C SDK's nested-track behaviour. The track uuid is derived natively exactly
 * as the C SDK derives it.
 *
 * <p>This is the nesting-only shape supported by the high-level ABI; sibling
 * ordering, counter units and similar are intentionally not exposed here.
 */
public final class PerfettoTrack {
  // Root scope of the chain. Mirrors RootType in tracing_sdk.h.
  static final int ROOT_GLOBAL = 0;
  static final int ROOT_PROCESS = 1;
  static final int ROOT_THREAD = 2;

  // Default per-level id: no disambiguation between same-named sibling tracks.
  private static final long DEFAULT_ID = 0;

  final int mRootType;
  // Names and ids of the chain, outermost (closest to the root) first.
  final String[] mNames;
  final long[] mIds;

  // The handle's native nested-tracks extra, built lazily on first use and held
  // for the handle's lifetime. Freed by the cleaner when the handle is collected.
  private volatile NestedTracks mNested;

  // Frees a handle's native track when the handle is collected. SystemCleaner
  // needs no native lib, so it is safe to hold statically.
  private static final PerfettoNativeMemoryCleaner sCleaner =
      new PerfettoNativeMemoryCleaner();

  private PerfettoTrack(int rootType, String[] names, long[] ids) {
    mRootType = rootType;
    mNames = names;
    mIds = ids;
  }

  /**
   * The handle's native nested-tracks extra, built once and reused.
   * Package-private; used by {@link PerfettoTrackEventBuilder#usingTrack}.
   *
   * <p>Lock-free lazy init: concurrent first callers may each build an equivalent
   * {@code NestedTracks}, but they are read-only and content-identical (same uuid
   * and descriptor, deduped per-sequence natively), so only one need survive. The
   * {@code volatile} field publishes the winner; the rest are freed by the cleaner.
   */
  NestedTracks nestedTracks() {
    NestedTracks n = mNested;
    if (n == null) {
      n = new NestedTracks(this, sCleaner);
      mNested = n;
    }
    return n;
  }

  /** A track named {@code name} rooted at the process track. */
  public static PerfettoTrack process(@CompileTimeConstant String name) {
    return new PerfettoTrack(ROOT_PROCESS, new String[] {name}, new long[] {DEFAULT_ID});
  }

  /** A track named {@code name} rooted at the calling thread's track. */
  public static PerfettoTrack thread(@CompileTimeConstant String name) {
    return new PerfettoTrack(ROOT_THREAD, new String[] {name}, new long[] {DEFAULT_ID});
  }

  /** A track named {@code name} rooted at the global scope. */
  public static PerfettoTrack global(@CompileTimeConstant String name) {
    return new PerfettoTrack(ROOT_GLOBAL, new String[] {name}, new long[] {DEFAULT_ID});
  }

  /** A child track named {@code name} nested under this one. */
  public PerfettoTrack child(@CompileTimeConstant String name) {
    return child(DEFAULT_ID, name);
  }

  /**
   * A child track named {@code name} nested under this one. {@code id} further
   * disambiguates the track from same-named siblings.
   */
  public PerfettoTrack child(long id, @CompileTimeConstant String name) {
    int n = mNames.length;
    String[] names = new String[n + 1];
    long[] ids = new long[n + 1];
    System.arraycopy(mNames, 0, names, 0, n);
    System.arraycopy(mIds, 0, ids, 0, n);
    names[n] = name;
    ids[n] = id;
    return new PerfettoTrack(mRootType, names, ids);
  }
}
