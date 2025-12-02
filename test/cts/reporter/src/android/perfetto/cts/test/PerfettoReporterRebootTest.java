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
import static com.google.common.truth.Truth.assertWithMessage;

import com.android.tradefed.device.DeviceNotAvailableException;
import com.android.tradefed.device.ITestDevice;
import com.android.tradefed.testtype.DeviceJUnit4ClassRunner;
import com.android.tradefed.testtype.junit4.BaseHostJUnit4Test;
import com.android.tradefed.util.CommandResult;
import com.android.tradefed.util.CommandStatus;
import com.android.tradefed.util.FileUtil;
import com.android.tradefed.util.Pair;
import com.android.tradefed.util.ProcessInfo;

import com.google.protobuf.InvalidProtocolBufferException;

import org.junit.After;
import org.junit.Assert;
import org.junit.Before;
import org.junit.BeforeClass;
import org.junit.Test;
import org.junit.runner.RunWith;

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
import java.util.UUID;

@RunWith(DeviceJUnit4ClassRunner.class)
public class PerfettoReporterRebootTest extends BaseHostJUnit4Test {
    private static final String PERFETTO_SERVICE_ERROR = "Service error:";
    private final static String PERSISTENT_TRACE_SESSION_NAME =
            "reboot_host_test_persistent_session";
    private final static String PERSISTENT_TRACE_MINIMAL_CONFIG =
            """
            unique_session_name: "%s"
            duration_ms: 3600000 # 1 hour
            persist_trace_after_reboot: true
            write_into_file: true
            # see comment in PerfettoReporterTest#TestEndToEndReportPersistent for details
            # about the file write period
            file_write_period_ms: 500

            # Make the trace as small as possible (see b/282508742).
            builtin_data_sources {
              disable_clock_snapshotting: true
              disable_system_info: true
              disable_service_events: true
              disable_chunk_usage_histograms: true
            }

            buffers {
              size_kb: 1024
              fill_policy: RING_BUFFER
            }

            android_report_config {
                reporter_service_package: "android.perfetto.cts.reporter"
                reporter_service_class: "android.perfetto.cts.reporter.PerfettoReportService"
                use_pipe_in_framework_for_testing: true
            }
            """
                    .formatted(PERSISTENT_TRACE_SESSION_NAME)
                    .stripIndent();
    private final static String ON_DEVICE_PERFETTO_CONFIG_PATH =
            "/data/misc/perfetto-configs/reboot_host_test_persistent_session.pb.txt";
    private static final String ON_DEVICE_PERSISTENT_TRACE_OUTPUT_PATH =
            "/data/misc/perfetto-traces/persistent/" + PERSISTENT_TRACE_SESSION_NAME + ".pftrace";
    private final static String ON_DEVICE_REPORTED_TRACES_DIR =
            "/sdcard/Android/data/android.perfetto.cts.reporter/files";
    private static File mTraceConfigFile = null;

    private ITestDevice mTestDevice = null;

    @BeforeClass
    public static void oneTimeSetUp() throws IOException {
        mTraceConfigFile = FileUtil.createTempFile("persistent_trace", ".pb.txt");
        FileUtil.writeToFile(PERSISTENT_TRACE_MINIMAL_CONFIG, mTraceConfigFile);
    }

    @Before
    public void SetUp() throws DeviceNotAvailableException {
        mTestDevice = getDevice();

        // Assert traced is running.
        ProcessInfo traced = mTestDevice.getProcessByName("traced");
        assertThat(traced).isNotNull();

        assertThat(mTestDevice.pushFile(mTraceConfigFile, ON_DEVICE_PERFETTO_CONFIG_PATH)).isTrue();
        mTestDevice.deleteFile(ON_DEVICE_PERSISTENT_TRACE_OUTPUT_PATH);
    }

    @After
    public void TearDown() throws DeviceNotAvailableException {
        mTestDevice.deleteFile(ON_DEVICE_PERFETTO_CONFIG_PATH);
        mTestDevice.deleteFile(ON_DEVICE_PERSISTENT_TRACE_OUTPUT_PATH);
        mTestDevice = null;
    }

    //TODO(ktimofeev): Add more tests:
    // 1. Test that we can disable traced and enable it back
    // 2. Test that we can't call "perfetto --upload-after-reboot" after traced is started.
    // 3. Test the we can start traced even if the "perfetto --upload-after-reboot" crashed. 

