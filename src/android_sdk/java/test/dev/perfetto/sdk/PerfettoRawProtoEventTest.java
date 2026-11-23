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

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertThrows;
import static org.junit.Assert.assertTrue;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.io.ByteArrayOutputStream;
import java.util.Arrays;
import org.junit.Test;
import org.junit.runner.RunWith;

/**
 * Byte-level tests for {@link PerfettoRawProtoEvent}, without a tracing session. Each test compares
 * the writer's output with {@link Ref}, a minimal, independent protobuf writer. The tests focus on
 * varint boundaries, length prefixes that grow after the payload is written, buffer growth,
 * open-message nesting rules, and fast paths.
 */
@RunWith(AndroidJUnit4.class)
public class PerfettoRawProtoEventTest {
  // Protobuf wire types.
  private static final int VARINT = 0;
  private static final int FIXED64 = 1;
  private static final int LENGTH_DELIMITED = 2;

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

  // A minimal protobuf writer, independent of the code under test.
  private static final class Ref {
    private final ByteArrayOutputStream mOut = new ByteArrayOutputStream();

    Ref varint(long value) {
      while ((value & ~0x7fL) != 0) {
        mOut.write((int) ((value & 0x7f) | 0x80));
        value >>>= 7;
      }
      mOut.write((int) value);
      return this;
    }

    Ref tag(int field, int wireType) {
      return varint(((long) field << 3) | wireType);
    }

    Ref varintField(int field, long value) {
      return tag(field, VARINT).varint(value);
    }

    Ref fixed64Field(int field, long value) {
      tag(field, FIXED64);
      for (int i = 0; i < 8; i++) {
        mOut.write((int) (value >>> (8 * i)));
      }
      return this;
    }

    Ref bytesField(int field, byte[] payload) {
      tag(field, LENGTH_DELIMITED).varint(payload.length);
      mOut.write(payload, 0, payload.length);
      return this;
    }

    Ref stringField(int field, String value) {
      return bytesField(field, latin1(value));
    }

    Ref raw(byte[] bytes) {
      mOut.write(bytes, 0, bytes.length);
      return this;
    }

    byte[] bytes() {
      return mOut.toByteArray();
    }
  }

  // Matches EXTRAS JNI string conversion: one byte per UTF-16 code unit, '?'
  // above 255, and the string ends at the first NUL.
  private static byte[] latin1(String value) {
    ByteArrayOutputStream out = new ByteArrayOutputStream();
    for (int i = 0; i < value.length(); i++) {
      char c = value.charAt(i);
      if (c == '\0') {
        break;
      }
      out.write(c <= 255 ? c : '?');
    }
    return out.toByteArray();
  }

  private static int varintSize(long value) {
    int size = 1;
    while ((value & ~0x7fL) != 0) {
      value >>>= 7;
      size++;
    }
    return size;
  }

  // A string for field 1 whose whole field (tag, length and payload) is
  // exactly `size` bytes.
  private static String stringOfFieldSize(int size) {
    for (int length = size - 2; length >= 0 && length >= size - 6; length--) {
      if (1 + varintSize(length) + length == size) {
        return "x".repeat(length);
      }
    }
    throw new AssertionError("No string field has size " + size);
  }

  // The debug annotation that addArg() writes: the name, then the value.
  private static byte[] debugAnnotation(String name, Ref value) {
    return new Ref()
        .bytesField(TRACK_EVENT_DEBUG_ANNOTATIONS,
            new Ref().stringField(DEBUG_ANNOTATION_NAME, name).raw(value.bytes()).bytes())
        .bytes();
  }

  // The fields of a finished event.
  private static byte[] fields(PerfettoRawProtoEvent event) {
    assertFalse("The event failed", event.failed());
    return Arrays.copyOf(event.buffer(), event.fieldsSize());
  }

  private static byte[] finishedFields(PerfettoRawProtoEvent event) {
    event.finish(null);
    return fields(event);
  }

