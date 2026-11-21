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

import java.lang.reflect.Method;
import java.util.Arrays;
import java.util.Objects;

/** Encodes protobuf fields; only explicit interning needs a slot trailer. */
final class PerfettoEncodedEvent {
  static final int INITIAL_BUFFER_SIZE = 256;
  static final int MAX_KEPT_BUFFER_SIZE = 16 * 1024;
  static final int MAX_EVENT_SIZE = 1024 * 1024;
  static final int IID_SLOT_SIZE = 10;
  static final int SLOT_RECORD_SIZE = 16;
  static final int MAX_NESTING_DEPTH = 64;

  private byte[] mBuffer;
  private long mBufferAddress;
  private int mPosition;
  private int mFieldsSize;
  private int mNameSize;
  private int mSlotsOffset;
  private int mSlotCount;
  private int mTrailerSize;
  private boolean mFailed;
  private int[] mSlotOffsets = new int[8];
  private int[] mSlotTypes = new int[8];
  private int[] mSlotStringOffsets = new int[8];
  private String[] mSlotStrings = new String[8];
  private int[] mOpenMessages = new int[8];
  private int mOpenCount;
  private int mCustomStart = -1;

  PerfettoEncodedEvent() {
    replaceBuffer(INITIAL_BUFFER_SIZE);
  }

  void reset() {
    Arrays.fill(mSlotStrings, 0, mSlotCount, null);
    mPosition = 0;
    mFieldsSize = 0;
    mNameSize = 0;
    mSlotsOffset = 0;
    mSlotCount = 0;
    mTrailerSize = 0;
    mFailed = false;
    mOpenCount = 0;
    mCustomStart = -1;
  }

  void release() {
    reset();
    if (mBuffer.length > MAX_KEPT_BUFFER_SIZE) {
      replaceBuffer(INITIAL_BUFFER_SIZE);
    }
    // A single large event must not permanently retain its slot metadata.
    if (mSlotOffsets.length > 256) {
      mSlotOffsets = new int[8];
      mSlotTypes = new int[8];
      mSlotStringOffsets = new int[8];
      mSlotStrings = new String[8];
    }
    if (mOpenMessages.length > 256)
      mOpenMessages = new int[8];
  }

  byte[] buffer() {
    return mBuffer;
  }
  long bufferAddress() {
    return mBufferAddress;
  }
  int size() {
    return mPosition;
  }
  int fieldsSize() {
    return mFieldsSize;
  }
  int nameSize() {
    return mNameSize;
  }
  int slotsOffset() {
    return mSlotsOffset;
  }
  int slotCount() {
    return mSlotCount;
  }

  // Keep the legacy JNI's string conversion: one byte per UTF-16 code unit,
  // values above 255 become '?', and C-string consumers stop at the first NUL.
  // Changing the encoding requires a separate caller-visible migration.
  private static int stringSize(String value) {
    if (value == null)
      return 0;
    int zero = value.indexOf('\0');
    return zero < 0 ? value.length() : zero;
  }

  private int copyString(String value, int size, int position) {
    for (int i = 0; i < size; ++i) {
      char c = value.charAt(i);
      mBuffer[position++] = (byte) (c <= 255 ? c : '?');
    }
    return position;
  }

  private void reserve(int count) {
    if (mFailed) {
      throw new IllegalArgumentException("Encoded event construction already failed");
    }
    if (count < 0 || count > MAX_EVENT_SIZE - mPosition - mTrailerSize) {
      mFailed = true;
      throw new IllegalArgumentException("Encoded event exceeds the 1 MiB transport limit");
    }
    ensureCapacity(mPosition + count + mTrailerSize);
  }

  // All buffer allocation goes through this method so the Android transport
  // can choose non-moving storage without changing the protobuf encoder.
  private void ensureCapacity(int required) {
    if (required > mBuffer.length) {
      int capacity = Math.min(MAX_EVENT_SIZE, Math.max(required, mBuffer.length * 2));
      replaceBuffer(capacity);
    }
  }

  // Callers serialize use of a builder, including a handoff to another thread.
  // Publish the replacement only after its stable address and contents agree.
  private void replaceBuffer(int capacity) {
    byte[] replacement = null;
    long address = 0;
    if (capacity <= MAX_KEPT_BUFFER_SIZE && NonMovingAllocator.sEnabled) {
      try {
        replacement = (byte[]) NonMovingAllocator.sNewArray.invoke(
            NonMovingAllocator.sRuntime, byte.class, capacity);
        address = (Long) NonMovingAllocator.sAddressOf.invoke(
            NonMovingAllocator.sRuntime, replacement);
        if (address == 0) {
          replacement = null;
          NonMovingAllocator.sEnabled = false;
        }
      } catch (ReflectiveOperationException | RuntimeException | LinkageError failure) {
        NonMovingAllocator.sEnabled = false;
        replacement = null;
        address = 0;
      }
    }
    if (replacement == null) replacement = new byte[capacity];
    if (mBuffer != null) System.arraycopy(mBuffer, 0, replacement, 0, mPosition);
    mBuffer = replacement;
    mBufferAddress = address;
  }

