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
 * Zero-allocation protobuf encoder. Pure Java equivalent of C protozero.
 *
 * Writes protobuf wire format directly to a pre-allocated byte buffer.
 * Designed for Perfetto trace packet encoding on the Android frame rendering
 * hot path.
 *
 * Nested messages use 4-byte redundant varint encoding for the length field,
 * matching protozero's approach for single-pass encoding without size
 * pre-computation. The redundant varint encodes small values in 4 bytes
 * (e.g., 5 is encoded as 0x85 0x80 0x80 0x00) which is valid protobuf
 * that all decoders handle correctly.
 *
 * Thread safety: not thread-safe. Each thread must use its own instance
 * (typically via ThreadLocal in TraceContext).
 */
public final class ProtoWriter {
    private static final int WIRE_TYPE_VARINT = 0;
    private static final int WIRE_TYPE_FIXED64 = 1;
    private static final int WIRE_TYPE_DELIMITED = 2;
    private static final int WIRE_TYPE_FIXED32 = 5;

    // Matches PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE in pb_msg.h.
    private static final int NESTED_LENGTH_FIELD_SIZE = 4;
    private static final int MAX_NESTING_DEPTH = 16;
    private static final int UTF8_SCRATCH_SIZE = 512;

    private byte[] mBuf;
    private int mPos;
    private final int[] mNestingStack = new int[MAX_NESTING_DEPTH];
    private int mNestingDepth;
    private final byte[] mUtf8Scratch = new byte[UTF8_SCRATCH_SIZE];

    public ProtoWriter() {
        this(32 * 1024);
    }

    public ProtoWriter(int bufferSize) {
        mBuf = new byte[bufferSize];
    }

    /** Reset write position. No allocation. */
    public void reset() {
        mPos = 0;
        mNestingDepth = 0;
    }

    /** Current write position (number of bytes written). */
    public int position() {
        return mPos;
    }

    /** The underlying buffer. Valid data from index 0 to position(). */
    public byte[] buffer() {
        return mBuf;
    }

    // ========================================================================
    // Varint fields (wire type 0)
    // ========================================================================

    /** Write uint32/uint64/int32/int64/enum field. */
    public void writeVarInt(int fieldId, long value) {
        writeRawVarInt(makeTag(fieldId, WIRE_TYPE_VARINT));
        writeRawVarInt(value);
    }

    /** Write sint32/sint64 field (zigzag encoded). */
    public void writeSInt(int fieldId, long value) {
        writeVarInt(fieldId, (value << 1) ^ (value >> 63));
    }

    /** Write bool field. */
    public void writeBool(int fieldId, boolean value) {
        writeRawVarInt(makeTag(fieldId, WIRE_TYPE_VARINT));
        ensureCapacity(1);
        mBuf[mPos++] = (byte) (value ? 1 : 0);
    }

    // ========================================================================
    // Fixed-size fields
    // ========================================================================

    /** Write fixed64/sfixed64 field. */
    public void writeFixed64(int fieldId, long value) {
        writeRawVarInt(makeTag(fieldId, WIRE_TYPE_FIXED64));
        putLongLE(value);
    }

    /** Write fixed32/sfixed32 field. */
    public void writeFixed32(int fieldId, int value) {
        writeRawVarInt(makeTag(fieldId, WIRE_TYPE_FIXED32));
        putIntLE(value);
    }

    /** Write double field. */
    public void writeDouble(int fieldId, double value) {
        writeFixed64(fieldId, Double.doubleToRawLongBits(value));
    }

    /** Write float field. */
    public void writeFloat(int fieldId, float value) {
        writeFixed32(fieldId, Float.floatToRawIntBits(value));
    }

    // ========================================================================
    // Length-delimited fields (wire type 2)
    // ========================================================================

    /**
     * Write string field. Uses ASCII fast path when all chars are <= 0x7F
     * (common for trace event names, categories, arg keys).
     */
    public void writeString(int fieldId, String value) {
        writeRawVarInt(makeTag(fieldId, WIRE_TYPE_DELIMITED));
        int len = value.length();

        boolean ascii = true;
        for (int i = 0; i < len; i++) {
            if (value.charAt(i) > 0x7F) {
                ascii = false;
                break;
            }
        }

        if (ascii) {
            writeRawVarInt(len);
            ensureCapacity(len);
            for (int i = 0; i < len; i++) {
                mBuf[mPos++] = (byte) value.charAt(i);
            }
        } else {
            writeStringUtf8(value);
        }
    }

    /** Write bytes field. */
    public void writeBytes(int fieldId, byte[] value, int offset, int length) {
        writeRawVarInt(makeTag(fieldId, WIRE_TYPE_DELIMITED));
        writeRawVarInt(length);
        ensureCapacity(length);
        System.arraycopy(value, offset, mBuf, mPos, length);
        mPos += length;
    }

    /** Write bytes field (full array). */
    public void writeBytes(int fieldId, byte[] value) {
        writeBytes(fieldId, value, 0, value.length);
    }

    // ========================================================================
    // Nested messages
    // ========================================================================

    /**
     * Begin a nested length-delimited message.
     *
     * Writes the field tag and reserves 4 bytes for the length (redundant
     * varint). Returns a nesting token that must be passed to endNested().
     *
     * The 4-byte redundant varint supports nested messages up to 256MB
     * (0x0FFFFFFF bytes). This matches protozero's limit.
     */
    public int beginNested(int fieldId) {
        writeRawVarInt(makeTag(fieldId, WIRE_TYPE_DELIMITED));
        ensureCapacity(NESTED_LENGTH_FIELD_SIZE);
        int bookmark = mPos;
        mPos += NESTED_LENGTH_FIELD_SIZE;
        mNestingStack[mNestingDepth++] = bookmark;
        return mNestingDepth - 1;
    }

