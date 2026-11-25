// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import type {Engine} from '../../../ui/src/trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
  type InferRowType,
  type QueryResult,
  type SpecType,
} from '../../../ui/src/trace_processor/query_result';

export interface WindowArgs {
  readonly startTs?: bigint;
  readonly endTs?: bigint;
}

function wArgs(w?: WindowArgs): string {
  const s = w?.startTs !== undefined ? w.startTs.toString() : 'NULL';
  const e = w?.endTs !== undefined ? w.endTs.toString() : 'NULL';
  return `${s}, ${e}`;
}

function qMod(engine: Engine, mod: string, call: string): Promise<QueryResult> {
  return engine.query(`INCLUDE PERFETTO MODULE sysmon.${mod}; SELECT * FROM ${call};`);
}

function mapRows<S extends SpecType, T>(
  res: QueryResult | undefined,
  spec: S,
  map: (r: InferRowType<S>) => T,
): T[] {
  if (!res) return [];
  const out: T[] = [];
  for (const it = res.iter(spec); it.valid(); it.next()) out.push(map(it));
  return out;
}

function mapFirst<S extends SpecType, T>(
  res: QueryResult,
  spec: S,
  map: (r: InferRowType<S>) => T,
): T | undefined {
  const it = res.iter(spec);
  return it.valid() ? map(it) : undefined;
}

export interface PulseRow {
  readonly startTs: bigint;
  readonly endTs: bigint;
  readonly durS: number;
  readonly cpuCount: number;
  readonly cpuBusyPct: number | null;
  readonly cpuIowaitPct: number | null;
  readonly cpuPsiSomePct: number | null;
  readonly memTotalBytes: number | null;
  readonly memAvailMinBytes: number | null;
  readonly memAvailLastBytes: number | null;
  readonly memAnonBytes: number | null;
  readonly memCacheBytes: number | null;
  readonly memDmabufGpuBytes: number | null;
  readonly memZramBytes: number | null;
  readonly memPsiSomePct: number | null;
  readonly memPsiFullPct: number | null;
  readonly lmkdKills: number;
  readonly directReclaimEvents: number;
  readonly ioPsiSomePct: number | null;
  readonly ioPsiFullPct: number | null;
  readonly ioReadMb: number;
  readonly ioWriteMb: number;
  readonly ioDstateMs: number;
  readonly peakTempC: number | null;
  readonly throttleEvents: number;
  readonly avgPowerMw: number | null;
  readonly battPct: number | null;
  readonly battCurrentMa: number | null;
  readonly battVoltageV: number | null;
  readonly hasSched: boolean;
  readonly hasPsi: boolean;
  readonly hasMeminfo: boolean;
  readonly hasDiskstat: boolean;
  readonly hasBlock: boolean;
  readonly hasThermal: boolean;
  readonly hasPower: boolean;
}

export async function loadPulse(
  engine: Engine,
  w?: WindowArgs,
): Promise<PulseRow | undefined> {
  const res = await qMod(engine, 'pulse', `sysmon_pulse(${wArgs(w)})`);
  return mapFirst(
    res,
    {
      start_ts: LONG,
      end_ts: LONG,
      dur_s: NUM,
      cpu_count: NUM,
      cpu_busy_pct: NUM_NULL,
      cpu_iowait_pct: NUM_NULL,
      cpu_psi_some_pct: NUM_NULL,
      mem_total_bytes: NUM_NULL,
      mem_avail_min_bytes: NUM_NULL,
      mem_avail_last_bytes: NUM_NULL,
      mem_anon_bytes: NUM_NULL,
      mem_cache_bytes: NUM_NULL,
      mem_dmabuf_gpu_bytes: NUM_NULL,
      mem_zram_bytes: NUM_NULL,
      mem_psi_some_pct: NUM_NULL,
      mem_psi_full_pct: NUM_NULL,
      lmkd_kills: NUM,
      direct_reclaim_events: NUM,
      io_psi_some_pct: NUM_NULL,
      io_psi_full_pct: NUM_NULL,
      io_read_mb: NUM,
      io_write_mb: NUM,
      io_dstate_ms: NUM,
      peak_temp_c: NUM_NULL,
      throttle_events: NUM,
      avg_power_mw: NUM_NULL,
      batt_pct: NUM_NULL,
      batt_current_ma: NUM_NULL,
      batt_voltage_v: NUM_NULL,
      has_sched: NUM,
      has_psi: NUM,
      has_meminfo: NUM,
      has_diskstat: NUM,
      has_block: NUM,
      has_thermal: NUM,
      has_power: NUM,
    },
    (it) => ({
      startTs: it.start_ts,
      endTs: it.end_ts,
      durS: it.dur_s,
      cpuCount: it.cpu_count,
      cpuBusyPct: it.cpu_busy_pct,
      cpuIowaitPct: it.cpu_iowait_pct,
      cpuPsiSomePct: it.cpu_psi_some_pct,
      memTotalBytes: it.mem_total_bytes,
      memAvailMinBytes: it.mem_avail_min_bytes,
      memAvailLastBytes: it.mem_avail_last_bytes,
      memAnonBytes: it.mem_anon_bytes,
      memCacheBytes: it.mem_cache_bytes,
      memDmabufGpuBytes: it.mem_dmabuf_gpu_bytes,
      memZramBytes: it.mem_zram_bytes,
      memPsiSomePct: it.mem_psi_some_pct,
      memPsiFullPct: it.mem_psi_full_pct,
      lmkdKills: it.lmkd_kills,
      directReclaimEvents: it.direct_reclaim_events,
      ioPsiSomePct: it.io_psi_some_pct,
      ioPsiFullPct: it.io_psi_full_pct,
      ioReadMb: it.io_read_mb,
      ioWriteMb: it.io_write_mb,
      ioDstateMs: it.io_dstate_ms,
      peakTempC: it.peak_temp_c,
      throttleEvents: it.throttle_events,
      avgPowerMw: it.avg_power_mw,
      battPct: it.batt_pct,
      battCurrentMa: it.batt_current_ma,
      battVoltageV: it.batt_voltage_v,
      hasSched: it.has_sched === 1,
      hasPsi: it.has_psi === 1,
      hasMeminfo: it.has_meminfo === 1,
      hasDiskstat: it.has_diskstat === 1,
      hasBlock: it.has_block === 1,
      hasThermal: it.has_thermal === 1,
      hasPower: it.has_power === 1,
    }),
  );
}