    @Test
    public void testPersistentTraceReportedAfterReboot() throws Exception {
        assertPerfettoCommandmentSuccess(
                "perfetto --no-guardrails --upload --background --txt --config "
                        + ON_DEVICE_PERFETTO_CONFIG_PATH);

        // Assert tracing session started.
        List<String> runningTracingSessionNames = assertGetRunningTracingSessionNames();
        assertThat(runningTracingSessionNames).contains(PERSISTENT_TRACE_SESSION_NAME);

        // Even though we can set the UUID in the trace config, we parse it from the trace file
        // to assert that the file was flushed to disk before the reboot.
        File traceFile = waitForNotEmptyFileAndPull(ON_DEVICE_PERSISTENT_TRACE_OUTPUT_PATH);
        Pair<String, UUID> sessionNameAndUuid = assertGetSessionNameAndUuidFromTraceFile(traceFile);

        assertThat(sessionNameAndUuid.first).isEqualTo(PERSISTENT_TRACE_SESSION_NAME);

        mTestDevice.reboot();

        assertWithMessage("Trace file must be unlinked after reboot")
                .that(mTestDevice.doesFileExist(ON_DEVICE_PERSISTENT_TRACE_OUTPUT_PATH))
                .isFalse();

        String reportedFilePath =
                ON_DEVICE_REPORTED_TRACES_DIR + "/" + sessionNameAndUuid.second.toString();

        File reportedTraceFile = waitForNotEmptyFileAndPull(reportedFilePath);
        Pair<String, UUID> reportedSessionNameAndUuid =
                assertGetSessionNameAndUuidFromTraceFile(reportedTraceFile);

        assertThat(reportedSessionNameAndUuid).isEqualTo(sessionNameAndUuid);
    }

    private Pair<String, UUID> assertGetSessionNameAndUuidFromTraceFile(File traceFile)
            throws Exception {
        UUID uuid = null;
        String sessionName = null;
        Trace trace = Trace.parseFrom(new FileInputStream(traceFile));
        for (TracePacket packet : trace.getPacketList()) {
            if (packet.hasTraceUuid()) {
                uuid = new UUID(packet.getTraceUuid().getMsb(), packet.getTraceUuid().getLsb());
            } else if (packet.hasTraceConfig()) {
                sessionName = packet.getTraceConfig().getUniqueSessionName();
            }
        }
        assertThat(sessionName).isNotNull();
        assertThat(uuid).isNotNull();
        return Pair.create(sessionName, uuid);
    }

    private File waitForNotEmptyFileAndPull(String onDeviceFilePath) throws Exception {
        File pulledFile = null;
        for (int i = 0; i < 10; i++) {
            if (mTestDevice.doesFileExist(onDeviceFilePath)) {
                Integer maybeSize = statFileSizeInBytes(onDeviceFilePath);
                if (maybeSize != null && maybeSize > 0) {
                    pulledFile = mTestDevice.pullFile(onDeviceFilePath);
                    assertThat(pulledFile).isNotNull();
                    break;
                }
            }
            Thread.sleep(Duration.ofSeconds(1));
        }
        if (pulledFile == null) {
            Assert.fail("Timed out waiting for a not empty file '%s'".formatted(onDeviceFilePath));
        }
        return pulledFile;
    }

    private void assertPerfettoCommandmentSuccess(String command)
            throws DeviceNotAvailableException {
        CommandResult result = mTestDevice.executeShellV2Command(command);
        String message = "Failed to start perfetto: " + command;
        assertWithMessage(message).that(result.getExitCode()).isEqualTo(0);
        // If the error is reported from the traced to perfetto_cmd, the exit code is zero.
        assertWithMessage(message).that(result.getStderr()).doesNotContain(PERFETTO_SERVICE_ERROR);
    }

    private Integer statFileSizeInBytes(String onDeviceFilePath) throws Exception {
        CommandResult statResult =
                mTestDevice.executeShellV2Command("stat -c %s " + onDeviceFilePath);
        if (statResult.getExitCode() != 0) return null;
        String stdout = statResult.getStdout();
        if (stdout == null || stdout.isEmpty()) return null;
        return Integer.parseInt(stdout.trim());
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
}
