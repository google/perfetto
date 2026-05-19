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
import dev.perfetto.sdk.PerfettoEvent;
import dev.perfetto.sdk.PerfettoTrace;
import dev.perfetto.sdk.PerfettoTrackEventBuilder;
import java.util.Set;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import perfetto.protos.DataSourceConfigOuterClass.DataSourceConfig;
import perfetto.protos.InternedDataOuterClass.InternedData;
import perfetto.protos.TraceConfigOuterClass.TraceConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig.BufferConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig.DataSource;
import perfetto.protos.TraceOuterClass.Trace;
import perfetto.protos.TracePacketOuterClass.TracePacket;
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
    boolean previous = PerfettoTrackEventBuilder.getUseJavaEmit();
    PerfettoTrackEventBuilder.setUseJavaEmit(true);
    try {
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
    } finally {
      PerfettoTrackEventBuilder.setUseJavaEmit(previous);
    }
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
