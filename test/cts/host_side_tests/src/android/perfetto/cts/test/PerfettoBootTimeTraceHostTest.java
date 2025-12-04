/*
 * Copyright (C) 2025 The Android Open Source Project
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

package android.perfetto.cts.test;

import static com.google.common.truth.Truth.assertThat;

import com.android.tradefed.device.DeviceNotAvailableException;
import com.android.tradefed.device.DeviceProperties;
import com.android.tradefed.device.ITestDevice;
import com.android.tradefed.testtype.DeviceJUnit4ClassRunner;
import com.android.tradefed.testtype.junit4.BaseHostJUnit4Test;
import com.android.tradefed.util.CommandResult;
import com.android.tradefed.util.CommandStatus;
import com.android.tradefed.util.FileUtil;
import com.android.tradefed.util.ProcessInfo;

import com.google.protobuf.InvalidProtocolBufferException;

import org.junit.After;
import org.junit.Assert;
import org.junit.Assume;
import org.junit.Before;
import org.junit.BeforeClass;
import org.junit.Test;
import org.junit.runner.RunWith;

import perfetto.protos.AndroidLog.AndroidLogPacket;
import perfetto.protos.TraceOuterClass.Trace;
import perfetto.protos.TracePacketOuterClass.TracePacket;
import perfetto.protos.TracingServiceStateOuterClass.TracingServiceState;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;

@RunWith(DeviceJUnit4ClassRunner.class)
public class PerfettoBootTimeTraceHostTest extends BaseHostJUnit4Test {
    private static final String BOOT_TRACE_ENABLE_PROPERTY = "persist.debug.perfetto.boottrace";
    private static final String BOOT_TRACE_RESULT_ON_DEVICE_PATH =
            "/data/misc/perfetto-traces/boottrace.perfetto-trace";
    private static final String BOOT_TRACE_CONFIG_ON_DEVICE_PATH =
            "/data/misc/perfetto-configs/boottrace.pbtxt";
    private static final String BOOT_TRACE_STOP_TRIGGER_CONFIG_ON_DEVICE_PATH =
            "/data/misc/perfetto-configs/stopboottracetrigger.pbtxt";

    private static final String BOOT_TRACE_CONFIG_SESSION_NAME = "PerfettoBootTimeTraceHostTest";
    private static final String BOOT_TRACE_CONFIG =
            """
            unique_session_name: "%s"
            buffers {
                size_kb: 1024
                fill_policy: RING_BUFFER
            }
            # Boost to the highest priority to always capture logcat updates
            priority_boost {
                policy: POLICY_SCHED_FIFO
                priority: 99
            }
            data_sources: {
                config {
                    name: "android.log"
                    target_buffer: 0
                    android_log_config {
                        log_ids: LID_DEFAULT
                        log_ids: LID_EVENTS
                        log_ids: LID_KERNEL
                        log_ids: LID_SYSTEM
                    }
                }
            }
            trigger_config {
              triggers: {
                name: "stop-boot-trace-trigger"
                stop_delay_ms: 0
              }
              trigger_mode: STOP_TRACING
              trigger_timeout_ms: 600000 # can't be used together with 'duration_ms', 10 minutes
            }
            """
                    .formatted(BOOT_TRACE_CONFIG_SESSION_NAME)
                    .stripIndent();

    private static final String BOOT_TRACE_STOP_TRIGGER_CONFIG =
            """
            activate_triggers: "stop-boot-trace-trigger"
            """
                    .stripIndent();

    private static final Duration TRACING_SESSION_STOP_MAX_WAIT_TIME = Duration.ofSeconds(30);
    private static final Duration TRACING_SESSION_STOP_DELAY_WAIT_TIME = Duration.ofSeconds(1);

    private static File mTraceConfigFile = null;
    private static File mTraceStopTriggerConfigFile = null;

    private ITestDevice mTestDevice = null;

    @BeforeClass
    public static void setUpClass() throws IOException {
        mTraceConfigFile = FileUtil.createTempFile("boot_trace_config", ".pbtxt");
        FileUtil.writeToFile(BOOT_TRACE_CONFIG, mTraceConfigFile);
        mTraceStopTriggerConfigFile =
                FileUtil.createTempFile("boot_trace_stop_trigger_config", ".pbtxt");
        FileUtil.writeToFile(BOOT_TRACE_STOP_TRIGGER_CONFIG, mTraceStopTriggerConfigFile);
    }

    @Before
    public void setUp() throws DeviceNotAvailableException {
        mTestDevice = getDevice();
        String buildType = mTestDevice.getProperty(DeviceProperties.BUILD_TYPE);
        Assume.assumeTrue(
                "Recording Perfetto traces on boot is supported only on 'userdebug' or 'eng'"
                    + " builds.",
                buildType.equals("userdebug") || buildType.equals("eng"));
        // Make sure device is in the expected state
        // Traced is running
        ProcessInfo traced = mTestDevice.getProcessByName("traced");
        assertThat(traced).isNotNull();
        // Boot trace was not triggered
        assertThat(mTestDevice.getProperty(BOOT_TRACE_ENABLE_PROPERTY)).isNotEqualTo("1");
        if (mTestDevice.doesFileExist(BOOT_TRACE_RESULT_ON_DEVICE_PATH)) {
            // Boot trace is not running
            CommandResult result =
                    mTestDevice.executeShellV2Command(
                            "lsof -t " + BOOT_TRACE_RESULT_ON_DEVICE_PATH);
            if (result.getStatus() == CommandStatus.SUCCESS) {
                String pid = result.getStdout().trim();
                if (!pid.isEmpty()) {
                    Assert.fail("Boot trace is running, pid: " + pid);
                }
            }
            mTestDevice.deleteFile(BOOT_TRACE_RESULT_ON_DEVICE_PATH);
        }
        cleanUpOnDeviceBootTraceFiles();
    }

    @After
    public void tearDown() throws DeviceNotAvailableException {
        cleanUpOnDeviceBootTraceFiles();
        mTestDevice = null;
    }

    @Test
    public void testBootTraceStart() throws Exception {
        assertThat(mTestDevice.pushFile(mTraceConfigFile, BOOT_TRACE_CONFIG_ON_DEVICE_PATH))
                .isTrue();
        assertThat(mTestDevice.setProperty(BOOT_TRACE_ENABLE_PROPERTY, "1")).isTrue();

        // Assert tracing session is not started when property set
        List<String> runningTracingSessionNames = assertGetRunningTracingSessionNames();
        assertThat(runningTracingSessionNames).doesNotContain(BOOT_TRACE_CONFIG_SESSION_NAME);

        mTestDevice.reboot();

        // Assert tracing session is started on device reboot
        runningTracingSessionNames = assertGetRunningTracingSessionNames();
        assertThat(runningTracingSessionNames).contains(BOOT_TRACE_CONFIG_SESSION_NAME);

        // Assert property is reset after reboot
        assertThat(mTestDevice.getProperty(BOOT_TRACE_ENABLE_PROPERTY)).isNotEqualTo("1");

        // Now push the trigger config to stop the tracing session
        assertThat(
                        mTestDevice.pushFile(
                                mTraceStopTriggerConfigFile,
                                BOOT_TRACE_STOP_TRIGGER_CONFIG_ON_DEVICE_PATH))
                .isTrue();
        CommandResult result =
                mTestDevice.executeShellV2Command(
                        "perfetto --txt -c " + BOOT_TRACE_STOP_TRIGGER_CONFIG_ON_DEVICE_PATH);
        assertThat(result.getStatus()).isEqualTo(CommandStatus.SUCCESS);

        // Assert boot tracing session stopped
        assertTracingSessionStopped(BOOT_TRACE_CONFIG_SESSION_NAME);

        assertThat(mTestDevice.doesFileExist(BOOT_TRACE_RESULT_ON_DEVICE_PATH)).isTrue();
        File traceFile = mTestDevice.pullFile(BOOT_TRACE_RESULT_ON_DEVICE_PATH);
        assertThat(traceFile).isNotNull();

        // Tracing session was started at the early boot stage, so we expect that:
        // 1. It has multiple logcat events with the "init" tag (issues by the init process)
        // 2. It was started before system is booted, so it has multiple "processing action .*
        // sys.boot_completed=1 .*" lines
        List<String> messages = getInitLogcatMessages(traceFile);
        assertThat(messages).isNotEmpty();
        boolean hasBootCompletedLine = false;
        for (String message : messages) {
            if (message.startsWith("processing action")
                    && message.contains("sys.boot_completed=1")) {
                hasBootCompletedLine = true;
                break;
            }
        }
        assertThat(hasBootCompletedLine).isTrue();
    }

    // This test verifies that when both trace and stop trigger configurations are pushed, the boot
    // tracing session starts automatically during early boot and stops once the device boot
    // process completes.
    @Test
    public void testBootTraceStartAndStop() throws Exception {
        assertThat(mTestDevice.pushFile(mTraceConfigFile, BOOT_TRACE_CONFIG_ON_DEVICE_PATH))
                .isTrue();
        assertThat(
                        mTestDevice.pushFile(
                                mTraceStopTriggerConfigFile,
                                BOOT_TRACE_STOP_TRIGGER_CONFIG_ON_DEVICE_PATH))
                .isTrue();
        assertThat(mTestDevice.setProperty(BOOT_TRACE_ENABLE_PROPERTY, "1")).isTrue();

        // Assert tracing session is not started when property set
        List<String> runningTracingSessionNames = assertGetRunningTracingSessionNames();
        assertThat(runningTracingSessionNames).doesNotContain(BOOT_TRACE_CONFIG_SESSION_NAME);

        // Assert there is no trace file before reboot
        assertThat(mTestDevice.doesFileExist(BOOT_TRACE_RESULT_ON_DEVICE_PATH)).isFalse();

        mTestDevice.reboot();

        // Assert property is reset after reboot
        assertThat(mTestDevice.getProperty(BOOT_TRACE_ENABLE_PROPERTY)).isNotEqualTo("1");
        // Assert tracing session was already stopped
        assertTracingSessionStopped(BOOT_TRACE_CONFIG_SESSION_NAME);

        assertThat(mTestDevice.doesFileExist(BOOT_TRACE_RESULT_ON_DEVICE_PATH)).isTrue();
        File traceFile = mTestDevice.pullFile(BOOT_TRACE_RESULT_ON_DEVICE_PATH);
        assertThat(traceFile).isNotNull();

        // Tracing session was started at the early boot stage, so we expect that:
        // 1. It has multiple logcat events with the "init" tag (issues by the init process)
        // 2. It is stopped by a trigger on a system boot, so it may or may not has any
        // "sys.boot_completed=1 .*" lines
        List<String> messages = getInitLogcatMessages(traceFile);
        assertThat(messages).isNotEmpty();
        boolean hasProcessingActionLine = false;
        for (String message : messages) {
            if (message.startsWith("processing action")) {
                hasProcessingActionLine = true;
                break;
            }
        }
        assertThat(hasProcessingActionLine).isTrue();
    }

    List<String> getInitLogcatMessages(File traceFile) throws IOException {
        ArrayList<String> messages = new ArrayList<>();
        Trace trace = Trace.parseFrom(new FileInputStream(traceFile));
        for (TracePacket packet : trace.getPacketList()) {
            if (packet.hasAndroidLog()) {
                AndroidLogPacket logPacket = packet.getAndroidLog();
                for (AndroidLogPacket.LogEvent event : logPacket.getEventsList()) {
                    if (event.getTag().equals("init")) {
                        messages.add(event.getMessage());
                    }
                }
            }
        }
        return messages;
    }

    private TracingServiceState assertGetTracingServiceState() throws DeviceNotAvailableException {
        ByteArrayOutputStream rawOutput = new ByteArrayOutputStream();
        CommandResult result = mTestDevice.executeShellV2Command("perfetto --query-raw", rawOutput);
        assertThat(result.getStatus()).isEqualTo(CommandStatus.SUCCESS);
        try {
            return TracingServiceState.parseFrom(rawOutput.toByteArray());
        } catch (InvalidProtocolBufferException e) {
            throw new RuntimeException(e);
        }
    }

    private List<String> assertGetRunningTracingSessionNames() throws DeviceNotAvailableException {
        TracingServiceState tracingServiceState = assertGetTracingServiceState();
        ArrayList<String> runningTracingSessionNames = new ArrayList<>();
        for (TracingServiceState.TracingSession session :
                tracingServiceState.getTracingSessionsList()) {
            if (session.getIsStarted()) {
                runningTracingSessionNames.add(session.getUniqueSessionName());
            }
        }
        return runningTracingSessionNames;
    }

    private void assertTracingSessionStopped(String tracingSessionName)
            throws InterruptedException, DeviceNotAvailableException {
        // Tracing session may not stop immediately, we give it some time to stop
        long attempts =
                TRACING_SESSION_STOP_MAX_WAIT_TIME.dividedBy(TRACING_SESSION_STOP_DELAY_WAIT_TIME);
        for (long i = 0; i < attempts; i++) {
            List<String> runningTracingSessionNames = assertGetRunningTracingSessionNames();
            if (!runningTracingSessionNames.contains(tracingSessionName)) {
                return;
            }
            Thread.sleep(TRACING_SESSION_STOP_DELAY_WAIT_TIME);
        }
        Assert.fail(
                "Tracing session '%s' is still running after %d seconds."
                        .formatted(
                                tracingSessionName,
                                TRACING_SESSION_STOP_MAX_WAIT_TIME.getSeconds()));
    }

    private void cleanUpOnDeviceBootTraceFiles() throws DeviceNotAvailableException {
        mTestDevice.deleteFile(BOOT_TRACE_STOP_TRIGGER_CONFIG_ON_DEVICE_PATH);
        mTestDevice.deleteFile(BOOT_TRACE_CONFIG_ON_DEVICE_PATH);
        mTestDevice.deleteFile(BOOT_TRACE_RESULT_ON_DEVICE_PATH);
    }
}
