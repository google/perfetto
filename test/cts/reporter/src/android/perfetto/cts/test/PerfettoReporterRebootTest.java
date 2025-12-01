package android.perfetto.cts.test;

import static com.google.common.truth.Truth.assertThat;

import com.android.tradefed.device.DeviceNotAvailableException;
import com.android.tradefed.device.ITestDevice;
import com.android.tradefed.testtype.DeviceJUnit4ClassRunner;
import com.android.tradefed.testtype.junit4.BaseHostJUnit4Test;
import com.android.tradefed.util.CommandResult;
import com.android.tradefed.util.CommandStatus;
import com.android.tradefed.util.FileUtil;

import org.junit.After;
import org.junit.Before;
import org.junit.Assert;
import org.junit.BeforeClass;
import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

import com.android.tradefed.log.LogUtil.CLog;

import com.google.protobuf.InvalidProtocolBufferException;

import perfetto.protos.PerfettoTrace;
import perfetto.protos.TraceOuterClass.Trace;
import perfetto.protos.TracePacketOuterClass.TracePacket;
import perfetto.protos.TracingServiceStateOuterClass.TracingServiceState;

@RunWith(DeviceJUnit4ClassRunner.class)
public class PerfettoReporterRebootTest extends BaseHostJUnit4Test {
    private final static String PERSISTENT_TRACE_SESSION_NAME =
            "reboot_host_test_persistent_session";
    private final static String PERSISTENT_TRACE_MINIMAL_CONFIG = """
            unique_session_name: "%s"
            duration_ms: 3600000 # 1 hour
            persist_trace_after_reboot: true
            write_into_file: true
            
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
            }
            """.formatted(PERSISTENT_TRACE_SESSION_NAME).trim();
    private final static String ON_DEVICE_PERFETTO_CONFIG_PATH =
            "/data/misc/perfetto-configs/reboot_host_test_persistent_session.pb.txt";
    private final static String ON_DEVICE_PERSISTENT_TRACE_OUTPUT_PATH =
            "/data/misc/perfetto-traces/persistent/" + PERSISTENT_TRACE_SESSION_NAME
                    + ".pftrace";
    private final static String ON_DEVICE_REPORTED_TRACES_DIR = "/sdcard/Android/data/android.perfetto.cts.reporter/files";
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
        Assert.assertTrue(mTestDevice.pushFile(mTraceConfigFile, ON_DEVICE_PERFETTO_CONFIG_PATH));
        mTestDevice.deleteFile(ON_DEVICE_PERSISTENT_TRACE_OUTPUT_PATH);
    }

    @After
    public void TearDown() throws DeviceNotAvailableException {
        mTestDevice.deleteFile(ON_DEVICE_PERFETTO_CONFIG_PATH);
        mTestDevice.deleteFile(ON_DEVICE_PERSISTENT_TRACE_OUTPUT_PATH);
        mTestDevice = null;
    }

    @Test
    public void testReboot() throws DeviceNotAvailableException, InterruptedException {
        CommandResult perfettoResult = mTestDevice.executeShellV2Command(
                "perfetto --upload --background --txt --config "
                        + ON_DEVICE_PERFETTO_CONFIG_PATH);
        Assert.assertEquals("Failed to start perfetto: " + perfettoResult.toString(),
                CommandStatus.SUCCESS,
                perfettoResult.getStatus());

        // Assert tracing session started
        List<String> runningTracingSessionNames = assertGetRunningTracingSessionNames();
        assertThat(runningTracingSessionNames).contains(PERSISTENT_TRACE_SESSION_NAME);

        String uniqueSessionName = "";
        UUID uuid = null;
        boolean traceFileExists = false;
        for (int i = 0; i < 10; i++) {
            if (mTestDevice.doesFileExist(ON_DEVICE_PERSISTENT_TRACE_OUTPUT_PATH)) {
                File traceFile = mTestDevice.pullFile(ON_DEVICE_PERSISTENT_TRACE_OUTPUT_PATH);
                if (traceFile != null) {
                    try {
                        Trace trace = Trace.parseFrom(new FileInputStream(traceFile));
                        for (TracePacket packet : trace.getPacketList()) {
                            if (packet.hasTraceConfig()) {
                                uniqueSessionName = packet.getTraceConfig().getUniqueSessionName();
                                if (packet.hasTraceUuid()) {
                                    uuid = new UUID(packet.getTraceUuid().getMsb(),
                                            packet.getTraceUuid().getLsb());
                                }
                                break;
                            }
                        }
                    } catch (IOException e) {
                        CLog.e("Failed to parse trace file: " + e.getMessage());
                    }
                    traceFileExists = true;
                    break;
                } else {
                    CLog.e("Failed to pull trace file.");
                }
            }
            Thread.sleep(1_000);
        }

        Assert.assertTrue("Timed out waiting for a trace file", traceFileExists);
        CLog.i("uniqueSessionName: " + uniqueSessionName + ", uuid: " + uuid);

        mTestDevice.reboot();
        CLog.i("rebooted!");

        Thread.sleep(3000);

        Assert.assertFalse("Trace file must be unlinked at that point",
                mTestDevice.doesFileExist(ON_DEVICE_PERSISTENT_TRACE_OUTPUT_PATH));

        if (uuid != null) {
            String uuidString = uuid.toString();
            CLog.i("uuid: " + uuidString);
            CLog.i("fixed uuid: " + uuidString.replace("-", ""));
        }

        //String reportedFilePath = ON_DEVICE_REPORTED_TRACES_DIR + "/" + uuid.toString();

//        boolean reportedFileExists = false;
//        for (int i = 0; i < 10; i++) {
//
//            Thread.sleep(1_000);
//        }

        CommandResult lsReportedResult = mTestDevice.executeShellV2Command(
                "ls -R /sdcard/Android/data/android.perfetto.cts.reporter/files");
        CLog.i("ls reported result, stdout: '" + lsReportedResult.getStdout() + "', stderr: '"
                + lsReportedResult.getStderr() + "'");

        CLog.i("now sleeping for 3 seconds....");
        Thread.sleep(3_000);

        lsReportedResult = mTestDevice.executeShellV2Command(
                "ls -R /sdcard/Android/data/android.perfetto.cts.reporter/files");
        CLog.i("ls reported result, stdout: '" + lsReportedResult.getStdout() + "', stderr: '"
                + lsReportedResult.getStderr() + "'");
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
