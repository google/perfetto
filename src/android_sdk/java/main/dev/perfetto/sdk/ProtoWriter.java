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
 * <p>Each public {@code write*} call does exactly one {@link #ensureCapacity}
 * (sized for tag + worst-case payload) and then writes the tag and payload via
 * {@code *NoCheck} helpers that hoist {@link #mBuf}/{@link #mPos} into locals.
 * This keeps the per-field cost to a single bounds/grow check and one field
 * write-back, which matters on runtimes that don't inline aggressively.
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

  // Worst-case encoded sizes, used to size a single ensureCapacity per field.
  private static final int MAX_VARINT_LEN = 10; // a 64-bit varint
  private static final int MAX_TAG_LEN = 5; // a field tag varint (field nums to 2^28)

  // Matches PROTOZERO_MESSAGE_LENGTH_FIELD_SIZE in pb_msg.h. A nested message
  // can therefore be up to 0x0FFFFFFF bytes (256MB), the protozero limit.
  private static final int NESTED_LENGTH_FIELD_SIZE = 4;
  private static final int MAX_NESTING_DEPTH = 16;
  private static final int UTF8_SCRATCH_SIZE = 512;
  // Small by default (a track-event body is well under this); grows on demand.
  // Kept small so that hundreds of per-thread writers stay cheap.
  private static final int DEFAULT_BUFFER_SIZE = 512;

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

  /** Writes a uint32/uint64/int32/int64/enum field. */
  public void writeVarInt(int fieldId, long value) {
    ensureCapacity(MAX_TAG_LEN + MAX_VARINT_LEN);
    putVarIntNoCheck(makeTag(fieldId, WIRE_TYPE_VARINT));
    putVarIntNoCheck(value);
  }

  /** Writes a bool field. */
  public void writeBool(int fieldId, boolean value) {
    ensureCapacity(MAX_TAG_LEN + 1);
    putVarIntNoCheck(makeTag(fieldId, WIRE_TYPE_VARINT));
    mBuf[mPos++] = (byte) (value ? 1 : 0);
  }

  /** Writes a fixed64/sfixed64 field. */
  public void writeFixed64(int fieldId, long value) {
    ensureCapacity(MAX_TAG_LEN + 8);
    putVarIntNoCheck(makeTag(fieldId, WIRE_TYPE_FIXED64));
    putLongLeNoCheck(value);
  }

  /** Writes a double field. */
  public void writeDouble(int fieldId, double value) {
    writeFixed64(fieldId, Double.doubleToRawLongBits(value));
  }

  /**
   * Writes a string field. Uses an ASCII fast path when every char is {@code <=
   * 0x7F} (the common case for event names, categories and arg keys), falling
   * back to UTF-8 encoding otherwise.
   */
  public void writeString(int fieldId, String value) {
    int len = value.length();
    // tag + length varint + the bytes, ensured in one shot. ASCII is the
    // overwhelmingly common case (event names, categories, arg keys), where the
    // byte length equals the char length: write that length, then check-and-copy
    // each char in a SINGLE pass (the previous code did one pass to verify ASCII
    // and a second to copy). There is no alloc-free bulk String->byte[] copy on
    // ART -- libcore's String.getBytes is itself a charAt loop, and
    // getBytes(charset) allocates -- so one combined pass over the chars is the
    // floor. A non-ASCII char rewinds and re-encodes the whole field as UTF-8.
    ensureCapacity(MAX_TAG_LEN + MAX_VARINT_LEN + len);
    int start = mPos;
    putVarIntNoCheck(makeTag(fieldId, WIRE_TYPE_DELIMITED));
    putVarIntNoCheck(len);
    byte[] buf = mBuf;
    int pos = mPos;
    for (int i = 0; i < len; i++) {
      char c = value.charAt(i);
      if (c > 0x7F) {
        mPos = start; // discard the optimistic tag/len/bytes; redo as UTF-8.
        writeStringUtf8(fieldId, value);
        return;
      }
      buf[pos++] = (byte) c;
    }
    mPos = pos;
  }

  /**
   * Begins a nested length-delimited message. Writes the field tag and reserves
   * {@link #NESTED_LENGTH_FIELD_SIZE} bytes for the length, back-filled by the
   * matching {@link #endNested}. Nesting is strictly LIFO.
   */
  public void beginNested(int fieldId) {
    ensureCapacity(MAX_TAG_LEN + NESTED_LENGTH_FIELD_SIZE);
    putVarIntNoCheck(makeTag(fieldId, WIRE_TYPE_DELIMITED));
    mNestingStack[mNestingDepth++] = mPos;
    mPos += NESTED_LENGTH_FIELD_SIZE;
  }

  /**
   * Ends the innermost nested message started with {@link #beginNested}, back-
   * filling its reserved 4-byte redundant varint with the body length. The
   * redundant form keeps the prefix a fixed width regardless of the value,
   * avoiding a second pass to compute sizes.
   */
  public void endNested() {
    int bookmark = mNestingStack[--mNestingDepth];
    int size = mPos - bookmark - NESTED_LENGTH_FIELD_SIZE;
    byte[] buf = mBuf;
    buf[bookmark] = (byte) ((size & 0x7F) | 0x80);
    buf[bookmark + 1] = (byte) (((size >> 7) & 0x7F) | 0x80);
    buf[bookmark + 2] = (byte) (((size >> 14) & 0x7F) | 0x80);
    buf[bookmark + 3] = (byte) ((size >> 21) & 0x7F);
  }

  // The *NoCheck writers assume the caller has already ensured capacity for the
  // bytes they write; they hoist mBuf/mPos to locals.

  private static long makeTag(int fieldId, int wireType) {
    return ((long) fieldId << 3) | wireType;
  }

  private void putVarIntNoCheck(long value) {
    byte[] buf = mBuf;
    int pos = mPos;
    while ((value & ~0x7FL) != 0) {
      buf[pos++] = (byte) ((value & 0x7F) | 0x80);
      value >>>= 7;
    }
    buf[pos++] = (byte) value;
    mPos = pos;
  }

  private void putLongLeNoCheck(long value) {
    byte[] buf = mBuf;
    int pos = mPos;
    buf[pos] = (byte) value;
    buf[pos + 1] = (byte) (value >> 8);
    buf[pos + 2] = (byte) (value >> 16);
    buf[pos + 3] = (byte) (value >> 24);
    buf[pos + 4] = (byte) (value >> 32);
    buf[pos + 5] = (byte) (value >> 40);
    buf[pos + 6] = (byte) (value >> 48);
    buf[pos + 7] = (byte) (value >> 56);
    mPos = pos + 8;
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

  private void writeStringUtf8(int fieldId, String s) {
    int utf8Len = encodeUtf8(s, mUtf8Scratch);
    byte[] src;
    int srcLen;
    if (utf8Len >= 0) {
      src = mUtf8Scratch;
      srcLen = utf8Len;
    } else {
      // Scratch too small (long non-ASCII string). Rare; allocate exactly once.
      src = new byte[-utf8Len];
      srcLen = encodeUtf8(s, src);
    }
    ensureCapacity(MAX_TAG_LEN + MAX_VARINT_LEN + srcLen);
    putVarIntNoCheck(makeTag(fieldId, WIRE_TYPE_DELIMITED));
    putVarIntNoCheck(srcLen);
    System.arraycopy(src, 0, mBuf, mPos, srcLen);
    mPos += srcLen;
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
