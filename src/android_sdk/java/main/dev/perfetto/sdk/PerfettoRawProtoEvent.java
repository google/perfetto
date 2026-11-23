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

import java.util.Objects;

/**
 * Serializes TrackEvent protobuf fields into a single byte array.
 *
 * <p>Layout: {@code [0, fieldsSize())} holds serialized TrackEvent fields, followed immediately by
 * {@code nameSize()} bytes of event name. Strings are inline (no interning).
 *
 * <p>Nested messages use length-delimited framing. {@link #beginNested} reserves a 1-byte length
 * placeholder and {@link #endNested} backpatches it, shifting the payload right only when it is
 * 128 bytes or longer.
 *
 * <p>Invalid input (over 1 MiB, invalid field number, nesting over 64, or writing to a parent
 * while a nested message is open) marks the event failed so callers drop it. With {@link
 * #setThrowOnError}, invalid input also throws.
 */
final class PerfettoRawProtoEvent {
  // ---------------------------------------------------------------------------
  // Constants and state
  // ---------------------------------------------------------------------------

  static final int INITIAL_BUFFER_SIZE = 256;
  // Largest array kept by release() for reuse.
  static final int MAX_KEPT_BUFFER_SIZE = 16 * 1024;
  // Largest array allocated as non-movable (larger events are copied).
  private static final int MAX_NON_MOVABLE_SIZE = 16 * 1024;
  // Keep in sync with PERFETTO_TE_HL_MAX_RAW_PROTO_EVENT_SIZE.
  static final int MAX_EVENT_SIZE = 1024 * 1024;
  static final int MAX_NESTING_DEPTH = 64;

  // Upper bounds for single-check capacity reservations.
  private static final int MAX_TAG_SIZE = 5;
  private static final int MAX_VARINT_SIZE = 10;
  private static final int FIXED64_SIZE = 8;
  private static final int MAX_ONE_BYTE_LENGTH = 127;
  // Largest event name kept by finish() for reuse on consecutive events.
  private static final int MAX_CACHED_NAME_SIZE = 128;

  // Protobuf wire types.
  // TODO(sashwinbalaji): Support more types.
  private static final int WIRE_TYPE_VARINT = 0;
  private static final int WIRE_TYPE_FIXED64 = 1;
  private static final int WIRE_TYPE_LENGTH_DELIMITED = 2;

  // TrackEvent field numbers (protos/perfetto/trace/track_event/track_event.proto).
  private static final int TRACK_EVENT_DEBUG_ANNOTATIONS = 4;
  private static final int TRACK_EVENT_COUNTER_VALUE = 30;
  private static final int TRACK_EVENT_DOUBLE_COUNTER_VALUE = 44;
  private static final int TRACK_EVENT_FLOW_IDS = 47;
  private static final int TRACK_EVENT_TERMINATING_FLOW_IDS = 48;
  private static final int TRACK_EVENT_CORRELATION_ID = 52;
  private static final int TRACK_EVENT_CORRELATION_ID_STR = 53;

  // DebugAnnotation field numbers (protos/perfetto/trace/track_event/debug_annotation.proto).
  private static final int DEBUG_ANNOTATION_BOOL_VALUE = 2;
  private static final int DEBUG_ANNOTATION_INT_VALUE = 4;
  private static final int DEBUG_ANNOTATION_DOUBLE_VALUE = 5;
  private static final int DEBUG_ANNOTATION_STRING_VALUE = 6;
  private static final int DEBUG_ANNOTATION_NAME = 10;

