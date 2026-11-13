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

package dev.perfetto.sdk.test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertThrows;
import static org.junit.Assert.assertTrue;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import com.google.protobuf.ByteString;
import com.google.protobuf.UnknownFieldSet;
import dev.perfetto.sdk.PerfettoTrace;
import dev.perfetto.sdk.PerfettoTrace.Category;
import dev.perfetto.sdk.PerfettoTrack;
import dev.perfetto.sdk.PerfettoTrackEventBuilder;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import perfetto.protos.DataSourceConfigOuterClass.DataSourceConfig;
import perfetto.protos.DebugAnnotationOuterClass.DebugAnnotation;
import perfetto.protos.InternedDataOuterClass.InternedData;
import perfetto.protos.TraceConfigOuterClass.TraceConfig;
import perfetto.protos.TraceOuterClass.Trace;
import perfetto.protos.TracePacketOuterClass.TracePacket;
import perfetto.protos.TrackEventConfigOuterClass.TrackEventConfig;
import perfetto.protos.TrackEventConfigOuterClass.TrackEventConfig.JavaEventEncoding;
import perfetto.protos.TrackEventOuterClass.TrackEvent;

/** Decoded-value comparisons for the Java encoder and per-recording selection. */
@RunWith(AndroidJUnit4.class)
public class PerfettoEncodingModeTest {
  private static final JavaEventEncoding LEGACY = JavaEventEncoding.JAVA_EVENT_ENCODING_LEGACY;
  private static final JavaEventEncoding ENCODED = JavaEventEncoding.JAVA_EVENT_ENCODING_ENCODED;
  private static final Category CATEGORY = new Category("java_encoding_test");
  private static final String PADDING = "x".repeat(17000);

  @Before
  public void setUp() {
    PerfettoTrace.registerWithDebugChecks(true);
    CATEGORY.register();
  }

  private static TraceConfig config(JavaEventEncoding mode, boolean v2) {
    TrackEventConfig te = TrackEventConfig.newBuilder()
                              .addEnabledCategories(CATEGORY.getName())
                              .setDisableIncrementalTimestamps(true)
                              .setExperimentalJavaEventEncoding(mode)
                              .build();
    DataSourceConfig ds =
        DataSourceConfig.newBuilder()
            .setName("track_event")
            .setTrackEventConfig(te)
            .setExperimentalTracingV2(DataSourceConfig.ExperimentalTracingV2Config.newBuilder()
                    .setUseV2ProbabilityPercent(v2 ? 100 : 0))
            .build();
    TraceConfig.BufferConfig.Builder buffer = TraceConfig.BufferConfig.newBuilder().setSizeKb(4096);
    if (v2)
      buffer.setExperimentalMode(TraceConfig.BufferConfig.ExperimentalMode.TRACE_BUFFER_V2);
    return TraceConfig.newBuilder()
        .addBuffers(buffer)
        .addDataSources(TraceConfig.DataSource.newBuilder().setConfig(ds))
        .build();
  }

  private static PerfettoTrace.Session start(JavaEventEncoding mode, boolean v2) {
    return new PerfettoTrace.Session(true, config(mode, v2).toByteArray());
  }

  private static void job(PerfettoTrackEventBuilder event, String name) {
    // FrameworksBaseTrackEvent.job_scheduler_job (2006), job_name_iid (21),
    // FrameworksBaseInternedData.android_job_name (44). Keep the real schema IDs.
    event.beginProto()
        .beginNested(2006)
        .addField(2, PADDING)
        .addFieldWithInterning(21, name, 44)
        .endNested()
        .endProto();
  }