  @Test
  public void varintValuesMatchReference() {
    long[] values = {0, 1, 127, 128, 16383, 16384, (1L << 35), -1, Long.MIN_VALUE, Long.MAX_VALUE};
    for (long value : values) {
      PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
      event.addVarint(1, value);
      assertArrayEquals(
          "value " + value, new Ref().varintField(1, value).bytes(), finishedFields(event));
    }
  }

  @Test
  public void fixed64ValuesMatchReference() {
    long[] values = {0, 1, -1, Long.MIN_VALUE, Double.doubleToRawLongBits(1.5)};
    for (long value : values) {
      PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
      event.addFixed64(3, value);
      assertArrayEquals(
          "value " + value, new Ref().fixed64Field(3, value).bytes(), finishedFields(event));
    }
  }

  @Test
  public void tagSizesFollowTheFieldNumber() {
    // Tags of 1, 2, 3, 4 and 5 bytes.
    int[] ids = {1, 15, 16, 2047, 2048, 262143, 262144, 33554431, 33554432, (1 << 29) - 1};
    for (int id : ids) {
      PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
      event.addVarint(id, 7);
      assertArrayEquals("id " + id, new Ref().varintField(id, 7).bytes(), finishedFields(event));
    }
  }

  @Test
  public void upperBitsOfFieldIdsAreIgnored() {
    // Android ProtoStream field ids carry type flags above bit 31.
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.addVarint((1L << 40) | 5, 9);
    assertArrayEquals(new Ref().varintField(5, 9).bytes(), finishedFields(event));
  }

  @Test
  public void invalidFieldNumbersFailTheEvent() {
    long[] ids = {0, -1, 1L << 29, Integer.MAX_VALUE};
    for (long id : ids) {
      PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
      event.addVarint(id, 1);
      assertTrue("id " + id, event.failed());
    }
  }

  @Test
  public void invalidInputThrowsInDebugMode() {
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.setThrowOnError(true);
    assertThrows(IllegalArgumentException.class, () -> event.addVarint(0, 1));
    assertTrue(event.failed());
  }

  @Test
  public void stringsMatchExtrasBytes() {
    String[] values = {
        "", "abc", "\u00e9", "\u4e2d", "ab\0cd", "\0", "x".repeat(127), "x".repeat(128),
        "x".repeat(150) + "\0" + "y".repeat(50), "y".repeat(200) + "\u4e2d",
    };
    for (String value : values) {
      PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
      event.addString(2, value);
      assertArrayEquals("length " + value.length(), new Ref().stringField(2, value).bytes(),
          finishedFields(event));
    }
  }

  @Test
  public void nullStringIsEmpty() {
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.addString(2, null);
    assertArrayEquals(new Ref().stringField(2, "").bytes(), finishedFields(event));
  }

  @Test
  public void nestedLengthsAtVarintBoundaries() {
    // Payloads around the one-byte (127) and two-byte (16383) length limits.
    int[] sizes = {0, 2, 126, 127, 128, 129, 16383, 16384, 16385};
    for (int size : sizes) {
      PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
      Ref payload = new Ref();
      event.beginNested(5);
      if (size > 0) {
        String value = stringOfFieldSize(size);
        event.addString(1, value);
        payload.stringField(1, value);
      }
      event.endNested(1);
      assertEquals(size, payload.bytes().length);
      assertArrayEquals(
          "size " + size, new Ref().bytesField(5, payload.bytes()).bytes(), finishedFields(event));
    }
  }

  @Test
  public void innerAndOuterLengthsBothGrow() {
    // The inner length grows first, then the outer one (two payload shifts).
    String value = "z".repeat(200);
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.beginNested(1);
    event.addVarint(2, 3);
    event.beginNested(4);
    event.addString(5, value);
    event.endNested(2);
    event.addVarint(6, 7);
    event.endNested(1);

    byte[] innerBytes = new Ref().stringField(5, value).bytes();
    byte[] outerBytes =
        new Ref().varintField(2, 3).bytesField(4, innerBytes).varintField(6, 7).bytes();
    assertArrayEquals(new Ref().bytesField(1, outerBytes).bytes(), finishedFields(event));
  }

