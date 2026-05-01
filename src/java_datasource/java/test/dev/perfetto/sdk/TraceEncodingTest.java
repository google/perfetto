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

import static org.junit.Assert.*;

import java.io.*;
import java.nio.file.*;
import java.util.Arrays;
import org.junit.Test;

/**
 * Encodes a complete trace with ProtoWriter, writes to disk, and verifies
 * with trace_processor_shell.
 */
public class TraceEncodingTest {
    static final int TP_TIMESTAMP = 8;
    static final int TP_CLOCK_ID = 58;
    static final int TP_TRACK_EVENT = 11;
    static final int TP_SEQ_ID = 10;
    static final int TP_SEQ_FLAGS = 13;
    static final int TP_INTERNED_DATA = 12;
    static final int TP_TRACK_DESCRIPTOR = 60;
    static final int TD_UUID = 1;
    static final int TD_NAME = 2;
    static final int TD_PROCESS = 3;
    static final int PD_PID = 1;
    static final int ID_EVENT_NAMES = 2;
    static final int EN_IID = 1;
    static final int EN_NAME = 2;
    static final int TE_NAME = 23;
    static final int TE_TYPE = 9;
    static final int TE_TRACK_UUID = 11;
    static final int TE_NAME_IID = 10;
    static final int TE_DEBUG_ANNOTATIONS = 4;
    static final int DA_NAME = 10;
    static final int DA_INT = 4;
    static final int DA_DOUBLE = 5;
    static final int DA_STRING = 6;
    static final int DA_BOOL = 2;

    @Test
    public void traceWithSlicesAndArgs() throws Exception {
        ProtoWriter trace = new ProtoWriter(16384);
        ProtoWriter pkt = new ProtoWriter(1024);

        // Track descriptor.
        pkt.writeVarInt(TP_SEQ_ID, 1);
        int td = pkt.beginNested(TP_TRACK_DESCRIPTOR);
        pkt.writeVarInt(TD_UUID, 42);
        pkt.writeString(TD_NAME, "TestTrack");
        int pd = pkt.beginNested(TD_PROCESS);
        pkt.writeVarInt(PD_PID, 1234);
        pkt.endNested(pd);
        pkt.endNested(td);
        wrapPacket(trace, pkt);

        // Slice begin with interned name + incremental state.
        pkt.reset();
        pkt.writeVarInt(TP_TIMESTAMP, 1000);
        pkt.writeVarInt(TP_CLOCK_ID, 6);
        pkt.writeVarInt(TP_SEQ_ID, 1);
        pkt.writeVarInt(TP_SEQ_FLAGS, 3);
        int id = pkt.beginNested(TP_INTERNED_DATA);
        int en = pkt.beginNested(ID_EVENT_NAMES);
        pkt.writeVarInt(EN_IID, 1);
        pkt.writeString(EN_NAME, "doFrame");
        pkt.endNested(en);
        pkt.endNested(id);
        int te = pkt.beginNested(TP_TRACK_EVENT);
        pkt.writeVarInt(TE_TYPE, 1);
        pkt.writeVarInt(TE_TRACK_UUID, 42);
        pkt.writeVarInt(TE_NAME_IID, 1);
        pkt.endNested(te);
        wrapPacket(trace, pkt);

        // Slice end.
        pkt.reset();
        pkt.writeVarInt(TP_TIMESTAMP, 3000);
        pkt.writeVarInt(TP_CLOCK_ID, 6);
        pkt.writeVarInt(TP_SEQ_ID, 1);
        pkt.writeVarInt(TP_SEQ_FLAGS, 2);
        te = pkt.beginNested(TP_TRACK_EVENT);
        pkt.writeVarInt(TE_TYPE, 2);
        pkt.writeVarInt(TE_TRACK_UUID, 42);
        pkt.endNested(te);
        wrapPacket(trace, pkt);

        // Instant with debug annotations (int, string, double, bool).
        pkt.reset();
        pkt.writeVarInt(TP_TIMESTAMP, 4000);
        pkt.writeVarInt(TP_CLOCK_ID, 6);
        pkt.writeVarInt(TP_SEQ_ID, 1);
        pkt.writeVarInt(TP_SEQ_FLAGS, 2);
        te = pkt.beginNested(TP_TRACK_EVENT);
        pkt.writeVarInt(TE_TYPE, 3);
        pkt.writeVarInt(TE_TRACK_UUID, 42);
        pkt.writeString(TE_NAME, "frameStats");
        writeDebugArg(pkt, "frame_id", 42);
        writeDebugArgStr(pkt, "layer", "com.example.app");
        writeDebugArgDouble(pkt, "jank_pct", 12.345);
        writeDebugArgBool(pkt, "is_janky", true);
        pkt.endNested(te);
        wrapPacket(trace, pkt);

        byte[] traceBytes = Arrays.copyOf(trace.buffer(), trace.position());
        Path traceFile = Files.createTempFile("perfetto_e2e_", ".pb");
        Files.write(traceFile, traceBytes);

        String tpShell = findTraceProcessorShell();
        if (tpShell == null) {
            assertTrue(traceBytes.length > 100);
            return;
        }

        String slices = runQuery(tpShell, traceFile,
                "SELECT name, dur FROM slice WHERE dur > 0 ORDER BY ts;");
        assertTrue(slices.contains("doFrame"));
        assertTrue(slices.contains("2000"));

        String args = runQuery(tpShell, traceFile,
                "SELECT key, int_value, string_value, real_value FROM args "
                + "WHERE arg_set_id IN "
                + "(SELECT arg_set_id FROM slice WHERE name = 'frameStats') "
                + "ORDER BY key;");
        assertTrue(args.contains("frame_id") && args.contains("42"));
        assertTrue(args.contains("jank_pct") && args.contains("12.345"));
        assertTrue(args.contains("layer") && args.contains("com.example"));

        Files.deleteIfExists(traceFile);
    }

