/*
 * Copyright (C) 2024 The Android Open Source Project
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

import static dev.perfetto.sdk.PerfettoTrace.Category;
import dev.perfetto.sdk.*;

import static com.google.common.truth.Truth.assertThat;

import static perfetto.protos.ChromeLatencyInfoOuterClass.ChromeLatencyInfo.LatencyComponentType.COMPONENT_INPUT_EVENT_LATENCY_BEGIN_RWH;
import static perfetto.protos.ChromeLatencyInfoOuterClass.ChromeLatencyInfo.LatencyComponentType.COMPONENT_INPUT_EVENT_LATENCY_SCROLL_UPDATE_ORIGINAL;

import android.util.ArraySet;

import android.os.Process;

import androidx.test.InstrumentationRegistry;
import androidx.test.ext.junit.runners.AndroidJUnit4;

import org.junit.Before;
import org.junit.Ignore;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import perfetto.protos.ChromeLatencyInfoOuterClass.ChromeLatencyInfo;
import perfetto.protos.ChromeLatencyInfoOuterClass.ChromeLatencyInfo.ComponentInfo;
import perfetto.protos.DataSourceConfigOuterClass.DataSourceConfig;
import perfetto.protos.DebugAnnotationOuterClass.DebugAnnotation;
import perfetto.protos.DebugAnnotationOuterClass.DebugAnnotationName;
import perfetto.protos.InternedDataOuterClass.InternedData;
import perfetto.protos.SourceLocationOuterClass.SourceLocation;
import perfetto.protos.TraceConfigOuterClass.TraceConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig.BufferConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig.DataSource;
import perfetto.protos.TraceConfigOuterClass.TraceConfig.TriggerConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig.TriggerConfig.Trigger;
import perfetto.protos.TraceOuterClass.Trace;
import perfetto.protos.TracePacketOuterClass.TracePacket;
import perfetto.protos.TrackDescriptorOuterClass.TrackDescriptor;
import perfetto.protos.TrackEventConfigOuterClass.TrackEventConfig;
import perfetto.protos.TrackEventOuterClass.EventCategory;
import perfetto.protos.TrackEventOuterClass.EventName;
import perfetto.protos.TrackEventOuterClass.TrackEvent;

import java.util.List;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * This class is used to test the native tracing support. Run this test
 * while tracing on the emulator and then run traceview to view the trace.
 */
@RunWith(AndroidJUnit4.class)
public class PerfettoTraceTest {
    static {
        System.loadLibrary("perfetto_jni_lib");
    }

    private static final String TAG = "PerfettoTraceTest";
    private static final String FOO = "foo";
    private static final String BAR = "bar";
    private static final String TEXT_ABOVE_4K_SIZE =
            new String(new char[8192]).replace('\0', 'a');

    private static final Category FOO_CATEGORY = new Category(FOO);
    private static final int MESSAGE = 1234567;
    private static final int MESSAGE_COUNT = 3;

    private final Set<String> mCategoryNames = new ArraySet<>();
    private final Set<String> mEventNames = new ArraySet<>();
    private final Set<String> mDebugAnnotationNames = new ArraySet<>();
    private final Set<String> mTrackNames = new ArraySet<>();

    @Before
    public void setUp() {
        PerfettoTrace.register(true);
        // 'var unused' suppress error-prone warning
        var unused = FOO_CATEGORY.register();

        mCategoryNames.clear();
        mEventNames.clear();
        mDebugAnnotationNames.clear();
        mTrackNames.clear();
    }

    @Test
    public void testDebugAnnotations() throws Exception {
        TraceConfig traceConfig = getTraceConfig(FOO);

        PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig.toByteArray());

        PerfettoTrace.instant(FOO_CATEGORY, "event")
                .setFlow(2)
                .setTerminatingFlow(3)
                .addArg("long_val", 10000000000L)
                .addArg("bool_val", true)
                .addArg("double_val", 3.14)
                .addArg("string_val", FOO)
                .emit();

        byte[] traceBytes = session.close();

        Trace trace = Trace.parseFrom(traceBytes);