  // One-byte wire tags written by the debug arg fast path.
  private static final byte TAG_DEBUG_ANNOTATIONS =
      (byte) ((TRACK_EVENT_DEBUG_ANNOTATIONS << 3) | WIRE_TYPE_LENGTH_DELIMITED);
  private static final byte TAG_DEBUG_ANNOTATION_NAME =
      (byte) ((DEBUG_ANNOTATION_NAME << 3) | WIRE_TYPE_LENGTH_DELIMITED);
  private static final byte TAG_DEBUG_ANNOTATION_BOOL_VALUE =
      (byte) ((DEBUG_ANNOTATION_BOOL_VALUE << 3) | WIRE_TYPE_VARINT);
  private static final byte TAG_DEBUG_ANNOTATION_INT_VALUE =
      (byte) ((DEBUG_ANNOTATION_INT_VALUE << 3) | WIRE_TYPE_VARINT);
  private static final byte TAG_DEBUG_ANNOTATION_DOUBLE_VALUE =
      (byte) ((DEBUG_ANNOTATION_DOUBLE_VALUE << 3) | WIRE_TYPE_FIXED64);
  private static final byte TAG_DEBUG_ANNOTATION_STRING_VALUE =
      (byte) ((DEBUG_ANNOTATION_STRING_VALUE << 3) | WIRE_TYPE_LENGTH_DELIMITED);

  private byte[] mBuffer;
  private long mBufferAddress;
  private int mPosition;
  private int mFieldsSize;
  private int mNameSize;
  private boolean mFailed;
  private boolean mThrowOnError;
  // Payload start offsets of open length-delimited messages (1-byte length placeholder sits at
  // offset - 1).
  private final int[] mOpenMessages = new int[MAX_NESTING_DEPTH];
  private int mOpenCount;
  // Pending counter value written at finish() so the last setCounter() wins.
  // mCounterField is 0 if unset, or TRACK_EVENT_COUNTER_VALUE / TRACK_EVENT_DOUBLE_COUNTER_VALUE.
  // mCounterBits holds the raw 64-bit value.
  private int mCounterField;
  private long mCounterBits;
  // Cached bytes of the previous finish() name (see writeName()). Not cleared
  // by release() because Java Strings are immutable.
  private final byte[] mCachedNameBytes = new byte[MAX_CACHED_NAME_SIZE];
  private String mCachedName;
  private int mCachedNameSize;

  PerfettoRawProtoEvent() {
    replaceBuffer(INITIAL_BUFFER_SIZE);
  }

  /** In debug mode, invalid input throws as well as failing the event. */
  void setThrowOnError(boolean throwOnError) {
    mThrowOnError = throwOnError;
  }

  /** True if the event is invalid and must be dropped. */
  boolean failed() {
    return mFailed;
  }

  /** True if nothing was added or failed since the last release(). */
  boolean isEmpty() {
    return mPosition == 0 && mCounterField == 0 && !mFailed;
  }

  byte[] buffer() {
    return mBuffer;
  }

  /** Non-movable address of {@link #buffer()}, or 0 if callers must copy the array. */
  long bufferAddress() {
    return mBufferAddress;
  }

  /** Total bytes to hand to native code (fieldsSize() + nameSize()). */
  int size() {
    return mPosition;
  }

  int fieldsSize() {
    return mFieldsSize;
  }

  int nameSize() {
    return mNameSize;
  }

  // ---------------------------------------------------------------------------
  // Storage and limits
  // ---------------------------------------------------------------------------

  /** Clears the event for reuse and shrinks buffers above MAX_KEPT_BUFFER_SIZE. */
  void release() {
    reset();
    if (mBuffer.length > MAX_KEPT_BUFFER_SIZE) {
      replaceBuffer(INITIAL_BUFFER_SIZE);
    }
  }

  private void reset() {
    mPosition = 0;
    mFieldsSize = 0;
    mNameSize = 0;
    mFailed = false;
    mOpenCount = 0;
    mCounterField = 0;
  }

  // Fast capacity check. Since mBuffer.length <= MAX_EVENT_SIZE, fitting in mBuffer also satisfies
  // the transport limit.
  private boolean fits(int count) {
    return count <= mBuffer.length - mPosition;
  }

  // Ensures room for `count` more bytes, growing mBuffer up to MAX_EVENT_SIZE if needed.
  private boolean reserve(long count) {
    if (count < 0 || count > MAX_EVENT_SIZE - mPosition) {
      fail(new IllegalArgumentException("Raw-proto event exceeds the 1 MiB transport limit"));
      return false;
    }
    int required = mPosition + (int) count;
    if (required <= mBuffer.length) {
      return true;
    }
    int capacity = Math.min(MAX_EVENT_SIZE, Math.max(required, mBuffer.length * 2));
    try {
      replaceBuffer(capacity);
      return true;
    } catch (OutOfMemoryError error) {
      // Tracing must not crash its caller in release mode.
      mFailed = true;
      if (mThrowOnError) {
        throw error;
      }
      return false;
    }
  }

