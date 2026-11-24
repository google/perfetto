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
import com.google.protobuf.CodedInputStream;
import com.google.protobuf.CodedOutputStream;
import com.google.protobuf.WireFormat;
import dev.perfetto.sdk.PerfettoTrace;
import dev.perfetto.sdk.PerfettoTrace.Category;
import dev.perfetto.sdk.PerfettoTrack;
import dev.perfetto.sdk.PerfettoTrackEventBuilder;
import java.io.IOException;
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
import perfetto.protos.TrackEventOuterClass.TrackEvent;

/** Decoded-value comparisons for raw-proto events and per-recording format selection. */
@RunWith(AndroidJUnit4.class)
public class PerfettoEventFormatTest {
  // Values of TrackEventConfig.experimental_raw_proto_events.
  private static final boolean EXTRAS = false;
  private static final boolean RAW_PROTO = true;
  private static final Category CATEGORY = new Category("java_event_format_test");
  private static final String PADDING = "x".repeat(17000);

  @Before
  public void setUp() {
    PerfettoTrace.registerWithDebugChecks(true);
    CATEGORY.register();
  }

  private static TraceConfig config(boolean rawProto, boolean v2) {
    TrackEventConfig te = TrackEventConfig.newBuilder()
                              .addEnabledCategories(CATEGORY.getName())
                              .setDisableIncrementalTimestamps(true)
                              .setExperimentalRawProtoEvents(rawProto)
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

  private static PerfettoTrace.Session start(boolean rawProto, boolean v2) {
    return new PerfettoTrace.Session(true, config(rawProto, v2).toByteArray());
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
    event.beginProto().beginNested(2006).addField(2, PADDING).endNested().endProto();
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
    // Interned fields need the EXTRAS format, in every session.
    PerfettoTrackEventBuilder interned =
        PerfettoTrace.instant(CATEGORY, "interned").useExtrasFormat();
    job(interned, "android-job");
    interned.emit();
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

  // The Android tree builds the test protos as lite, which do not expose unknown
  // fields (the job_scheduler_job extension, custom fields). Read them from the
  // wire format instead. Returns `message` without the entries of `field`; their
  // payloads (length-delimited) or values (varint) go to the lists, if not null.
  private static ByteString removeField(ByteString message, int field,
      List<ByteString> payloads, List<Long> varints) throws IOException {
    ByteString.Output rest = ByteString.newOutput();
    CodedOutputStream out = CodedOutputStream.newInstance(rest);
    CodedInputStream in = message.newCodedInput();
    for (int tag = in.readTag(); tag != 0; tag = in.readTag()) {
      if (WireFormat.getTagFieldNumber(tag) != field) {
        in.skipField(tag, out);
        continue;
      }
      switch (WireFormat.getTagWireType(tag)) {
        case WireFormat.WIRETYPE_LENGTH_DELIMITED: {
          ByteString payload = in.readBytes();
          if (payloads != null) {
            payloads.add(payload);
          }
          break;
        }
        case WireFormat.WIRETYPE_VARINT: {
          long value = in.readUInt64();
          if (varints != null) {
            varints.add(value);
          }
          break;
        }
        default:
          throw new AssertionError("Unexpected wire type in field " + field);
      }
    }
    out.flush();
    return rest.toByteString();
  }

  private static List<ByteString> payloads(ByteString message, int field) throws IOException {
    List<ByteString> payloads = new ArrayList<>();
    removeField(message, field, payloads, null);
    return payloads;
  }

  private static List<Long> varints(ByteString message, int field) throws IOException {
    List<Long> varints = new ArrayList<>();
    removeField(message, field, null, varints);
    return varints;
  }

  // One length-delimited field.
  private static ByteString encodeField(int field, ByteString payload) throws IOException {
    ByteString.Output bytes = ByteString.newOutput();
    CodedOutputStream out = CodedOutputStream.newInstance(bytes);
    out.writeBytes(field, payload);
    out.flush();
    return bytes.toByteString();
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
        for (ByteString job : payloads(data.toByteString(), 44)) {
          sequence.jobs.put(varints(job, 1).get(0), payloads(job, 2).get(0));
        }
      }
      if (!packet.hasTrackEvent())
        continue;
      TrackEvent event = packet.getTrackEvent();
      List<ByteString> jobs = new ArrayList<>();
      ByteString withoutJobs = removeField(event.toByteString(), 2006, jobs, null);
      TrackEvent.Builder canonical = TrackEvent.parseFrom(withoutJobs).toBuilder();
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
      ByteString canonicalJobs = ByteString.EMPTY;
      for (ByteString job : jobs) {
        List<Long> iids = new ArrayList<>();
        ByteString jobWithoutIid = removeField(job, 21, null, iids);
        if (iids.isEmpty()) {
          canonicalJobs = canonicalJobs.concat(encodeField(2006, job));
          continue;
        }
        long iid = iids.get(0);
        ByteString name = resolve(sequence.jobs, iid);
        result.jobIids.add(iid);
        result.jobs.add(name);
        // Compare the resolved value, not a packet-sequence-local numeric ID.
        canonicalJobs = canonicalJobs.concat(
            encodeField(2006, jobWithoutIid.concat(encodeField(21, name))));
      }
      result.events.add(
          TrackEvent.parseFrom(canonical.build().toByteString().concat(canonicalJobs)));
      result.timestamps.add(packet.getTimestamp());
    }
    return result;
  }

