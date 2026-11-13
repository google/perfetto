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

package dev.perfetto.sdk.bench;

import dev.perfetto.sdk.PerfettoTrace;
import dev.perfetto.sdk.PerfettoTrace.Category;
import dev.perfetto.sdk.PerfettoTrack;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.lang.management.ManagementFactory;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import perfetto.protos.TraceOuterClass.Trace;
import perfetto.protos.TracePacketOuterClass.TracePacket;

/**
 * Host JVM microbenchmarks for the Java SDK event path.
 *
 * <p>Run it with {@code tools/run_android_sdk_host_test --benchmark}. Each scenario prints one
 * tab separated line:
 *
 * <pre>
 *   name  cpu_ns  wall_ns  alloc_bytes  trace_bytes
 * </pre>
 *
 * <ul>
 *   <li>cpu_ns: the median over the repetitions of the thread CPU time per event.
 *   <li>wall_ns: the same for the wall time. For multi-thread scenarios, this is per event per
 *       thread.
 *   <li>alloc_bytes: the Java heap bytes the thread allocated per event, over all repetitions.
 *   <li>trace_bytes: the trace bytes per event, from a separate session that only holds this
 *       scenario. It includes the interned data and the track descriptors.
 * </ul>
 *
 * <p>Options: {@code --filter=<substring>}, {@code --scenario=<name>}, {@code --reps=<n>}
 * (default 5), {@code --rep-ms=<ms>} (default 200), {@code --list},
 * {@code --encoding=default|legacy|encoded|mixed}, {@code --writer=v1|v2}, and
 * {@code --verified-iters=<n>}.
 *
 * <p>The verified mode warms the JIT in a separate recording, then starts fresh recordings for
 * each repetition and times exactly n iterations. It decodes each trace after timing and requires
 * the exact event count in every session. A full or dropping buffer fails the run. Session setup,
 * stop and decoding are outside timing; the first event's sequence setup is included. In this
 * mode trace_bytes includes metadata and is summed across both sessions when encoding=mixed.
 * The duration-based mode is retained for exploratory use and does not check delivery.
 */
public final class PerfettoTraceBenchmark {
  private static final Category ON = new Category("bench_on");
  private static final Category OFF = new Category("bench_off");

  private static final PerfettoTrack NESTED =
      PerfettoTrack.process("bench_parent").child("bench_child");

  private static final String[] NAMES64 = new String[64];
  private static final String LONG_NAME = "event_" + "x".repeat(256);
  private static final String LONG_ARG_A = "a_" + "x".repeat(128);
  private static final String LONG_ARG_B = "b_" + "x".repeat(128);
  private static final String LONG_ARG_C = "c_" + "x".repeat(128);
  private static final String LONG_ARG_D = "d_" + "x".repeat(128);
  private static final StackTraceElement[] STACK30 = new StackTraceElement[30];

  static {
    for (int i = 0; i < NAMES64.length; i++) {
      NAMES64[i] = "rotating_name_" + i;
    }
    for (int i = 0; i < STACK30.length; i++) {
      STACK30[i] =
          new StackTraceElement("com.example.Class" + i, "method" + i, "File" + i + ".java", i);
    }
  }