  private static void emitCoverage() {
    PerfettoTrackEventBuilder event =
        PerfettoTrace.instant(CATEGORY, "all_fields")
            .addArg("bool", true)
            .addArg("integer", -42L)
            .addArg("double", 1.5)
            .addArg("string", "value")
            .addFlow(7)
            .addFlow(8)
            .addTerminatingFlow(9)
            .usingNamedTrack(42, "named", PerfettoTrace.getProcessTrackUuid());
    job(event, "android-job");
    // The late inline debug name has to move before custom proto fields,
    // including the nested IID slot whose length prefix grew twice.
    event.addArg("after_proto", 123L)
        .setCorrelationId("request")
        .beginProto()
        .addField(1000, -1L)
        .addField(1000, 42L)
        .addField(1001, 3.25)
        .addField(1002, "proto-string")
        .addField(1003, true)
        .endProto()
        .emit();
    PerfettoTrace.begin(CATEGORY, "slice").emit();
    PerfettoTrace.end(CATEGORY).emit();
    PerfettoTrace.counter(CATEGORY, -4L, "counter").emit();
    PerfettoTrace.counter(CATEGORY, 2.5, "counter").emit();
    PerfettoTrace.state(CATEGORY, "active", "state").emit();
    PerfettoTrace.instant(CATEGORY, "nested_track")
        .usingTrack(PerfettoTrack.process("parent").child("child"))
        .emit();
    PerfettoTrace.instant(CATEGORY, "dynamic_track")
        .usingNamedTrackWithDynamicName(9, "dynamic", PerfettoTrace.getProcessTrackUuid())
        .setCorrelationId(123L)
        .emit();
    // Custom fields override built-ins whether the built-in setter came first or last.
    PerfettoTrace.counter(CATEGORY, 1L, "counter")
        .beginProto()
        .addField(30, 7L)
        .endProto()
        .setCounter(2L)
        .emit();
    PerfettoTrace.counter(CATEGORY, 2L, "counter").beginProto().addField(30, 7L).endProto().emit();
  }

  private static final class Sequence {
    final Map<Long, ByteString> names = new HashMap<>();
    final Map<Long, ByteString> categories = new HashMap<>();
    final Map<Long, ByteString> debugNames = new HashMap<>();
    final Map<Long, ByteString> jobs = new HashMap<>();
  }

  private static final class Decoded {
    final List<TrackEvent> events = new ArrayList<>();
    final List<Long> timestamps = new ArrayList<>();
    final List<Long> jobIids = new ArrayList<>();
    final List<ByteString> jobs = new ArrayList<>();
  }

  private static ByteString resolve(Map<Long, ByteString> table, long iid) {
    ByteString value = table.get(iid);
    assertNotNull("No definition for IID " + iid, value);
    return value;
  }

  private static Decoded decode(byte[] bytes) throws Exception {
    Decoded result = new Decoded();
    Map<Integer, Sequence> sequences = new HashMap<>();
    for (TracePacket packet : Trace.parseFrom(bytes).getPacketList()) {
      int id = packet.getTrustedPacketSequenceId();
      if ((packet.getSequenceFlags() & 1) != 0)
        sequences.remove(id);
      Sequence sequence = sequences.computeIfAbsent(id, ignored -> new Sequence());
      if (packet.hasInternedData()) {
        InternedData data = packet.getInternedData();
        data.getEventNamesList().forEach(v -> sequence.names.put(v.getIid(), v.getNameBytes()));
        data.getEventCategoriesList().forEach(
            v -> sequence.categories.put(v.getIid(), v.getNameBytes()));
        data.getDebugAnnotationNamesList().forEach(
            v -> sequence.debugNames.put(v.getIid(), v.getNameBytes()));
        for (ByteString job : data.getUnknownFields().getField(44).getLengthDelimitedList()) {
          UnknownFieldSet fields = UnknownFieldSet.parseFrom(job);
          sequence.jobs.put(fields.getField(1).getVarintList().get(0),
              fields.getField(2).getLengthDelimitedList().get(0));
        }
      }
      if (!packet.hasTrackEvent())
        continue;
      TrackEvent event = packet.getTrackEvent();
      TrackEvent.Builder canonical = event.toBuilder();
      if (event.hasNameIid()) {
        canonical.clearNameIid().setNameBytes(resolve(sequence.names, event.getNameIid()));
      }
      canonical.clearCategoryIids();
      for (long iid : event.getCategoryIidsList()) {
        canonical.addCategoriesBytes(resolve(sequence.categories, iid));
      }
      for (int i = 0; i < event.getDebugAnnotationsCount(); ++i) {
        DebugAnnotation arg = event.getDebugAnnotations(i);
        if (arg.hasNameIid()) {
          canonical.setDebugAnnotations(i,
              arg.toBuilder().clearNameIid().setNameBytes(
                  resolve(sequence.debugNames, arg.getNameIid())));
        }
      }
      UnknownFieldSet.Field.Builder canonicalJobs = UnknownFieldSet.Field.newBuilder();
      for (ByteString job : event.getUnknownFields().getField(2006).getLengthDelimitedList()) {
        UnknownFieldSet fields = UnknownFieldSet.parseFrom(job);
        long iid = fields.getField(21).getVarintList().get(0);
        ByteString name = resolve(sequence.jobs, iid);
        result.jobIids.add(iid);
        result.jobs.add(name);
        // Compare the resolved value, not a packet-sequence-local numeric ID.
        canonicalJobs.addLengthDelimited(fields.toBuilder()
                .clearField(21)
                .addField(21, UnknownFieldSet.Field.newBuilder().addLengthDelimited(name).build())
                .build()
                .toByteString());
      }
      if (!event.getUnknownFields().getField(2006).getLengthDelimitedList().isEmpty()) {
        canonical.setUnknownFields(event.getUnknownFields()
                .toBuilder()
                .clearField(2006)
                .addField(2006, canonicalJobs.build())
                .build());
      }
      result.events.add(canonical.build());
      result.timestamps.add(packet.getTimestamp());
    }
    return result;
  }

