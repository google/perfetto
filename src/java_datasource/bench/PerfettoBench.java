package dev.perfetto.sdk;

import java.util.Arrays;

/**
 * End-to-end benchmark: Java ProtoWriter encoding + real JNI + real shmem.
 *
 * Loads a Linux-native JNI library that wraps the Perfetto C SDK,
 * registers a custom data source, starts an in-process tracing session,
 * and benchmarks the full path: encode in Java -> JNI -> AppendBytes -> shmem.
 */
public class PerfettoBench {
    // JNI methods - real Perfetto C SDK underneath
    static native void nativeInit();                       // PerfettoProducerInit
    static native long nativeRegisterDs(String name);      // PerfettoDsImplCreate + Register
    static native long nativeStartSession(String dsName);  // Start in-process session
    static native void nativeStopSession(long session);    // Stop + read
    static native void nativeWritePacket(long dsImpl, byte[] buf, int len); // iterate + AppendBytes

    static {
        System.loadLibrary("perfetto_bench_jni");
    }

    // TracePacket field numbers
    static final int TP_TIMESTAMP = 8;
    static final int TP_SEQ_ID = 10;
    static final int TP_TRACK_EVENT = 11;
    static final int TP_SEQ_FLAGS = 13;
    static final int TE_TYPE = 9;
    static final int TE_NAME_IID = 10;
    static final int TE_TRACK_UUID = 11;
    static final int TE_DEBUG_ANNOTATIONS = 4;
    static final int DA_NAME_IID = 1;
    static final int DA_UINT = 7;

    static final int WARMUP = 100_000;
    static final int ITERATIONS = 2_000_000;
    static final int REPS = 5;

    public static void main(String[] args) {
        nativeInit();
        String dsName = "dev.perfetto.java_bench";
        long dsImpl = nativeRegisterDs(dsName);
        if (dsImpl == 0) {
            System.err.println("Failed to register data source");
            return;
        }

        ProtoWriter w = new ProtoWriter(4096);

        System.out.println("Java DataSource E2E Benchmark (real JNI + real shmem)");
        System.out.println("=====================================================");
        System.out.println();

        // ---------- Benchmark 1: Encoding only (no session, disabled fast path) ----------
        benchmarkRun("ProtoWriter encode only (no shmem)", REPS, () -> {
            for (int i = 0; i < WARMUP; i++) {
                w.reset();
                encodeSliceBegin(w);
            }
            long start = System.nanoTime();
            for (int i = 0; i < ITERATIONS; i++) {
                w.reset();
                encodeSliceBegin(w);
            }
            return System.nanoTime() - start;
        });

        // ---------- Benchmark 2: Full E2E - encode + JNI + shmem ----------
        {
            long session = nativeStartSession(dsName);
            // Warmup with real shmem writes
            for (int i = 0; i < WARMUP; i++) {
                w.reset();
                encodeSliceBegin(w);
                nativeWritePacket(dsImpl, w.buffer(), w.position());
            }

            benchmarkRun("Full E2E: encode + JNI + shmem (basic)", REPS, () -> {
                long start = System.nanoTime();
                for (int i = 0; i < ITERATIONS; i++) {
                    w.reset();
                    encodeSliceBegin(w);
                    nativeWritePacket(dsImpl, w.buffer(), w.position());
                }
                return System.nanoTime() - start;
            });

            nativeStopSession(session);
        }

        // ---------- Benchmark 3: Full E2E with debug arg ----------
        {
            long session = nativeStartSession(dsName);
            for (int i = 0; i < WARMUP; i++) {
                w.reset();
                encodeSliceBeginWithArg(w);
                nativeWritePacket(dsImpl, w.buffer(), w.position());
            }

            benchmarkRun("Full E2E: encode + JNI + shmem (+ debug arg)", REPS, () -> {
                long start = System.nanoTime();
                for (int i = 0; i < ITERATIONS; i++) {
                    w.reset();
                    encodeSliceBeginWithArg(w);
                    nativeWritePacket(dsImpl, w.buffer(), w.position());
                }
                return System.nanoTime() - start;
            });

            nativeStopSession(session);
        }

        // ---------- Benchmark 4: JNI + shmem only (pre-encoded packet) ----------
        {
            long session = nativeStartSession(dsName);
            w.reset();
            encodeSliceBegin(w);
            byte[] preEncoded = Arrays.copyOf(w.buffer(), w.position());

            for (int i = 0; i < WARMUP; i++) {
                nativeWritePacket(dsImpl, preEncoded, preEncoded.length);
            }

            benchmarkRun("JNI + shmem only (pre-encoded, no encode cost)", REPS, () -> {
                long start = System.nanoTime();
                for (int i = 0; i < ITERATIONS; i++) {
                    nativeWritePacket(dsImpl, preEncoded, preEncoded.length);
                }
                return System.nanoTime() - start;
            });

            nativeStopSession(session);
        }

        // ---------- Benchmark 5: Builder API encode only ----------
        {
            PacketBuilder pb = new PacketBuilder(null);
            benchmarkRun("Builder encode: 3 fields + nested", REPS, () -> {
                for (int i = 0; i < WARMUP; i++) {
                    w.reset();
                    encodeWithBuilder(pb, w);
                }
                long start = System.nanoTime();
                for (int i = 0; i < ITERATIONS; i++) {
                    w.reset();
                    encodeWithBuilder(pb, w);
                }
                return System.nanoTime() - start;
            });
        }

        // Packet sizes
        System.out.println("\nPacket sizes:");
        w.reset(); encodeSliceBegin(w);
        System.out.printf("  SliceBegin (raw):   %d bytes%n", w.position());
        w.reset(); encodeSliceBeginWithArg(w);
        System.out.printf("  SliceBegin+Arg:     %d bytes%n", w.position());
    }