  // VMRuntime's hidden API is available only to the Android platform copy of
  // the SDK. Ordinary class loaders and host JVMs keep the portable transport.
  private static final class NonMovingAllocator {
    static final Object sRuntime;
    static final Method sNewArray;
    static final Method sAddressOf;
    static volatile boolean sEnabled;

    static {
      Object runtime = null;
      Method newArray = null;
      Method addressOf = null;
      try {
        if ("Dalvik".equals(System.getProperty("java.vm.name"))
            && PerfettoEncodedEvent.class.getClassLoader() == Object.class.getClassLoader()) {
          Class<?> type = Class.forName("dalvik.system.VMRuntime");
          runtime = type.getMethod("getRuntime").invoke(null);
          newArray = type.getMethod("newNonMovableArray", Class.class, int.class);
          addressOf = type.getMethod("addressOf", Object.class);
        }
      } catch (ReflectiveOperationException | RuntimeException | LinkageError unavailable) {
        runtime = null;
      }
      sRuntime = runtime;
      sNewArray = newArray;
      sAddressOf = addressOf;
      sEnabled = runtime != null;
    }
  }

  private static int fieldNumber(long id) {
    // Android ProtoStream field IDs carry type/count flags in the upper bits.
    int number = (int) id;
    if (number <= 0 || number > 0x1fffffff) {
      throw new IllegalArgumentException("Invalid protobuf field number");
    }
    return number;
  }

  private void tag(long id, int wireType) {
    varint(Integer.toUnsignedLong((fieldNumber(id) << 3) | wireType));
  }

  private void varint(long value) {
    int count = 1;
    for (long rest = value; (rest & ~0x7fL) != 0; rest >>>= 7) ++count;
    reserve(count);
    while ((value & ~0x7fL) != 0) {
      mBuffer[mPosition++] = (byte) ((value & 0x7f) | 0x80);
      value >>>= 7;
    }
    mBuffer[mPosition++] = (byte) value;
  }

  void addVarint(long id, long value) {
    tag(id, 0);
    varint(value);
  }

  void addFixed64(long id, long value) {
    tag(id, 1);
    reserve(8);
    for (int i = 0; i < 8; ++i) {
      mBuffer[mPosition++] = (byte) value;
      value >>>= 8;
    }
  }

  void addString(long id, String value) {
    int size = stringSize(value);
    // Check before writing the tag/length or allocating payload storage.
    reserve(size + 10);
    tag(id, 2);
    varint(size);
    mPosition = copyString(value, size, mPosition);
  }

  int beginNested(long id) {
    if (mOpenCount >= MAX_NESTING_DEPTH) {
      mFailed = true;
      throw new IllegalArgumentException("Encoded proto nesting exceeds 64 messages");
    }
    return beginMessage(id);
  }

  private int beginMessage(long id) {
    tag(id, 2);
    reserve(1);
    mBuffer[mPosition++] = 0;
    if (mOpenCount == mOpenMessages.length) {
      mOpenMessages = Arrays.copyOf(mOpenMessages, mOpenCount * 2);
    }
    int token = mOpenCount++;
    mOpenMessages[token] = mPosition;
    return token;
  }

  void checkNestedDepth(int expected) {
    if (mOpenCount != expected) {
      mFailed = true;
      throw new IllegalStateException("End nested proto fields before writing to their parent");
    }
  }

  void endNested(int token) {
    if (mOpenCount == 0 || token != mOpenCount - 1) {
      mFailed = true;
      throw new IllegalStateException("Nested fields must be closed in order");
    }
    int payload = mOpenMessages[--mOpenCount];
    int size = mPosition - payload;
    if (size < 128) {
      mBuffer[payload - 1] = (byte) size;
      return;
    }
    int extra = 0;
    for (int n = size; n >= 128; n >>>= 7) ++extra;
    reserve(extra);
    System.arraycopy(mBuffer, payload, mBuffer, payload + extra, size);
    int position = payload - 1;
    int remaining = size;
    while (remaining >= 128) {
      mBuffer[position++] = (byte) ((remaining & 0x7f) | 0x80);
      remaining >>>= 7;
    }
    mBuffer[position] = (byte) remaining;
    mPosition += extra;
    for (int i = mSlotCount - 1; i >= 0 && mSlotOffsets[i] >= payload; --i) {
      mSlotOffsets[i] += extra;
    }
  }