  @Test
  public void unregisteredCategoryDropsAnAlreadyBuiltEventInEveryMode() throws Exception {
    JavaEventEncoding[][] selections = {{LEGACY}, {ENCODED}, {LEGACY, ENCODED}};
    for (JavaEventEncoding[] modes : selections) {
      List<PerfettoTrace.Session> sessions = new ArrayList<>();
      for (JavaEventEncoding mode : modes) sessions.add(start(mode, false));
      PerfettoTrackEventBuilder event =
          PerfettoTrace.instant(CATEGORY, "before_unregister").addArg("value", 1L);
      CATEGORY.unregister();
      event.emit();
      CATEGORY.register();
      PerfettoTrace.instant(CATEGORY, "after_register").addArg("value", 2L).emit();
      for (PerfettoTrace.Session session : sessions) {
        Decoded trace = decode(session.close());
        assertEquals(1, trace.events.size());
        assertEquals("after_register", trace.events.get(0).getName());
      }
    }
  }

  @Test
  public void allFieldsMatchLegacyOnBothWriters() throws Exception {
    for (boolean v2 : new boolean[] {false, true}) {
      PerfettoTrace.Session legacy = start(LEGACY, v2);
      emitCoverage();
      Decoded expected = decode(legacy.close());
      PerfettoTrace.Session encoded = start(ENCODED, v2);
      emitCoverage();
      Decoded actual = decode(encoded.close());
      assertEquals(10, actual.events.size());
      assertEquals(expected.events, actual.events);
      assertEquals(List.of(ByteString.copyFromUtf8("android-job")), actual.jobs);
      assertEquals(7L, actual.events.get(8).getCounterValue());
      assertEquals(7L, actual.events.get(9).getCounterValue());
    }
  }

  @Test
  public void rootBuiltinsCanBeAddedWhileNestedFieldsAreOpen() throws Exception {
    List<TrackEvent> expected = null;
    for (JavaEventEncoding mode : new JavaEventEncoding[] {LEGACY, ENCODED}) {
      PerfettoTrace.Session session = start(mode, false);
      PerfettoTrackEventBuilder root = PerfettoTrace.instant(CATEGORY, "open_nested");
      PerfettoTrackEventBuilder nested = root.beginProto().beginNested(2006);
      nested.addField(2, PADDING).addFieldWithInterning(21, "nested-job", 44);
      // The inline name grows this debug annotation's length prefix too.
      root.addArg("late_root_arg_" + "x".repeat(200), 17L).setCorrelationId("root");
      nested.endNested().endProto().emit();
      Decoded actual = decode(session.close());
      assertEquals(1, actual.events.size());
      if (expected == null)
        expected = actual.events;
      else
        assertEquals(expected, actual.events);
    }
  }

