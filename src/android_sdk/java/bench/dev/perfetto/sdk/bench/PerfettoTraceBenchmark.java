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
import java.lang.management.ManagementFactory;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

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
 * (default 5), {@code --rep-ms=<ms>} (default 200), {@code --list}.
 */
public final class PerfettoTraceBenchmark {
  private static final Category ON = new Category("bench_on");
  private static final Category OFF = new Category("bench_off");

  private static final PerfettoTrack NESTED =
      PerfettoTrack.process("bench_parent").child("bench_child");

  private static final String[] NAMES64 = new String[64];
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
  private static byte[] traceConfig(int sizeKb, boolean ringBuffer) {
    ByteArrayOutputStream buffer = new ByteArrayOutputStream();
    tag(buffer, 1, 0); // size_kb
    varint(buffer, sizeKb);
    tag(buffer, 4, 0); // fill_policy: RING_BUFFER = 1, DISCARD = 2
    varint(buffer, ringBuffer ? 1 : 2);

    ByteArrayOutputStream te = new ByteArrayOutputStream();
    lenField(te, 2, "bench_on".getBytes(StandardCharsets.UTF_8)); // enabled_categories

    ByteArrayOutputStream dsc = new ByteArrayOutputStream();
    lenField(dsc, 1, "track_event".getBytes(StandardCharsets.UTF_8)); // name
    lenField(dsc, 113, te.toByteArray()); // track_event_config

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
        "%.2f\t%.2f\t%.2f", cpu[reps / 2], wall[reps / 2], alloc / (events * reps));
  }

  // Measure trace size separately, subtracting empty-session overhead.
  private static double traceBytesPerEvent(Scenario s, double emptySessionBytes) {
    int iters = s.mName.startsWith("callstack") ? 500 : 10_000;
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig(256 * 1024, false));
    s.run(iters);
    byte[] trace = session.close();
    return (trace.length - emptySessionBytes) / ((double) iters * s.mEventsPerIter);
  }

  public static void main(String[] args) {
    String filter = "";
    String scenario = null;
    int reps = 5;
    int repMs = 200;
    boolean list = false;
    for (String arg : args) {
      if (arg.startsWith("--filter=")) {
        filter = arg.substring("--filter=".length());
      } else if (arg.startsWith("--scenario=")) {
        scenario = arg.substring("--scenario=".length());
      } else if (arg.startsWith("--reps=")) {
        reps = Integer.parseInt(arg.substring("--reps=".length()));
      } else if (arg.startsWith("--rep-ms=")) {
        repMs = Integer.parseInt(arg.substring("--rep-ms=".length()));
      } else if (arg.equals("--list")) {
        list = true;
      } else {
        throw new IllegalArgumentException("Unknown argument " + arg);
      }
    }

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

    PerfettoTrace.register(true);
    ON.register();
    OFF.register();

    PerfettoTrace.Session empty = new PerfettoTrace.Session(true, traceConfig(1024, false));
    double emptyBytes = empty.close().length;
    double[] traceBytes = new double[selected.size()];
    for (int i = 0; i < selected.size(); i++) {
      traceBytes[i] = traceBytesPerEvent(selected.get(i), emptyBytes);
    }

    PerfettoTrace.Session session = new PerfettoTrace.Session(true, traceConfig(32 * 1024, true));
    for (int i = 0; i < selected.size(); i++) {
      Scenario s = selected.get(i);
      System.out.println(
          s.mName + "\t" + measure(s, reps, repMs) + "\t" + String.format("%.2f", traceBytes[i]));
    }
    session.close();
    System.exit(0);
  }
}