    private void wrapPacket(ProtoWriter trace, ProtoWriter pkt) {
        trace.writeBytes(1, pkt.buffer(), 0, pkt.position());
    }

    private void writeDebugArg(ProtoWriter w, String name, long value) {
        int da = w.beginNested(TE_DEBUG_ANNOTATIONS);
        w.writeString(DA_NAME, name);
        w.writeVarInt(DA_INT, value);
        w.endNested(da);
    }

    private void writeDebugArgStr(ProtoWriter w, String name, String value) {
        int da = w.beginNested(TE_DEBUG_ANNOTATIONS);
        w.writeString(DA_NAME, name);
        w.writeString(DA_STRING, value);
        w.endNested(da);
    }

    private void writeDebugArgDouble(ProtoWriter w, String name, double value) {
        int da = w.beginNested(TE_DEBUG_ANNOTATIONS);
        w.writeString(DA_NAME, name);
        w.writeDouble(DA_DOUBLE, value);
        w.endNested(da);
    }

    private void writeDebugArgBool(ProtoWriter w, String name, boolean value) {
        int da = w.beginNested(TE_DEBUG_ANNOTATIONS);
        w.writeString(DA_NAME, name);
        w.writeBool(DA_BOOL, value);
        w.endNested(da);
    }

    private String runQuery(String tp, Path trace, String sql) throws Exception {
        ProcessBuilder pb = new ProcessBuilder(tp, "--query-file", "/dev/stdin",
                trace.toString());
        pb.redirectErrorStream(true);
        Process proc = pb.start();
        proc.getOutputStream().write(sql.getBytes());
        proc.getOutputStream().close();
        String out = new String(proc.getInputStream().readAllBytes());
        assertEquals("query failed: " + out, 0, proc.waitFor());
        return out;
    }

    private static String findTraceProcessorShell() {
        File outDir = new File("out");
        if (!outDir.isDirectory()) return null;
        File[] dirs = outDir.listFiles();
        if (dirs == null) return null;
        for (File d : dirs) {
            File tp = new File(d, "trace_processor_shell");
            if (tp.canExecute()) return tp.getPath();
        }
        return null;
    }
}