        boolean hasTrackEvent = false;
        boolean hasDebugAnnotations = false;
        for (TracePacket packet: trace.getPacketList()) {
            TrackEvent event;
            if (packet.hasTrackEvent()) {
                hasTrackEvent = true;
                event = packet.getTrackEvent();

                if (TrackEvent.Type.TYPE_INSTANT.equals(event.getType())
                        && event.getDebugAnnotationsCount() == 4 && event.getFlowIdsCount() == 1
                        && event.getTerminatingFlowIdsCount() == 1) {
                    hasDebugAnnotations = true;

                    List<DebugAnnotation> annotations = event.getDebugAnnotationsList();

                    assertThat(annotations.get(0).getIntValue()).isEqualTo(10000000000L);
                    assertThat(annotations.get(1).getBoolValue()).isTrue();
                    assertThat(annotations.get(2).getDoubleValue()).isEqualTo(3.14);
                    assertThat(annotations.get(3).getStringValue()).isEqualTo(FOO);
                }
            }

            collectInternedData(packet);
        }

        assertThat(hasTrackEvent).isTrue();
        assertThat(hasDebugAnnotations).isTrue();
        assertThat(mCategoryNames).contains(FOO);

        assertThat(mDebugAnnotationNames).contains("long_val");
        assertThat(mDebugAnnotationNames).contains("bool_val");
        assertThat(mDebugAnnotationNames).contains("double_val");
        assertThat(mDebugAnnotationNames).contains("string_val");
    }

    @Test
    public void testNamedTrack() throws Exception {
        TraceConfig traceConfig = getTraceConfig(FOO);

        PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig.toByteArray());

        PerfettoTrace.begin(FOO_CATEGORY, "event")
                .usingNamedTrack(PerfettoTrace.getProcessTrackUuid(), FOO)
                .emit();


        PerfettoTrace.end(FOO_CATEGORY)
                .usingNamedTrack(PerfettoTrace.getThreadTrackUuid(Process.myTid()), "bar")
                .emit();

        Trace trace = Trace.parseFrom(session.close());

        boolean hasTrackEvent = false;
        boolean hasTrackUuid = false;
        for (TracePacket packet: trace.getPacketList()) {
            TrackEvent event;
            if (packet.hasTrackEvent()) {
                hasTrackEvent = true;
                event = packet.getTrackEvent();

                if (TrackEvent.Type.TYPE_SLICE_BEGIN.equals(event.getType())
                        && event.hasTrackUuid()) {
                    hasTrackUuid = true;
                }

                if (TrackEvent.Type.TYPE_SLICE_END.equals(event.getType())
                        && event.hasTrackUuid()) {
                    hasTrackUuid &= true;
                }
            }

            collectInternedData(packet);
            collectTrackNames(packet);
        }

        assertThat(hasTrackEvent).isTrue();
        assertThat(hasTrackUuid).isTrue();
        assertThat(mCategoryNames).contains(FOO);
        assertThat(mTrackNames).contains(FOO);
        assertThat(mTrackNames).contains("bar");
    }

    @Test
    public void testProcessThreadNamedTrack() throws Exception {
        TraceConfig traceConfig = getTraceConfig(FOO);

        PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig.toByteArray());

        PerfettoTrace.begin(FOO_CATEGORY, "event")
                .usingProcessNamedTrack(FOO)
                .emit();


        PerfettoTrace.end(FOO_CATEGORY)
                .usingThreadNamedTrack(Process.myTid(), "bar")
                .emit();

        Trace trace = Trace.parseFrom(session.close());

        boolean hasTrackEvent = false;
        boolean hasTrackUuid = false;
        for (TracePacket packet: trace.getPacketList()) {
            TrackEvent event;
            if (packet.hasTrackEvent()) {
                hasTrackEvent = true;
                event = packet.getTrackEvent();

                if (TrackEvent.Type.TYPE_SLICE_BEGIN.equals(event.getType())
                        && event.hasTrackUuid()) {
                    hasTrackUuid = true;
                }

                if (TrackEvent.Type.TYPE_SLICE_END.equals(event.getType())
                        && event.hasTrackUuid()) {
                    hasTrackUuid &= true;
                }
            }

            collectInternedData(packet);
            collectTrackNames(packet);
        }

        assertThat(hasTrackEvent).isTrue();
        assertThat(hasTrackUuid).isTrue();
        assertThat(mCategoryNames).contains(FOO);
        assertThat(mTrackNames).contains(FOO);
        assertThat(mTrackNames).contains("bar");
    }

    @Test
    public void testCounterSimple() throws Exception {
        TraceConfig traceConfig = getTraceConfig(FOO);

        PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig.toByteArray());

        PerfettoTrace.counter(FOO_CATEGORY, 16, FOO).emit();

        PerfettoTrace.counter(FOO_CATEGORY, 3.14, "bar").emit();

        Trace trace = Trace.parseFrom(session.close());

        boolean hasTrackEvent = false;
        boolean hasCounterValue = false;
        boolean hasDoubleCounterValue = false;
        for (TracePacket packet: trace.getPacketList()) {
            TrackEvent event;
            if (packet.hasTrackEvent()) {
                hasTrackEvent = true;
                event = packet.getTrackEvent();

                if (TrackEvent.Type.TYPE_COUNTER.equals(event.getType())
                        && event.getCounterValue() == 16) {
                    hasCounterValue = true;
                }

                if (TrackEvent.Type.TYPE_COUNTER.equals(event.getType())
                        && event.getDoubleCounterValue() == 3.14) {
                    hasDoubleCounterValue = true;
                }
            }

            collectTrackNames(packet);
        }

        assertThat(hasTrackEvent).isTrue();
        assertThat(hasCounterValue).isTrue();
        assertThat(hasDoubleCounterValue).isTrue();
        assertThat(mTrackNames).contains(FOO);
        assertThat(mTrackNames).contains(BAR);
    }

    @Test
    public void testCounter() throws Exception {
        TraceConfig traceConfig = getTraceConfig(FOO);

        PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig.toByteArray());

        PerfettoTrace.counter(FOO_CATEGORY, 16)
                .usingCounterTrack(PerfettoTrace.getProcessTrackUuid(), FOO).emit();

        PerfettoTrace.counter(FOO_CATEGORY, 3.14)
                .usingCounterTrack(PerfettoTrace.getThreadTrackUuid(Process.myTid()),
                                   "bar").emit();

        Trace trace = Trace.parseFrom(session.close());

        boolean hasTrackEvent = false;
        boolean hasCounterValue = false;
        boolean hasDoubleCounterValue = false;
        for (TracePacket packet: trace.getPacketList()) {
            TrackEvent event;
            if (packet.hasTrackEvent()) {
                hasTrackEvent = true;
                event = packet.getTrackEvent();

                if (TrackEvent.Type.TYPE_COUNTER.equals(event.getType())
                        && event.getCounterValue() == 16) {
                    hasCounterValue = true;
                }

                if (TrackEvent.Type.TYPE_COUNTER.equals(event.getType())
                        && event.getDoubleCounterValue() == 3.14) {
                    hasDoubleCounterValue = true;
                }
            }

            collectTrackNames(packet);
        }

        assertThat(hasTrackEvent).isTrue();
        assertThat(hasCounterValue).isTrue();
        assertThat(hasDoubleCounterValue).isTrue();
        assertThat(mTrackNames).contains(FOO);
        assertThat(mTrackNames).contains("bar");
    }

    @Test
    public void testProcessThreadCounter() throws Exception {
        TraceConfig traceConfig = getTraceConfig(FOO);

        PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig.toByteArray());

        PerfettoTrace.counter(FOO_CATEGORY, 16).usingProcessCounterTrack(FOO).emit();

        PerfettoTrace.counter(FOO_CATEGORY, 3.14)
                .usingThreadCounterTrack(Process.myTid(), "bar").emit();

        Trace trace = Trace.parseFrom(session.close());

        boolean hasTrackEvent = false;
        boolean hasCounterValue = false;
        boolean hasDoubleCounterValue = false;
        for (TracePacket packet: trace.getPacketList()) {
            TrackEvent event;
            if (packet.hasTrackEvent()) {
                hasTrackEvent = true;
                event = packet.getTrackEvent();

                if (TrackEvent.Type.TYPE_COUNTER.equals(event.getType())
                        && event.getCounterValue() == 16) {
                    hasCounterValue = true;
                }

                if (TrackEvent.Type.TYPE_COUNTER.equals(event.getType())
                        && event.getDoubleCounterValue() == 3.14) {
                    hasDoubleCounterValue = true;
                }
            }

            collectTrackNames(packet);
        }

        assertThat(hasTrackEvent).isTrue();
        assertThat(hasCounterValue).isTrue();
        assertThat(hasDoubleCounterValue).isTrue();
        assertThat(mTrackNames).contains(FOO);
        assertThat(mTrackNames).contains("bar");
    }

    @Test
    public void testProto() throws Exception {
        TraceConfig traceConfig = getTraceConfig(FOO);

        PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig.toByteArray());

        PerfettoTrace.instant(FOO_CATEGORY, "event_proto")
                .beginProto()
                .beginNested(33L)
                .addField(4L, 2L)
                .addField(3, "ActivityManagerService.java:11489")
                .endNested()
                .addField(2001, "AIDL::IActivityManager")
                .endProto()
                .emit();

        byte[] traceBytes = session.close();

        Trace trace = Trace.parseFrom(traceBytes);

        boolean hasTrackEvent = false;
        boolean hasSourceLocation = false;
        for (TracePacket packet: trace.getPacketList()) {
            TrackEvent event;
            if (packet.hasTrackEvent()) {
                hasTrackEvent = true;
                event = packet.getTrackEvent();

                if (TrackEvent.Type.TYPE_INSTANT.equals(event.getType())
                        && event.hasSourceLocation()) {
                    SourceLocation loc = event.getSourceLocation();
                    if ("ActivityManagerService.java:11489".equals(loc.getFunctionName())
                            && loc.getLineNumber() == 2) {
                        hasSourceLocation = true;
                    }
                }
            }

            collectInternedData(packet);
        }

        assertThat(hasTrackEvent).isTrue();
        assertThat(hasSourceLocation).isTrue();
        assertThat(mCategoryNames).contains(FOO);
    }

    @Test
    public void testProtoWithSlowPath() throws Exception {
        TraceConfig traceConfig = getTraceConfig(FOO);

        PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig.toByteArray());

        PerfettoTrace.instant(FOO_CATEGORY, "event_proto")
                .beginProto()
                .beginNested(33L)
                .addField(4L, 2L)
                .addField(3, TEXT_ABOVE_4K_SIZE)
                .endNested()
                .addField(2001, "AIDL::IActivityManager")
                .endProto()
                .emit();

        byte[] traceBytes = session.close();

        Trace trace = Trace.parseFrom(traceBytes);

        boolean hasTrackEvent = false;
        boolean hasSourceLocation = false;
        for (TracePacket packet: trace.getPacketList()) {
            TrackEvent event;
            if (packet.hasTrackEvent()) {
                hasTrackEvent = true;
                event = packet.getTrackEvent();

                if (TrackEvent.Type.TYPE_INSTANT.equals(event.getType())
                        && event.hasSourceLocation()) {
                    SourceLocation loc = event.getSourceLocation();
                    if (TEXT_ABOVE_4K_SIZE.equals(loc.getFunctionName())
                            && loc.getLineNumber() == 2) {
                        hasSourceLocation = true;
                    }
                }
            }

            collectInternedData(packet);
        }

        assertThat(hasTrackEvent).isTrue();
        assertThat(hasSourceLocation).isTrue();
        assertThat(mCategoryNames).contains(FOO);
    }

    @Test
    public void testProtoNested() throws Exception {
        TraceConfig traceConfig = getTraceConfig(FOO);

        PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig.toByteArray());

        PerfettoTrace.instant(FOO_CATEGORY, "event_proto_nested")
                .beginProto()
                .beginNested(29L)
                .beginNested(4L)
                .addField(1L, 2)
                .addField(2L, 20000)
                .endNested()
                .beginNested(4L)
                .addField(1L, 1)
                .addField(2L, 40000)
                .endNested()
                .endNested()
                .endProto()
                .emit();

        byte[] traceBytes = session.close();

        Trace trace = Trace.parseFrom(traceBytes);

        boolean hasTrackEvent = false;
        boolean hasChromeLatencyInfo = false;

        for (TracePacket packet: trace.getPacketList()) {
            TrackEvent event;
            if (packet.hasTrackEvent()) {
                hasTrackEvent = true;
                event = packet.getTrackEvent();

                if (TrackEvent.Type.TYPE_INSTANT.equals(event.getType())
                        && event.hasChromeLatencyInfo()) {
                    ChromeLatencyInfo latencyInfo = event.getChromeLatencyInfo();
                    if (latencyInfo.getComponentInfoCount() == 2) {
                        hasChromeLatencyInfo = true;
                        ComponentInfo cmpInfo1 = latencyInfo.getComponentInfo(0);
                        assertThat(cmpInfo1.getComponentType())
                                .isEqualTo(COMPONENT_INPUT_EVENT_LATENCY_SCROLL_UPDATE_ORIGINAL);
                        assertThat(cmpInfo1.getTimeUs()).isEqualTo(20000);

                        ComponentInfo cmpInfo2 = latencyInfo.getComponentInfo(1);
                        assertThat(cmpInfo2.getComponentType())
                                .isEqualTo(COMPONENT_INPUT_EVENT_LATENCY_BEGIN_RWH);
                        assertThat(cmpInfo2.getTimeUs()).isEqualTo(40000);
                    }
                }
            }

            collectInternedData(packet);
        }

        assertThat(hasTrackEvent).isTrue();
        assertThat(hasChromeLatencyInfo).isTrue();
        assertThat(mCategoryNames).contains(FOO);
    }

    @Test
    public void testActivateTrigger() throws Exception {
        TraceConfig traceConfig = getTriggerTraceConfig(FOO, FOO);

        PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig.toByteArray());

        PerfettoTrace.instant(FOO_CATEGORY, "event_trigger").emit();

        PerfettoTrace.activateTrigger(FOO, 1000);

        byte[] traceBytes = session.close();

        Trace trace = Trace.parseFrom(traceBytes);

        boolean hasTrackEvent = false;
        boolean hasChromeLatencyInfo = false;

        for (TracePacket packet: trace.getPacketList()) {
            TrackEvent event;
            if (packet.hasTrackEvent()) {
                hasTrackEvent = true;
            }

            collectInternedData(packet);
        }

        assertThat(mCategoryNames).contains(FOO);
    }

    @Test
    public void testRegister() throws Exception {
        TraceConfig traceConfig = getTraceConfig(BAR);

        Category barCategory = new Category(BAR);
        PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig.toByteArray());

        PerfettoTrace.instant(barCategory, "event")
                .addArg("before", 1)
                .emit();
        // 'var unused' suppress error-prone warning
        var unused = barCategory.register();

        PerfettoTrace.instant(barCategory, "event")
                .addArg("after", 1)
                .emit();

        byte[] traceBytes = session.close();

        Trace trace = Trace.parseFrom(traceBytes);

        boolean hasTrackEvent = false;
        for (TracePacket packet: trace.getPacketList()) {
            TrackEvent event;
            if (packet.hasTrackEvent()) {
                hasTrackEvent = true;
                event = packet.getTrackEvent();
            }

            collectInternedData(packet);
        }

        assertThat(hasTrackEvent).isTrue();
        assertThat(mCategoryNames).contains(BAR);

        assertThat(mDebugAnnotationNames).contains("after");
        assertThat(mDebugAnnotationNames).doesNotContain("before");
    }

    private TrackEvent getTrackEvent(Trace trace, int idx) {
        int curIdx = 0;
        for (TracePacket packet: trace.getPacketList()) {
            if (packet.hasTrackEvent()) {
                if (curIdx++ == idx) {
                    return packet.getTrackEvent();
                }
            }
        }

        return null;
    }

    private TraceConfig getTraceConfig(String cat) {
        BufferConfig bufferConfig = BufferConfig.newBuilder().setSizeKb(1024).build();
        TrackEventConfig trackEventConfig = TrackEventConfig
                .newBuilder()
                .addEnabledCategories(cat)
                .build();
        DataSourceConfig dsConfig = DataSourceConfig
                .newBuilder()
                .setName("track_event")
                .setTargetBuffer(0)
                .setTrackEventConfig(trackEventConfig)
                .build();
        DataSource ds = DataSource.newBuilder().setConfig(dsConfig).build();
        TraceConfig traceConfig = TraceConfig
                .newBuilder()
                .addBuffers(bufferConfig)
                .addDataSources(ds)
                .build();
        return traceConfig;
    }

    private TraceConfig getTriggerTraceConfig(String cat, String triggerName) {
        BufferConfig bufferConfig = BufferConfig.newBuilder().setSizeKb(1024).build();
        TrackEventConfig trackEventConfig = TrackEventConfig
                .newBuilder()
                .addEnabledCategories(cat)
                .build();
        DataSourceConfig dsConfig = DataSourceConfig
                .newBuilder()
                .setName("track_event")
                .setTargetBuffer(0)
                .setTrackEventConfig(trackEventConfig)
                .build();
        DataSource ds = DataSource.newBuilder().setConfig(dsConfig).build();
        Trigger trigger = Trigger.newBuilder().setName(triggerName).build();
        TriggerConfig triggerConfig = TriggerConfig
                .newBuilder()
                .setTriggerMode(TriggerConfig.TriggerMode.STOP_TRACING)
                .setTriggerTimeoutMs(1000)
                .addTriggers(trigger)
                .build();
        TraceConfig traceConfig = TraceConfig
                .newBuilder()
                .addBuffers(bufferConfig)
                .addDataSources(ds)
                .setTriggerConfig(triggerConfig)
                .build();
        return traceConfig;
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
        for (DebugAnnotationName dbg : data.getDebugAnnotationNamesList()) {
            mDebugAnnotationNames.add(dbg.getName());
        }
    }

    private void collectTrackNames(TracePacket packet) {
        if (!packet.hasTrackDescriptor()) {
            return;
        }
        TrackDescriptor desc = packet.getTrackDescriptor();
        mTrackNames.add(desc.getName());
    }
}