  @Test
  public void mixedModesEmitOnceWithTheSameTimestamp() throws Exception {
    for (boolean v2 : new boolean[] {false, true}) {
      PerfettoTrace.Session legacy = start(LEGACY, v2);
      PerfettoTrace.Session encoded = start(ENCODED, v2);
      emitCoverage();
      Decoded a = decode(legacy.close());
      Decoded b = decode(encoded.close());
      assertEquals(10, a.events.size());
      assertEquals(a.events, b.events);
      assertEquals(a.timestamps, b.timestamps);
    }
  }

  @Test
  public void encodedNamesAreInlineAndOnlyExplicitFieldsAreInterned() throws Exception {
    for (boolean v2 : new boolean[] {false, true}) {
      PerfettoTrace.Session legacy = start(LEGACY, v2);
      PerfettoTrace.Session encoded = start(ENCODED, v2);
      PerfettoTrace.instant(CATEGORY, "ordinary").addArg("key", "value").emit();
      PerfettoTrackEventBuilder explicit = PerfettoTrace.instant(CATEGORY, "explicit");
      job(explicit, "android-job");
      explicit.addArg("late", 42L).emit();
      byte[] legacyBytes = legacy.close();
      byte[] encodedBytes = encoded.close();
      assertEquals(decode(legacyBytes).events, decode(encodedBytes).events);
      int events = 0;
      int jobs = 0;
      for (TracePacket packet : Trace.parseFrom(encodedBytes).getPacketList()) {
        InternedData data = packet.getInternedData();
        assertEquals(0, data.getEventNamesCount());
        assertEquals(0, data.getEventCategoriesCount());
        assertEquals(0, data.getDebugAnnotationNamesCount());
        jobs += data.getUnknownFields().getField(44).getLengthDelimitedList().size();
        if (!packet.hasTrackEvent()) continue;
        TrackEvent event = packet.getTrackEvent();
        assertFalse(event.hasNameIid());
        assertEquals(events++ == 0 ? "ordinary" : "explicit", event.getName());
        assertEquals(0, event.getCategoryIidsCount());
        assertEquals(List.of(CATEGORY.getName()), event.getCategoriesList());
        assertEquals(1, event.getDebugAnnotationsCount());
        assertTrue(event.getDebugAnnotations(0).hasName());
        assertFalse(event.getDebugAnnotations(0).hasNameIid());
        if (event.getName().equals("ordinary")) assertFalse(packet.hasInternedData());
      }
      assertEquals(2, events);
      assertEquals(1, jobs);
      // The concurrently active legacy recording retains its original wire format.
      for (TracePacket packet : Trace.parseFrom(legacyBytes).getPacketList()) {
        if (!packet.hasTrackEvent()) continue;
        TrackEvent event = packet.getTrackEvent();
        assertTrue(event.hasNameIid());
        assertEquals(1, event.getCategoryIidsCount());
        assertTrue(event.getDebugAnnotations(0).hasNameIid());
      }
    }
  }

  @Test
  public void jobInterningUsesEachSessionsOwnIds() throws Exception {
    for (boolean v2 : new boolean[] {false, true}) {
      PerfettoTrace.Session a = start(ENCODED, v2);
      PerfettoTrackEventBuilder seed = PerfettoTrace.instant(CATEGORY, "seed");
      job(seed, "seed-job");
      seed.emit();
      PerfettoTrace.Session b = start(ENCODED, v2);
      PerfettoTrackEventBuilder event = PerfettoTrace.instant(CATEGORY, "common");
      job(event, "shared-job");
      event.emit();
      Decoded first = decode(a.close());
      Decoded second = decode(b.close());
      assertEquals(2, first.events.size());
      assertEquals(1, second.events.size());
      assertEquals(first.events.get(1), second.events.get(0));
      assertNotEquals(first.jobIids.get(1), second.jobIids.get(0));
      assertEquals(ByteString.copyFromUtf8("shared-job"), second.jobs.get(0));
      // A new sequence must emit definitions again despite builder/cache reuse.
      PerfettoTrace.Session reset = start(ENCODED, v2);
      PerfettoTrackEventBuilder again = PerfettoTrace.instant(CATEGORY, "common");
      job(again, "shared-job");
      again.emit();
      assertEquals(second.events, decode(reset.close()).events);
    }
  }

