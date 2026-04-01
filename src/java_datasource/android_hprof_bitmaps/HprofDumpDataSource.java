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

// AOSP path: frameworks/base/services/core/java/com/android/server/am/
//
// Uses dev.perfetto.sdk.PerfettoDataSource (the high-performance SDK).
// NOT the crappy android.tracing.perfetto.DataSource that allocates per trace.

package com.android.server.am;

import android.os.ParcelFileDescriptor;
import android.os.RemoteCallback;
import android.os.SystemClock;
import android.os.UserHandle;
import android.util.Log;

import dev.perfetto.sdk.PerfettoDataSource;
import dev.perfetto.sdk.ProtoWriter;
import dev.perfetto.sdk.TraceContext;

import java.io.File;
import java.io.IOException;
import java.lang.ref.WeakReference;
import java.nio.file.Files;

/**
 * Perfetto data source that triggers a Java heap dump on a target process
 * and embeds the raw .hprof bytes in the trace.
 *
 * Uses the high-performance PerfettoDataSource SDK: zero allocations on
 * the write path. Hprof bytes go directly to shared memory via ProtoWriter.
 */
public final class HprofDumpDataSource extends PerfettoDataSource {
    private static final String TAG = "HprofDumpDataSource";
    private static final String DUMP_DIR = "/data/local/tmp/perfetto_hprof";
    private static final int CHUNK_SIZE = 4 * 1024 * 1024;

    // TracePacket field numbers.
    private static final int TP_TIMESTAMP = 8;
    private static final int TP_CLOCK_ID = 58;
    private static final int TP_SEQ_ID = 10;

    // HprofDump on TracePacket (field 129).
    private static final int TP_HPROF_DUMP = 129;
    private static final int HD_PID = 1;
    private static final int HD_HPROF_DATA = 2;
    private static final int HD_CHUNK_INDEX = 3;
    private static final int HD_LAST_CHUNK = 4;

    // HprofDumpConfig field numbers (matches hprof_dump_config.proto).
    private static final int CFG_PID = 1;
    private static final int CFG_PROCESS_CMDLINE = 2;
    private static final int CFG_RUN_GC = 3;
    private static final int CFG_DUMP_BITMAPS = 4;
    private static final int CFG_BITMAP_FORMAT = 5;

    static final HprofDumpDataSource INSTANCE = new HprofDumpDataSource();

    private WeakReference<ActivityManagerService> mAms;
    private volatile long mConfigPid;
    private volatile String mConfigProcess;
    private volatile boolean mConfigRunGc = true;
    private volatile boolean mConfigDumpBitmaps;
    private volatile String mConfigBitmapFormat = "png";

    public void init(ActivityManagerService ams) {
        mAms = new WeakReference<>(ams);
        register("android.hprof_dump");
        Log.i(TAG, "Registered with Perfetto");
    }

    @Override
    protected void onSetup(int instanceIndex, byte[] config) {
        parseConfig(config);
    }

    @Override
    protected void onStart(int instanceIndex) {
        String process = mConfigProcess;
        if (process == null || process.isEmpty()) {
            if (mConfigPid > 0) {
                process = String.valueOf(mConfigPid);
            } else {
                Log.e(TAG, "No target process specified in config");
                return;
            }
        }
        triggerDump(process, mConfigRunGc, mConfigDumpBitmaps,
                mConfigBitmapFormat);
    }

    private void parseConfig(byte[] data) {
        if (data == null || data.length == 0) return;
        java.nio.ByteBuffer buf = java.nio.ByteBuffer.wrap(data);
        while (buf.hasRemaining()) {
            int tag = readVarInt(buf);
            if (tag == 0) break;
            int fieldId = tag >>> 3;
            int wireType = tag & 0x7;
            switch (fieldId) {
                case CFG_PID: mConfigPid = readVarInt(buf); break;
                case CFG_PROCESS_CMDLINE: mConfigProcess = readLenDelim(buf); break;
                case CFG_RUN_GC: mConfigRunGc = readVarInt(buf) != 0; break;
                case CFG_DUMP_BITMAPS: mConfigDumpBitmaps = readVarInt(buf) != 0; break;
                case CFG_BITMAP_FORMAT: mConfigBitmapFormat = readLenDelim(buf); break;
                default: skipField(buf, wireType); break;
            }
        }
    }

