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

import static com.google.common.truth.Truth.assertThat;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import dev.perfetto.sdk.PerfettoDataSource;
import dev.perfetto.sdk.PerfettoTrace;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import perfetto.protos.DataSourceConfigOuterClass.DataSourceConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig.BufferConfig;
import perfetto.protos.TraceConfigOuterClass.TraceConfig.DataSource;
import perfetto.protos.TraceOuterClass.Trace;
import perfetto.protos.TracePacketOuterClass.TracePacket;

/** End-to-end tests for custom {@link PerfettoDataSource}s over in-process sessions. */
@RunWith(AndroidJUnit4.class)
public class PerfettoDataSourceTest {
  private static final int TRACE_PACKET_TIMESTAMP = 8; // TracePacket.timestamp

  /** A data source that records lifecycle calls so tests can assert on them. */
  private static final class CountingDataSource extends PerfettoDataSource {
    final AtomicInteger setups = new AtomicInteger();
    final AtomicInteger starts = new AtomicInteger();
    final AtomicInteger stops = new AtomicInteger();
    volatile byte[] lastConfig;

    @Override
    protected void onSetup(int instanceIndex, byte[] config) {
      lastConfig = config;
      setups.incrementAndGet();
    }

    @Override
    protected void onStart(int instanceIndex) {
      starts.incrementAndGet();
    }

    @Override
    protected void onStop(int instanceIndex) {
      stops.incrementAndGet();
    }
  }

  @Before
  public void setUp() {
    System.loadLibrary("perfetto_jni");
    PerfettoTrace.register(/* isBackendInProcess= */ true);
  }

  @Test
  public void emitsPackets() throws Exception {
    String name = "com.example.cds.emits";
    CountingDataSource ds = register(name);
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, config(name));
    awaitAtLeast(ds.starts, 1);

    emit(ds, 0x101);
    emit(ds, 0x102);
    emit(ds, 0x103);

    Set<Long> timestamps = timestampsIn(session.close());
    assertThat(timestamps).containsAtLeast(0x101L, 0x102L, 0x103L);
  }

  @Test
  public void runsLifecycleAndTogglesEnabled() throws Exception {
    String name = "com.example.cds.lifecycle";
    CountingDataSource ds = register(name);
    assertThat(ds.trace()).isNull(); // disabled before any session

    PerfettoTrace.Session session = new PerfettoTrace.Session(true, config(name));
    awaitAtLeast(ds.starts, 1);
    assertThat(ds.trace()).isNotNull(); // enabled while running
    assertThat(ds.lastConfig).isNotNull(); // onSetup got the config bytes

    session.close();
    awaitAtLeast(ds.stops, 1);

    assertThat(ds.setups.get()).isEqualTo(1);
    assertThat(ds.starts.get()).isEqualTo(1);
    assertThat(ds.stops.get()).isEqualTo(1);
    assertThat(ds.trace()).isNull(); // disabled again after stop
  }

  @Test
  public void reEnablesForASecondSession() throws Exception {
    String name = "com.example.cds.reenable";
    CountingDataSource ds = register(name);

    PerfettoTrace.Session first = new PerfettoTrace.Session(true, config(name));
    awaitAtLeast(ds.starts, 1);
    emit(ds, 0x201);
    first.close();
    awaitAtLeast(ds.stops, 1);
    assertThat(ds.trace()).isNull();

    PerfettoTrace.Session second = new PerfettoTrace.Session(true, config(name));
    awaitAtLeast(ds.starts, 2);
    assertThat(ds.trace()).isNotNull();
    emit(ds, 0x202);

    assertThat(timestampsIn(second.close())).contains(0x202L);
    assertThat(ds.starts.get()).isEqualTo(2);
  }

  @Test
  public void writesToAllConcurrentSessions() throws Exception {
    String name = "com.example.cds.concurrent";
    CountingDataSource ds = register(name);

    PerfettoTrace.Session a = new PerfettoTrace.Session(true, config(name));
    PerfettoTrace.Session b = new PerfettoTrace.Session(true, config(name));
    awaitAtLeast(ds.starts, 2); // both instances active

    emit(ds, 0x301); // fanned out to both instances

    Set<Long> traceA = timestampsIn(a.close());
    Set<Long> traceB = timestampsIn(b.close());
    assertThat(traceA).contains(0x301L);
    assertThat(traceB).contains(0x301L);

    awaitAtLeast(ds.stops, 2);
    assertThat(ds.trace()).isNull(); // disabled once both stopped
  }

  @Test
  public void flushKeepsPackets() throws Exception {
    String name = "com.example.cds.flush";
    CountingDataSource ds = register(name);
    PerfettoTrace.Session session = new PerfettoTrace.Session(true, config(name));
    awaitAtLeast(ds.starts, 1);

    emit(ds, 0x401);
    PerfettoDataSource.TraceContext ctx = ds.trace();
    ctx.flush();

    assertThat(timestampsIn(session.close())).contains(0x401L);
  }

  private static CountingDataSource register(String name) {
    CountingDataSource ds = new CountingDataSource();
    ds.register(name);
    return ds;
  }

  private static void emit(PerfettoDataSource ds, long timestamp) {
    PerfettoDataSource.TraceContext ctx = ds.trace();
    assertThat(ctx).isNotNull();
    ctx.newPacket().writeVarInt(TRACE_PACKET_TIMESTAMP, timestamp);
    ctx.commit();
  }

  private static Set<Long> timestampsIn(byte[] traceBytes) throws Exception {
    Set<Long> out = new HashSet<>();
    for (TracePacket packet : Trace.parseFrom(traceBytes).getPacketList()) {
      out.add(packet.getTimestamp());
    }
    return out;
  }

  private static void awaitAtLeast(AtomicInteger counter, int target) throws InterruptedException {
    for (int i = 0; i < 500 && counter.get() < target; i++) {
      Thread.sleep(10);
    }
  }

  private static byte[] config(String dataSourceName) {
    BufferConfig buffer = BufferConfig.newBuilder().setSizeKb(1024).build();
    DataSourceConfig dsConfig = DataSourceConfig.newBuilder().setName(dataSourceName).build();
    DataSource ds = DataSource.newBuilder().setConfig(dsConfig).build();
    return TraceConfig.newBuilder().addBuffers(buffer).addDataSources(ds).build().toByteArray();
  }
}