  @Test
  public void builderModesStayFixedAcrossSessionChanges() throws Exception {
    PerfettoTrace.Session a = start(LEGACY, false);
    PerfettoTrackEventBuilder old = PerfettoTrace.instant(CATEGORY, "old").addArg("value", 1L);
    PerfettoTrace.Session b = start(ENCODED, false);
    old.emit(); // ENCODED was not selected when this builder started.
    PerfettoTrackEventBuilder mixed = PerfettoTrace.instant(CATEGORY, "mixed").addArg("value", 2L);
    Decoded stopped = decode(a.close());
    PerfettoTrace.Session c = start(LEGACY, false);
    mixed.emit(); // A later recording of a selected mode may receive it.
    Decoded encoded = decode(b.close());
    Decoded lateLegacy = decode(c.close());
    assertEquals(1, stopped.events.size());
    assertEquals("old", stopped.events.get(0).getName());
    assertEquals(1, encoded.events.size());
    assertEquals("mixed", encoded.events.get(0).getName());
    assertEquals(encoded.events, lateLegacy.events);
  }

  @Test
  public void stringsKeepLegacyBytesInEveryMode() throws Exception {
    String[] values = {
        null, "", "ascii", "left\0right", "\u00e9", "snow\u2603", "pair\ud83d\ude00", "lone\ud800"};
    List<TrackEvent> expected = null;
    for (JavaEventEncoding mode : new JavaEventEncoding[] {LEGACY, ENCODED}) {
      PerfettoTrace.Session session = start(mode, false);
      for (int i = 0; i < values.length; ++i) {
        PerfettoTrackEventBuilder event = PerfettoTrace.instant(CATEGORY, values[i])
                                              .addArg("key" + i, values[i])
                                              .setCorrelationId(values[i]);
        event.beginProto().addField(1002, values[i]).endProto();
        job(event, values[i]);
        event.emit();
      }
      Decoded actual = decode(session.close());
      assertEquals(values.length, actual.events.size());
      if (expected == null)
        expected = actual.events;
      else
        assertEquals(expected, actual.events);
    }
  }

  private static void emitCallstacks(StackTraceElement[] stack) {
    PerfettoTrace.expensiveDebugCallStack(CATEGORY, "stack", stack).emit();
    PerfettoTrace.expensiveDebugCallStack(CATEGORY, "weighted_stack", stack, 2.5).emit();
  }

  @Test
  public void callstackNamePiecesMatchLegacyInEveryMode() throws Exception {
    StackTraceElement[] stack = {
        new StackTraceElement("com.example.Reader", "read", "Reader.java", 17),
        new StackTraceElement("", "", null, -1),
        new StackTraceElement("left\0ignored", "method", "file\0ignored", 0),
        new StackTraceElement("Class", "method\0ignored", "\u00e9.java", 42),
        new StackTraceElement("\u00e9.\u96ea", "method\ud83d\ude00", "source\u96ea", 19),
        new StackTraceElement("lone\ud800", "tail\udc00", "file", -2),
    };
    String[] names = {
        "com.example.Reader.read", ".", "left", "Class.method", "\u00e9.?.method??", "lone?.tail?"};
    for (boolean v2 : new boolean[] {false, true}) {
      PerfettoTrace.Session baseline = start(LEGACY, v2);
      emitCallstacks(stack);
      Decoded expected = decode(baseline.close());
      assertEquals(2, expected.events.size());
      assertEquals(stack.length, expected.events.get(0).getCallstack().getFramesCount());
      for (int i = 0; i < names.length; ++i) {
        assertEquals(ByteString.copyFrom(names[names.length - 1 - i], StandardCharsets.ISO_8859_1),
            expected.events.get(0).getCallstack().getFrames(i).getFunctionNameBytes());
      }

      PerfettoTrace.Session onlyEncoded = start(ENCODED, v2);
      emitCallstacks(stack);
      assertEquals(expected.events, decode(onlyEncoded.close()).events);

      PerfettoTrace.Session legacy = start(LEGACY, v2);
      PerfettoTrace.Session encoded = start(ENCODED, v2);
      emitCallstacks(stack);
      Decoded a = decode(legacy.close());
      Decoded b = decode(encoded.close());
      assertEquals(expected.events, a.events);
      assertEquals(expected.events, b.events);
      assertEquals(a.timestamps, b.timestamps);
    }
  }