    private static int readVarInt(java.nio.ByteBuffer buf) {
        int r = 0, shift = 0;
        while (buf.hasRemaining()) {
            byte b = buf.get();
            r |= (b & 0x7F) << shift;
            if ((b & 0x80) == 0) return r;
            shift += 7;
        }
        return r;
    }

    private static String readLenDelim(java.nio.ByteBuffer buf) {
        int len = readVarInt(buf);
        if (len <= 0 || len > buf.remaining()) return null;
        byte[] b = new byte[len];
        buf.get(b);
        return new String(b);
    }

    private static void skipField(java.nio.ByteBuffer buf, int wt) {
        switch (wt) {
            case 0: readVarInt(buf); break;
            case 1: buf.position(Math.min(buf.position() + 8, buf.limit())); break;
            case 2: int l = readVarInt(buf); buf.position(Math.min(buf.position() + l, buf.limit())); break;
            case 5: buf.position(Math.min(buf.position() + 4, buf.limit())); break;
        }
    }

    private void triggerDump(String process, boolean runGc,
            boolean dumpBitmaps, String bitmapFormat) {
        ActivityManagerService ams = mAms != null ? mAms.get() : null;
        if (ams == null) {
            Log.e(TAG, "AMS not available");
            return;
        }

        String ts = String.valueOf(SystemClock.elapsedRealtimeNanos());
        File dumpDir = new File(DUMP_DIR, ts);
        dumpDir.mkdirs();
        String hprofPath = new File(dumpDir, "dump.hprof").getAbsolutePath();

        try {
            ParcelFileDescriptor fd = ParcelFileDescriptor.open(
                    new File(hprofPath),
                    ParcelFileDescriptor.MODE_CREATE
                            | ParcelFileDescriptor.MODE_WRITE_ONLY
                            | ParcelFileDescriptor.MODE_TRUNCATE);

            RemoteCallback callback = new RemoteCallback(
                    result -> onDumpComplete(dumpDir, hprofPath));

            ams.dumpHeap(process, UserHandle.USER_CURRENT,
                    true, false, runGc,
                    dumpBitmaps ? bitmapFormat : null,
                    hprofPath, fd, callback);

            Log.i(TAG, "Triggered dump for " + process);
        } catch (Exception e) {
            Log.e(TAG, "Failed to trigger dump", e);
        }
    }

    private void onDumpComplete(File dumpDir, String hprofPath) {
        try {
            File hprofFile = new File(hprofPath);
            if (hprofFile.exists() && hprofFile.length() > 0) {
                writeHprofPackets(hprofFile);
            }
        } catch (Exception e) {
            Log.e(TAG, "Failed to write dump to trace", e);
        } finally {
            deleteRecursive(dumpDir);
        }
    }

    private void writeHprofPackets(File file) throws IOException {
        byte[] data = Files.readAllBytes(file.toPath());
        int totalChunks = (data.length + CHUNK_SIZE - 1) / CHUNK_SIZE;

        for (int i = 0; i < totalChunks; i++) {
            int offset = i * CHUNK_SIZE;
            int len = Math.min(CHUNK_SIZE, data.length - offset);

            TraceContext ctx = trace();
            if (ctx == null) return;

            ProtoWriter w = ctx.getWriter();
            w.writeVarInt(TP_TIMESTAMP, SystemClock.elapsedRealtimeNanos());
            w.writeVarInt(TP_CLOCK_ID, 6);
            w.writeVarInt(TP_SEQ_ID, 1);
            int dump = w.beginNested(TP_HPROF_DUMP);
            w.writeVarInt(HD_PID, android.os.Process.myPid());
            w.writeBytes(HD_HPROF_DATA, data, offset, len);
            w.writeVarInt(HD_CHUNK_INDEX, i);
            if (i == totalChunks - 1) {
                w.writeVarInt(HD_LAST_CHUNK, 1);
            }
            w.endNested(dump);
            ctx.commitPacket();
        }

        Log.i(TAG, "Wrote " + data.length + " bytes hprof ("
                + ((data.length + CHUNK_SIZE - 1) / CHUNK_SIZE) + " chunks)");
    }

    private static void deleteRecursive(File dir) {
        File[] files = dir.listFiles();
        if (files != null) {
            for (File f : files) {
                if (f.isDirectory()) deleteRecursive(f);
                f.delete();
            }
        }
        dir.delete();
    }
}