  @Test
  public void unregisteredCategoryDropsAnAlreadyBuiltEventInEveryFormat() throws Exception {
    boolean[][] selections = {{EXTRAS}, {RAW_PROTO}, {EXTRAS, RAW_PROTO}};
    for (boolean[] selection : selections) {
      List<PerfettoTrace.Session> sessions = new ArrayList<>();
      for (boolean rawProto : selection) sessions.add(start(rawProto, false));
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
  public void allFieldsMatchExtrasOnBothWriters() throws Exception {
    for (boolean v2 : new boolean[] {false, true}) {
      PerfettoTrace.Session extras = start(EXTRAS, v2);
      emitCoverage();
      Decoded expected = decode(extras.close());
      PerfettoTrace.Session rawProto = start(RAW_PROTO, v2);
      emitCoverage();
      Decoded actual = decode(rawProto.close());
      assertEquals(9, actual.events.size());
      assertEquals(expected.events, actual.events);
      assertEquals(List.of(ByteString.copyFromUtf8("android-job")), actual.jobs);
    }
  }

  @Test
  public void rootBuiltinsWhileNestedFieldsAreOpenFailWithoutEmission() throws Exception {
    PerfettoTrace.Session session = start(RAW_PROTO, false);
    PerfettoTrackEventBuilder root = PerfettoTrace.instant(CATEGORY, "open_nested");
    PerfettoTrackEventBuilder nested = root.beginProto().beginNested(2006);
    nested.addField(2, PADDING);
    assertThrows(IllegalStateException.class, () -> root.addArg("during_nested", 17L));
    assertThrows(IllegalArgumentException.class, root::emit);
    PerfettoTrace.instant(CATEGORY, "after_open_nested_failure")
        .beginProto()
        .beginNested(2006)
        .addField(2, PADDING)
        .endNested()
        .endProto()
        .addArg("after_proto", 17L)
        .setCorrelationId("root")
        .emit();
    Decoded actual = decode(session.close());
    assertEquals(1, actual.events.size());
    assertEquals("after_open_nested_failure", actual.events.get(0).getName());
    assertEquals(1, actual.events.get(0).getDebugAnnotationsCount());
    assertEquals("root", actual.events.get(0).getCorrelationIdStr());
  }

  @Test
  public void mixedFormatsEmitOnceWithTheSameTimestamp() throws Exception {
    for (boolean v2 : new boolean[] {false, true}) {
      PerfettoTrace.Session extras = start(EXTRAS, v2);
      PerfettoTrace.Session rawProto = start(RAW_PROTO, v2);
      emitCoverage();
      Decoded a = decode(extras.close());
      Decoded b = decode(rawProto.close());
      assertEquals(9, a.events.size());
      assertEquals(a.events, b.events);
      assertEquals(a.timestamps, b.timestamps);
    }
  }

  @Test
  public void rawProtoNamesAreInlineAndInternedEventsUseExtras() throws Exception {
    for (boolean v2 : new boolean[] {false, true}) {
      PerfettoTrace.Session extras = start(EXTRAS, v2);
      PerfettoTrace.Session rawProto = start(RAW_PROTO, v2);
      PerfettoTrace.instant(CATEGORY, "ordinary").addArg("key", "value").emit();
      PerfettoTrackEventBuilder explicit =
          PerfettoTrace.instant(CATEGORY, "explicit").useExtrasFormat();
      job(explicit, "android-job");
      explicit.addArg("late", 42L).emit();
      byte[] extrasBytes = extras.close();
      byte[] rawProtoBytes = rawProto.close();
      assertEquals(decode(extrasBytes).events, decode(rawProtoBytes).events);
      assertEquals(List.of(ByteString.copyFromUtf8("android-job")), decode(rawProtoBytes).jobs);
      int events = 0;
      for (TracePacket packet : Trace.parseFrom(rawProtoBytes).getPacketList()) {
        if (!packet.hasTrackEvent()) continue;
        TrackEvent event = packet.getTrackEvent();
        if (events++ == 0) {
          // RAW_PROTO: inline names, nothing interned.
          assertEquals("ordinary", event.getName());
          assertFalse(event.hasNameIid());
          assertEquals(0, event.getCategoryIidsCount());
          assertEquals(List.of(CATEGORY.getName()), event.getCategoriesList());
          assertTrue(event.getDebugAnnotations(0).hasName());
          assertFalse(event.getDebugAnnotations(0).hasNameIid());
          assertFalse(packet.hasInternedData());
        } else {
          // useExtrasFormat(): the EXTRAS format, with interned names.
          assertTrue(event.hasNameIid());
          assertEquals(1, event.getCategoryIidsCount());
          assertTrue(event.getDebugAnnotations(0).hasNameIid());
        }
      }
      assertEquals(2, events);
      // The concurrently active EXTRAS recording retains its original wire format.
      for (TracePacket packet : Trace.parseFrom(extrasBytes).getPacketList()) {
        if (!packet.hasTrackEvent()) continue;
        TrackEvent event = packet.getTrackEvent();
        assertTrue(event.hasNameIid());
        assertEquals(1, event.getCategoryIidsCount());
        assertTrue(event.getDebugAnnotations(0).hasNameIid());
      }
    }
  }

  @Test
  public void interningWithoutUseExtrasFormatDropsOnlyThatFieldInRawProto() throws Exception {
    PerfettoTrace.Session extras = start(EXTRAS, false);
    PerfettoTrace.Session rawProto = start(RAW_PROTO, false);
    PerfettoTrackEventBuilder event = PerfettoTrace.instant(CATEGORY, "job");
    job(event, "android-job");
    event.emit();
    Decoded a = decode(extras.close());
    Decoded b = decode(rawProto.close());
    assertEquals(List.of(ByteString.copyFromUtf8("android-job")), a.jobs);
    assertEquals(1, b.events.size());
    assertTrue(b.jobs.isEmpty());
    assertEquals(1, payloads(b.events.get(0).toByteString(), 2006).size());
  }

  @Test
  public void useExtrasFormatMustComeBeforeFields() throws Exception {
    PerfettoTrace.Session session = start(RAW_PROTO, false);
    PerfettoTrackEventBuilder event = PerfettoTrace.instant(CATEGORY, "late").addArg("value", 1L);
    assertThrows(IllegalStateException.class, event::useExtrasFormat);
    // A track set before the call is kept.
    PerfettoTrackEventBuilder tracked = PerfettoTrace.instant(CATEGORY, "tracked")
                                            .usingTrack(PerfettoTrack.process("extras_track"))
                                            .useExtrasFormat();
    job(tracked, "android-job");
    tracked.emit();
    Decoded trace = decode(session.close());
    assertEquals(1, trace.events.size());
    assertTrue(trace.events.get(0).hasTrackUuid());
    assertEquals(List.of(ByteString.copyFromUtf8("android-job")), trace.jobs);
  }

  @Test
  public void jobInterningUsesEachSessionsOwnIds() throws Exception {
    for (boolean v2 : new boolean[] {false, true}) {
      PerfettoTrace.Session a = start(RAW_PROTO, v2);
      PerfettoTrackEventBuilder seed =
          PerfettoTrace.instant(CATEGORY, "seed").useExtrasFormat();
      job(seed, "seed-job");
      seed.emit();
      PerfettoTrace.Session b = start(RAW_PROTO, v2);
      PerfettoTrackEventBuilder event =
          PerfettoTrace.instant(CATEGORY, "common").useExtrasFormat();
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
      PerfettoTrace.Session reset = start(RAW_PROTO, v2);
      PerfettoTrackEventBuilder again =
          PerfettoTrace.instant(CATEGORY, "common").useExtrasFormat();
      job(again, "shared-job");
      again.emit();
      assertEquals(second.events, decode(reset.close()).events);
    }
  }

  @Test
  public void builderFormatsStayFixedAcrossSessionChanges() throws Exception {
    PerfettoTrace.Session a = start(EXTRAS, false);
    PerfettoTrackEventBuilder old = PerfettoTrace.instant(CATEGORY, "old").addArg("value", 1L);
    PerfettoTrace.Session b = start(RAW_PROTO, false);
    // EXTRAS-only events reach every session that is active at emit time. A
    // RAW_PROTO session that started after newEvent() gets this valid EXTRAS event.
    old.emit();
    PerfettoTrackEventBuilder mixed = PerfettoTrace.instant(CATEGORY, "mixed").addArg("value", 2L);
    Decoded stopped = decode(a.close());
    PerfettoTrace.Session c = start(EXTRAS, false);
    mixed.emit(); // A later recording of a selected format may receive it.
    Decoded rawProto = decode(b.close());
    Decoded lateExtras = decode(c.close());
    assertEquals(1, stopped.events.size());
    assertEquals("old", stopped.events.get(0).getName());
    assertEquals(2, rawProto.events.size());
    assertEquals("old", rawProto.events.get(0).getName());
    assertEquals("mixed", rawProto.events.get(1).getName());
    assertEquals(rawProto.events.subList(1, 2), lateExtras.events);
  }

  @Test
  public void rootSettersOnProtoChildBuildersAreDroppedInEveryFormat() throws Exception {
    for (int formats = 1; formats <= 3; ++formats) {
      List<PerfettoTrace.Session> sessions = new ArrayList<>();
      if ((formats & 1) != 0) sessions.add(start(EXTRAS, false));
      if ((formats & 2) != 0) sessions.add(start(RAW_PROTO, false));
      PerfettoTrackEventBuilder root =
          PerfettoTrackEventBuilder.newEvent(3, CATEGORY, /* isDebug= */ false)
              .setEventName("child_root_setters");
      PerfettoTrackEventBuilder proto = root.beginProto();
      proto.addArg("dropped", 1L)
          .addFlow(5)
          .setCorrelationId(7L)
          .usingTrack(PerfettoTrack.process("dropped_track"));
      proto.addField(1000, 3L).endProto().emit();
      for (PerfettoTrace.Session session : sessions) {
        Decoded trace = decode(session.close());
        assertEquals(1, trace.events.size());
        TrackEvent event = trace.events.get(0);
        assertEquals("child_root_setters", event.getName());
        assertEquals(0, event.getDebugAnnotationsCount());
        assertEquals(0, event.getFlowIdsCount());
        assertFalse(event.hasCorrelationId());
        assertFalse(event.hasTrackUuid());
        assertEquals(1, varints(event.toByteString(), 1000).size());
      }
    }
  }

  @Test
  public void invalidRawProtoEventsAreDroppedWithoutDebugChecks() throws Exception {
    PerfettoTrace.Session extras = start(EXTRAS, false);
    PerfettoTrace.Session rawProto = start(RAW_PROTO, false);
    PerfettoTrackEventBuilder.newEvent(3, CATEGORY, /* isDebug= */ false)
        .setEventName("oversized")
        .addArg("huge", "x".repeat(1024 * 1024))
        .emit();
    PerfettoTrackEventBuilder root =
        PerfettoTrackEventBuilder.newEvent(3, CATEGORY, /* isDebug= */ false)
            .setEventName("parent_write");
    PerfettoTrackEventBuilder proto = root.beginProto();
    proto.beginNested(1000).addField(1, "nested");
    proto.addField(1001, "parent"); // Not supported while the nested field is open.
    proto.endProto().emit();
    PerfettoTrace.instant(CATEGORY, "after").emit();
    for (PerfettoTrace.Session session : new PerfettoTrace.Session[] {extras, rawProto}) {
      Decoded trace = decode(session.close());
      assertEquals(1, trace.events.size());
      assertEquals("after", trace.events.get(0).getName());
    }
  }

  @Test
  public void stringsKeepExtrasBytesInEveryFormat() throws Exception {
    String[] values = {
        null, "", "ascii", "left\0right", "\u00e9", "snow\u2603", "pair\ud83d\ude00", "lone\ud800"};
    List<TrackEvent> expected = null;
    for (boolean rawProto : new boolean[] {EXTRAS, RAW_PROTO}) {
      PerfettoTrace.Session session = start(rawProto, false);
      for (int i = 0; i < values.length; ++i) {
        PerfettoTrackEventBuilder event = PerfettoTrace.instant(CATEGORY, values[i])
                                              .addArg("key" + i, values[i])
                                              .setCorrelationId(values[i]);
        event.beginProto().addField(1002, values[i]).endProto();
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
  public void callstackNamesMatchExtrasInEveryFormat() throws Exception {
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
      PerfettoTrace.Session baseline = start(EXTRAS, v2);
      emitCallstacks(stack);
      Decoded expected = decode(baseline.close());
      assertEquals(2, expected.events.size());
      assertEquals(stack.length, expected.events.get(0).getCallstack().getFramesCount());
      for (int i = 0; i < names.length; ++i) {
        assertEquals(ByteString.copyFrom(names[names.length - 1 - i], StandardCharsets.ISO_8859_1),
            expected.events.get(0).getCallstack().getFrames(i).getFunctionNameBytes());
      }

      PerfettoTrace.Session onlyRawProto = start(RAW_PROTO, v2);
      emitCallstacks(stack);
      assertEquals(expected.events, decode(onlyRawProto.close()).events);

      PerfettoTrace.Session extras = start(EXTRAS, v2);
      PerfettoTrace.Session rawProto = start(RAW_PROTO, v2);
      emitCallstacks(stack);
      Decoded a = decode(extras.close());
      Decoded b = decode(rawProto.close());
      assertEquals(expected.events, a.events);
      assertEquals(expected.events, b.events);
      assertEquals(a.timestamps, b.timestamps);
    }
  }

  @Test
  public void explicitInterningZeroStillSuppressesOnlyThatField() throws Exception {
    for (boolean rawProto : new boolean[] {EXTRAS, RAW_PROTO}) {
      PerfettoTrace.Session session = start(rawProto, false);
      PerfettoTrace.instant(CATEGORY, "zero")
          .beginProto()
          .addFieldWithInterning(1000, "omitted", 0)
          .addField(1001, 7L)
          .endProto()
          .emit();
      Decoded trace = decode(session.close());
      assertEquals(1, trace.events.size());
      ByteString event = trace.events.get(0).toByteString();
      assertTrue(varints(event, 1000).isEmpty());
      assertEquals(List.of(7L), varints(event, 1001));
    }
  }

  @Test
  public void oversizedConstructionFailsBeforeEmissionAndBuilderCanBeReused() throws Exception {
    PerfettoTrace.Session session = start(RAW_PROTO, false);
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
    PerfettoTrace.Session session = start(RAW_PROTO, false);
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
  public void nestedFieldsAtTheDepthLimitAndRecoveryBeyondIt() throws Exception {
    PerfettoTrace.Session session = start(RAW_PROTO, false);
    PerfettoTrackEventBuilder root = PerfettoTrace.instant(CATEGORY, "deep");
    PerfettoTrackEventBuilder nested = root.beginProto();
    for (int i = 0; i < 64; ++i) nested = nested.beginNested(1000);
    nested.addField(21, 5L);
    for (int i = 0; i < 64; ++i) nested = nested.endNested();
    nested.endProto().addArg("after_depth_limit", 1L).emit();

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
    ByteString fields = trace.events.get(0).toByteString();
    for (int i = 0; i < 64; ++i) {
      fields = payloads(fields, 1000).get(0);
    }
    List<Long> values = varints(fields, 21);
    assertEquals(List.of(5L), values);
    assertEquals("after_depth_failure", trace.events.get(1).getName());
  }

  @Test
  public void rawProtoStorageSurvivesSequentialThreadHandoffAndReuse() throws Exception {
    PerfettoTrace.Session extras = start(EXTRAS, false);
    PerfettoTrace.Session rawProto = start(RAW_PROTO, false);
    PerfettoTrackEventBuilder event = PerfettoTrace.instant(CATEGORY, "handoff")
                                         .usingTrack(PerfettoTrack.process("handoff_track"))
                                         .addArg("before", "retained");
    event.beginProto()
        .beginNested(2006)
        .addField(2, "handoff-job")
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
    Decoded a = decode(extras.close());
    Decoded b = decode(rawProto.close());
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
  public void directTransportIsTrustedAndEmitsAnEmptyEvent() throws Exception {
    // The platform fast path performs no checks in release builds (Java owns the
    // one bounds check). An empty event needs no storage.
    PerfettoTrace.Session session = start(RAW_PROTO, false);
    emitEmpty(Entrypoint.RAW_PROTO, 3, 0);
    PerfettoTrace.instant(CATEGORY, "after_direct").emit();
    Decoded trace = decode(session.close());
    assertEquals(2, trace.events.size());
    assertEquals("after_direct", trace.events.get(1).getName());
  }

  // The raw-proto entrypoints. RAW_PROTO (@CriticalNative) checks nothing in
  // release builds, while WITH_FORMATS returns false for bad input.
  private enum Entrypoint {
    RAW_PROTO(false),
    WITH_FORMATS(true);

    final boolean checks;

    Entrypoint(boolean checks) {
      this.checks = checks;
    }
  }

  @Test
  public void emptyTransportKeepsEveryEventTypeInEachFormatAndWriter() throws Exception {
    for (boolean v2 : new boolean[] {false, true}) {
      for (Entrypoint entrypoint : Entrypoint.values()) {
        PerfettoTrace.Session session = start(RAW_PROTO, v2);
        for (int type = 1; type <= 5; ++type) {
          assertTrue(emitEmpty(entrypoint, type, 0));
        }
        Decoded trace = decode(session.close());
        assertEquals(5, trace.events.size());
        for (int type = 1; type <= 5; ++type) {
          assertEquals(type, trace.events.get(type - 1).getType().getNumber());
          assertEquals("", trace.events.get(type - 1).getName());
        }
      }
    }
  }

  @Test
  public void checkedTransportRejectsInvalidLayout() throws Exception {
    PerfettoTrace.Session session = start(RAW_PROTO, false);
    int emitted = 0;
    for (Entrypoint entrypoint : Entrypoint.values()) {
      if (!entrypoint.checks) continue;
      // Nonempty fields cannot fit a zero-byte transport.
      assertFalse(emitEmpty(entrypoint, 2, 1));
      assertTrue(emitEmpty(entrypoint, 2, 0));
      ++emitted;
    }
    Decoded trace = decode(session.close());
    assertEquals(emitted, trace.events.size());
    for (TrackEvent event : trace.events) {
      assertEquals(TrackEvent.Type.TYPE_SLICE_END, event.getType());
    }
  }

  @Test
  public void emptyEndStillUsesItsExplicitTrackInMixedFormat() throws Exception {
    for (boolean v2 : new boolean[] {false, true}) {
      PerfettoTrace.Session extras = start(EXTRAS, v2);
      PerfettoTrace.Session rawProto = start(RAW_PROTO, v2);
      PerfettoTrack track = PerfettoTrack.process("empty_end_track");
      PerfettoTrace.begin(CATEGORY, "slice").usingTrack(track).emit();
      PerfettoTrace.end(CATEGORY).usingTrack(track).emit();
      Decoded a = decode(extras.close());
      Decoded b = decode(rawProto.close());
      assertEquals(2, a.events.size());
      assertEquals(TrackEvent.Type.TYPE_SLICE_END, a.events.get(1).getType());
      assertEquals(a.events.get(0).getTrackUuid(), a.events.get(1).getTrackUuid());
      assertEquals(a.events, b.events);
      assertEquals(a.timestamps, b.timestamps);
    }
  }

  // Emits an event with an empty (zero-byte) transport whose layout claims
  // fieldsSize. Returns false if the entrypoint rejected it.
  private static boolean emitEmpty(Entrypoint entrypoint, int type, int fieldsSize)
      throws Exception {
    Class<?> extra = Class.forName("dev.perfetto.sdk.PerfettoTrackEventExtra");
    try {
      switch (entrypoint) {
        case RAW_PROTO: {
          Method method = extra.getDeclaredMethod("native_emit_raw_proto", int.class,
              long.class, long.class, long.class, int.class, int.class);
          method.setAccessible(true);
          method.invoke(null, type, CATEGORY.getPtr(), 0L, 0L, fieldsSize, 0);
          return true;
        }
        case WITH_FORMATS:
          return emitChecked(type, null, fieldsSize, 0, /* size= */ 0);
      }
    } catch (InvocationTargetException e) {
      if (e.getCause() instanceof Exception) throw (Exception) e.getCause();
      throw e;
    }
    return false;
  }

  // Calls native_emit_with_formats (the checked path) for a RAW_PROTO-only
  // event that copies `buffer`.
  private static boolean emitChecked(int type, byte[] buffer, int fieldsSize, int nameSize,
      int size) throws Exception {
    Method method = Class.forName("dev.perfetto.sdk.PerfettoTrackEventExtra")
                        .getDeclaredMethod("native_emit_with_formats", int.class, long.class,
                            long.class, long.class, byte[].class, long.class, int.class,
                            int.class, int.class);
    method.setAccessible(true);
    return (Boolean) method.invoke(
        null, type, CATEGORY.getPtr(), 0L, 0L, buffer, 0L, fieldsSize, nameSize, size);
  }

  @Test
  public void malformedTransportIsRejectedBeforeAnySessionWrites() throws Exception {
    PerfettoTrace.Session session = start(RAW_PROTO, false);
    assertFalse(emitChecked(3, new byte[] {8}, 1, 0, 1)); // Truncated varint.
    assertTrue(decode(session.close()).events.isEmpty());
  }

  @Test
  public void counterKeepsTheLastValueAsInExtras() throws Exception {
    List<TrackEvent> expected = null;
    for (boolean rawProto : new boolean[] {EXTRAS, RAW_PROTO}) {
      PerfettoTrace.Session session = start(rawProto, false);
      PerfettoTrace.counter(CATEGORY, 1L, "counter").setCounter(2.5).emit();
      PerfettoTrace.counter(CATEGORY, 2.5, "counter").setCounter(3L).emit();
      Decoded trace = decode(session.close());
      assertEquals(2, trace.events.size());
      if (expected == null) {
        expected = trace.events;
      } else {
        assertEquals(expected, trace.events);
      }
      assertEquals(2.5, trace.events.get(0).getDoubleCounterValue(), 0);
      assertEquals(3L, trace.events.get(1).getCounterValue());
    }
  }

  // PERFETTO_TE_TYPE_INSTANT.
  private static final int TYPE_INSTANT = 3;

  @Test
  public void failedEventInReleaseModeIsDroppedWithoutStaleBytes() throws Exception {
    boolean[][] selections = {{RAW_PROTO}, {EXTRAS, RAW_PROTO}};
    for (boolean[] selection : selections) {
      List<PerfettoTrace.Session> sessions = new ArrayList<>();
      for (boolean rawProto : selection) sessions.add(start(rawProto, false));
      PerfettoTrackEventBuilder root =
          PerfettoTrackEventBuilder.newEvent(TYPE_INSTANT, CATEGORY, /* isDebug= */ false)
              .setEventName("failed");
      PerfettoTrackEventBuilder proto = root.beginProto();
      proto.beginNested(1000).addField(1, "stale-nested");
      // Fails the raw-proto event because its nested field is still open.
      // Release mode drops the whole event instead of throwing.
      proto.addField(1001, "parent");
      root.emit();
      PerfettoTrace.instant(CATEGORY, "next").addArg("value", 1L).emit();
      for (PerfettoTrace.Session session : sessions) {
        Decoded trace = decode(session.close());
        assertEquals(1, trace.events.size());
        TrackEvent next = trace.events.get(0);
        assertEquals("next", next.getName());
        assertEquals(1, next.getDebugAnnotationsCount());
        assertTrue(payloads(next.toByteString(), 1000).isEmpty());
        assertTrue(payloads(next.toByteString(), 1001).isEmpty());
      }
    }
  }
}