export interface PressureBucketRow {
  readonly bin: number;
  readonly relS: number;
  readonly cpuPressure: number;
  readonly memPressure: number;
  readonly ioPressure: number;
  readonly thermalPressure: number;
  readonly hasLmkd: boolean;
}

export async function loadPressureBuckets(
  engine: Engine,
  w?: WindowArgs,
  numBins = 48,
): Promise<PressureBucketRow[]> {
  const res = await qMod(engine, 'pulse', `sysmon_pressure_buckets(${wArgs(w)}, ${numBins})`);
  return mapRows(
    res,
    {
      bin: NUM,
      rel_s: NUM,
      cpu_pressure: NUM,
      mem_pressure: NUM,
      io_pressure: NUM,
      thermal_pressure: NUM,
      has_lmkd: NUM,
    },
    (it) => ({
      bin: it.bin,
      relS: it.rel_s,
      cpuPressure: it.cpu_pressure,
      memPressure: it.mem_pressure,
      ioPressure: it.io_pressure,
      thermalPressure: it.thermal_pressure,
      hasLmkd: it.has_lmkd === 1,
    }),
  );
}

export interface LedgerRow {
  readonly category: string;
  readonly bytes: number;
  readonly deltaBytes: number;
  readonly pctOfTotal: number;
}

export interface MemSeriesRow {
  readonly relS: number;
  readonly anonMib: number;
  readonly cacheMib: number;
  readonly kernelSlabMib: number;
  readonly dmabufGpuMib: number;
  readonly zramMib: number;
  readonly freeMib: number;
  readonly otherMib: number;
  readonly availMib: number;
}

export interface MemPressureSeriesRow {
  readonly relS: number;
  readonly psiMemSomePct: number;
  readonly psiMemFullPct: number;
  readonly pgmajfaultS: number;
  readonly pswpinS: number;
  readonly pswpoutS: number;
  readonly reclaimScanS: number;
}

export interface ReclaimRow {
  readonly psiMemSomePct: number | null;
  readonly psiMemFullPct: number | null;
  readonly directReclaimEvents: number;
  readonly kswapdWakes: number;
  readonly allocstallDelta: number;
  readonly compactStallDelta: number;
  readonly pgmajfaultDelta: number;
  readonly pswpinDelta: number;
  readonly pswpoutDelta: number;
  readonly lmkdKills: number;
  readonly activeFileMib: number | null;
  readonly inactiveFileMib: number | null;
  readonly activeAnonMib: number | null;
  readonly inactiveAnonMib: number | null;
}

export interface LmkdRow {
  readonly relS: number;
  readonly pid: number | null;
  readonly processName: string;
  readonly oomScoreAdj: number | null;
  readonly rssMib: number;
}

export interface TopMemProcRow {
  readonly upid: number;
  readonly pid: number;
  readonly processName: string;
  readonly anonMib: number;
  readonly swapMib: number;
  readonly fileMib: number;
  readonly rssMib: number;
  readonly dmabufMib: number;
  readonly anonSwapMib: number;
  readonly deltaMib: number;
  readonly oomScoreAdj: number | null;
}

export interface MemoryBundle {
  readonly ledger: LedgerRow[];
  readonly series: MemSeriesRow[];
  readonly pressureSeries: MemPressureSeriesRow[];
  readonly reclaim?: ReclaimRow;
  readonly lmkd: LmkdRow[];
  readonly topProcs: TopMemProcRow[];
}