    static void encodeSliceBegin(ProtoWriter w) {
        w.writeVarInt(TP_TIMESTAMP, 123456789000L);
        w.writeVarInt(TP_SEQ_ID, 1);
        w.writeVarInt(TP_SEQ_FLAGS, 2);
        int te = w.beginNested(TP_TRACK_EVENT);
        w.writeVarInt(TE_TYPE, 1);
        w.writeVarInt(TE_TRACK_UUID, 12345);
        w.writeVarInt(TE_NAME_IID, 1);
        w.endNested(te);
    }

    // Same packet via builder API (should produce identical bytes)
    static void encodeWithBuilder(PacketBuilder pb, ProtoWriter w) {
        pb.start(w)
                .writeVarInt(TP_TIMESTAMP, 123456789000L)
                .writeVarInt(TP_SEQ_ID, 1)
                .writeVarInt(TP_SEQ_FLAGS, 2)
                .beginNested(TP_TRACK_EVENT)
                    .writeVarInt(TE_TYPE, 1)
                    .writeVarInt(TE_TRACK_UUID, 12345)
                    .writeVarInt(TE_NAME_IID, 1)
                .endNested()
                .commit();
    }

    static void encodeSliceBeginWithArg(ProtoWriter w) {
        w.writeVarInt(TP_TIMESTAMP, 123456789000L);
        w.writeVarInt(TP_SEQ_ID, 1);
        w.writeVarInt(TP_SEQ_FLAGS, 2);
        int te = w.beginNested(TP_TRACK_EVENT);
        w.writeVarInt(TE_TYPE, 1);
        w.writeVarInt(TE_TRACK_UUID, 12345);
        w.writeVarInt(TE_NAME_IID, 1);
        int da = w.beginNested(TE_DEBUG_ANNOTATIONS);
        w.writeVarInt(DA_NAME_IID, 1);
        w.writeVarInt(DA_UINT, 42);
        w.endNested(da);
        w.endNested(te);
    }

    interface BenchFn { long run(); }

    static void benchmarkRun(String name, int reps, BenchFn fn) {
        long[] times = new long[reps];
        for (int r = 0; r < reps; r++) {
            times[r] = fn.run();
        }
        java.util.Arrays.sort(times);
        long median = times[reps / 2];
        double nsPerOp = (double) median / ITERATIONS;
        System.out.printf("  %-55s %7.1f ns/op%n", name, nsPerOp);
    }
}