  @Test
  public void explicitInterningZeroStillSuppressesOnlyThatField() throws Exception {
    for (JavaEventEncoding mode : new JavaEventEncoding[] {LEGACY, ENCODED}) {
      PerfettoTrace.Session session = start(mode, false);
      PerfettoTrace.instant(CATEGORY, "zero")
          .beginProto()
          .addFieldWithInterning(1000, "omitted", 0)
          .addField(1001, 7L)
          .endProto()
          .emit();
      Decoded trace = decode(session.close());
      assertEquals(1, trace.events.size());
      assertTrue(trace.events.get(0).getUnknownFields().getField(1000).getVarintList().isEmpty());
      assertEquals(
          List.of(7L), trace.events.get(0).getUnknownFields().getField(1001).getVarintList());
    }
  }

  @Test
  public void oversizedConstructionFailsBeforeEmissionAndBuilderCanBeReused() throws Exception {
    PerfettoTrace.Session session = start(ENCODED, false);
    PerfettoTrackEventBuilder event = PerfettoTrace.instant(CATEGORY, "oversized");
    assertThrows(
        IllegalArgumentException.class, () -> event.addArg("huge", "x".repeat(1024 * 1024)));
    assertThrows(IllegalArgumentException.class, event::emit);
    PerfettoTrace.instant(CATEGORY, "after_failure").addArg("value", 9L).emit();
    Decoded trace = decode(session.close());
    assertEquals(1, trace.events.size());
    assertEquals("after_failure", trace.events.get(0).getName());
  }

  @Test
  public void protoParentWritesDuringNestedFieldsFailWithoutEmission() throws Exception {
    PerfettoTrace.Session session = start(ENCODED, false);
    PerfettoTrackEventBuilder root = PerfettoTrace.instant(CATEGORY, "invalid_parent_write");
    PerfettoTrackEventBuilder proto = root.beginProto();
    proto.beginNested(1000).addField(1, "nested");
    assertThrows(IllegalStateException.class, () -> proto.addField(1001, "parent"));
    assertThrows(IllegalArgumentException.class, root::emit);
    PerfettoTrace.instant(CATEGORY, "after_parent_failure").emit();
    Decoded trace = decode(session.close());
    assertEquals(1, trace.events.size());
    assertEquals("after_parent_failure", trace.events.get(0).getName());
  }

  @Test
  public void nestedInterningAtTheDepthLimitAndRecoveryBeyondIt() throws Exception {
    PerfettoTrace.Session session = start(ENCODED, false);
    PerfettoTrackEventBuilder root = PerfettoTrace.instant(CATEGORY, "deep");
    PerfettoTrackEventBuilder nested = root.beginProto();
    for (int i = 0; i < 64; ++i) nested = nested.beginNested(1000);
    nested.addFieldWithInterning(21, "deep-name", 44);
    root.addArg("root_at_depth_limit", 1L);
    for (int i = 0; i < 64; ++i) nested = nested.endNested();
    nested.endProto().emit();

    PerfettoTrackEventBuilder rejected = PerfettoTrace.instant(CATEGORY, "too_deep");
    PerfettoTrackEventBuilder cursor = rejected.beginProto();
    for (int i = 0; i < 64; ++i) cursor = cursor.beginNested(1000);
    PerfettoTrackEventBuilder last = cursor;
    assertThrows(IllegalArgumentException.class, () -> last.beginNested(1000));
    assertThrows(IllegalArgumentException.class, rejected::emit);
    PerfettoTrace.instant(CATEGORY, "after_depth_failure").emit();
    Decoded trace = decode(session.close());
    assertEquals(2, trace.events.size());
    assertEquals("deep", trace.events.get(0).getName());
    UnknownFieldSet fields = trace.events.get(0).getUnknownFields();
    for (int i = 0; i < 64; ++i) {
      fields = UnknownFieldSet.parseFrom(fields.getField(1000).getLengthDelimitedList().get(0));
    }
    assertEquals(1, fields.getField(21).getVarintList().size());
    assertTrue(fields.getField(21).getVarintList().get(0) > 0);
    assertEquals("after_depth_failure", trace.events.get(1).getName());
  }

