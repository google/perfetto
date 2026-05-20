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

import java.nio.ByteBuffer;

/**
 * A custom Perfetto data source defined in Java.
 *
 * <p>Subclass this, register it under a name that matches the trace config, and
 * emit arbitrary {@code TracePacket} protos on the hot path. Encoding is done in
 * Java with {@link ProtoWriter} into a reused per-thread buffer; a single native
 * call copies the packet (via an off-heap {@link EmitBuffer}) to every active
 * tracing session instance. The hot path makes no Java-heap allocation, and when
 * the data source is disabled {@link #trace} is a single volatile read.
 *
 * <pre>{@code
 * final class MyDataSource extends PerfettoDataSource {
 *   static final MyDataSource INSTANCE = new MyDataSource();
 *   static { INSTANCE.register("com.example.my_data_source"); }
 * }
 *
 * PerfettoDataSource.TraceContext ctx = MyDataSource.INSTANCE.trace();
 * if (ctx != null) {
 *   ProtoWriter w = ctx.newPacket();
 *   w.writeVarInt(TRACE_PACKET_TIMESTAMP, ts);
 *   int p = w.beginNested(TRACE_PACKET_MY_EVENT);
 *   w.writeString(MY_EVENT_NAME, name);
 *   w.endNested(p);
 *   ctx.commit();
 * }
 * }</pre>
 *
 * <p>Per-sequence string interning is available via {@link TraceContext#internPool()};
 * it is reset automatically whenever an instance's incremental state is cleared.
 *
 * <p>Thread-safe: {@link #trace} returns a per-thread {@link TraceContext}.
 */
public abstract class PerfettoDataSource {
  private long mNativeDsPtr;

  // Single read in trace() is the fast-path disabled check. Set true on the
  // first instance's onStart, false when the last instance stops.
  private volatile boolean mEnabled;

  private final ThreadLocal<TraceContext> mTls =
      ThreadLocal.withInitial(TraceContext::new);

  /** Registers this data source under {@code name} (must match the trace config). */
  public final void register(String name) {
    mNativeDsPtr = nativeRegister(this, name);
  }

  /**
   * Returns a {@link TraceContext} to emit a packet on, or {@code null} if the
   * data source is not enabled. When non-null, encode into {@link
   * TraceContext#writer()} and call {@link TraceContext#commit()}.
   */
  public final TraceContext trace() {
    if (!mEnabled) {
      return null;
    }
    TraceContext ctx = mTls.get();
    ctx.begin(mNativeDsPtr);
    return ctx;
  }

  /** Called once per instance when its config is available. Override if needed. */
  protected void onSetup(int instanceIndex, byte[] config) {}

  /** Called when an instance starts tracing. Override if needed. */
  protected void onStart(int instanceIndex) {}

  /** Called when an instance stops tracing. Override if needed. */
  protected void onStop(int instanceIndex) {}

  /** Called when an instance is flushed. Override if needed. */
  protected void onFlush(int instanceIndex) {}

  // Called from native when the enabled state changes.
  @SuppressWarnings("unused")
  private void onEnabledChanged(boolean enabled) {
    mEnabled = enabled;
  }

  /**
   * Per-thread context for emitting one packet at a time. Reused across packets;
   * obtain it from {@link #trace()}.
   */
  public static final class TraceContext {
    private final ProtoWriter mWriter = new ProtoWriter();
    private final EmitBuffer mXfer = new EmitBuffer();
    private final InternPool mInternPool = new InternPool();
    private long mDsPtr;

    private TraceContext() {}

    void begin(long dsPtr) {
      mDsPtr = dsPtr;
      // If any instance reset its incremental state, our interned ids are stale.
      if (nativeCheckAnyIncrementalStateCleared(dsPtr)) {
        mInternPool.reset();
      }
    }

    /**
     * Starts a new {@code TracePacket}, returning the writer to encode it into.
     * Call {@link #commit} when done. May be called repeatedly to emit several
     * packets from one {@link #trace} call.
     */
    public ProtoWriter newPacket() {
      mWriter.reset();
      return mWriter;
    }

    /** Per-sequence string interning, reset when incremental state clears. */
    public InternPool internPool() {
      return mInternPool;
    }

    /** Writes the packet encoded since {@link #newPacket} to all active instances. */
    public void commit() {
      int len = mWriter.position();
      if (len == 0) {
        return;
      }
      mXfer.ensureCapacity(len);
      ByteBuffer b = mXfer.buf;
      b.clear();
      b.put(mWriter.buffer(), 0, len);
      nativeWritePacket(mDsPtr, mXfer.addr, len);
    }

    /** Requests a flush of buffered data on all active instances. */
    public void flush() {
      nativeFlush(mDsPtr);
    }
  }

  // Only nativeRegister needs JNIEnv (global ref + method ids). The rest take
  // only primitives and touch no JVM state, so they are @CriticalNative on ART.
  @FastNative
  private static native long nativeRegister(PerfettoDataSource ds, String name);

  @CriticalNative
  static native boolean nativeCheckAnyIncrementalStateCleared(long dsPtr);

  @CriticalNative
  static native void nativeWritePacket(long dsPtr, long addr, int len);

  @CriticalNative
  static native void nativeFlush(long dsPtr);
}