  // Allocates a `capacity`-byte buffer (non-movable up to MAX_NON_MOVABLE_SIZE when available)
  // and copies any bytes written so far.
  private void replaceBuffer(int capacity) {
    byte[] replacement = null;
    long address = 0;
    if (capacity <= MAX_NON_MOVABLE_SIZE) {
      replacement = NonMovableByteArrays.allocate(capacity);
      if (replacement != null) {
        address = NonMovableByteArrays.addressOf(replacement);
      }
    }
    if (replacement == null) {
      replacement = new byte[capacity];
    }
    if (mPosition > 0) {
      System.arraycopy(mBuffer, 0, replacement, 0, mPosition);
    }
    mBuffer = replacement;
    mBufferAddress = address;
  }

  private void fail(RuntimeException error) {
    mFailed = true;
    if (mThrowOnError) {
      throw error;
    }
  }

  // ---------------------------------------------------------------------------
  // Wire primitives
  // ---------------------------------------------------------------------------

  // Returns the tag for `id`, or -1 (failing the event) for an invalid field number.
  private long tag(long id, int wireType) {
    // Android ProtoStream field IDs carry type/count flags in the upper bits.
    int number = (int) id;
    if (number <= 0 || number > 0x1fffffff) {
      fail(new IllegalArgumentException("Invalid protobuf field number"));
      return -1;
    }
    return Integer.toUnsignedLong((number << 3) | wireType);
  }

  // Writes a varint without a capacity check (callers checked fits()/reserve()).
  private void putVarint(long value) {
    // Fast path: values in [0, 127] encode as one byte (most tags and small ints).
    if ((value >>> 7) == 0) {
      mBuffer[mPosition++] = (byte) value;
      return;
    }
    putMultiByteVarint(value);
  }

  private void putMultiByteVarint(long value) {
    byte[] buffer = mBuffer;
    int position = mPosition;
    while ((value >>> 7) != 0) {
      buffer[position++] = (byte) ((value & 0x7f) | 0x80);
      value >>>= 7;
    }
    buffer[position++] = (byte) value;
    mPosition = position;
  }

  // Writes eight little-endian bytes without a capacity check.
  private void putFixed64(long value) {
    byte[] buffer = mBuffer;
    int position = mPosition;
    buffer[position] = (byte) value;
    buffer[position + 1] = (byte) (value >>> 8);
    buffer[position + 2] = (byte) (value >>> 16);
    buffer[position + 3] = (byte) (value >>> 24);
    buffer[position + 4] = (byte) (value >>> 32);
    buffer[position + 5] = (byte) (value >>> 40);
    buffer[position + 6] = (byte) (value >>> 48);
    buffer[position + 7] = (byte) (value >>> 56);
    mPosition = position + FIXED64_SIZE;
  }

  // Tag is (field << 3) | wireType, so field numbers 1..15 fit in 7 bits (1 byte).
  private static int builtinTagSize(int field) {
    return field < 16 ? 1 : 2;
  }

  // Writes the tag of a built-in TRACK_EVENT_* constant (all < 2048) without a capacity check.
  private void putBuiltinTag(int field, int wireType) {
    int tag = (field << 3) | wireType;
    byte[] buffer = mBuffer;
    int position = mPosition;
    if (tag < 0x80) {
      buffer[position++] = (byte) tag;
    } else {
      buffer[position++] = (byte) (tag | 0x80);
      buffer[position++] = (byte) (tag >>> 7);
    }
    mPosition = position;
  }

  private static int varintSize(long value) {
    return (64 - Long.numberOfLeadingZeros(value | 1) + 6) / 7;
  }

  // Matches EXTRAS JNI string conversion (Latin-1, '?' above 255, stop at first NUL).
  private static int stringSize(String value) {
    if (value == null) {
      return 0;
    }
    int zero = value.indexOf('\0');
    return zero < 0 ? value.length() : zero;
  }

