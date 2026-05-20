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

import static com.google.common.truth.Truth.assertThat;
import static dev.perfetto.sdk.PerfettoTrace.Category;

import android.util.ArraySet;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import com.google.protobuf.ByteString;
import dev.perfetto.sdk.PerfettoEvent;
import dev.perfetto.sdk.PerfettoTrace;
import dev.perfetto.sdk.PerfettoTrack;
import java.util.HashSet;
import java.util.Set;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import perfetto.protos.CounterDescriptorOuterClass.CounterDescriptor;
import perfetto.protos.DataSourceConfigOuterClass.DataSourceConfig;
import perfetto.protos.InternedDataOuterClass.InternedData;
import perfetto.protos.SourceLocationOuterClass.SourceLocation;
import perfetto.protos.TraceConfigOuterClass.TraceConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig.BufferConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig.DataSource;
import perfetto.protos.TraceOuterClass.Trace;
import perfetto.protos.TracePacketOuterClass.TracePacket;
import perfetto.protos.TrackDescriptorOuterClass.TrackDescriptor;
import perfetto.protos.TrackEventConfigOuterClass.TrackEventConfig;
import perfetto.protos.TrackEventOuterClass.EventCategory;
import perfetto.protos.TrackEventOuterClass.EventName;
import perfetto.protos.TrackEventOuterClass.TrackEvent;

/**
 * Round-trips the Low Level (Java-side) emit path: starts an in-process tracing
 * session, emits events through {@link PerfettoEvent}, and decodes the produced
 * trace to verify the TrackEvents, their types, and the interned category /
 * event names.
 */
@RunWith(AndroidJUnit4.class)
public class PerfettoEventEmitTest {
  private static final String FOO = "foo";

  // Keep in sync with PerfettoTrace.
  private static final int TYPE_SLICE_BEGIN = 1;
  private static final int TYPE_SLICE_END = 2;
  private static final int TYPE_INSTANT = 3;

  private static final Category FOO_CATEGORY = new Category(FOO);

  private final Set<String> mCategoryNames = new ArraySet<>();
  private final Set<String> mEventNames = new ArraySet<>();

  @Before
  public void setUp() {
    System.loadLibrary("perfetto_jni");
    PerfettoTrace.register(true);
    var unused = FOO_CATEGORY.register();
    mCategoryNames.clear();
    mEventNames.clear();
  }

  @Test
  public void emitsInstantWithNameAndCategory() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    PerfettoEvent.emit(TYPE_INSTANT, FOO_CATEGORY, "event");

    Trace trace = Trace.parseFrom(session.close());

    boolean hasInstant = false;
    for (TracePacket packet : trace.getPacketList()) {
      if (packet.hasTrackEvent()
          && TrackEvent.Type.TYPE_INSTANT.equals(packet.getTrackEvent().getType())) {
        hasInstant = true;
      }
      collectInternedData(packet);
    }