    /**
     * End a nested message started with beginNested().
     *
     * Backfills the 4-byte redundant varint length at the bookmark position.
     * The redundant encoding writes the size using exactly 4 bytes with
     * leading-zero continuation bits:
     *
     *   byte 0: (size & 0x7F) | 0x80
     *   byte 1: ((size >> 7) & 0x7F) | 0x80
     *   byte 2: ((size >> 14) & 0x7F) | 0x80
     *   byte 3: (size >> 21) & 0x7F
     *
     * This is valid protobuf (decoders must handle leading-zero varints)
     * and avoids a double pass to compute sizes upfront.
     */
    public void endNested(int token) {
        mNestingDepth--;
        int bookmark = mNestingStack[token];
        int size = mPos - bookmark - NESTED_LENGTH_FIELD_SIZE;
        mBuf[bookmark] = (byte) ((size & 0x7F) | 0x80);
        mBuf[bookmark + 1] = (byte) (((size >> 7) & 0x7F) | 0x80);
        mBuf[bookmark + 2] = (byte) (((size >> 14) & 0x7F) | 0x80);
        mBuf[bookmark + 3] = (byte) ((size >> 21) & 0x7F);
    }

    // ========================================================================
    // Append raw bytes (for pre-encoded data)
    // ========================================================================

    /** Append raw bytes to the output. */
    public void appendRawBytes(byte[] data, int offset, int length) {
        ensureCapacity(length);
        System.arraycopy(data, offset, mBuf, mPos, length);
        mPos += length;
    }

    // ========================================================================
    // Internal methods
    // ========================================================================

    private static long makeTag(int fieldId, int wireType) {
        return ((long) fieldId << 3) | wireType;
    }

    private void writeRawVarInt(long value) {
        ensureCapacity(10);
        while ((value & ~0x7FL) != 0) {
            mBuf[mPos++] = (byte) ((value & 0x7F) | 0x80);
            value >>>= 7;
        }
        mBuf[mPos++] = (byte) value;
    }

    private void putLongLE(long value) {
        ensureCapacity(8);
        mBuf[mPos++] = (byte) value;
        mBuf[mPos++] = (byte) (value >> 8);
        mBuf[mPos++] = (byte) (value >> 16);
        mBuf[mPos++] = (byte) (value >> 24);
        mBuf[mPos++] = (byte) (value >> 32);
        mBuf[mPos++] = (byte) (value >> 40);
        mBuf[mPos++] = (byte) (value >> 48);
        mBuf[mPos++] = (byte) (value >> 56);
    }

    private void putIntLE(int value) {
        ensureCapacity(4);
        mBuf[mPos++] = (byte) value;
        mBuf[mPos++] = (byte) (value >> 8);
        mBuf[mPos++] = (byte) (value >> 16);
        mBuf[mPos++] = (byte) (value >> 24);
    }

    private void ensureCapacity(int needed) {
        if (mPos + needed <= mBuf.length) {
            return;
        }
        int newSize = Math.max(mBuf.length * 2, mPos + needed);
        byte[] newBuf = new byte[newSize];
        System.arraycopy(mBuf, 0, newBuf, 0, mPos);
        mBuf = newBuf;
    }

    private void writeStringUtf8(String s) {
        int utf8Len = encodeUtf8(s, mUtf8Scratch);
        if (utf8Len >= 0) {
            writeRawVarInt(utf8Len);
            ensureCapacity(utf8Len);
            System.arraycopy(mUtf8Scratch, 0, mBuf, mPos, utf8Len);
            mPos += utf8Len;
        } else {
            // Scratch too small (string > ~170 chars non-ASCII). Rare path.
            byte[] big = new byte[-utf8Len];
            int actualLen = encodeUtf8(s, big);
            writeRawVarInt(actualLen);
            ensureCapacity(actualLen);
            System.arraycopy(big, 0, mBuf, mPos, actualLen);
            mPos += actualLen;
        }
    }

    /**
     * Encode string as UTF-8 into dst.
     * Returns byte count on success.
     * Returns negative value (-needed_size) if dst is too small.
     */
    private static int encodeUtf8(String s, byte[] dst) {
        int len = s.length();
        int dp = 0;
        for (int i = 0; i < len; i++) {
            char c = s.charAt(i);
            if (c <= 0x7F) {
                if (dp >= dst.length)
                    return -(len * 3);
                dst[dp++] = (byte) c;
            } else if (c <= 0x7FF) {
                if (dp + 2 > dst.length)
                    return -(len * 3);
                dst[dp++] = (byte) (0xC0 | (c >> 6));
                dst[dp++] = (byte) (0x80 | (c & 0x3F));
            } else if (Character.isHighSurrogate(c) && i + 1 < len) {
                char low = s.charAt(++i);
                int cp = Character.toCodePoint(c, low);
                if (dp + 4 > dst.length)
                    return -(len * 4);
                dst[dp++] = (byte) (0xF0 | (cp >> 18));
                dst[dp++] = (byte) (0x80 | ((cp >> 12) & 0x3F));
                dst[dp++] = (byte) (0x80 | ((cp >> 6) & 0x3F));
                dst[dp++] = (byte) (0x80 | (cp & 0x3F));
            } else {
                if (dp + 3 > dst.length)
                    return -(len * 3);
                dst[dp++] = (byte) (0xE0 | (c >> 12));
                dst[dp++] = (byte) (0x80 | ((c >> 6) & 0x3F));
                dst[dp++] = (byte) (0x80 | (c & 0x3F));
            }
        }
        return dp;
    }
}