export async function loadMemoryBundle(
  engine: Engine,
  w?: WindowArgs,
  limit = 15,
): Promise<MemoryBundle> {
  const args = wArgs(w);
  const [ledgerRes, seriesRes, pressureRes, reclaimRes, lmkdRes, procsRes] =
    await Promise.all([
      qMod(engine, 'memory', `sysmon_memtotal_ledger(${args})`),
      qMod(engine, 'memory', `sysmon_memory_series(${args})`),
      qMod(engine, 'memory', `sysmon_memory_pressure_series(${args})`),
      qMod(engine, 'memory', `sysmon_reclaim_summary(${args})`),
      qMod(engine, 'memory', `sysmon_lmkd_kills(${args})`),
      qMod(engine, 'memory', `sysmon_top_memory_processes(${args}, ${limit})`),
    ]);

  return {
    ledger: mapRows(
      ledgerRes,
      {category: STR, bytes: NUM, delta_bytes: NUM, pct_of_total: NUM},
      (it) => ({
        category: it.category,
        bytes: it.bytes,
        deltaBytes: it.delta_bytes,
        pctOfTotal: it.pct_of_total,
      }),
    ),
    series: mapRows(
      seriesRes,
      {
        rel_s: NUM,
        anon_mib: NUM,
        cache_mib: NUM,
        kernel_slab_mib: NUM,
        dmabuf_gpu_mib: NUM,
        zram_mib: NUM,
        free_mib: NUM,
        other_mib: NUM,
        avail_mib: NUM,
      },
      (it) => ({
        relS: it.rel_s,
        anonMib: it.anon_mib,
        cacheMib: it.cache_mib,
        kernelSlabMib: it.kernel_slab_mib,
        dmabufGpuMib: it.dmabuf_gpu_mib,
        zramMib: it.zram_mib,
        freeMib: it.free_mib,
        otherMib: it.other_mib,
        availMib: it.avail_mib,
      }),
    ),
    pressureSeries: mapRows(
      pressureRes,
      {
        rel_s: NUM,
        psi_mem_some_pct: NUM,
        psi_mem_full_pct: NUM,
        pgmajfault_s: NUM,
        pswpin_s: NUM,
        pswpout_s: NUM,
        reclaim_scan_s: NUM,
      },
      (it) => ({
        relS: it.rel_s,
        psiMemSomePct: it.psi_mem_some_pct,
        psiMemFullPct: it.psi_mem_full_pct,
        pgmajfaultS: it.pgmajfault_s,
        pswpinS: it.pswpin_s,
        pswpoutS: it.pswpout_s,
        reclaimScanS: it.reclaim_scan_s,
      }),
    ),
    reclaim: mapFirst(
      reclaimRes,
      {
        psi_mem_some_pct: NUM_NULL,
        psi_mem_full_pct: NUM_NULL,
        direct_reclaim_events: NUM,
        kswapd_wakes: NUM,
        allocstall_delta: NUM,
        compact_stall_delta: NUM,
        pgmajfault_delta: NUM,
        pswpin_delta: NUM,
        pswpout_delta: NUM,
        lmkd_kills: NUM,
        active_file_mib: NUM_NULL,
        inactive_file_mib: NUM_NULL,
        active_anon_mib: NUM_NULL,
        inactive_anon_mib: NUM_NULL,
      },
      (it) => ({
        psiMemSomePct: it.psi_mem_some_pct,
        psiMemFullPct: it.psi_mem_full_pct,
        directReclaimEvents: it.direct_reclaim_events,
        kswapdWakes: it.kswapd_wakes,
        allocstallDelta: it.allocstall_delta,
        compactStallDelta: it.compact_stall_delta,
        pgmajfaultDelta: it.pgmajfault_delta,
        pswpinDelta: it.pswpin_delta,
        pswpoutDelta: it.pswpout_delta,
        lmkdKills: it.lmkd_kills,
        activeFileMib: it.active_file_mib,
        inactiveFileMib: it.inactive_file_mib,
        activeAnonMib: it.active_anon_mib,
        inactiveAnonMib: it.inactive_anon_mib,
      }),
    ),
    lmkd: mapRows(
      lmkdRes,
      {rel_s: NUM, pid: NUM_NULL, process_name: STR, oom_score_adj: NUM_NULL, rss_mib: NUM},
      (it) => ({
        relS: it.rel_s,
        pid: it.pid,
        processName: it.process_name,
        oomScoreAdj: it.oom_score_adj,
        rssMib: it.rss_mib,
      }),
    ),
    topProcs: mapRows(
      procsRes,
      {
        upid: NUM,
        pid: NUM,
        process_name: STR,
        anon_mib: NUM,
        swap_mib: NUM,
        file_mib: NUM,
        rss_mib: NUM,
        dmabuf_mib: NUM,
        anon_swap_mib: NUM,
        delta_mib: NUM,
        oom_score_adj: NUM_NULL,
      },
      (it) => ({
        upid: it.upid,
        pid: it.pid,
        processName: it.process_name,
        anonMib: it.anon_mib,
        swapMib: it.swap_mib,
        fileMib: it.file_mib,
        rssMib: it.rss_mib,
        dmabufMib: it.dmabuf_mib,
        anonSwapMib: it.anon_swap_mib,
        deltaMib: it.delta_mib,
        oomScoreAdj: it.oom_score_adj,
      }),
    ),
  };
}

export interface CpuCoreRow {
  readonly cpu: number;
  readonly busyPct: number | null;
  readonly userPct: number | null;
  readonly sysPct: number | null;
  readonly iowaitPct: number | null;
  readonly irqPct: number | null;
  readonly avgFreqKhz: number | null;
  readonly maxFreqKhz: number | null;
}

export interface CpuSeriesRow {
  readonly relS: number;
  readonly busyPct: number;
  readonly userPct: number;
  readonly sysPct: number;
  readonly iowaitPct: number;
  readonly irqPct: number;
  readonly psiCpuPct: number;
}

export interface CpuFreqSeriesRow {
  readonly relS: number;
  readonly cluster: string;
  readonly mhz: number;
}

export interface WaitBreakdownRow {
  readonly category: string;
  readonly label: string;
  readonly totalMs: number;
  readonly sharePct: number;
  readonly eventCount: number;
}