  // Copies up to `length` chars of `value`, stopping at the first NUL. Returns the end position.
  private int copyString(String value, int length, int position) {
    byte[] buffer = mBuffer;
    for (int i = 0; i < length; ++i) {
      char c = value.charAt(i);
      if ((char) (c - 1) < 255) {
        buffer[position++] = (byte) c;
      } else if (c == '\0') {
        break;
      } else {
        buffer[position++] = '?';
      }
    }
    return position;
  }

  // ---------------------------------------------------------------------------
  // Custom proto fields
  // ---------------------------------------------------------------------------

  void addVarint(long id, long value) {
    if (mFailed) {
      return;
    }
    long tag = tag(id, WIRE_TYPE_VARINT);
    if (tag < 0) {
      return;
    }
    if (!fits(MAX_TAG_SIZE + MAX_VARINT_SIZE)
        && !reserve(varintSize(tag) + varintSize(value))) {
      return;
    }
    putVarint(tag);
    putVarint(value);
  }

  void addFixed64(long id, long value) {
    if (mFailed) {
      return;
    }
    long tag = tag(id, WIRE_TYPE_FIXED64);
    if (tag < 0) {
      return;
    }
    if (!fits(MAX_TAG_SIZE + FIXED64_SIZE) && !reserve(varintSize(tag) + FIXED64_SIZE)) {
      return;
    }
    putVarint(tag);
    putFixed64(value);
  }

  void addString(long id, String value) {
    if (mFailed) {
      return;
    }
    long tag = tag(id, WIRE_TYPE_LENGTH_DELIMITED);
    if (tag < 0) {
      return;
    }
    int length = value == null ? 0 : value.length();
    if (length <= MAX_ONE_BYTE_LENGTH && fits(MAX_TAG_SIZE + 1 + length)) {
      // One-pass fast path: reserve 1 length byte and patch after copying (which may stop at NUL).
      putVarint(tag);
      int lengthPosition = mPosition;
      int end = copyString(value, length, lengthPosition + 1);
      mBuffer[lengthPosition] = (byte) (end - lengthPosition - 1);
      mPosition = end;
      return;
    }
    int size = stringSize(value);
    if (!reserve((long) varintSize(tag) + varintSize(size) + size)) {
      return;
    }
    putVarint(tag);
    putVarint(size);
    mPosition = copyString(value, size, mPosition);
  }

  void beginNested(long id) {
    if (mFailed) {
      return;
    }
    if (mOpenCount >= MAX_NESTING_DEPTH) {
      fail(new IllegalArgumentException("Raw-proto nesting exceeds 64 messages"));
      return;
    }
    beginMessage(id);
  }

  void endNested(int expectedDepth) {
    endMessage(expectedDepth);
  }

  private boolean beginMessage(long id) {
    long tag = tag(id, WIRE_TYPE_LENGTH_DELIMITED);
    if (tag < 0) {
      return false;
    }
    if (!fits(MAX_TAG_SIZE + 1) && !reserve(varintSize(tag) + 1)) {
      return false;
    }
    putVarint(tag);
    mBuffer[mPosition++] = 0;
    mOpenMessages[mOpenCount++] = mPosition;
    return true;
  }

  private void endMessage(int expectedDepth) {
    if (mFailed) {
      return;
    }
    if (expectedDepth <= 0 || mOpenCount != expectedDepth) {
      fail(new IllegalStateException("Nested fields must be closed in order"));
      return;
    }
    int payload = mOpenMessages[--mOpenCount];
    int size = mPosition - payload;
    if (size <= MAX_ONE_BYTE_LENGTH) {
      mBuffer[payload - 1] = (byte) size;
      return;
    }
    // Payload length needs more than 1 byte, so shift the payload right and write the varint.
    int extra = varintSize(size) - 1;
    if (!reserve(extra)) {
      return;
    }
    System.arraycopy(mBuffer, payload, mBuffer, payload + extra, size);
    int savedPosition = mPosition + extra;
    mPosition = payload - 1;
    putMultiByteVarint(size);
    mPosition = savedPosition;
  }