  @Test
  public void tripleNestingWithInnerGrowth() {
    String value = "q".repeat(300);
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.beginNested(1);
    event.beginNested(2);
    event.beginNested(3);
    event.addString(4, value);
    event.endNested(3);
    event.addVarint(5, 1);
    event.endNested(2);
    event.addVarint(6, 2);
    event.endNested(1);

    byte[] cBytes = new Ref().stringField(4, value).bytes();
    byte[] bBytes = new Ref().bytesField(3, cBytes).varintField(5, 1).bytes();
    byte[] aBytes = new Ref().bytesField(2, bBytes).varintField(6, 2).bytes();
    assertArrayEquals(new Ref().bytesField(1, aBytes).bytes(), finishedFields(event));
  }

  @Test
  public void bufferGrowsWhileAMessageIsOpen() {
    // Larger than the initial array and than the kept size, written inside an
    // open message.
    String value = "g".repeat(PerfettoRawProtoEvent.MAX_KEPT_BUFFER_SIZE + 100);
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.addVarint(1, 1);
    event.beginNested(2);
    event.addString(3, value);
    event.endNested(1);

    byte[] payload = new Ref().stringField(3, value).bytes();
    assertArrayEquals(new Ref().varintField(1, 1).bytesField(2, payload).bytes(),
        finishedFields(event));

    // release() gives the large array back, and the next event starts empty.
    event.release();
    assertEquals(PerfettoRawProtoEvent.INITIAL_BUFFER_SIZE, event.buffer().length);
    event.addVarint(1, 2);
    assertArrayEquals(new Ref().varintField(1, 2).bytes(), finishedFields(event));
  }

  @Test
  public void builtinsAndCustomFieldsAppendInWriteOrder() {
    String value = "c".repeat(200);
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.setCorrelationId(7);
    event.beginNested(100);
    event.addString(1, value);
    event.endNested(1);
    event.setCorrelationId("corr");
    event.addFlowId(-2);
    event.addTerminatingFlowId(3);
    event.addArg("late", 5L);

    byte[] custom = new Ref().bytesField(100, new Ref().stringField(1, value).bytes()).bytes();
    assertArrayEquals(
        new Ref()
            .varintField(TRACK_EVENT_CORRELATION_ID, 7)
            .raw(custom)
            .stringField(TRACK_EVENT_CORRELATION_ID_STR, "corr")
            .fixed64Field(TRACK_EVENT_FLOW_IDS, -2)
            .fixed64Field(TRACK_EVENT_TERMINATING_FLOW_IDS, 3)
            .raw(debugAnnotation("late", new Ref().varintField(DEBUG_ANNOTATION_INT_VALUE, 5)))
            .bytes(),
        finishedFields(event));
  }

  @Test
  public void rootBuiltinsFailWhileNestedMessageIsOpen() {
    Runnable[] rootWrites = {
        () -> {
          PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
          event.beginNested(100);
          event.setCorrelationId(7);
          assertTrue(event.failed());
        },
        () -> {
          PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
          event.beginNested(100);
          event.setCorrelationId("corr");
          assertTrue(event.failed());
        },
        () -> {
          PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
          event.beginNested(100);
          event.addFlowId(1);
          assertTrue(event.failed());
        },
        () -> {
          PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
          event.beginNested(100);
          event.addTerminatingFlowId(2);
          assertTrue(event.failed());
        },
        () -> {
          PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
          event.beginNested(100);
          event.addArg("short", 1L);
          assertTrue(event.failed());
        },
        () -> {
          PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
          event.beginNested(100);
          event.addArg("long_" + "x".repeat(200), 1L);
          assertTrue(event.failed());
        },
    };
    for (Runnable write : rootWrites) {
      write.run();
    }
  }

  @Test
  public void counterValueIsWrittenAtFinishAndLastValueWins() {
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    assertTrue(event.isEmpty());
    event.setCounterValue(1L);
    assertFalse(event.isEmpty());
    event.addVarint(1, 2);
    event.setCounterValue(3.5);
    event.setCounterValue(42L);

    assertArrayEquals(new Ref()
                          .varintField(1, 2)
                          .varintField(TRACK_EVENT_COUNTER_VALUE, 42)
                          .bytes(),
        finishedFields(event));
  }