  @Test
  public void encodedStorageSurvivesSequentialThreadHandoffAndReuse() throws Exception {
    PerfettoTrace.Session legacy = start(LEGACY, false);
    PerfettoTrace.Session encoded = start(ENCODED, false);
    PerfettoTrackEventBuilder event = PerfettoTrace.instant(CATEGORY, "handoff")
                                         .usingTrack(PerfettoTrack.process("handoff_track"))
                                         .addArg("before", "retained");
    event.beginProto()
        .beginNested(2006)
        .addFieldWithInterning(21, "handoff-job", 44)
        .endNested()
        .endProto();
    Throwable[] failure = new Throwable[1];
    Thread worker = new Thread(() -> {
      try {
        event.addArg("after", "worker").emit();
        PerfettoTrace.instant(CATEGORY, "reused_on_worker").addArg("value", 7L).emit();
      } catch (Throwable error) {
        failure[0] = error;
      }
    });
    worker.start();
    worker.join(30_000);
    assertFalse("Worker did not finish emitting", worker.isAlive());
    if (failure[0] != null) throw new AssertionError(failure[0]);
    PerfettoTrace.instant(CATEGORY, "after_handoff").emit();
    Decoded a = decode(legacy.close());
    Decoded b = decode(encoded.close());
    assertEquals(3, a.events.size());
    assertEquals(3, b.events.size());
    Map<String, TrackEvent> first = new HashMap<>();
    Map<String, TrackEvent> second = new HashMap<>();
    a.events.forEach(value -> first.put(value.getName(), value));
    b.events.forEach(value -> second.put(value.getName(), value));
    assertTrue(first.containsKey("handoff"));
    assertTrue(first.containsKey("reused_on_worker"));
    assertTrue(first.containsKey("after_handoff"));
    assertEquals(first, second);
  }

  @Test
  public void directTransportRejectsInvalidStorageBeforeDereferencing() throws Exception {
    PerfettoTrace.Session session = start(ENCODED, false);
    Method emit = Class.forName("dev.perfetto.sdk.PerfettoTrackEventExtra")
                      .getDeclaredMethod("native_emit_java_direct", int.class, long.class,
                          int.class, long.class, long.class, byte[].class, long.class,
                          int.class, int.class, int.class, int.class, int.class);
    emit.setAccessible(true);
    // A deliberately unusable address must not be read after a bounds failure.
    InvocationTargetException bounds = assertThrows(InvocationTargetException.class,
        () -> emit.invoke(null, 3, CATEGORY.getPtr(), 2, 0L, 0L,
            new byte[1], 1L, 2, 2, 0, 2, 0));
    assertTrue(bounds.getCause() instanceof IllegalArgumentException);
    InvocationTargetException address = assertThrows(InvocationTargetException.class,
        () -> emit.invoke(null, 3, CATEGORY.getPtr(), 2, 0L, 0L,
            new byte[1], 0L, 1, 1, 0, 1, 0));
    assertTrue(address.getCause() instanceof IllegalArgumentException);
    PerfettoTrace.instant(CATEGORY, "after_rejection").emit();
    Decoded trace = decode(session.close());
    assertEquals(1, trace.events.size());
    assertEquals("after_rejection", trace.events.get(0).getName());
  }

  @Test
  public void emptyTransportKeepsEveryEventTypeInEachModeAndWriter() throws Exception {
    for (boolean v2 : new boolean[] {false, true}) {
      for (boolean direct : new boolean[] {false, true}) {
        Method emit = emptyEmitter(direct);
        for (int modes = direct ? 2 : 1; modes <= 3; ++modes) {
          List<PerfettoTrace.Session> sessions = new ArrayList<>();
          if ((modes & 1) != 0) sessions.add(start(LEGACY, v2));
          if ((modes & 2) != 0) sessions.add(start(ENCODED, v2));
          for (int type = 1; type <= 5; ++type) {
            emit.invoke(null, emptyEventArguments(direct, type, modes));
          }
          List<TrackEvent> expected = null;
          for (PerfettoTrace.Session session : sessions) {
            Decoded trace = decode(session.close());
            assertEquals(5, trace.events.size());
            for (int type = 1; type <= 5; ++type) {
              assertEquals(type, trace.events.get(type - 1).getType().getNumber());
              assertEquals("", trace.events.get(type - 1).getName());
            }
            if (expected == null) expected = trace.events;
            else assertEquals(expected, trace.events);
          }
        }
      }
    }
  }