  private static final com.sun.management.ThreadMXBean THREAD_BEAN =
      (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();

  private abstract static class Scenario {
    final String mName;
    final int mEventsPerIter;

    Scenario(String name, int eventsPerIter) {
      mName = name;
      mEventsPerIter = eventsPerIter;
    }

    abstract void run(int iters);
  }

  private static final Scenario[] SCENARIOS = {
    new Scenario("disabled_instant", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.instant(OFF, "disabled").emit();
        }
      }
    },
    new Scenario("disabled_args", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.instant(OFF, "disabled").addArg("i", i).addArg("s", "v").emit();
        }
      }
    },
    new Scenario("begin_end", 2) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.begin(ON, "slice").emit();
          PerfettoTrace.end(ON).emit();
        }
      }
    },
    new Scenario("instant", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.instant(ON, "instant").emit();
        }
      }
    },
    new Scenario("instant_names64", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.instant(ON, NAMES64[i & 63]).emit();
        }
      }
    },
    new Scenario("counter_long", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.counter(ON, (long) i, "bench_counter").emit();
        }
      }
    },
    new Scenario("counter_double", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.counter(ON, i * 0.5, "bench_dcounter").emit();
        }
      }
    },
    new Scenario("args_int_x1", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.instant(ON, "args").addArg("a", i).emit();
        }
      }
    },
    new Scenario("args_int_x4", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.instant(ON, "args")
              .addArg("a", i)
              .addArg("b", i)
              .addArg("c", i)
              .addArg("d", i)
              .emit();
        }
      }
    },
    new Scenario("args_string_x1", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.instant(ON, "args").addArg("s", "value").emit();
        }
      }
    },
    new Scenario("args_mix", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.instant(ON, "args")
              .addArg("long", i)
              .addArg("bool", true)
              .addArg("double", 1.5)
              .addArg("string", "value")
              .emit();
        }
      }
    },
    // Repeated long strings expose the extra trace bytes from inline names.
    new Scenario("args_long_names", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.instant(ON, LONG_NAME)
              .addArg(LONG_ARG_A, i)
              .addArg(LONG_ARG_B, true)
              .addArg(LONG_ARG_C, 1.5)
              .addArg(LONG_ARG_D, "value")
              .emit();
        }
      }
    },
    new Scenario("flows", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.instant(ON, "flow").addFlow(i).addTerminatingFlow(i).emit();
        }
      }
    },
    new Scenario("named_track", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.instant(ON, "named").usingProcessNamedTrack(7, "bench_named").emit();
        }
      }
    },
    new Scenario("nested_track", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.instant(ON, "nested").usingTrack(NESTED).emit();
        }
      }
    },
    new Scenario("proto_field", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.instant(ON, "proto")
              .beginProto()
              .addField(2001, i)
              .addField(2002, "str")
              .endProto()
              .emit();
        }
      }
    },
    new Scenario("proto_interned", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.instant(ON, "interned")
              .beginProto()
              .beginNested(2006)
              .addFieldWithInterning(21, NAMES64[i & 63], 44)
              .endNested()
              .endProto()
              .emit();
        }
      }
    },
    new Scenario("callstack_30", 1) {
      @Override
      void run(int iters) {
        for (int i = 0; i < iters; i++) {
          PerfettoTrace.expensiveDebugCallStack(ON, "stack", STACK30).emit();
        }
      }
    },
    new MultiThreadScenario("mt2_begin_end", 2),
    new MultiThreadScenario("mt4_begin_end", 4),
  };

  // Sum CPU time and allocations across emitting threads.
  private static final class MultiThreadScenario extends Scenario {
    private final int mThreads;
    long mThreadCpuNs;
    long mThreadAllocBytes;

    MultiThreadScenario(String name, int threads) {
      super(name, 2 * threads);
      mThreads = threads;
    }

    @Override
    void run(int iters) {
      Thread[] threads = new Thread[mThreads];
      long[] cpu = new long[mThreads];
      long[] alloc = new long[mThreads];
      for (int t = 0; t < mThreads; t++) {
        final int idx = t;
        threads[t] =
            new Thread(
                () -> {
                  long c0 = THREAD_BEAN.getCurrentThreadCpuTime();
                  long a0 = THREAD_BEAN.getCurrentThreadAllocatedBytes();
                  for (int i = 0; i < iters; i++) {
                    PerfettoTrace.begin(ON, "mt_slice").emit();
                    PerfettoTrace.end(ON).emit();
                  }
                  cpu[idx] = THREAD_BEAN.getCurrentThreadCpuTime() - c0;
                  alloc[idx] = THREAD_BEAN.getCurrentThreadAllocatedBytes() - a0;
                });
      }
      for (Thread t : threads) {
        t.start();
      }
      for (Thread t : threads) {
        try {
          t.join();
        } catch (InterruptedException e) {
          throw new RuntimeException(e);
        }
      }
      mThreadCpuNs = 0;
      mThreadAllocBytes = 0;
      for (int t = 0; t < mThreads; t++) {
        mThreadCpuNs += cpu[t];
        mThreadAllocBytes += alloc[t];
      }
    }
  }

  private static void varint(ByteArrayOutputStream out, long v) {
    while ((v & ~0x7FL) != 0) {
      out.write((int) ((v & 0x7F) | 0x80));
      v >>>= 7;
    }
    out.write((int) v);
  }

  private static void tag(ByteArrayOutputStream out, int field, int wireType) {
    varint(out, ((long) field << 3) | wireType);
  }

  private static void lenField(ByteArrayOutputStream out, int field, byte[] bytes) {
    tag(out, field, 2);
    varint(out, bytes.length);
    out.write(bytes, 0, bytes.length);
  }

  /** Configures one buffer and the "bench_on" category. */
  private static byte[] traceConfig(
      int sizeKb, boolean ringBuffer, int encodingMode, boolean writerV2) {
    ByteArrayOutputStream buffer = new ByteArrayOutputStream();
    tag(buffer, 1, 0); // size_kb
    varint(buffer, sizeKb);
    tag(buffer, 4, 0); // fill_policy: RING_BUFFER = 1, DISCARD = 2
    varint(buffer, ringBuffer ? 1 : 2);
    if (writerV2) {
      tag(buffer, 8, 0); // experimental_mode: TRACE_BUFFER_V2 = 1.
      varint(buffer, 1);
    }

    ByteArrayOutputStream te = new ByteArrayOutputStream();
    lenField(te, 1, "*".getBytes(StandardCharsets.UTF_8)); // disabled_categories
    lenField(te, 2, "bench_on".getBytes(StandardCharsets.UTF_8)); // enabled_categories
    if (encodingMode != 0) {
      tag(te, 11, 0); // experimental_java_event_encoding: LEGACY = 1, ENCODED = 2.
      varint(te, encodingMode);
    }

    ByteArrayOutputStream dsc = new ByteArrayOutputStream();
    lenField(dsc, 1, "track_event".getBytes(StandardCharsets.UTF_8)); // name
    lenField(dsc, 113, te.toByteArray()); // track_event_config
    ByteArrayOutputStream writer = new ByteArrayOutputStream();
    tag(writer, 1, 0); // use_v2_probability_percent
    varint(writer, writerV2 ? 100 : 0);
    lenField(dsc, 148, writer.toByteArray()); // experimental_tracing_v2

    ByteArrayOutputStream ds = new ByteArrayOutputStream();
    lenField(ds, 1, dsc.toByteArray()); // config

    ByteArrayOutputStream cfg = new ByteArrayOutputStream();
    lenField(cfg, 1, buffer.toByteArray()); // buffers
    lenField(cfg, 2, ds.toByteArray()); // data_sources
    return cfg.toByteArray();
  }

  private static long cpuNs(Scenario s) {
    if (s instanceof MultiThreadScenario) {
      return ((MultiThreadScenario) s).mThreadCpuNs;
    }
    return THREAD_BEAN.getCurrentThreadCpuTime();
  }

  // Target repMs of wall time per repetition.
  private static int calibrate(Scenario s, int repMs) {
    int iters = 1000;
    while (true) {
      long t0 = System.nanoTime();
      s.run(iters);
      long ns = System.nanoTime() - t0;
      if (ns > 20_000_000L || iters >= (1 << 26)) {
        return (int) Math.max(1, Math.min(1L << 28, (long) iters * repMs * 1_000_000L / ns));
      }
      iters *= 4;
    }
  }

  private static String measure(Scenario s, int reps, int repMs) {
    int iters = calibrate(s, repMs);
    // Warm up the JIT before measuring.
    for (int w = 0; w < 5; w++) {
      s.run(iters);
    }
    boolean mt = s instanceof MultiThreadScenario;
    double events = (double) iters * s.mEventsPerIter;
    double[] cpu = new double[reps];
    double[] wall = new double[reps];
    long alloc = 0;
    for (int r = 0; r < reps; r++) {
      long a0 = THREAD_BEAN.getCurrentThreadAllocatedBytes();
      long c0 = mt ? 0 : THREAD_BEAN.getCurrentThreadCpuTime();
      long t0 = System.nanoTime();
      s.run(iters);
      long t1 = System.nanoTime();
      long c1 = cpuNs(s);
      long a1 = THREAD_BEAN.getCurrentThreadAllocatedBytes();
      cpu[r] = (c1 - c0) / events;
      wall[r] = (t1 - t0) / (mt ? (double) iters * 2 : events);
      alloc += mt ? ((MultiThreadScenario) s).mThreadAllocBytes : a1 - a0;
    }
    Arrays.sort(cpu);
    Arrays.sort(wall);
    return String.format(
        Locale.ROOT, "%.2f\t%.2f\t%.2f", cpu[reps / 2], wall[reps / 2], alloc / (events * reps));
  }

  // Own all requested sessions, including the LEGACY+ENCODED mixed-mode pair.
  private static final class Recording implements AutoCloseable {
    private final List<PerfettoTrace.Session> mSessions = new ArrayList<>();
    final List<byte[]> mTraces = new ArrayList<>();
    private boolean mClosed;

    Recording(int sizeKb, boolean ringBuffer, String encoding, boolean writerV2) {
      int[] modes;
      switch (encoding) {
        case "mixed": modes = new int[] {1, 2}; break;
        case "legacy": modes = new int[] {1}; break;
        case "encoded": modes = new int[] {2}; break;
        case "default": modes = new int[] {0}; break;
        default: throw new IllegalArgumentException("Unknown encoding " + encoding);
      }
      for (int mode : modes) {
        mSessions.add(new PerfettoTrace.Session(
            true, traceConfig(sizeKb, ringBuffer, mode, writerV2)));
      }
    }

    @Override
    public void close() {
      if (mClosed) return;
      mClosed = true;
      for (PerfettoTrace.Session session : mSessions) {
        mTraces.add(session.close());
      }
    }

    long traceBytes() {
      long bytes = 0;
      for (byte[] trace : mTraces) bytes += trace.length;
      return bytes;
    }
  }

  private static void verifyEventCount(Recording recording, long expected) throws IOException {
    for (int i = 0; i < recording.mTraces.size(); ++i) {
      long count = 0;
      for (TracePacket packet : Trace.parseFrom(recording.mTraces.get(i)).getPacketList()) {
        if (packet.hasTrackEvent()) ++count;
      }
      if (count != expected) {
        throw new IllegalStateException(
            "Session " + i + " received " + count + " events; expected " + expected
                + ". Reduce --verified-iters if the buffer filled.");
      }
    }
  }

  private static String measureVerified(
      Scenario s, int reps, int iters, String encoding, boolean writerV2) throws IOException {
    // No warm-up events enter any measured recording. The Java/native code and
    // builder caches are warm, while per-recording sequence metadata is fresh.
    try (Recording warmup = new Recording(32 * 1024, true, encoding, writerV2)) {
      for (int w = 0; w < 5; ++w) s.run(10_000);
    }
    boolean mt = s instanceof MultiThreadScenario;
    double events = (double) iters * s.mEventsPerIter;
    long expected = s.mName.startsWith("disabled_") ? 0 : (long) iters * s.mEventsPerIter;
    double[] cpu = new double[reps];
    double[] wall = new double[reps];
    long alloc = 0;
    long traceBytes = 0;
    for (int r = 0; r < reps; ++r) {
      try (Recording recording = new Recording(8 * 1024, false, encoding, writerV2)) {
        long a0 = THREAD_BEAN.getCurrentThreadAllocatedBytes();
        long c0 = mt ? 0 : THREAD_BEAN.getCurrentThreadCpuTime();
        long t0 = System.nanoTime();
        s.run(iters);
        long t1 = System.nanoTime();
        long c1 = cpuNs(s);
        long a1 = THREAD_BEAN.getCurrentThreadAllocatedBytes();
        cpu[r] = (c1 - c0) / events;
        wall[r] = (t1 - t0) / (mt ? (double) iters * 2 : events);
        alloc += mt ? ((MultiThreadScenario) s).mThreadAllocBytes : a1 - a0;
        recording.close();
        verifyEventCount(recording, expected);
        traceBytes += recording.traceBytes();
      }
    }
    System.out.println("# samples " + s.mName + " cpu_ns=" + Arrays.toString(cpu)
        + " wall_ns=" + Arrays.toString(wall));
    Arrays.sort(cpu);
    Arrays.sort(wall);
    return String.format(Locale.ROOT, "%.2f\t%.2f\t%.2f\t%.2f",
        cpu[reps / 2], wall[reps / 2], alloc / (events * reps), traceBytes / (events * reps));
  }

  // Duration-based measurements retain the historical separate trace-size run.
  private static double traceBytesPerEvent(
      Scenario s, double emptySessionBytes, String encoding, boolean writerV2) {
    int iters = s.mName.startsWith("callstack") ? 500 : 10_000;
    try (Recording recording = new Recording(256 * 1024, false, encoding, writerV2)) {
      s.run(iters);
      recording.close();
      return (recording.traceBytes() - emptySessionBytes) / ((double) iters * s.mEventsPerIter);
    }
  }

  public static void main(String[] args) throws IOException {
    String filter = "";
    String scenario = null;
    int reps = 5;
    int repMs = 200;
    boolean list = false;
    String encoding = "default";
    String writer = "v1";
    int verifiedIters = 0;
    for (String arg : args) {
      if (arg.startsWith("--filter=")) {
        filter = arg.substring("--filter=".length());
      } else if (arg.startsWith("--scenario=")) {
        scenario = arg.substring("--scenario=".length());
      } else if (arg.startsWith("--reps=")) {
        reps = Integer.parseInt(arg.substring("--reps=".length()));
      } else if (arg.startsWith("--rep-ms=")) {
        repMs = Integer.parseInt(arg.substring("--rep-ms=".length()));
      } else if (arg.startsWith("--encoding=")) {
        encoding = arg.substring("--encoding=".length());
      } else if (arg.startsWith("--writer=")) {
        writer = arg.substring("--writer=".length());
      } else if (arg.startsWith("--verified-iters=")) {
        verifiedIters = Integer.parseInt(arg.substring("--verified-iters=".length()));
        if (verifiedIters <= 0) {
          throw new IllegalArgumentException("--verified-iters must be positive");
        }
      } else if (arg.equals("--list")) {
        list = true;
      } else {
        throw new IllegalArgumentException("Unknown argument " + arg);
      }
    }

    if (reps <= 0 || repMs <= 0) {
      throw new IllegalArgumentException("--reps and --rep-ms must be positive");
    }
    if (!List.of("default", "legacy", "encoded", "mixed").contains(encoding)) {
      throw new IllegalArgumentException("Unknown encoding " + encoding);
    }
    if (!writer.equals("v1") && !writer.equals("v2")) {
      throw new IllegalArgumentException("Unknown writer " + writer);
    }
    boolean writerV2 = writer.equals("v2");
    List<Scenario> selected = new ArrayList<>();
    for (Scenario s : SCENARIOS) {
      if (scenario != null ? s.mName.equals(scenario) : s.mName.contains(filter)) {
        selected.add(s);
      }
    }
    if (list) {
      for (Scenario s : selected) {
        System.out.println(s.mName);
      }
      return;
    }
    if (selected.isEmpty()) {
      throw new IllegalArgumentException("No matching benchmark scenario");
    }

    PerfettoTrace.register(true);
    ON.register();
    OFF.register();

    System.out.println("# encoding=" + encoding + " writer=" + writer
        + " verified_iters=" + verifiedIters);
    System.out.println("# name\tcpu_ns\twall_ns\talloc_bytes\ttrace_bytes");
    if (verifiedIters > 0) {
      for (Scenario s : selected) {
        System.out.println(s.mName + "\t"
            + measureVerified(s, reps, verifiedIters, encoding, writerV2));
      }
      System.exit(0);
      return;
    }

    double emptyBytes;
    try (Recording empty = new Recording(1024, false, encoding, writerV2)) {
      empty.close();
      emptyBytes = empty.traceBytes();
    }
    double[] traceBytes = new double[selected.size()];
    for (int i = 0; i < selected.size(); i++) {
      traceBytes[i] = traceBytesPerEvent(selected.get(i), emptyBytes, encoding, writerV2);
    }
    try (Recording recording = new Recording(32 * 1024, true, encoding, writerV2)) {
      for (int i = 0; i < selected.size(); i++) {
        Scenario s = selected.get(i);
        System.out.println(s.mName + "\t" + measure(s, reps, repMs) + "\t"
            + String.format(Locale.ROOT, "%.2f", traceBytes[i]));
      }
    }
    System.exit(0);
  }
}
