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
 * Maps strings to integer IDs (interned IDs or "iids") for each interning
 * type. When the same string is used repeatedly across trace packets,
 * the full string is emitted once in an InternedData message and subsequent
 * uses reference it by iid.
 *
 * The pool is reset when the incremental state is cleared (e.g., after a
 * flush). After reset, all strings must be re-emitted.
 *
 * Thread safety: not thread-safe. Each thread uses its own pool via
 * TraceContext.
 */
public final class InternPool {
    /**
     * Result of an intern operation. Contains the iid and whether this is a
     * new entry that needs to be emitted in InternedData.
     *
     * Fields are public for zero-overhead access on the hot path. This object
     * is reused across calls -- do not hold a reference to it.
     */
    public static final class InternResult {
        public int iid;
        public boolean isNew;

        void set(int iid, boolean isNew) {
            this.iid = iid;
            this.isNew = isNew;
        }
    }

    // One map per interning type. Indexed by user-defined type constants.
    // Each map: string -> iid.
    private static final int MAX_INTERN_TYPES = 16;
    @SuppressWarnings("unchecked")
    private final HashMap<String, Integer>[] mTables = new HashMap[MAX_INTERN_TYPES];
    private final int[] mNextIid = new int[MAX_INTERN_TYPES];

    // Reusable result object to avoid allocation on every intern() call.
    private final InternResult mResult = new InternResult();

    private int mGeneration;

    public InternPool() {
        for (int i = 0; i < MAX_INTERN_TYPES; i++) {
            mNextIid[i] = 1; // iid 0 is reserved
        }
    }

    /**
     * Intern a string for the given type.
     *
     * Returns an InternResult (reused object, not allocated) with the iid
     * and whether this is a new entry. If isNew is true, the caller must
     * emit an InternedData entry for this string.
     *
     * @param type Interning type index (0 to MAX_INTERN_TYPES-1).
     *             Use constants for event_names, debug_annotation_names, etc.
     * @param value The string to intern.
     * @return Reused InternResult. Copy the values if you need to keep them.
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
     * Reset all interning tables. Called when incremental state is cleared.
     * After reset, all strings will be re-emitted on next use.
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

    /** Current generation. Changes on every reset(). */
    public int generation() {
        return mGeneration;
    }
}