  // Fails the event unless exactly `expected` nested messages are open.
  void checkNestedDepth(int expected) {
    if (!mFailed && mOpenCount != expected) {
      fail(new IllegalStateException("End nested proto fields before writing to their parent"));
    }
  }

  // ---------------------------------------------------------------------------
  // TrackEvent built-ins
  // ---------------------------------------------------------------------------

  void addFlowId(long id) {
    addBuiltinFixed64(TRACK_EVENT_FLOW_IDS, id);
  }

  void addTerminatingFlowId(long id) {
    addBuiltinFixed64(TRACK_EVENT_TERMINATING_FLOW_IDS, id);
  }

  void setCorrelationId(long id) {
    addBuiltinVarint(TRACK_EVENT_CORRELATION_ID, id);
  }

  void setCorrelationId(String id) {
    addBuiltinString(TRACK_EVENT_CORRELATION_ID_STR, id);
  }

  void setCounterValue(long value) {
    mCounterField = TRACK_EVENT_COUNTER_VALUE;
    mCounterBits = value;
  }

  void setCounterValue(double value) {
    mCounterField = TRACK_EVENT_DOUBLE_COUNTER_VALUE;
    mCounterBits = Double.doubleToRawLongBits(value);
  }

  private void writeCounterValue() {
    if (mCounterField == TRACK_EVENT_COUNTER_VALUE) {
      addBuiltinVarint(TRACK_EVENT_COUNTER_VALUE, mCounterBits);
    } else if (mCounterField == TRACK_EVENT_DOUBLE_COUNTER_VALUE) {
      addBuiltinFixed64(TRACK_EVENT_DOUBLE_COUNTER_VALUE, mCounterBits);
    }
  }

  private void addBuiltinVarint(int field, long value) {
    checkNestedDepth(0);
    if (mFailed) {
      return;
    }
    if (!fits(2 + MAX_VARINT_SIZE)
        && !reserve(builtinTagSize(field) + varintSize(value))) {
      return;
    }
    putBuiltinTag(field, WIRE_TYPE_VARINT);
    putVarint(value);
  }

  private void addBuiltinFixed64(int field, long value) {
    checkNestedDepth(0);
    if (mFailed) {
      return;
    }
    if (!fits(2 + FIXED64_SIZE) && !reserve(builtinTagSize(field) + FIXED64_SIZE)) {
      return;
    }
    putBuiltinTag(field, WIRE_TYPE_FIXED64);
    putFixed64(value);
  }

  private void addBuiltinString(int field, String value) {
    checkNestedDepth(0);
    addString(field, value);
  }

  // ---------------------------------------------------------------------------
  // Debug args (TrackEvent.debug_annotations)
  // ---------------------------------------------------------------------------

  void addArg(String name, long value) {
    int lengthPosition = beginShortArg(name, 1 + MAX_VARINT_SIZE);
    if (lengthPosition >= 0) {
      mBuffer[mPosition++] = TAG_DEBUG_ANNOTATION_INT_VALUE;
      putVarint(value);
      endShortArg(lengthPosition);
      return;
    }
    if (!beginArg(name)) {
      return;
    }
    addVarint(DEBUG_ANNOTATION_INT_VALUE, value);
    endArg();
  }

  void addArg(String name, boolean value) {
    int lengthPosition = beginShortArg(name, 2);
    if (lengthPosition >= 0) {
      mBuffer[mPosition] = TAG_DEBUG_ANNOTATION_BOOL_VALUE;
      mBuffer[mPosition + 1] = (byte) (value ? 1 : 0);
      mPosition += 2;
      endShortArg(lengthPosition);
      return;
    }
    if (!beginArg(name)) {
      return;
    }
    addVarint(DEBUG_ANNOTATION_BOOL_VALUE, value ? 1 : 0);
    endArg();
  }