export interface TopThreadRow {
  readonly utid: number;
  readonly tid: number;
  readonly threadName: string;
  readonly processName: string;
  readonly runningMs: number;
  readonly runnableMs: number;
  readonly ioWaitMs: number;
  readonly dOtherMs: number;
  readonly totalActiveMs: number;
}

export interface CpuCoreSeriesRow {
  readonly relS: number;
  readonly cpu: number;
  readonly busyPct: number;
}

export interface CpuBundle {
  readonly cores: CpuCoreRow[];
  readonly series: CpuSeriesRow[];
  readonly freqSeries: CpuFreqSeriesRow[];
  readonly coreSeries: CpuCoreSeriesRow[];
  readonly waits: WaitBreakdownRow[];
  readonly topThreads: TopThreadRow[];
}

export async function loadCpuBundle(
  engine: Engine,
  w?: WindowArgs,
  limit = 15,
  liveOnly = false,
): Promise<CpuBundle> {
  const args = wArgs(w);
  const [coresRes, seriesRes, freqRes, coreSerRes, waitsRes, threadsRes] =
    await Promise.all([
      qMod(engine, 'cpu', `sysmon_cpu_cores(${args})`),
      qMod(engine, 'cpu', `sysmon_cpu_series(${args})`),
      qMod(engine, 'cpu', `sysmon_cpu_freq_series(${args})`),
      qMod(engine, 'cpu', `sysmon_cpu_core_series(${args})`),
      liveOnly ? undefined : qMod(engine, 'cpu', `sysmon_wait_breakdown(${args})`),
      liveOnly ? undefined : qMod(engine, 'cpu', `sysmon_top_cpu_threads(${args}, ${limit})`),
    ]);

  return {
    cores: mapRows(
      coresRes,
      {
        cpu: NUM,
        busy_pct: NUM_NULL,
        user_pct: NUM_NULL,
        sys_pct: NUM_NULL,
        iowait_pct: NUM_NULL,
        irq_pct: NUM_NULL,
        avg_freq_khz: NUM_NULL,
        max_freq_khz: NUM_NULL,
      },
      (it) => ({
        cpu: it.cpu,
        busyPct: it.busy_pct,
        userPct: it.user_pct,
        sysPct: it.sys_pct,
        iowaitPct: it.iowait_pct,
        irqPct: it.irq_pct,
        avgFreqKhz: it.avg_freq_khz,
        maxFreqKhz: it.max_freq_khz,
      }),
    ),
    series: mapRows(
      seriesRes,
      {
        rel_s: NUM,
        busy_pct: NUM,
        user_pct: NUM,
        sys_pct: NUM,
        iowait_pct: NUM,
        irq_pct: NUM,
        psi_cpu_pct: NUM,
      },
      (it) => ({
        relS: it.rel_s,
        busyPct: it.busy_pct,
        userPct: it.user_pct,
        sysPct: it.sys_pct,
        iowaitPct: it.iowait_pct,
        irqPct: it.irq_pct,
        psiCpuPct: it.psi_cpu_pct,
      }),
    ),
    freqSeries: mapRows(
      freqRes,
      {rel_s: NUM, cluster: STR, mhz: NUM},
      (it) => ({relS: it.rel_s, cluster: it.cluster, mhz: it.mhz}),
    ),
    coreSeries: mapRows(
      coreSerRes,
      {rel_s: NUM, cpu: NUM, busy_pct: NUM},
      (it) => ({relS: it.rel_s, cpu: it.cpu, busyPct: it.busy_pct}),
    ),
    waits: mapRows(
      waitsRes,
      {category: STR, label: STR, total_ms: NUM, share_pct: NUM, event_count: NUM},
      (it) => ({
        category: it.category,
        label: it.label,
        totalMs: it.total_ms,
        sharePct: it.share_pct,
        eventCount: it.event_count,
      }),
    ),
    topThreads: mapRows(
      threadsRes,
      {
        utid: NUM,
        tid: NUM,
        thread_name: STR,
        process_name: STR,
        running_ms: NUM,
        runnable_ms: NUM,
        io_wait_ms: NUM,
        d_other_ms: NUM,
        total_active_ms: NUM,
      },
      (it) => ({
        utid: it.utid,
        tid: it.tid,
        threadName: it.thread_name,
        processName: it.process_name,
        runningMs: it.running_ms,
        runnableMs: it.runnable_ms,
        ioWaitMs: it.io_wait_ms,
        dOtherMs: it.d_other_ms,
        totalActiveMs: it.total_active_ms,
      }),
    ),
  };
}

export interface WaitExplainerRow {
  readonly threadName: string;
  readonly processName: string;
  readonly blockedFunction: string;
  readonly topWaker: string;
  readonly waitCount: number;
  readonly totalMs: number;
  readonly maxMs: number;
}

export async function loadWaitExplainer(
  engine: Engine,
  category: 'runnable' | 'io_wait' | 'uninterruptible',
  w?: WindowArgs,
  limit = 20,
): Promise<WaitExplainerRow[]> {
  const res = await qMod(
    engine,
    'cpu',
    `sysmon_wait_explainer(${wArgs(w)}, '${category}', ${limit})`,
  );
  return mapRows(
    res,
    {
      thread_name: STR,
      process_name: STR,
      blocked_function: STR,
      top_waker: STR,
      wait_count: NUM,
      total_ms: NUM,
      max_ms: NUM,
    },
    (it) => ({
      threadName: it.thread_name,
      processName: it.process_name,
      blockedFunction: it.blocked_function,
      topWaker: it.top_waker,
      waitCount: it.wait_count,
      totalMs: it.total_ms,
      maxMs: it.max_ms,
    }),
  );
}