    assertThat(hasInstant).isTrue();
    assertThat(mCategoryNames).contains(FOO);
    assertThat(mEventNames).contains("event");
  }

  @Test
  public void emitsSliceBeginEndPair() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    PerfettoEvent.emit(TYPE_SLICE_BEGIN, FOO_CATEGORY, "slice");
    PerfettoEvent.emit(TYPE_SLICE_END, FOO_CATEGORY, "");

    Trace trace = Trace.parseFrom(session.close());

    boolean hasBegin = false;
    boolean hasEnd = false;
    for (TracePacket packet : trace.getPacketList()) {
      if (!packet.hasTrackEvent()) {
        continue;
      }
      TrackEvent event = packet.getTrackEvent();
      if (TrackEvent.Type.TYPE_SLICE_BEGIN.equals(event.getType())) {
        hasBegin = true;
      }
      if (TrackEvent.Type.TYPE_SLICE_END.equals(event.getType())) {
        hasEnd = true;
      }
      collectInternedData(packet);
    }

    assertThat(hasBegin).isTrue();
    assertThat(hasEnd).isTrue();
    assertThat(mEventNames).contains("slice");
  }

  @Test
  public void builderRoutesExtraFreeEventsThroughJavaEmit() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    // No extras -> Java-side emit path.
    PerfettoTrace.begin(FOO_CATEGORY, "routed_begin").emit();
    PerfettoTrace.end(FOO_CATEGORY).emit();
    PerfettoTrace.instant(FOO_CATEGORY, "routed_instant").emit();

    Trace trace = Trace.parseFrom(session.close());

    boolean hasBegin = false;
    boolean hasEnd = false;
    boolean hasInstant = false;
    for (TracePacket packet : trace.getPacketList()) {
      if (packet.hasTrackEvent()) {
        TrackEvent event = packet.getTrackEvent();
        if (TrackEvent.Type.TYPE_SLICE_BEGIN.equals(event.getType())) {
          hasBegin = true;
        }
        if (TrackEvent.Type.TYPE_SLICE_END.equals(event.getType())) {
          hasEnd = true;
        }
        if (TrackEvent.Type.TYPE_INSTANT.equals(event.getType())) {
          hasInstant = true;
        }
      }
      collectInternedData(packet);
    }

    assertThat(hasBegin).isTrue();
    assertThat(hasEnd).isTrue();
    assertThat(hasInstant).isTrue();
    assertThat(mEventNames).contains("routed_begin");
    assertThat(mEventNames).contains("routed_instant");
  }

  @Test
  public void builderRoutesDebugArgsThroughJavaEmit() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    PerfettoTrace.instant(FOO_CATEGORY, "event")
        .addArg("int_arg", 10000000000L)
        .addArg("bool_arg", true)
        .addArg("double_arg", 3.14)
        .addArg("string_arg", "value")
        .emit();

    Trace trace = Trace.parseFrom(session.close());

    boolean hasArgs = false;
    for (TracePacket packet : trace.getPacketList()) {
      if (packet.hasTrackEvent() && packet.getTrackEvent().getDebugAnnotationsCount() == 4) {
        var ann = packet.getTrackEvent().getDebugAnnotationsList();
        assertThat(ann.get(0).getName()).isEqualTo("int_arg");
        assertThat(ann.get(0).getIntValue()).isEqualTo(10000000000L);
        assertThat(ann.get(1).getBoolValue()).isTrue();
        assertThat(ann.get(2).getDoubleValue()).isEqualTo(3.14);
        assertThat(ann.get(3).getStringValue()).isEqualTo("value");
        hasArgs = true;
      }
    }
    assertThat(hasArgs).isTrue();
  }

  @Test
  public void growsEmitBufferForLargeBodyAndReusesAfter() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    // A value well past the 512-byte default EmitBuffer, so emit must grow the
    // off-heap buffer (and re-fetch its native address) mid-flight.
    StringBuilder sb = new StringBuilder();
    for (int i = 0; i < 2048; i++) {
      sb.append((char) ('a' + (i % 26)));
    }
    String big = sb.toString();

    PerfettoTrace.instant(FOO_CATEGORY, "big").addArg("blob", big).emit();
    // A small event right after, on the same thread, to confirm the grown
    // buffer is reused correctly.
    PerfettoTrace.instant(FOO_CATEGORY, "small").addArg("k", 7L).emit();

    Trace trace = Trace.parseFrom(session.close());

    boolean hasBig = false;
    boolean hasSmall = false;
    for (TracePacket packet : trace.getPacketList()) {
      if (!packet.hasTrackEvent()) {
        continue;
      }
      var ann = packet.getTrackEvent().getDebugAnnotationsList();
      if (ann.size() == 1 && ann.get(0).getName().equals("blob")) {
        assertThat(ann.get(0).getStringValue()).isEqualTo(big);
        hasBig = true;
      }
      if (ann.size() == 1 && ann.get(0).getName().equals("k")) {
        assertThat(ann.get(0).getIntValue()).isEqualTo(7L);
        hasSmall = true;
      }
    }
    assertThat(hasBig).isTrue();
    assertThat(hasSmall).isTrue();
  }

  @Test
  public void builderRoutesNamedTrackThroughJavaEmit() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    PerfettoTrace.begin(FOO_CATEGORY, "event").usingProcessNamedTrack(7, "mytrack").emit();
    PerfettoTrace.end(FOO_CATEGORY).usingProcessNamedTrack(7, "mytrack").emit();

    Trace trace = Trace.parseFrom(session.close());

    int descriptorCount = 0;
    long descriptorUuid = 0;
    long beginTrackUuid = 0;
    long endTrackUuid = 0;
    for (TracePacket packet : trace.getPacketList()) {
      if (packet.hasTrackDescriptor()) {
        TrackDescriptor td = packet.getTrackDescriptor();
        if ("mytrack".equals(td.getStaticName())) {
          descriptorCount++;
          descriptorUuid = td.getUuid();
        }
      }
      if (packet.hasTrackEvent() && packet.getTrackEvent().hasTrackUuid()) {
        TrackEvent event = packet.getTrackEvent();
        if (TrackEvent.Type.TYPE_SLICE_BEGIN.equals(event.getType())) {
          beginTrackUuid = event.getTrackUuid();
        } else if (TrackEvent.Type.TYPE_SLICE_END.equals(event.getType())) {
          endTrackUuid = event.getTrackUuid();
        }
      }
    }

    // Descriptor emitted exactly once (deduped via PerfettoTeLlTrackSeen);
    // both events reference it.
    assertThat(descriptorCount).isEqualTo(1);
    assertThat(beginTrackUuid).isEqualTo(descriptorUuid);
    assertThat(endTrackUuid).isEqualTo(descriptorUuid);
  }

  @Test
  public void usingNestedTrackEmitsAncestorDescriptors() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    PerfettoTrack parent = PerfettoTrack.process("parent_track");
    PerfettoTrack child = parent.child("child_track");
    PerfettoTrace.instant(FOO_CATEGORY, "nested").usingTrack(child).emit();

    Trace trace = Trace.parseFrom(session.close());

    int parentDescriptors = 0;
    int childDescriptors = 0;
    long parentUuid = 0;
    long childUuid = 0;
    long childParentUuid = 0;
    long eventTrackUuid = 0;
    for (TracePacket packet : trace.getPacketList()) {
      if (packet.hasTrackDescriptor()) {
        TrackDescriptor td = packet.getTrackDescriptor();
        if ("parent_track".equals(td.getStaticName())) {
          parentDescriptors++;
          parentUuid = td.getUuid();
        }
        if ("child_track".equals(td.getStaticName())) {
          childDescriptors++;
          childUuid = td.getUuid();
          childParentUuid = td.getParentUuid();
        }
      }
      if (packet.hasTrackEvent() && packet.getTrackEvent().hasTrackUuid()) {
        eventTrackUuid = packet.getTrackEvent().getTrackUuid();
      }
    }

    // Both levels' descriptors are emitted once, the child is linked under the
    // parent, and the event attaches to the leaf.
    assertThat(parentDescriptors).isEqualTo(1);
    assertThat(childDescriptors).isEqualTo(1);
    assertThat(childParentUuid).isEqualTo(parentUuid);
    assertThat(eventTrackUuid).isEqualTo(childUuid);
    assertThat(eventTrackUuid).isEqualTo(child.getUuid());
  }

  @Test
  public void usingGlobalTrackEmitsRootLevelDescriptor() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    PerfettoTrack g = PerfettoTrack.global("global_track");
    PerfettoTrace.instant(FOO_CATEGORY, "on_global").usingTrack(g).emit();

    Trace trace = Trace.parseFrom(session.close());

    boolean hasDescriptor = false;
    long descriptorParent = -1;
    long eventTrackUuid = 0;
    for (TracePacket packet : trace.getPacketList()) {
      if (packet.hasTrackDescriptor()
          && "global_track".equals(packet.getTrackDescriptor().getStaticName())) {
        hasDescriptor = true;
        descriptorParent = packet.getTrackDescriptor().getParentUuid();
      }
      if (packet.hasTrackEvent() && packet.getTrackEvent().hasTrackUuid()) {
        eventTrackUuid = packet.getTrackEvent().getTrackUuid();
      }
    }

    // Global tracks are root-level: no parent uuid (0).
    assertThat(hasDescriptor).isTrue();
    assertThat(descriptorParent).isEqualTo(0);
    assertThat(eventTrackUuid).isEqualTo(g.getUuid());
  }

  @Test
  public void usingCounterTrackWritesUnits() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    PerfettoTrack mem =
        PerfettoTrack.processCounter("mem_counter")
            .withUnit(PerfettoTrack.CounterUnit.SIZE_BYTES)
            .withUnitMultiplier(1024)
            .withIsIncremental(true);
    PerfettoTrace.counter(FOO_CATEGORY, 5).usingTrack(mem).emit();

    Trace trace = Trace.parseFrom(session.close());

    boolean checked = false;
    for (TracePacket packet : trace.getPacketList()) {
      if (packet.hasTrackDescriptor()
          && "mem_counter".equals(packet.getTrackDescriptor().getStaticName())
          && packet.getTrackDescriptor().hasCounter()) {
        CounterDescriptor cd = packet.getTrackDescriptor().getCounter();
        assertThat(cd.getUnit()).isEqualTo(CounterDescriptor.Unit.UNIT_SIZE_BYTES);
        assertThat(cd.getUnitMultiplier()).isEqualTo(1024);
        assertThat(cd.getIsIncremental()).isTrue();
        checked = true;
      }
    }
    assertThat(checked).isTrue();
  }

  @Test
  public void usingTrackWritesChildOrderingAndSiblingRank() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    PerfettoTrack parent =
        PerfettoTrack.process("ordered_parent")
            .withChildOrdering(PerfettoTrack.ChildOrdering.EXPLICIT);
    PerfettoTrack child = parent.child("ranked_child").withSiblingOrderRank(7);
    PerfettoTrace.instant(FOO_CATEGORY, "ordered").usingTrack(child).emit();

    Trace trace = Trace.parseFrom(session.close());

    boolean parentHasExplicitOrdering = false;
    boolean childHasRank = false;
    for (TracePacket packet : trace.getPacketList()) {
      if (!packet.hasTrackDescriptor()) {
        continue;
      }
      TrackDescriptor td = packet.getTrackDescriptor();
      if ("ordered_parent".equals(td.getStaticName())) {
        parentHasExplicitOrdering =
            td.getChildOrdering() == TrackDescriptor.ChildTracksOrdering.EXPLICIT;
      }
      if ("ranked_child".equals(td.getStaticName())) {
        childHasRank = td.getSiblingOrderRank() == 7;
      }
    }
    assertThat(parentHasExplicitOrdering).isTrue();
    assertThat(childHasRank).isTrue();
  }

  @Test
  public void emitsWithExplicitTimestamp() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    PerfettoTrace.instant(FOO_CATEGORY, "ts").setTimestamp(123456789L).emit();

    Trace trace = Trace.parseFrom(session.close());

    boolean found = false;
    for (TracePacket packet : trace.getPacketList()) {
      if (packet.hasTrackEvent()
          && TrackEvent.Type.TYPE_INSTANT.equals(packet.getTrackEvent().getType())) {
        // The explicit timestamp + boottime clock are written on the packet.
        assertThat(packet.getTimestamp()).isEqualTo(123456789L);
        assertThat(packet.getTimestampClockId()).isEqualTo(6); // CLOCK_BOOTTIME
        found = true;
      }
    }
    assertThat(found).isTrue();
  }

  @Test
  public void emitsTypedProtoFields() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    // High, unused field ids so the values survive as unknown fields we can read
    // back (the generated TrackEvent proto would drop known-but-wrong-type ids).
    PerfettoTrace.instant(FOO_CATEGORY, "typed")
        .beginProto()
        .addFieldFixed64(9001, 0x1122334455667788L)
        .addFieldFixed32(9002, 0x11223344)
        .addFieldFloat(9003, 1.5f)
        .addFieldBytes(9004, new byte[] {1, 2, 3})
        .endProto()
        .emit();

    Trace trace = Trace.parseFrom(session.close());

    boolean found = false;
    for (TracePacket packet : trace.getPacketList()) {
      if (!packet.hasTrackEvent() || packet.getTrackEvent().getUnknownFields().asMap().isEmpty()) {
        continue;
      }
      var uf = packet.getTrackEvent().getUnknownFields();
      assertThat(uf.getField(9001).getFixed64List()).contains(0x1122334455667788L);
      assertThat(uf.getField(9002).getFixed32List()).contains(0x11223344);
      assertThat(uf.getField(9003).getFixed32List()).contains(Float.floatToRawIntBits(1.5f));
      assertThat(uf.getField(9004).getLengthDelimitedList())
          .contains(ByteString.copyFrom(new byte[] {1, 2, 3}));
      found = true;
    }
    assertThat(found).isTrue();
  }

  @Test
  public void builderRoutesFlowsThroughJavaEmit() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    PerfettoTrace.instant(FOO_CATEGORY, "event")
        .addFlow(2)
        .addFlow(3)
        .addTerminatingFlow(4)
        .emit();

    Trace trace = Trace.parseFrom(session.close());

    boolean found = false;
    for (TracePacket packet : trace.getPacketList()) {
      if (packet.hasTrackEvent()) {
        TrackEvent event = packet.getTrackEvent();
        if (event.getFlowIdsCount() == 2 && event.getTerminatingFlowIdsCount() == 1) {
          // Flow ids are folded with the process track uuid, so we check the
          // counts and distinctness rather than exact values.
          assertThat(new HashSet<>(event.getFlowIdsList())).hasSize(2);
          found = true;
        }
      }
    }
    assertThat(found).isTrue();
  }

  @Test
  public void builderRoutesCounterThroughJavaEmit() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    PerfettoTrace.counter(FOO_CATEGORY, 42).usingProcessCounterTrack("ctr").emit();
    PerfettoTrace.counter(FOO_CATEGORY, 3.5)
        .usingProcessCounterTrackWithDynamicName("ctr2")
        .emit();

    Trace trace = Trace.parseFrom(session.close());

    boolean hasCounterDescriptor = false;
    boolean hasIntCounter = false;
    boolean hasDoubleCounter = false;
    for (TracePacket packet : trace.getPacketList()) {
      if (packet.hasTrackDescriptor() && packet.getTrackDescriptor().hasCounter()) {
        hasCounterDescriptor = true;
      }
      if (packet.hasTrackEvent()) {
        TrackEvent event = packet.getTrackEvent();
        if (TrackEvent.Type.TYPE_COUNTER.equals(event.getType())) {
          if (event.getCounterValue() == 42) {
            hasIntCounter = true;
          }
          if (event.getDoubleCounterValue() == 3.5) {
            hasDoubleCounter = true;
          }
        }
      }
    }
    assertThat(hasCounterDescriptor).isTrue();
    assertThat(hasIntCounter).isTrue();
    assertThat(hasDoubleCounter).isTrue();
  }

  @Test
  public void builderRoutesProtoThroughJavaEmit() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    PerfettoTrace.instant(FOO_CATEGORY, "event_proto")
        .beginProto()
        .beginNested(33L) // TrackEvent.source_location
        .addField(4L, 2L) // line_number
        .addField(3L, "ActivityManagerService.java:11489") // function_name
        .endNested()
        .addField(2001L, "AIDL::IActivityManager")
        .endProto()
        .emit();

    Trace trace = Trace.parseFrom(session.close());

    boolean hasSourceLocation = false;
    for (TracePacket packet : trace.getPacketList()) {
      if (packet.hasTrackEvent() && packet.getTrackEvent().hasSourceLocation()) {
        SourceLocation loc = packet.getTrackEvent().getSourceLocation();
        if ("ActivityManagerService.java:11489".equals(loc.getFunctionName())
            && loc.getLineNumber() == 2) {
          hasSourceLocation = true;
        }
      }
    }
    assertThat(hasSourceLocation).isTrue();
  }

  @Test
  public void builderRoutesInternedProtoThroughJavaEmit() throws Exception {
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    // internedTypeId 44 = InternedData.android_job_name (an InternedString).
    PerfettoTrace.instant(FOO_CATEGORY, "event_with_interning")
        .beginProto()
        .addFieldWithInterning(1L, "my_interned_string", 44L)
        .endProto()
        .emit();

    Trace trace = Trace.parseFrom(session.close());

    boolean hasInternedString = false;
    for (TracePacket packet : trace.getPacketList()) {
      if (packet.hasInternedData()
          && packet.getInternedData().getAndroidJobNameCount() > 0
          && "my_interned_string"
              .equals(packet.getInternedData().getAndroidJobName(0).getName())) {
        hasInternedString = true;
      }
    }
    assertThat(hasInternedString).isTrue();
  }

  @Test
  public void disabledCategoryEmitsNothing() throws Exception {
    Category barCategory = new Category("bar").register();
    // Only FOO is enabled by the config; bar should be dropped.
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig().toByteArray());

    PerfettoEvent.emit(TYPE_INSTANT, barCategory, "event");

    Trace trace = Trace.parseFrom(session.close());
    for (TracePacket packet : trace.getPacketList()) {
      assertThat(packet.hasTrackEvent()).isFalse();
    }
  }

  private static TraceConfig traceConfig() {
    BufferConfig bufferConfig = BufferConfig.newBuilder().setSizeKb(1024).build();
    TrackEventConfig trackEventConfig =
        TrackEventConfig.newBuilder().addEnabledCategories(FOO).build();
    DataSourceConfig dsConfig =
        DataSourceConfig.newBuilder()
            .setName("track_event")
            .setTargetBuffer(0)
            .setTrackEventConfig(trackEventConfig)
            .build();
    DataSource ds = DataSource.newBuilder().setConfig(dsConfig).build();
    return TraceConfig.newBuilder().addBuffers(bufferConfig).addDataSources(ds).build();
  }

  private void collectInternedData(TracePacket packet) {
    if (!packet.hasInternedData()) {
      return;
    }
    InternedData data = packet.getInternedData();
    for (EventCategory cat : data.getEventCategoriesList()) {
      mCategoryNames.add(cat.getName());
    }
    for (EventName ev : data.getEventNamesList()) {
      mEventNames.add(ev.getName());
    }
  }
}
