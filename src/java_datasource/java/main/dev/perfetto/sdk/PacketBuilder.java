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
 * Fluent builder for constructing TracePacket proto messages.
 *
 * Zero allocations. Cached per-thread in TraceContext and reused across
 * trace points. All methods write directly to the underlying ProtoWriter.
 *
 * This is a thin wrapper over ProtoWriter that provides:
 * - Fluent chaining (all write methods return {@code this})
 * - Managed nesting (LIFO endNested with auto-close on commit)
 * - One-call commit to all active tracing sessions
 *
 * Usage:
 *   TraceContext ctx = dataSource.trace();
 *   if (ctx == null) return;
 *   ctx.newPacket()
 *       .writeVarInt(TIMESTAMP_FIELD, timestamp)
 *       .beginNested(MY_PAYLOAD_FIELD)
 *           .writeString(NAME_FIELD, name)
 *           .writeVarInt(VALUE_FIELD, value)
 *       .endNested()
 *       .commit();
 *
 * Thread safety: not thread-safe. Each thread has its own instance.
 */
public final class PacketBuilder {
    private static final int MAX_NESTING_DEPTH = 16;

    private final TraceContext mCtx;
    private ProtoWriter mWriter;
    private final int[] mTokenStack = new int[MAX_NESTING_DEPTH];
    private int mTokenDepth;

    PacketBuilder(TraceContext ctx) {
        mCtx = ctx;
    }

    /** Start building a new packet. Called by TraceContext.newPacket(). */
    PacketBuilder start(ProtoWriter writer) {
        mWriter = writer;
        mTokenDepth = 0;
        return this;
    }

    // ====================================================================
    // Field writers (mirror ProtoWriter but return this for chaining)
    // ====================================================================

    /** Write a varint field (uint32/uint64/int32/int64/bool/enum). */
    public PacketBuilder writeVarInt(int fieldId, long value) {
        mWriter.writeVarInt(fieldId, value);
        return this;
    }

    /** Write a sint32/sint64 field (zigzag encoded). */
    public PacketBuilder writeSInt(int fieldId, long value) {
        mWriter.writeSInt(fieldId, value);
        return this;
    }

    /** Write a bool field. */
    public PacketBuilder writeBool(int fieldId, boolean value) {
        mWriter.writeBool(fieldId, value);
        return this;
    }

    /** Write a fixed64/sfixed64 field. */
    public PacketBuilder writeFixed64(int fieldId, long value) {
        mWriter.writeFixed64(fieldId, value);
        return this;
    }

    /** Write a fixed32/sfixed32 field. */
    public PacketBuilder writeFixed32(int fieldId, int value) {
        mWriter.writeFixed32(fieldId, value);
        return this;
    }

    /** Write a double field. */
    public PacketBuilder writeDouble(int fieldId, double value) {
        mWriter.writeDouble(fieldId, value);
        return this;
    }

    /** Write a float field. */
    public PacketBuilder writeFloat(int fieldId, float value) {
        mWriter.writeFloat(fieldId, value);
        return this;
    }

    /** Write a string field. */
    public PacketBuilder writeString(int fieldId, String value) {
        mWriter.writeString(fieldId, value);
        return this;
    }

    /** Write a bytes field. */
    public PacketBuilder writeBytes(int fieldId, byte[] value) {
        mWriter.writeBytes(fieldId, value);
        return this;
    }

    /** Write a bytes field with offset and length. */
    public PacketBuilder writeBytes(int fieldId, byte[] value, int offset, int length) {
        mWriter.writeBytes(fieldId, value, offset, length);
        return this;
    }

    // ====================================================================
    // Nesting
    // ====================================================================

    /** Begin a nested message. Must be matched by endNested(). */
    public PacketBuilder beginNested(int fieldId) {
        mTokenStack[mTokenDepth++] = mWriter.beginNested(fieldId);
        return this;
    }

    /** End the most recently opened nested message. */
    public PacketBuilder endNested() {
        mWriter.endNested(mTokenStack[--mTokenDepth]);
        return this;
    }

    // ====================================================================
    // Commit
    // ====================================================================

    /**
     * Finalize the packet and write it to all active tracing sessions.
     * Closes any unclosed nested messages and commits via TraceContext.
     */
    public void commit() {
        while (mTokenDepth > 0) {
            mWriter.endNested(mTokenStack[--mTokenDepth]);
        }
        // mCtx is null in standalone/test usage where the caller reads the
        // encoded bytes from the ProtoWriter directly.
        if (mCtx != null) {
            mCtx.commitPacket();
        }
    }
}