  @Test
  public void doubleCounterValueIsWrittenAtFinish() {
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.addVarint(100, 1);
    event.setCounterValue(2.5);

    assertArrayEquals(new Ref()
                          .varintField(100, 1)
                          .fixed64Field(TRACK_EVENT_DOUBLE_COUNTER_VALUE,
                              Double.doubleToRawLongBits(2.5))
                          .bytes(),
        finishedFields(event));
  }

  @Test
  public void releaseClearsThePendingCounterValue() {
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.setCounterValue(7L);
    event.release();
    assertTrue(event.isEmpty());
    assertArrayEquals(new byte[0], finishedFields(event));
  }

  @Test
  public void argsMatchReference() {
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.addArg("i", 42L);
    event.addArg("b", true);
    event.addArg("d", 2.5);
    event.addArg("s", "value");

    assertArrayEquals(
        new Ref()
            .raw(debugAnnotation("i", new Ref().varintField(DEBUG_ANNOTATION_INT_VALUE, 42)))
            .raw(debugAnnotation("b", new Ref().varintField(DEBUG_ANNOTATION_BOOL_VALUE, 1)))
            .raw(debugAnnotation("d",
                new Ref().fixed64Field(
                    DEBUG_ANNOTATION_DOUBLE_VALUE, Double.doubleToRawLongBits(2.5))))
            .raw(debugAnnotation(
                "s", new Ref().stringField(DEBUG_ANNOTATION_STRING_VALUE, "value")))
            .bytes(),
        finishedFields(event));
  }

  @Test
  public void stringArgsAroundTheShortPathLimit() {
    // The arg payload is 4 + name + value bytes. 127 takes the short path; 128
    // takes the general path and a two-byte length.
    String name = "abc";
    for (int payloadSize : new int[] {126, 127, 128, 129}) {
      String value = "v".repeat(payloadSize - 4 - name.length());
      PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
      event.addArg(name, value);
      byte[] expected =
          debugAnnotation(name, new Ref().stringField(DEBUG_ANNOTATION_STRING_VALUE, value));
      assertArrayEquals("payload " + payloadSize, expected, finishedFields(event));
    }
  }

  @Test
  public void intArgsAroundTheShortPathLimit() {
    // The short path reserves 11 bytes for an int value, so it takes names of up
    // to 114 chars. -1 uses all 10 varint bytes: a payload of exactly 127, then 128.
    for (int nameLength : new int[] {113, 114, 115, 116}) {
      String name = "n".repeat(nameLength);
      PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
      event.addArg(name, -1L);
      byte[] expected =
          debugAnnotation(name, new Ref().varintField(DEBUG_ANNOTATION_INT_VALUE, -1));
      assertArrayEquals("name length " + nameLength, expected, finishedFields(event));
    }
  }

  @Test
  public void nameFollowsTheFields() {
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.addVarint(1, 1);
    event.finish("ev\u00e9nt\u4e2d\0tail");

    byte[] fields = new Ref().varintField(1, 1).bytes();
    byte[] name = latin1("ev\u00e9nt\u4e2d");
    assertEquals(fields.length, event.fieldsSize());
    assertEquals(name.length, event.nameSize());
    assertEquals(fields.length + name.length, event.size());
    assertArrayEquals(fields, fields(event));
    assertArrayEquals(name,
        Arrays.copyOfRange(event.buffer(), event.fieldsSize(), event.size()));
  }

  @Test
  public void unnamedEventsHaveNoName() {
    for (String name : new String[] {null, ""}) {
      PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
      event.addVarint(1, 1);
      event.finish(name);
      assertEquals(0, event.nameSize());
      assertEquals(event.fieldsSize(), event.size());
    }
  }

  private static byte[] finishedName(PerfettoRawProtoEvent event) {
    assertFalse("The event failed", event.failed());
    return Arrays.copyOfRange(event.buffer(), event.fieldsSize(), event.size());
  }