export interface StorageSummaryRow {
  readonly durS: number;
  readonly readMb: number;
  readonly writeMb: number;
  readonly readMbS: number;
  readonly writeMbS: number;
  readonly blockOps: number;
  readonly ioPsiSomePct: number | null;
  readonly ioPsiFullPct: number | null;
  readonly ioDstateMs: number;
  readonly dirtyMib: number;
  readonly writebackMib: number;
  readonly pgpginMb: number;
  readonly pgpgoutMb: number;
  readonly swapIoMb: number;
}

export interface StorageSeriesRow {
  readonly relS: number;
  readonly readMbS: number;
  readonly writeMbS: number;
  readonly ioPsiPct: number;
  readonly ioPsiFullPct: number;
  readonly dirtyMib: number;
  readonly writebackMib: number;
}

export interface StorageProcRow {
  readonly upid: number;
  readonly processName: string;
  readonly readMb: number;
  readonly writeMb: number;
  readonly totalIoMb: number;
  readonly blockOps: number;
  readonly ioWaitMs: number;
}

export interface StorageBundle {
  readonly summary?: StorageSummaryRow;
  readonly series: StorageSeriesRow[];
  readonly procs: StorageProcRow[];
}

export async function loadStorageBundle(
  engine: Engine,
  w?: WindowArgs,
  limit = 15,
  liveOnly = false,
): Promise<StorageBundle> {
  const args = wArgs(w);
  const [sumRes, serRes, procRes] = await Promise.all([
    qMod(engine, 'storage', `sysmon_storage_summary(${args})`),
    qMod(engine, 'storage', `sysmon_storage_series(${args})`),
    liveOnly ? undefined : qMod(engine, 'storage', `sysmon_storage_processes(${args}, ${limit})`),
  ]);

  return {
    summary: mapFirst(
      sumRes,
      {
        dur_s: NUM,
        read_mb: NUM,
        write_mb: NUM,
        read_mb_s: NUM,
        write_mb_s: NUM,
        block_ops: NUM,
        io_psi_some_pct: NUM_NULL,
        io_psi_full_pct: NUM_NULL,
        io_dstate_ms: NUM,
        dirty_mib: NUM,
        writeback_mib: NUM,
        pgpgin_mb: NUM,
        pgpgout_mb: NUM,
        swap_io_mb: NUM,
      },
      (it) => ({
        durS: it.dur_s,
        readMb: it.read_mb,
        writeMb: it.write_mb,
        readMbS: it.read_mb_s,
        writeMbS: it.write_mb_s,
        blockOps: it.block_ops,
        ioPsiSomePct: it.io_psi_some_pct,
        ioPsiFullPct: it.io_psi_full_pct,
        ioDstateMs: it.io_dstate_ms,
        dirtyMib: it.dirty_mib,
        writebackMib: it.writeback_mib,
        pgpginMb: it.pgpgin_mb,
        pgpgoutMb: it.pgpgout_mb,
        swapIoMb: it.swap_io_mb,
      }),
    ),
    series: mapRows(
      serRes,
      {
        rel_s: NUM,
        read_mb_s: NUM,
        write_mb_s: NUM,
        io_psi_pct: NUM,
        io_psi_full_pct: NUM,
        dirty_mib: NUM,
        writeback_mib: NUM,
      },
      (it) => ({
        relS: it.rel_s,
        readMbS: it.read_mb_s,
        writeMbS: it.write_mb_s,
        ioPsiPct: it.io_psi_pct,
        ioPsiFullPct: it.io_psi_full_pct,
        dirtyMib: it.dirty_mib,
        writebackMib: it.writeback_mib,
      }),
    ),
    procs: mapRows(
      procRes,
      {
        upid: NUM,
        process_name: STR,
        read_mb: NUM,
        write_mb: NUM,
        total_io_mb: NUM,
        block_ops: NUM,
        io_wait_ms: NUM,
      },
      (it) => ({
        upid: it.upid,
        processName: it.process_name,
        readMb: it.read_mb,
        writeMb: it.write_mb,
        totalIoMb: it.total_io_mb,
        blockOps: it.block_ops,
        ioWaitMs: it.io_wait_ms,
      }),
    ),
  };
}

export interface ThermalZoneRow {
  readonly zone: string;
  readonly minC: number;
  readonly avgC: number;
  readonly peakC: number;
  readonly samples: number;
}

export interface ThermalSeriesRow {
  readonly relS: number;
  readonly zone: string;
  readonly tempC: number;
}

export interface PowerRailRow {
  readonly rail: string;
  readonly avgMw: number;
  readonly energyMj: number;
  readonly samples: number;
}

export interface PowerSeriesRow {
  readonly relS: number;
  readonly rail: string;
  readonly mw: number;
}

export interface CoolingDeviceRow {
  readonly device: string;
  readonly lastState: number;
  readonly maxState: number;
  readonly updates: number;
}

export interface ThermalBundle {
  readonly zones: ThermalZoneRow[];
  readonly zoneSeries: ThermalSeriesRow[];
  readonly rails: PowerRailRow[];
  readonly powerSeries: PowerSeriesRow[];
  readonly cooling: CoolingDeviceRow[];
}