  void addInternedString(long id, String value, long type) {
    // Existing callers use zero to suppress this optional field.
    if ((int) type == 0)
      return;
    int table = fieldNumber(type);
    int size = stringSize(value);
    reserve(size + SLOT_RECORD_SIZE + IID_SLOT_SIZE + 5);
    if (mSlotCount == mSlotOffsets.length) {
      int capacity = mSlotCount * 2;
      mSlotOffsets = Arrays.copyOf(mSlotOffsets, capacity);
      mSlotTypes = Arrays.copyOf(mSlotTypes, capacity);
      mSlotStringOffsets = Arrays.copyOf(mSlotStringOffsets, capacity);
      mSlotStrings = Arrays.copyOf(mSlotStrings, capacity);
    }
    tag(id, 0);
    mSlotOffsets[mSlotCount] = mPosition;
    mSlotTypes[mSlotCount] = table;
    mSlotStrings[mSlotCount++] = value;
    // Reserve enough bytes for any uint64 IID. Native emission writes the
    // canonical varint and adjusts enclosing message lengths for this sequence.
    for (int i = 0; i < IID_SLOT_SIZE - 1; ++i) mBuffer[mPosition++] = (byte) 0x80;
    mBuffer[mPosition++] = 0;
    mTrailerSize += size + SLOT_RECORD_SIZE;
  }

  void beginProto() {
    if (mCustomStart < 0)
      mCustomStart = mPosition;
  }

  void addBuiltinVarint(long id, long value) {
    int start = mPosition;
    addVarint(id, value);
    moveBuiltinBeforeCustom(start);
  }

  void addBuiltinFixed64(long id, long value) {
    int start = mPosition;
    addFixed64(id, value);
    moveBuiltinBeforeCustom(start);
  }

  void addBuiltinString(long id, String value) {
    int start = mPosition;
    addString(id, value);
    moveBuiltinBeforeCustom(start);
  }

  // Legacy writes custom proto fields last, even if a built-in setter follows
  // endProto(). Rotate that uncommon late built-in before the custom bytes.
  // The usual built-ins-before-proto order needs no movement or second buffer.
  private void moveBuiltinBeforeCustom(int start) {
    if (mCustomStart < 0)
      return;
    int size = mPosition - start;
    reverseBytes(mCustomStart, start);
    reverseBytes(start, mPosition);
    reverseBytes(mCustomStart, mPosition);
    // Built-ins contain no IID slots. Only the custom slots shift, and their
    // order stays the same.
    for (int i = 0; i < mSlotCount; ++i) {
      if (mSlotOffsets[i] >= mCustomStart) {
        mSlotOffsets[i] += size;
      }
    }
    for (int i = 0; i < mOpenCount; ++i) {
      if (mOpenMessages[i] >= mCustomStart)
        mOpenMessages[i] += size;
    }
    mCustomStart += size;
  }

  private void reverseBytes(int begin, int end) {
    while (begin < --end) {
      byte value = mBuffer[begin];
      mBuffer[begin++] = mBuffer[end];
      mBuffer[end] = value;
    }
  }

  private int beginArg(String name) {
    Objects.requireNonNull(name);
    // A root debug arg may be added while custom nested fields are open. It
    // will move before them, so its actual protobuf depth is always one.
    int payload = beginMessage(4); // TrackEvent.debug_annotations.
    addString(10, name); // DebugAnnotation.name.
    return payload;
  }

  void addArg(String name, long value) {
    int start = mPosition;
    int payload = beginArg(name);
    addVarint(4, value);
    endNested(payload);
    moveBuiltinBeforeCustom(start);
  }

  void addArg(String name, boolean value) {
    int start = mPosition;
    int payload = beginArg(name);
    addVarint(2, value ? 1 : 0);
    endNested(payload);
    moveBuiltinBeforeCustom(start);
  }

  void addArg(String name, double value) {
    int start = mPosition;
    int payload = beginArg(name);
    addFixed64(5, Double.doubleToRawLongBits(value));
    endNested(payload);
    moveBuiltinBeforeCustom(start);
  }

  void addArg(String name, String value) {
    int start = mPosition;
    int payload = beginArg(name);
    addString(6, value);
    endNested(payload);
    moveBuiltinBeforeCustom(start);
  }

  void finish(String name) {
    while (mOpenCount != 0) endNested(mOpenCount - 1);
    mFieldsSize = mPosition;
    mNameSize = stringSize(name);
    reserve(mNameSize);
    mPosition = copyString(name, mNameSize, mPosition);
    for (int i = 0; i < mSlotCount; ++i) {
      mSlotStringOffsets[i] = mPosition;
      mPosition = copyString(mSlotStrings[i], stringSize(mSlotStrings[i]), mPosition);
    }
    mSlotsOffset = mPosition;
    for (int i = 0; i < mSlotCount; ++i) {
      putInt(mSlotOffsets[i]);
      putInt(mSlotTypes[i]);
      putInt(mSlotStringOffsets[i]);
      putInt(stringSize(mSlotStrings[i]));
    }
  }

  private void putInt(int value) {
    for (int i = 0; i < 4; ++i) {
      mBuffer[mPosition++] = (byte) value;
      value >>>= 8;
    }
  }
}