  @Test
  public void repeatedNamesMatchReference() {
    // Each name runs twice so the second event hits the 1-entry name cache when
    // it fits MAX_CACHED_NAME_SIZE.
    String[] names = {
        "event", "ev\u00e9nt\u4e2d\0tail", "\0", "x".repeat(128), "y".repeat(129), "event",
    };
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    for (String name : names) {
      for (int i = 0; i < 2; i++) {
        event.addVarint(1, i);
        event.finish(name);
        assertArrayEquals(new Ref().varintField(1, i).bytes(), fields(event));
        assertArrayEquals("name length " + name.length(), latin1(name), finishedName(event));
        event.release();
      }
    }

    // An unnamed event keeps the cached name for the next event.
    event.finish(null);
    event.release();
    event.finish(names[0]);
    assertArrayEquals(latin1(names[0]), finishedName(event));
  }

  @Test
  public void cachedNameGrowsTheArray() {
    String name = "n".repeat(100);
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.finish(name);
    event.release();

    // Fill the initial buffer so appending the cached name has to grow it.
    String value = stringOfFieldSize(PerfettoRawProtoEvent.INITIAL_BUFFER_SIZE);
    event.addString(1, value);
    assertEquals(PerfettoRawProtoEvent.INITIAL_BUFFER_SIZE, event.size());
    event.finish(name);
    assertArrayEquals(new Ref().stringField(1, value).bytes(), fields(event));
    assertArrayEquals(latin1(name), finishedName(event));
  }

  @Test
  public void finishClosesOpenMessagesBeforeWritingCounter() {
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.beginNested(1);
    event.beginNested(2);
    event.addVarint(3, 4);
    event.setCounterValue(9L);
    event.finish("n");

    byte[] inner = new Ref().varintField(3, 4).bytes();
    byte[] outer = new Ref().bytesField(2, inner).bytes();
    assertArrayEquals(
        new Ref()
            .bytesField(1, outer)
            .varintField(TRACK_EVENT_COUNTER_VALUE, 9L)
            .bytes(),
        fields(event));
  }

  @Test
  public void nestingStopsAtTheDepthLimit() {
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    for (int i = 0; i < PerfettoRawProtoEvent.MAX_NESTING_DEPTH; i++) {
      event.beginNested(1);
    }
    assertFalse(event.failed());
    event.beginNested(1);
    assertTrue(event.failed());
  }

  @Test
  public void nestedMessagesAtTheDepthLimitMatchReference() {
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    for (int i = 0; i < PerfettoRawProtoEvent.MAX_NESTING_DEPTH; i++) {
      event.beginNested(1);
    }
    event.addVarint(2, 3);
    for (int depth = PerfettoRawProtoEvent.MAX_NESTING_DEPTH; depth >= 1; depth--) {
      event.endNested(depth);
    }

    byte[] expected = new Ref().varintField(2, 3).bytes();
    for (int i = 0; i < PerfettoRawProtoEvent.MAX_NESTING_DEPTH; i++) {
      expected = new Ref().bytesField(1, expected).bytes();
    }
    assertArrayEquals(expected, finishedFields(event));
  }

  @Test
  public void nestedMessagesMustBeClosedInOrder() {
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.beginNested(1);
    event.beginNested(2);
    event.endNested(1);
    assertTrue(event.failed());
  }

  @Test
  public void writesToAParentWhileAChildIsOpenFail() {
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.beginNested(1);
    event.checkNestedDepth(0);
    assertTrue(event.failed());
  }

  @Test
  public void oversizedEventsFailAndTheEventCanBeReused() {
    PerfettoRawProtoEvent event = new PerfettoRawProtoEvent();
    event.addString(1, "x".repeat(PerfettoRawProtoEvent.MAX_EVENT_SIZE));
    assertTrue(event.failed());
    assertFalse(event.isEmpty());

    // A failed event ignores later writes.
    event.addVarint(1, 1);
    assertTrue(event.failed());

    event.release();
    assertFalse(event.failed());
    assertTrue(event.isEmpty());
    event.addVarint(1, 1);
    assertArrayEquals(new Ref().varintField(1, 1).bytes(), finishedFields(event));
  }
}
