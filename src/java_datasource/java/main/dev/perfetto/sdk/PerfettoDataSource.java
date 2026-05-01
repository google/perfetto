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

import dalvik.annotation.optimization.CriticalNative;
import dalvik.annotation.optimization.FastNative;

/**
 * High-performance custom data source for Perfetto.
 *
 * Zero heap allocations on the trace hot path. Proto encoding is done
 * entirely in Java (ProtoWriter) to a pre-allocated thread-local buffer.
 * A single JNI call writes the encoded packet to all active tracing
 * session instances' shared memory.
 *
 * Usage:
 *   // Define and register (once at startup):
 *   class MyDataSource extends PerfettoDataSource {
 *     static final MyDataSource INSTANCE = new MyDataSource();
 *     static { INSTANCE.register("my.data_source"); }
 *   }
 *
 *   // Builder API (zero allocs, fluent):
 *   TraceContext ctx = MyDataSource.INSTANCE.trace();
 *   if (ctx == null) return;
 *   ctx.newPacket()
 *       .writeVarInt(TIMESTAMP_FIELD, timestamp)
 *       .beginNested(MY_PAYLOAD_FIELD)
 *           .writeString(NAME_FIELD, name)
 *           .writeVarInt(VALUE_FIELD, value)
 *       .endNested()
 *       .commit();
 *
 *   // Low-level ProtoWriter API (maximum control):
 *   TraceContext ctx = MyDataSource.INSTANCE.trace();
 *   if (ctx == null) return;
 *   ProtoWriter w = ctx.getWriter();
 *   w.writeVarInt(TIMESTAMP_FIELD, timestamp);
 *   int payload = w.beginNested(MY_PAYLOAD_FIELD);
 *   w.writeString(NAME_FIELD, name);
 *   w.endNested(payload);
 *   ctx.commitPacket();
 *
 * Thread safety: this class is thread-safe. trace() returns per-thread
 * TraceContext instances.
 */
public abstract class PerfettoDataSource {
    long mNativeDsPtr;

    // Volatile: single read in trace() is the fast-path disabled check.
    // Set true in OnStart (any instance), false in OnStop (when no
    // instances remain, checked via native enabled_ptr).
    volatile boolean mEnabled;

    // Per-thread TraceContext. Allocated once, then reused forever.
    final ThreadLocal<TraceContext> mTlsContext = new ThreadLocal<>();

    /**
     * Register this data source with the tracing service.
     *
     * @param name Data source name matching the trace config.
     */
    public final void register(String name) {
        mNativeDsPtr = nativeRegister(this, name);
    }

    /**
     * Begin a trace operation. Returns a TraceContext if tracing is enabled,
     * null otherwise.
     *
     * Cost when disabled: 1 volatile read (~1ns).
     * Cost when enabled: volatile read + ThreadLocal.get() (~4ns).
     *
     * Call ctx.commitPacket() when done writing.
     */
    public final TraceContext trace() {
        if (!mEnabled) {
            return null;
        }
        TraceContext ctx = mTlsContext.get();
        if (ctx == null) {
            ctx = new TraceContext();
            mTlsContext.set(ctx);
        }
        ctx.begin(mNativeDsPtr);
        return ctx;
    }

    // ====================================================================
    // Lifecycle callbacks (override in subclass if needed)
    // ====================================================================

    protected void onSetup(int instanceIndex, byte[] config) {}
    protected void onStart(int instanceIndex) {}
    protected void onStop(int instanceIndex) {}
    protected void onFlush(int instanceIndex) {}

    // Called from JNI
    @SuppressWarnings("unused")
    void onEnabledChanged(boolean enabled) {
        mEnabled = enabled;
    }

    // ====================================================================
    // JNI
    // ====================================================================

    @FastNative
    static native long nativeRegister(PerfettoDataSource ds, String name);

    @CriticalNative
    static native boolean nativeCheckAnyIncrementalStateCleared(long dsPtr);

    @FastNative
    static native void nativeWritePacketToAllInstances(
            long dsPtr, byte[] buf, int len);

    @CriticalNative
    static native void nativeFlush(long dsPtr);
}