  void addArg(String name, double value) {
    int lengthPosition = beginShortArg(name, 1 + FIXED64_SIZE);
    if (lengthPosition >= 0) {
      mBuffer[mPosition++] = TAG_DEBUG_ANNOTATION_DOUBLE_VALUE;
      putFixed64(Double.doubleToRawLongBits(value));
      endShortArg(lengthPosition);
      return;
    }
    if (!beginArg(name)) {
      return;
    }
    addFixed64(DEBUG_ANNOTATION_DOUBLE_VALUE, Double.doubleToRawLongBits(value));
    endArg();
  }

  void addArg(String name, String value) {
    int valueLength = value == null ? 0 : value.length();
    int lengthPosition = beginShortArg(name, 2 + valueLength);
    if (lengthPosition >= 0) {
      int position = mPosition;
      mBuffer[position] = TAG_DEBUG_ANNOTATION_STRING_VALUE;
      int end = copyString(value, valueLength, position + 2);
      mBuffer[position + 1] = (byte) (end - position - 2);
      mPosition = end;
      endShortArg(lengthPosition);
      return;
    }
    if (!beginArg(name)) {
      return;
    }
    addString(DEBUG_ANNOTATION_STRING_VALUE, value);
    endArg();
  }

  // Fast path for a short debug arg in one capacity check without touching mOpenMessages.
  // Returns the arg's 1-byte length offset for endShortArg(), or -1 to fall back to beginArg().
  // Layout: arg tag, arg length (1 byte), name tag, name length (1 byte), name, value field.
  private int beginShortArg(String name, int valueBound) {
    int nameLength = name.length(); // Throws NPE for null name, like beginArg().
    int payloadBound = 2 + nameLength + valueBound;
    if (mFailed
        || mOpenCount != 0
        || payloadBound > MAX_ONE_BYTE_LENGTH
        || !fits(2 + payloadBound)) {
      return -1;
    }
    byte[] buffer = mBuffer;
    int position = mPosition;
    buffer[position] = TAG_DEBUG_ANNOTATIONS;
    buffer[position + 2] = TAG_DEBUG_ANNOTATION_NAME;
    int end = copyString(name, nameLength, position + 4);
    buffer[position + 3] = (byte) (end - position - 4);
    mPosition = end;
    return position + 1;
  }

  private void endShortArg(int lengthPosition) {
    mBuffer[lengthPosition] = (byte) (mPosition - lengthPosition - 1);
  }

  private boolean beginArg(String name) {
    Objects.requireNonNull(name);
    checkNestedDepth(0);
    if (mFailed || !beginMessage(TRACK_EVENT_DEBUG_ANNOTATIONS)) {
      return false;
    }
    addString(DEBUG_ANNOTATION_NAME, name);
    return !mFailed;
  }

  private void endArg() {
    endMessage(1);
  }

  // ---------------------------------------------------------------------------
  // Completion
  // ---------------------------------------------------------------------------

  /**
   * Closes any open nested messages, writes the pending counter value, and appends the event name
   * after the fields. Does nothing if the event has already failed.
   */
  void finish(String name) {
    if (mFailed) {
      return;
    }
    while (mOpenCount != 0 && !mFailed) {
      endMessage(mOpenCount);
    }
    writeCounterValue();
    if (mFailed) {
      return;
    }
    mFieldsSize = mPosition;
    if (name != null && !name.isEmpty()) {
      writeName(name);
    }
    mNameSize = mPosition - mFieldsSize;
  }

  // Appends `name` (non-null, non-empty). Keeps the last name up to
  // MAX_CACHED_NAME_SIZE bytes so the same String reference copies in bulk
  // instead of converting char by char.
  private void writeName(String name) {
    if (name == mCachedName) {
      int size = mCachedNameSize;
      if (!fits(size) && !reserve(size)) {
        return;
      }
      System.arraycopy(mCachedNameBytes, 0, mBuffer, mPosition, size);
      mPosition += size;
      return;
    }
    int length = name.length();
    if (!fits(length) && !reserve(stringSize(name))) {
      return;
    }
    int start = mPosition;
    mPosition = copyString(name, length, start);
    int size = mPosition - start;
    if (size <= MAX_CACHED_NAME_SIZE) {
      System.arraycopy(mBuffer, start, mCachedNameBytes, 0, size);
      mCachedName = name;
      mCachedNameSize = size;
    }
  }
}