export async function loadThermalBundle(
  engine: Engine,
  w?: WindowArgs,
  limit = 15,
): Promise<ThermalBundle> {
  const args = wArgs(w);
  const [zRes, zsRes, rRes, psRes, cRes] = await Promise.all([
    qMod(engine, 'thermal', `sysmon_thermal_zones(${args}, ${limit})`),
    qMod(engine, 'thermal', `sysmon_thermal_series(${args}, 5)`),
    qMod(engine, 'thermal', `sysmon_power_rails(${args}, ${limit})`),
    qMod(engine, 'thermal', `sysmon_power_series(${args}, 6)`),
    qMod(engine, 'thermal', `sysmon_cooling_devices(${args})`),
  ]);

  return {
    zones: mapRows(
      zRes,
      {zone: STR, min_c: NUM, avg_c: NUM, peak_c: NUM, samples: NUM},
      (it) => ({
        zone: it.zone,
        minC: it.min_c,
        avgC: it.avg_c,
        peakC: it.peak_c,
        samples: it.samples,
      }),
    ),
    zoneSeries: mapRows(
      zsRes,
      {rel_s: NUM, zone: STR, temp_c: NUM},
      (it) => ({relS: it.rel_s, zone: it.zone, tempC: it.temp_c}),
    ),
    rails: mapRows(
      rRes,
      {rail: STR, avg_mw: NUM, energy_mj: NUM, samples: NUM},
      (it) => ({
        rail: it.rail,
        avgMw: it.avg_mw,
        energyMj: it.energy_mj,
        samples: it.samples,
      }),
    ),
    powerSeries: mapRows(
      psRes,
      {rel_s: NUM, rail: STR, mw: NUM},
      (it) => ({relS: it.rel_s, rail: it.rail, mw: it.mw}),
    ),
    cooling: mapRows(
      cRes,
      {device: STR, last_state: NUM, max_state: NUM, updates: NUM},
      (it) => ({
        device: it.device,
        lastState: it.last_state,
        maxState: it.max_state,
        updates: it.updates,
      }),
    ),
  };
}

export interface TopProcessRow {
  readonly upid: number;
  readonly pid: number;
  readonly processName: string;
  readonly cpuMs: number;
  readonly cpuPct: number;
  readonly runnableMs: number;
  readonly anonSwapMib: number;
  readonly memDeltaMib: number;
  readonly ioMb: number;
  readonly ioWaitMs: number;
  readonly oomScoreAdj: number | null;
  readonly dominantResource: string;
  readonly anonMib: number;
  readonly swapMib: number;
  readonly fileMib: number;
  readonly dmabufMib: number;
  readonly ageS: number | null;
  readonly isTop: boolean;
  readonly frozen: boolean;
  readonly cpuTrend?: readonly number[];
  readonly ramTrend?: readonly number[];
}

export async function loadTopProcesses(
  engine: Engine,
  w?: WindowArgs,
  limit = 12,
): Promise<TopProcessRow[]> {
  const res = await qMod(engine, 'processes', `sysmon_top_processes(${wArgs(w)}, ${limit})`);
  return mapRows(
    res,
    {
      upid: NUM,
      pid: NUM,
      process_name: STR,
      cpu_ms: NUM,
      cpu_pct: NUM,
      runnable_ms: NUM,
      anon_swap_mib: NUM,
      mem_delta_mib: NUM,
      io_mb: NUM,
      io_wait_ms: NUM,
      oom_score_adj: NUM_NULL,
      dominant_resource: STR_NULL,
      anon_mib: NUM,
      swap_mib: NUM,
      file_mib: NUM,
      dmabuf_mib: NUM,
      age_s: NUM_NULL,
      is_top: NUM,
      frozen: NUM,
    },
    (it) => ({
      upid: it.upid,
      pid: it.pid,
      processName: it.process_name,
      cpuMs: it.cpu_ms,
      cpuPct: it.cpu_pct,
      runnableMs: it.runnable_ms,
      anonSwapMib: it.anon_swap_mib,
      memDeltaMib: it.mem_delta_mib,
      ioMb: it.io_mb,
      ioWaitMs: it.io_wait_ms,
      oomScoreAdj: it.oom_score_adj,
      dominantResource: it.dominant_resource ?? 'CPU',
      anonMib: it.anon_mib,
      swapMib: it.swap_mib,
      fileMib: it.file_mib,
      dmabufMib: it.dmabuf_mib,
      ageS: it.age_s,
      isTop: it.is_top === 1,
      frozen: it.frozen === 1,
    }),
  );
}

// Loads the Task Manager process list together with binned CPU% and Anon+Swap
// sparkline arrays in parallel.
export async function loadTaskManager(
  engine: Engine,
  w?: WindowArgs,
  limit = 120,
  numBins = 15,
): Promise<TopProcessRow[]> {
  const [procs, trendsRes] = await Promise.all([
    loadTopProcesses(engine, w, limit),
    qMod(engine, 'processes', `sysmon_process_trends(${wArgs(w)}, ${Math.min(limit, 80)}, ${numBins})`),
  ]);

  const cpuByUpid = new Map<number, number[]>();
  const ramByUpid = new Map<number, number[]>();
  for (
    const it = trendsRes.iter({upid: NUM, bin: NUM, cpu_pct: NUM, anon_swap_mib: NUM});
    it.valid();
    it.next()
  ) {
    let cs = cpuByUpid.get(it.upid);
    if (!cs) cpuByUpid.set(it.upid, (cs = []));
    cs.push(it.cpu_pct);
    let rs = ramByUpid.get(it.upid);
    if (!rs) ramByUpid.set(it.upid, (rs = []));
    rs.push(it.anon_swap_mib);
  }

  return procs.map((p) => ({
    ...p,
    cpuTrend: cpuByUpid.get(p.upid),
    ramTrend: ramByUpid.get(p.upid),
  }));
}

