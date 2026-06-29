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

/** An immutable, reusable handle to a counter track, passed to {@link PerfettoTrace#counter}. */
public final class PerfettoCounterTrack {
  static final int ROOT_GLOBAL = 0;
  static final int ROOT_PROCESS = 1;

  final int mRootType;
  final String mName;
  final boolean mIsNameStatic;

  private PerfettoCounterTrack(int rootType, String name, boolean isNameStatic) {
    mRootType = rootType;
    mName = name;
    mIsNameStatic = isNameStatic;
  }

  /** A counter track named {@code name} under the process track. */
  public static PerfettoCounterTrack process(@CompileTimeConstant String name) {
    return new PerfettoCounterTrack(ROOT_PROCESS, name, true);
  }

  public static PerfettoCounterTrack processWithDynamicName(String name) {
    return new PerfettoCounterTrack(ROOT_PROCESS, name, false);
  }

  /** A counter track named {@code name} at the global scope. */
  public static PerfettoCounterTrack global(@CompileTimeConstant String name) {
    return new PerfettoCounterTrack(ROOT_GLOBAL, name, true);
  }

  public static PerfettoCounterTrack globalWithDynamicName(String name) {
    return new PerfettoCounterTrack(ROOT_GLOBAL, name, false);
  }

  long parentUuid() {
    return mRootType == ROOT_PROCESS
        ? PerfettoTrace.getProcessTrackUuid()
        : PerfettoTrace.getGlobalTrackUuid();
  }
}
