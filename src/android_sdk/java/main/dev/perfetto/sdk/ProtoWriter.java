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
 * Zero-allocation protobuf encoder. Pure-Java equivalent of C protozero.
 *
 * <p>Writes protobuf wire format directly into a pre-allocated byte buffer.
 * Designed for encoding Perfetto trace packets on the frame-rendering hot path,
 * where allocation and GC pressure are unacceptable.
 *
 * <p>Nested messages use a 4-byte redundant varint for the length prefix,
 * matching protozero's single-pass strategy: the length is reserved before the
 * submessage body is written and back-filled on {@link #endNested}, so sizes
 * never need to be pre-computed. The redundant form encodes small values in a
 * fixed 4 bytes (e.g. 5 becomes {@code 0x85 0x80 0x80 0x00}); this is valid
 * protobuf that every conformant decoder accepts.
 *
 * <p>Thread safety: not thread-safe. Each thread must use its own instance
 * (typically held in a {@link ThreadLocal}).
 *
 * @hide
 */
public final class ProtoWriter {
  private static final int WIRE_TYPE_VARINT = 0;
  private static final int WIRE_TYPE_FIXED64 = 1;
  private static final int WIRE_TYPE_DELIMITED = 2;
  private static final int WIRE_TYPE_FIXED32 = 5;

  // Matches PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE in pb_msg.h. A nested message
  // can therefore be up to 0x0FFFFFFF bytes (256MB), the protozero limit.
  private static final int NESTED_LENGTH_FIELD_SIZE = 4;
  private static final int MAX_NESTING_DEPTH = 16;
  private static final int UTF8_SCRATCH_SIZE = 512;
  private static final int DEFAULT_BUFFER_SIZE = 32 * 1024;

  private byte[] mBuf;
  private int mPos;
  private final int[] mNestingStack = new int[MAX_NESTING_DEPTH];
  private int mNestingDepth;
  private final byte[] mUtf8Scratch = new byte[UTF8_SCRATCH_SIZE];

  public ProtoWriter() {
    this(DEFAULT_BUFFER_SIZE);
  }

  public ProtoWriter(int bufferSize) {
    mBuf = new byte[bufferSize];
  }

  /** Resets the write position so the buffer can be reused. No allocation. */
  public void reset() {
    mPos = 0;
    mNestingDepth = 0;
  }

  /** Number of bytes written so far. */
  public int position() {
    return mPos;
  }

  /** The backing buffer. Valid data spans {@code [0, position())}. */
  public byte[] buffer() {
    return mBuf;
  }

  // ==========================================================================
  // Varint fields (wire type 0)
  // ==========================================================================

  /** Writes a uint32/uint64/int32/int64/enum field. */
  public void writeVarInt(int fieldId, long value) {
    writeRawVarInt(makeTag(fieldId, WIRE_TYPE_VARINT));
    writeRawVarInt(value);
  }

  /** Writes a sint32/sint64 field (zigzag encoded). */
  public void writeSInt(int fieldId, long value) {
    writeVarInt(fieldId, (value << 1) ^ (value >> 63));
  }

  /** Writes a bool field. */
  public void writeBool(int fieldId, boolean value) {
    writeRawVarInt(makeTag(fieldId, WIRE_TYPE_VARINT));
    ensureCapacity(1);
    mBuf[mPos++] = (byte) (value ? 1 : 0);
  }

  // ==========================================================================
  // Fixed-size fields (wire types 1 and 5)
  // ==========================================================================

  /** Writes a fixed64/sfixed64 field. */
  public void writeFixed64(int fieldId, long value) {
    writeRawVarInt(makeTag(fieldId, WIRE_TYPE_FIXED64));
    putLongLE(value);
  }

  /** Writes a fixed32/sfixed32 field. */
  public void writeFixed32(int fieldId, int value) {
    writeRawVarInt(makeTag(fieldId, WIRE_TYPE_FIXED32));
    putIntLE(value);
  }

  /** Writes a double field. */
  public void writeDouble(int fieldId, double value) {
    writeFixed64(fieldId, Double.doubleToRawLongBits(value));
  }

  /** Writes a float field. */
  public void writeFloat(int fieldId, float value) {
    writeFixed32(fieldId, Float.floatToRawIntBits(value));
  }

  // ==========================================================================
  // Length-delimited fields (wire type 2)
  // ==========================================================================

  /**
   * Writes a string field. Uses an ASCII fast path when every char is {@code <=
   * 0x7F} (the common case for event names, categories and arg keys), falling
   * back to UTF-8 encoding otherwise.
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

  /** Writes a bytes field from a slice of {@code value}. */
  public void writeBytes(int fieldId, byte[] value, int offset, int length) {
    writeRawVarInt(makeTag(fieldId, WIRE_TYPE_DELIMITED));
    writeRawVarInt(length);
    ensureCapacity(length);
    System.arraycopy(value, offset, mBuf, mPos, length);
    mPos += length;
  }

  /** Writes a bytes field from the whole array. */
  public void writeBytes(int fieldId, byte[] value) {
    writeBytes(fieldId, value, 0, value.length);
  }

  // ==========================================================================
  // Nested messages
  // ==========================================================================

  /**
   * Begins a nested length-delimited message. Writes the field tag and reserves
   * {@link #NESTED_LENGTH_FIELD_SIZE} bytes for the length, returning a token
   * that must be passed to the matching {@link #endNested}.
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
   * Ends a nested message started with {@link #beginNested}, back-filling the
   * reserved 4-byte redundant varint with the body length. The redundant form
   * keeps the prefix a fixed width regardless of the value, avoiding a second
   * pass to compute sizes.
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

  // ==========================================================================
  // Raw bytes (for pre-encoded data)
  // ==========================================================================

  /** Appends raw, already-encoded bytes verbatim. */
  public void appendRawBytes(byte[] data, int offset, int length) {
    ensureCapacity(length);
    System.arraycopy(data, offset, mBuf, mPos, length);
    mPos += length;
  }

  // ==========================================================================
  // Internal helpers
  // ==========================================================================

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
      // Scratch too small (long non-ASCII string). Rare; allocate exactly once.
      byte[] big = new byte[-utf8Len];
      int actualLen = encodeUtf8(s, big);
      writeRawVarInt(actualLen);
      ensureCapacity(actualLen);
      System.arraycopy(big, 0, mBuf, mPos, actualLen);
      mPos += actualLen;
    }
  }

  /**
   * Encodes {@code s} as UTF-8 into {@code dst}. Returns the byte count on
   * success, or {@code -needed} (a negative lower bound on the required size) if
   * {@code dst} is too small.
   */
  private static int encodeUtf8(String s, byte[] dst) {
    int len = s.length();
    int dp = 0;
    for (int i = 0; i < len; i++) {
      char c = s.charAt(i);
      if (c <= 0x7F) {
        if (dp >= dst.length) {
          return -(len * 3);
        }
        dst[dp++] = (byte) c;
      } else if (c <= 0x7FF) {
        if (dp + 2 > dst.length) {
          return -(len * 3);
        }
        dst[dp++] = (byte) (0xC0 | (c >> 6));
        dst[dp++] = (byte) (0x80 | (c & 0x3F));
      } else if (Character.isHighSurrogate(c) && i + 1 < len) {
        char low = s.charAt(++i);
        int cp = Character.toCodePoint(c, low);
        if (dp + 4 > dst.length) {
          return -(len * 4);
        }
        dst[dp++] = (byte) (0xF0 | (cp >> 18));
        dst[dp++] = (byte) (0x80 | ((cp >> 12) & 0x3F));
        dst[dp++] = (byte) (0x80 | ((cp >> 6) & 0x3F));
        dst[dp++] = (byte) (0x80 | (cp & 0x3F));
      } else {
        if (dp + 3 > dst.length) {
          return -(len * 3);
        }
        dst[dp++] = (byte) (0xE0 | (c >> 12));
        dst[dp++] = (byte) (0x80 | ((c >> 6) & 0x3F));
        dst[dp++] = (byte) (0x80 | (c & 0x3F));
      }
    }
    return dp;
  }
}
