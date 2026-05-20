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

import java.util.HashMap;

/**
 * String interning pool for Perfetto trace packets.
 *
 * <p>Maps strings to integer ids ("iids"), one id space per interning type.
 * When a string recurs across packets on a sequence, its full text is emitted
 * once inside an {@code InternedData} message and later uses reference it by
 * iid, keeping the trace small on the wire.
 *
 * <p>This mirrors, on the Java side, the per-sequence interning that the C SDK
 * keeps in its incremental state. The pool must be {@link #reset} whenever the
 * native incremental state is cleared (e.g. after a flush), after which every
 * string is re-emitted on next use.
 *
 * <p>Thread safety: not thread-safe. Each thread uses its own pool.
 *
 * @hide
 */
public final class InternPool {
  /**
   * Outcome of an {@link #intern} call: the assigned iid and whether the entry
   * is new and therefore needs an {@code InternedData} record emitted.
   *
   * <p>The instance is reused across calls to avoid per-call allocation; callers
   * must read the fields immediately and not retain the object.
   */
  public static final class InternResult {
    public int iid;
    public boolean isNew;

    void set(int iid, boolean isNew) {
      this.iid = iid;
      this.isNew = isNew;
    }
  }

  // One table per interning type, indexed by caller-defined type constants
  // (event names, debug-annotation names, ...). Each table maps string -> iid.
  private static final int MAX_INTERN_TYPES = 16;

  @SuppressWarnings("unchecked")
  private final HashMap<String, Integer>[] mTables = new HashMap[MAX_INTERN_TYPES];

  private final int[] mNextIid = new int[MAX_INTERN_TYPES];

  // Reused to avoid allocating on the hot path.
  private final InternResult mResult = new InternResult();

  private int mGeneration;

  public InternPool() {
    for (int i = 0; i < MAX_INTERN_TYPES; i++) {
      mNextIid[i] = 1; // iid 0 is reserved.
    }
  }

  /**
   * Interns {@code value} under {@code type}.
   *
   * @param type interning type index in {@code [0, MAX_INTERN_TYPES)}.
   * @param value the string to intern.
   * @return the reused {@link InternResult}; copy out the fields if you need to
   *     keep them. {@code isNew} is true when the caller must emit an
   *     {@code InternedData} entry for {@code value}.
   */
  public InternResult intern(int type, String value) {
    HashMap<String, Integer> table = mTables[type];
    if (table == null) {
      table = new HashMap<>();
      mTables[type] = table;
    }

    Integer existing = table.get(value);
    if (existing != null) {
      mResult.set(existing, false);
    } else {
      int iid = mNextIid[type]++;
      table.put(value, iid);
      mResult.set(iid, true);
    }
    return mResult;
  }

  /**
   * Clears every interning table. Call this when the native incremental state is
   * cleared; all strings are re-emitted on next use.
   */
  public void reset() {
    for (int i = 0; i < MAX_INTERN_TYPES; i++) {
      if (mTables[i] != null) {
        mTables[i].clear();
      }
      mNextIid[i] = 1;
    }
    mGeneration++;
  }

  /** Generation counter; bumped on every {@link #reset}. */
  public int generation() {
    return mGeneration;
  }
}
