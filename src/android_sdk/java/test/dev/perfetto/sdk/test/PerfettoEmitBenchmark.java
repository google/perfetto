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
import dev.perfetto.sdk.PerfettoTrackEventBuilder;
import java.lang.management.ManagementFactory;
import perfetto.protos.DataSourceConfigOuterClass.DataSourceConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig.BufferConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig.DataSource;
import perfetto.protos.TrackEventConfigOuterClass.TrackEventConfig;

/**
 * Microbenchmark comparing the High Level emit path against the Java-side Low
 * Level emit path for extra-free track events.
 *
 * <p>Run on host via {@code tools/run_android_sdk_host_test --bench}. Reports
 * wall-clock ns/op and per-thread allocated bytes/op for each path. Allocation
 * is the deterministic signal: the Java emit path should not allocate on the
 * hot path.
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

    // Allocation-probe mode: run only the Java emit path in a tight loop so an
    // external malloc counter (or perf) can attribute native allocations to the
    // emit path alone, with no HL path and no benchmark scaffolding in the way.
    if (Boolean.getBoolean("perfetto.bench.alloc")) {
      // -Dperfetto.bench.java=true (default) probes the Java emit path; =false
      // probes the HL path, so an external malloc counter can confirm the two
      // paths allocate identically (i.e. the per-emit mallocs are SDK-internal).
      boolean java = Boolean.parseBoolean(System.getProperty("perfetto.bench.java", "true"));
      PerfettoTrackEventBuilder.setUseJavaEmit(java);
      for (int i = 0; i < WARMUP_ITERS; i++) {
        emitInstant();
      }
      long tid = Thread.currentThread().threadId();
      long jBefore = TMX.getThreadAllocatedBytes(tid);
      for (int i = 0; i < MEASURE_ITERS; i++) {
        emitInstant();
      }
      long jBytes = TMX.getThreadAllocatedBytes(tid) - jBefore;
      System.err.printf(
          "JAVA_ALLOC path=%s iters=%d javaBytesPerOp=%.4f%n",
          java ? "java" : "hl", MEASURE_ITERS, (double) jBytes / MEASURE_ITERS);
      PerfettoTrackEventBuilder.setUseJavaEmit(false);
      session.close();
      return;
    }

    System.out.println("=== PerfettoEmitBenchmark ===");
    System.out.printf("warmup=%d measure=%d trials=%d%n", WARMUP_ITERS, MEASURE_ITERS, TRIALS);
    System.out.println();

    benchScenario("instant (name+category)", PerfettoEmitBenchmark::emitInstant);
    benchScenario("slice begin+end", PerfettoEmitBenchmark::emitSlicePair);

    // HL-only: marginal cost of debug args. The new path falls back to HL for
    // events with extras, so both columns are HL today; the gap between this and
    // the extra-free instant above is the per-event JNI/native work that a
    // Java-side encoded body could eliminate.
    benchScenario("instant + 3 debug args", PerfettoEmitBenchmark::emitInstantWithArgs);

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

  private static void benchScenario(String label, EmitOp op) {
    Result hl = measure(op, /* useJavaEmit= */ false);
    Result ll = measure(op, /* useJavaEmit= */ true);

    System.out.println(label);
    System.out.printf("  HL  : %7.1f ns/op   %6d bytes/op%n", hl.nsPerOp, hl.bytesPerOp);
    System.out.printf("  Java: %7.1f ns/op   %6d bytes/op%n", ll.nsPerOp, ll.bytesPerOp);
    double speedup = hl.nsPerOp / ll.nsPerOp;
    System.out.printf(
        "  ->  %.2fx time, %+d bytes/op%n%n", speedup, ll.bytesPerOp - hl.bytesPerOp);
  }

  private static Result measure(EmitOp op, boolean useJavaEmit) {
    PerfettoTrackEventBuilder.setUseJavaEmit(useJavaEmit);

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

    PerfettoTrackEventBuilder.setUseJavaEmit(false);
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
