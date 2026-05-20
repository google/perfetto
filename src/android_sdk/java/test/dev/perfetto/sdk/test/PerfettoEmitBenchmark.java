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

import com.sun.management.ThreadMXBean;
import dev.perfetto.sdk.PerfettoTrace;
import dev.perfetto.sdk.PerfettoTrace.Category;
import java.lang.management.ManagementFactory;
import perfetto.protos.DataSourceConfigOuterClass.DataSourceConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig.BufferConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig.DataSource;
import perfetto.protos.TrackEventConfigOuterClass.TrackEventConfig;

/**
 * Microbenchmark for the track-event emit path.
 *
 * <p>Run on host via {@code tools/run_android_sdk_host_test --bench}. Reports
 * wall-clock ns/op and per-thread allocated bytes/op for each scenario.
 * Allocation is the deterministic signal: the emit path should not allocate on
 * the Java heap.
 */
public final class PerfettoEmitBenchmark {
  private static final String FOO = "foo";
  private static final Category FOO_CATEGORY = new Category(FOO);

  private static final int WARMUP_ITERS = 200_000;
  private static final int MEASURE_ITERS =
      Integer.getInteger("perfetto.bench.iters", 2_000_000);
  private static final int TRIALS = Integer.getInteger("perfetto.bench.trials", 5);

  private static final ThreadMXBean TMX =
      (ThreadMXBean) ManagementFactory.getThreadMXBean();

  private PerfettoEmitBenchmark() {}

  public static void main(String[] args) {
    System.loadLibrary("perfetto_jni");
    PerfettoTrace.register(true);
    var unused = FOO_CATEGORY.register();

    PerfettoTrace.Session session =
        new PerfettoTrace.Session(true, traceConfig().toByteArray());

    // Allocation-probe mode: run a single scenario in a tight loop so an
    // external malloc counter (or perf) can attribute native allocations to the
    // emit path alone, with no benchmark scaffolding in the way.
    if (Boolean.getBoolean("perfetto.bench.alloc")) {
      // -Dperfetto.bench.args=true probes instant + 3 debug args with *distinct*
      // arg names each iteration; otherwise a bare instant.
      boolean withArgs = Boolean.getBoolean("perfetto.bench.args");
      String[] names = buildArgNames();
      for (int i = 0; i < WARMUP_ITERS; i++) {
        if (withArgs) {
          emitWithDistinctArgs(names, i);
        } else {
          emitInstant();
        }
      }
      long tid = Thread.currentThread().threadId();
      long jBefore = TMX.getThreadAllocatedBytes(tid);
      for (int i = 0; i < MEASURE_ITERS; i++) {
        if (withArgs) {
          emitWithDistinctArgs(names, WARMUP_ITERS + i);
        } else {
          emitInstant();
        }
      }
      long jBytes = TMX.getThreadAllocatedBytes(tid) - jBefore;
      System.err.printf(
          "JAVA_ALLOC iters=%d javaBytesPerOp=%.4f%n",
          MEASURE_ITERS, (double) jBytes / MEASURE_ITERS);
      session.close();
      return;
    }

    System.out.println("=== PerfettoEmitBenchmark ===");
    System.out.printf("warmup=%d measure=%d trials=%d%n", WARMUP_ITERS, MEASURE_ITERS, TRIALS);
    System.out.println();

    benchScenario("instant (name+category)", PerfettoEmitBenchmark::emitInstant);
    benchScenario("slice begin+end", PerfettoEmitBenchmark::emitSlicePair);
    benchScenario("instant + 3 debug args", PerfettoEmitBenchmark::emitInstantWithArgs);
    benchScenario("instant on named track", PerfettoEmitBenchmark::emitOnTrack);
    benchScenario("instant + 2 flows", PerfettoEmitBenchmark::emitWithFlows);
    benchScenario("counter on counter track", PerfettoEmitBenchmark::emitCounter);

    session.close();
  }

  private interface EmitOp {
    void run();
  }

  private static void emitInstant() {
    PerfettoTrace.instant(FOO_CATEGORY, "event").emit();
  }

  private static void emitSlicePair() {
    PerfettoTrace.begin(FOO_CATEGORY, "slice").emit();
    PerfettoTrace.end(FOO_CATEGORY).emit();
  }

  private static void emitInstantWithArgs() {
    PerfettoTrace.instant(FOO_CATEGORY, "event")
        .addArg("int_arg", 42L)
        .addArg("bool_arg", true)
        .addArg("str_arg", "value")
        .emit();
  }

  private static void emitOnTrack() {
    PerfettoTrace.instant(FOO_CATEGORY, "event").usingProcessNamedTrack(7, "track").emit();
  }

  private static void emitWithFlows() {
    PerfettoTrace.instant(FOO_CATEGORY, "event").addFlow(11).addTerminatingFlow(22).emit();
  }

  private static void emitCounter() {
    PerfettoTrace.counter(FOO_CATEGORY, 42).usingProcessCounterTrack("ctr").emit();
  }

  // A fixed pool of distinct arg names so cycling through them generates no
  // strings in the measured loop.
  private static String[] buildArgNames() {
    String[] names = new String[64];
    for (int i = 0; i < names.length; i++) {
      names[i] = "arg_" + i;
    }
    return names;
  }

  private static void emitWithDistinctArgs(String[] names, int i) {
    int n = names.length;
    PerfettoTrace.instant(FOO_CATEGORY, "event")
        .addArg(names[(i * 3) % n], 42L)
        .addArg(names[(i * 3 + 1) % n], true)
        .addArg(names[(i * 3 + 2) % n], "value")
        .emit();
  }

  private static void benchScenario(String label, EmitOp op) {
    Result r = measure(op);
    System.out.println(label);
    System.out.printf("  %7.1f ns/op   %6d bytes/op%n%n", r.nsPerOp, r.bytesPerOp);
  }

  private static Result measure(EmitOp op) {
    for (int i = 0; i < WARMUP_ITERS; i++) {
      op.run();
    }

    double bestNsPerOp = Double.MAX_VALUE;
    for (int t = 0; t < TRIALS; t++) {
      long start = System.nanoTime();
      for (int i = 0; i < MEASURE_ITERS; i++) {
        op.run();
      }
      long elapsed = System.nanoTime() - start;
      bestNsPerOp = Math.min(bestNsPerOp, (double) elapsed / MEASURE_ITERS);
    }

    long tid = Thread.currentThread().threadId();
    long allocBefore = TMX.getThreadAllocatedBytes(tid);
    for (int i = 0; i < MEASURE_ITERS; i++) {
      op.run();
    }
    long allocBytes = TMX.getThreadAllocatedBytes(tid) - allocBefore;

    return new Result(bestNsPerOp, allocBytes / MEASURE_ITERS);
  }

  private static final class Result {
    final double nsPerOp;
    final long bytesPerOp;

    Result(double nsPerOp, long bytesPerOp) {
      this.nsPerOp = nsPerOp;
      this.bytesPerOp = bytesPerOp;
    }
  }

  private static TraceConfig traceConfig() {
    BufferConfig bufferConfig = BufferConfig.newBuilder().setSizeKb(8192).build();
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
}
