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

/**
 * Per-thread tracing context for a data source.
 *
 * Manages a ProtoWriter and InternPool, and handles writing encoded trace
 * packets to all active tracing session instances via a single JNI call.
 *
 * Cached per-thread via ThreadLocal in PerfettoDataSource. No allocation
 * after first use on each thread.
 *
 * Builder API:
 *   ctx.newPacket()
 *       .writeVarInt(TIMESTAMP_FIELD, timestamp)
 *       .beginNested(MY_PAYLOAD_FIELD)
 *           .writeString(NAME_FIELD, name)
 *       .endNested()
 *       .commit();
 *
 * Low-level ProtoWriter API:
 *   ProtoWriter w = ctx.getWriter();
 *   w.writeVarInt(TIMESTAMP_FIELD, timestamp);
 *   ctx.commitPacket();
 *
 * Thread safety: not thread-safe. Each thread has its own instance.
 */
public final class TraceContext {
    private final ProtoWriter mWriter;
    private final InternPool mInternPool;
    private final PacketBuilder mPacketBuilder;
    private long mDsPtr;

    // Set true on begin(). Cleared after the incremental state check.
    // Ensures we check exactly once per trace() call, and only when
    // the caller actually uses interning.
    private boolean mNeedsIncrStateCheck;

    TraceContext() {
        mWriter = new ProtoWriter();
        mInternPool = new InternPool();
        mPacketBuilder = new PacketBuilder(this);
    }

    /**
     * Begin a trace operation. Called by PerfettoDataSource.trace().
     * Resets the writer and marks incremental state for checking.
     */
    void begin(long dsPtr) {
        mDsPtr = dsPtr;
        mWriter.reset();
        mNeedsIncrStateCheck = true;
    }

    /** Get the ProtoWriter for encoding TracePacket fields. */
    public ProtoWriter getWriter() {
        return mWriter;
    }

    /**
     * Get the interning pool for this thread.
     *
     * The InternPool is valid only after calling
     * {@link #resetIncrementalStateIfNeeded()}. Calling intern() without
     * checking incremental state first may produce broken traces when
     * multiple tracing sessions are active.
     */
    public InternPool getInternPool() {
        return mInternPool;
    }

    /**
     * Check and reset incremental state if any session requires it.
     *
     * Must be called once per trace() before using the InternPool. This
     * is a single JNI call that checks all active session instances.
     *
     * When this returns true:
     * 1. The InternPool has been reset (all entries cleared).
     * 2. The caller must write sequence_flags with
     *    SEQ_INCREMENTAL_STATE_CLEARED in the packet.
     * 3. All subsequent intern() calls return isNew=true until the pool
     *    is warm again.
     *
     * When this returns false, the InternPool is still valid from the
     * previous trace point and no special action is needed.
     *
     * This method is idempotent within a single trace() call -- the
     * second call always returns false.
     *
     * If you don't use interning, don't call this. No JNI overhead.
     */
    public boolean resetIncrementalStateIfNeeded() {
        if (!mNeedsIncrStateCheck) {
            return false;
        }
        mNeedsIncrStateCheck = false;

        boolean cleared =
                PerfettoDataSource.nativeCheckAnyIncrementalStateCleared(mDsPtr);
        if (cleared) {
            mInternPool.reset();
        }
        return cleared;
    }

    // ====================================================================
    // Packet builder: fluent API for constructing proto messages.
    // Zero allocations -- the PacketBuilder is cached per-thread.
    // ====================================================================

    /**
     * Start building a new TracePacket with a fluent API.
     *
     * Example:
     *   ctx.newPacket()
     *       .writeVarInt(TIMESTAMP_FIELD, timestamp)
     *       .beginNested(MY_PAYLOAD_FIELD)
     *           .writeString(NAME_FIELD, name)
     *           .writeVarInt(VALUE_FIELD, value)
     *       .endNested()
     *       .commit();
     */
    public PacketBuilder newPacket() {
        return mPacketBuilder.start(mWriter);
    }

    // ====================================================================
    // Low-level API: direct ProtoWriter access for maximum control.
    // ====================================================================

    /**
     * Write the encoded packet to all active tracing session instances
     * and reset the writer for the next packet.
     *
     * Single JNI call. The C stream writer handles chunk boundaries
     * and size field patching internally.
     */
    public void commitPacket() {
        int len = mWriter.position();
        if (len > 0) {
            PerfettoDataSource.nativeWritePacketToAllInstances(
                    mDsPtr, mWriter.buffer(), len);
        }
        mWriter.reset();
    }
}