export interface TopAppRow {
  readonly topApp: string | null;
  readonly topUpid: number | null;
  readonly topDurS: number;
  readonly source: string;
  readonly fgCount: number;
  readonly visCount: number;
  readonly percCount: number;
  readonly svcCount: number;
  readonly cachedCount: number;
  readonly nativeCount: number;
  readonly lmkdKills: number;
}

export interface OomBucketSummaryRow {
  readonly bucketIdx: number;
  readonly bucketName: string;
  readonly procCount: number;
  readonly anonSwapMib: number;
  readonly fileMib: number;
  readonly dmabufMib: number;
  readonly cpuPct: number;
}

export interface AppStateIntervalRow {
  readonly upid: number;
  readonly pid: number;
  readonly processName: string;
  readonly bucketIdx: number;
  readonly bucketName: string;
  readonly oomScoreAdj: number;
  readonly startRelS: number;
  readonly endRelS: number;
  readonly changed: boolean;
  readonly isTop: boolean;
  readonly killed: boolean;
  readonly frozen: boolean;
}

export interface OomMemSeriesRow {
  readonly relS: number;
  readonly bucketIdx: number;
  readonly bucketName: string;
  readonly anonSwapMib: number;
  readonly fileMib: number;
  readonly dmabufMib: number;
}

export interface OomBucketProcSeriesRow {
  readonly relS: number;
  readonly processName: string;
  readonly anonSwapMib: number;
  readonly fileMib: number;
  readonly dmabufMib: number;
}

export interface AppEventRow {
  readonly relS: number;
  readonly kind: string;
  readonly severity: string;
  readonly processName: string;
  readonly title: string;
  readonly detail: string;
  readonly targetTab: string;
}

export interface AppsBundle {
  readonly topApp?: TopAppRow;
  readonly buckets: OomBucketSummaryRow[];
  readonly intervals: AppStateIntervalRow[];
  readonly oomSeries: OomMemSeriesRow[];
  readonly events: AppEventRow[];
}

export async function loadAppsBundle(
  engine: Engine,
  w?: WindowArgs,
  maxLanes = 18,
): Promise<AppsBundle> {
  const args = wArgs(w);
  const [topRes, buckRes, ivRes, oomSerRes, evRes] = await Promise.all([
    qMod(engine, 'apps', `sysmon_top_app(${args})`),
    qMod(engine, 'apps', `sysmon_oom_bucket_summary(${args})`),
    qMod(engine, 'apps', `sysmon_app_state_intervals(${args}, ${maxLanes})`),
    qMod(engine, 'apps', `sysmon_oom_memory_series(${args})`),
    qMod(engine, 'apps', `sysmon_app_events(${args}, 30)`),
  ]);

  return {
    topApp: mapFirst(
      topRes,
      {
        top_app: STR_NULL,
        top_upid: NUM_NULL,
        top_dur_s: NUM,
        source: STR,
        fg_count: NUM,
        vis_count: NUM,
        perc_count: NUM,
        svc_count: NUM,
        cached_count: NUM,
        native_count: NUM,
        lmkd_kills: NUM,
      },
      (it) => ({
        topApp: it.top_app,
        topUpid: it.top_upid,
        topDurS: it.top_dur_s,
        source: it.source,
        fgCount: it.fg_count,
        visCount: it.vis_count,
        percCount: it.perc_count,
        svcCount: it.svc_count,
        cachedCount: it.cached_count,
        nativeCount: it.native_count,
        lmkdKills: it.lmkd_kills,
      }),
    ),
    buckets: mapRows(
      buckRes,
      {
        bucket_idx: NUM,
        bucket_name: STR,
        proc_count: NUM,
        anon_swap_mib: NUM,
        file_mib: NUM,
        dmabuf_mib: NUM,
        cpu_pct: NUM,
      },
      (it) => ({
        bucketIdx: it.bucket_idx,
        bucketName: it.bucket_name,
        procCount: it.proc_count,
        anonSwapMib: it.anon_swap_mib,
        fileMib: it.file_mib,
        dmabufMib: it.dmabuf_mib,
        cpuPct: it.cpu_pct,
      }),
    ),
    intervals: mapRows(
      ivRes,
      {
        upid: NUM,
        pid: NUM,
        process_name: STR,
        bucket_idx: NUM,
        bucket_name: STR,
        oom_score_adj: NUM,
        start_rel_s: NUM,
        end_rel_s: NUM,
        changed: NUM,
        is_top: NUM,
        killed: NUM,
        frozen: NUM,
      },
      (it) => ({
        upid: it.upid,
        pid: it.pid,
        processName: it.process_name,
        bucketIdx: it.bucket_idx,
        bucketName: it.bucket_name,
        oomScoreAdj: it.oom_score_adj,
        startRelS: it.start_rel_s,
        endRelS: it.end_rel_s,
        changed: it.changed === 1,
        isTop: it.is_top === 1,
        killed: it.killed === 1,
        frozen: it.frozen === 1,
      }),
    ),
    oomSeries: mapRows(
      oomSerRes,
      {
        rel_s: NUM,
        bucket_idx: NUM,
        bucket_name: STR,
        anon_swap_mib: NUM,
        file_mib: NUM,
        dmabuf_mib: NUM,
      },
      (it) => ({
        relS: it.rel_s,
        bucketIdx: it.bucket_idx,
        bucketName: it.bucket_name,
        anonSwapMib: it.anon_swap_mib,
        fileMib: it.file_mib,
        dmabufMib: it.dmabuf_mib,
      }),
    ),
    events: mapRows(
      evRes,
      {
        rel_s: NUM,
        kind: STR,
        severity: STR,
        process_name: STR,
        title: STR,
        detail: STR,
        target_tab: STR,
      },
      (it) => ({
        relS: it.rel_s,
        kind: it.kind,
        severity: it.severity,
        processName: it.process_name,
        title: it.title,
        detail: it.detail,
        targetTab: it.target_tab,
      }),
    ),
  };
}