  @Test
  public void emptyTransportStillRejectsInvalidLayoutAndModes() throws Exception {
    PerfettoTrace.Session session = start(ENCODED, false);
    for (boolean direct : new boolean[] {false, true}) {
      Method emit = emptyEmitter(direct);
      Object[] badLayout = emptyEventArguments(direct, 2, 2);
      badLayout[8] = 1; // Nonempty fields cannot fit a zero-byte transport.
      InvocationTargetException layout = assertThrows(
          InvocationTargetException.class, () -> emit.invoke(null, badLayout));
      assertTrue(layout.getCause() instanceof IllegalArgumentException);
      InvocationTargetException modes = assertThrows(InvocationTargetException.class,
          () -> emit.invoke(null, emptyEventArguments(direct, 2, 6)));
      assertTrue(modes.getCause() instanceof IllegalArgumentException);
      emit.invoke(null, emptyEventArguments(direct, 2, 2));
    }
    Decoded trace = decode(session.close());
    assertEquals(2, trace.events.size());
    for (TrackEvent event : trace.events) {
      assertEquals(TrackEvent.Type.TYPE_SLICE_END, event.getType());
    }
  }

  @Test
  public void emptyEndStillUsesItsExplicitTrackInMixedMode() throws Exception {
    for (boolean v2 : new boolean[] {false, true}) {
      PerfettoTrace.Session legacy = start(LEGACY, v2);
      PerfettoTrace.Session encoded = start(ENCODED, v2);
      PerfettoTrack track = PerfettoTrack.process("empty_end_track");
      PerfettoTrace.begin(CATEGORY, "slice").usingTrack(track).emit();
      PerfettoTrace.end(CATEGORY).usingTrack(track).emit();
      Decoded a = decode(legacy.close());
      Decoded b = decode(encoded.close());
      assertEquals(2, a.events.size());
      assertEquals(TrackEvent.Type.TYPE_SLICE_END, a.events.get(1).getType());
      assertEquals(a.events.get(0).getTrackUuid(), a.events.get(1).getTrackUuid());
      assertEquals(a.events, b.events);
      assertEquals(a.timestamps, b.timestamps);
    }
  }

  private static Method emptyEmitter(boolean direct) throws Exception {
    Class<?> extra = Class.forName("dev.perfetto.sdk.PerfettoTrackEventExtra");
    Method method = direct
        ? extra.getDeclaredMethod("native_emit_java_direct", int.class, long.class, int.class,
              long.class, long.class, byte[].class, long.class, int.class, int.class,
              int.class, int.class, int.class)
        : extra.getDeclaredMethod("native_emit_java", int.class, long.class, int.class,
              long.class, long.class, String.class, byte[].class, int.class, int.class,
              int.class, int.class, int.class);
    method.setAccessible(true);
    return method;
  }

  private static Object[] emptyEventArguments(boolean direct, int type, int modes) {
    // Both methods put size/boundaries after the owner/name and address/array.
    return new Object[] {
        type, CATEGORY.getPtr(), modes, 0L, 0L, null, direct ? 0L : null, 0, 0, 0, 0, 0};
  }

  @Test
  public void malformedTransportIsRejectedBeforeAnySessionWrites() throws Exception {
    PerfettoTrace.Session session = start(ENCODED, false);
    Method emit = Class.forName("dev.perfetto.sdk.PerfettoTrackEventExtra")
                      .getDeclaredMethod("native_emit_java", int.class, long.class, int.class,
                          long.class, long.class, String.class, byte[].class, int.class, int.class,
                          int.class, int.class, int.class);
    emit.setAccessible(true);
    InvocationTargetException error = assertThrows(InvocationTargetException.class,
        ()
            -> emit.invoke(null, 3, CATEGORY.getPtr(), 2, 0L, 0L, null, new byte[] {8}, 1, 1, 0, 1,
                0)); // Truncated varint.
    assertTrue(error.getCause() instanceof IllegalArgumentException);
    assertTrue(decode(session.close()).events.isEmpty());
  }
}