export async function loadOomBucketDrilldown(
  engine: Engine,
  bucketIdx: number,
  w?: WindowArgs,
  topN = 8,
): Promise<OomBucketProcSeriesRow[]> {
  const res = await qMod(
    engine,
    'apps',
    `sysmon_oom_bucket_proc_series(${wArgs(w)}, ${bucketIdx}, ${topN})`,
  );
  return mapRows(
    res,
    {rel_s: NUM, process_name: STR, anon_swap_mib: NUM, file_mib: NUM, dmabuf_mib: NUM},
    (it) => ({
      relS: it.rel_s,
      processName: it.process_name,
      anonSwapMib: it.anon_swap_mib,
      fileMib: it.file_mib,
      dmabufMib: it.dmabuf_mib,
    }),
  );
}

export interface ProcDetailSeriesRow {
  readonly relS: number;
  readonly cpuPct: number;
  readonly anonMib: number | null;
  readonly swapMib: number | null;
  readonly fileMib: number | null;
  readonly shmemMib: number | null;
  readonly dmabufMib: number | null;
  readonly oomScoreAdj: number | null;
}

export interface ProcThreadRow {
  readonly utid: number;
  readonly tid: number;
  readonly threadName: string;
  readonly runningMs: number;
  readonly runnableMs: number;
  readonly ioWaitMs: number;
  readonly dOtherMs: number;
  readonly totalActiveMs: number;
  readonly cpuDelayRatio: number;
  readonly topBlockedFn: string | null;
}

export interface RunnableHistRow {
  readonly bucketOrder: number;
  readonly bucketLabel: string;
  readonly cnt: number;
}

export interface ProcessDetailBundle {
  readonly series: ProcDetailSeriesRow[];
  readonly threads: ProcThreadRow[];
  readonly runnableHist: RunnableHistRow[];
}

export async function loadProcessDetailBundle(
  engine: Engine,
  upid: number,
  w?: WindowArgs,
  includeThreads = true,
): Promise<ProcessDetailBundle> {
  const args = wArgs(w);
  const [serRes, thrRes, histRes] = await Promise.all([
    qMod(engine, 'processes', `sysmon_process_detail_series(${upid}, ${args})`),
    includeThreads
      ? qMod(engine, 'processes', `sysmon_process_threads(${upid}, ${args}, 25)`)
      : undefined,
    includeThreads
      ? qMod(engine, 'processes', `sysmon_process_runnable_hist(${upid}, ${args})`)
      : undefined,
  ]);

  return {
    series: mapRows(
      serRes,
      {
        rel_s: NUM,
        cpu_pct: NUM,
        anon_mib: NUM_NULL,
        swap_mib: NUM_NULL,
        file_mib: NUM_NULL,
        shmem_mib: NUM_NULL,
        dmabuf_mib: NUM_NULL,
        oom_score_adj: NUM_NULL,
      },
      (it) => ({
        relS: it.rel_s,
        cpuPct: it.cpu_pct,
        anonMib: it.anon_mib,
        swapMib: it.swap_mib,
        fileMib: it.file_mib,
        shmemMib: it.shmem_mib,
        dmabufMib: it.dmabuf_mib,
        oomScoreAdj: it.oom_score_adj,
      }),
    ),
    threads: mapRows(
      thrRes,
      {
        utid: NUM,
        tid: NUM,
        thread_name: STR,
        running_ms: NUM,
        runnable_ms: NUM,
        io_wait_ms: NUM,
        d_other_ms: NUM,
        total_active_ms: NUM,
        cpu_delay_ratio: NUM,
        top_blocked_fn: STR_NULL,
      },
      (it) => ({
        utid: it.utid,
        tid: it.tid,
        threadName: it.thread_name,
        runningMs: it.running_ms,
        runnableMs: it.runnable_ms,
        ioWaitMs: it.io_wait_ms,
        dOtherMs: it.d_other_ms,
        totalActiveMs: it.total_active_ms,
        cpuDelayRatio: it.cpu_delay_ratio,
        topBlockedFn: it.top_blocked_fn,
      }),
    ),
    runnableHist: mapRows(
      histRes,
      {bucket_order: NUM, bucket_label: STR, cnt: NUM},
      (it) => ({
        bucketOrder: it.bucket_order,
        bucketLabel: it.bucket_label,
        cnt: it.cnt,
      }),
    ),
  };
}
