// // Copyright (C) 2025 The Android Open Source Project
// //
// // Licensed under the Apache License, Version 2.0 (the "License");
// // you may not use this file except in compliance with the License.
// // You may obtain a copy of the License at
// //
// //      http://www.apache.org/licenses/LICENSE-2.0
// //
// // Unless required by applicable law or agreed to in writing, software
// // distributed under the License is distributed on an "AS IS" BASIS,
// // WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// // See the License for the specific language governing permissions and
// // limitations under the License.

import {PerfettoPlugin} from '../../public/plugin';

import {App} from '../../public/app';
import {Trace} from '../../public/trace';
import {NUM, STR} from '../../trace_processor/query_result';
import {exists} from '../../base/utils';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {TrackNode} from '../../public/workspace';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {sqlNameSafe} from '../../base/string_utils';
import {AsyncDisposableStack} from '../../base/disposable_stack';
import {
  BaseCounterTrack,
  CounterOptions,
} from '../../components/tracks/base_counter_track';
import {uuidv4} from '../../base/uuid';
import {ArmTelemetryCpuSpec} from '../../public/cpu_info';

type ApplicableMetricDesc = {
  name: string;
  cpus: number[];
  events: string[];
  formula: string;
};

const SME2_SPEC_CPUID = '0x41d8d';

class MetricCounterTrack extends BaseCounterTrack {
  constructor(
    trace: Trace,
    uri: string,
    private readonly metricName: string,
    private readonly cpu: number,
    options?: Partial<CounterOptions>,
  ) {
    super(trace, uri, options);
  }

  getSqlSource(): string {
    return `select * from ${this.metricName} where cpu = ${this.cpu}`;
  }
}

export default class implements PerfettoPlugin {
  static readonly id = 'com.arm.telemetry';
  static readonly dependencies = [StandardGroupsPlugin];

  private _ctx?: Trace;
  private _countersPerCpu?: Map<number, string[]>;
  private _cpuidMap?: Map<number, string>;
  private _updating: boolean = false;
  private _requireUpdate: boolean = false;
  private _metricsThrash?: AsyncDisposableStack;

  static onActivate(_app: App) {}

  async onTraceLoad(ctx: Trace): Promise<void> {
    if (!(await this.checkRequirements(ctx))) {
      return;
    }

    this._ctx = ctx;
    this._metricsThrash = new AsyncDisposableStack();

    this._ctx.trash.use(
      ctx.cpuInfos.addOnChangeCallback(
        (_change: 'ADD' | 'REMOVE' | 'UPDATE', _desc: ArmTelemetryCpuSpec) => {
          setTimeout(() => this.updateMetricTracks(), 0);
        },
      ),
    );

    // Fetch the perf counters to process them in a way that suits
    // metric computation
    this._countersPerCpu = await this.getPerfCountersNamePerCpu();

    // Retrieve the CPUID of each CPU
    this._cpuidMap = await this.getCpuIdMap();

    // Create activity views required by the counters and metrics
    await this.createCpuActivityViews();

    // Create all the counters view and tables
    const counters = [...new Set([...this._countersPerCpu.values()].flat())];
    for (const c of counters) {
      await this.createPerfCounterView(c);
    }

    // Update the metric tracks if possible
    await this.updateMetricTracks();

    ctx.trash.defer(() => {
      this._ctx = undefined;
      this._countersPerCpu = undefined;
      this._cpuidMap = undefined;
      this._updating = false;
      this._requireUpdate = false;
      this._metricsThrash = undefined;
    });
  }

  // Validate that all the requirements are met to execute the plugin.
  // - Existence meta data asociated with the CPU table
  private async checkRequirements(ctx: Trace): Promise<boolean> {
    // We need a CPU table to retrieve metrics and operate
    const q = await ctx.engine.query(`
      SELECT
        cpu,
        EXTRACT_ARG(ARG_SET_ID, 'arm_cpu_implementer') as implementer,
        EXTRACT_ARG(ARG_SET_ID, 'arm_cpu_part') as part
      FROM cpu
      WHERE implementer IS NOT NULL and part IS NOT NULL
    `);
    return q.numRows() !== 0;
  }

  // Return a map describing the perf counters associated with their CPU.
  private async getPerfCountersNamePerCpu(): Promise<Map<number, string[]>> {
    if (!this._ctx) {
      throw new Error(
        'Impossible to retrieve perf counters if the trace is not loaded',
      );
    }

    const q = await this._ctx.engine.query(
      'SELECT name, cpu FROM perf_counter_track',
    );
    const counters: Map<number, string[]> = new Map();
    const it = q.iter({name: STR, cpu: NUM});
    for (let row = 0; it.valid(); row++, it.next()) {
      if (counters.has(it.cpu)) {
        counters.get(it.cpu)!.push(it.name);
      } else {
        counters.set(it.cpu, [it.name]);
      }
    }

    return counters;
  }

  // Create views of the CPU activity (busy or idle).
  // - cpu_busy_slice: Slices when the CPU is busy
  //    - ts: timestamp marking the begining of the slice
  //    - cpu: cpu of the slice
  //    - dur: duration of the slice
  //    - utid: Thread identifier associated with the slice.
  // - cpu_idle_slice: Slices when the CPU is idle
  //    - ts: timestamp marking the begining of the slice
  //    - cpu: cpu of the slice
  //    - dur: duration of the slice
  //    - utid: Thread identifier associated with the slice.
  async createCpuActivityViews() {
    if (!this._ctx) {
      throw new Error(
        'Impossible to create CPU activity views if the trace is not loaded',
      );
    }

    await this._ctx.engine.query(`
        DROP TABLE IF EXISTS cpu_busy_slice;
        CREATE PERFETTO TABLE cpu_busy_slice AS
        WITH whole_counter_slice(ts, dur) AS
          (VALUES ((SELECT ts FROM counter ORDER BY ts LIMIT 1),
            (SELECT ts FROM counter ORDER BY ts DESC LIMIT 1) - (SELECT ts FROM counter ORDER BY ts LIMIT 1))
          )
        SELECT cpu,ts,dur,utid
        FROM sched_slice
        WHERE utid != 0
        UNION
        -- Add placeholder if the sched_slice is not available
        SELECT DISTINCT cpu, ts, dur, NULL AS utid
        FROM perf_counter_track
          JOIN whole_counter_slice
        WHERE NOT EXISTS (SELECT * FROM sched_slice)
        ORDER BY cpu,ts;
      `);

    await this._ctx.engine.query(`
            DROP TABLE IF EXISTS cpu_idle_slice;
            CREATE PERFETTO TABLE cpu_idle_slice AS
            SELECT
              ts,
              cpu,
              0 as value,
              dur,
              utid
            FROM sched_slice
            WHERE utid = 0
            ORDER BY cpu,ts;
          `);
  }

  // Create the view in the DB of the counter named in input.
  // The views is named after the counter and contains the following fields:
  // - ts: The timestamp of the sample
  // - cpu: The cpu of the sample
  // - {counter}_counter: The accumulated value of the counter
  // - {counter}: The delta between the counter and the one from the next sample
  // - dur: The duration of the sample
  // The view is also SPAN_JOIN using the cpu_busy_slice table.
  // As a result, no sample happens while the CPU is idle.
  private async createPerfCounterView(counterName: string) {
    if (!this._ctx) {
      throw new Error(
        'Impossible to create Perf counter views if the trace is not loaded',
      );
    }

    const tableName = sqlNameSafe(counterName);

    await this._ctx.engine.query(`
        DROP VIEW IF EXISTS __raw_${tableName};
        CREATE VIEW __raw_${tableName} AS
        SELECT
          ts,
          cpu,
          value AS ${counterName}_counter,
          LEAD(value, 1, value) OVER (PARTITION BY cpu ORDER BY ts) - value AS ${counterName},
          LEAD(ts, 1, ts ) OVER (PARTITION BY cpu ORDER BY ts ) - ts AS dur
        FROM counter
          JOIN perf_counter_track
            ON perf_counter_track.id = counter.track_id
        WHERE
          name like '${counterName}';

        DROP TABLE IF EXISTS __virtual_${tableName};
        CREATE VIRTUAL TABLE __virtual_${tableName} USING SPAN_JOIN(
          __raw_${tableName} PARTITIONED cpu,
          cpu_busy_slice PARTITIONED cpu
        );

        DROP TABLE IF EXISTS ${tableName};
        CREATE TABLE ${tableName} AS
        SELECT *
        FROM __virtual_${tableName}
        ORDER BY cpu,ts;
      `);
  }

  // Update metric tracks.
  //
  // This function is called when the trace is loaded
  // and whenever the cpu info change.
  //
  // We validate between steps if the trace is still loaded and
  // whether an update is pendng or not.
  private async updateMetricTracks(): Promise<void> {
    // sanity check
    if (!this._ctx) {
      return;
    }

    if (this._updating) {
      this._requireUpdate = true;
      return;
    }

    // Restart the process and cleanup the resources
    const restartUpdate = () => {
      this._updating = false;
      if (this._requireUpdate) {
        // When we bail out we should remove the thrash
        setTimeout(() => this.updateMetricTracks(), 0);
      }
    };

    // Capture the context localy in case a new trace is loaded while an
    // async operation is pending
    const ctx = this._ctx;
    const canProgress = () => ctx === this._ctx && !this._requireUpdate;

    this._updating = true;
    this._requireUpdate = false;

    // Remove previous thrash if any left.
    // Doing it here as it must be done asynchronously
    if (!this._metricsThrash!.disposed) {
      await this._metricsThrash!.asyncDispose();
    }

    if (!canProgress()) {
      restartUpdate();
      return;
    }

    const metrics = this.getComputableMetrics(
      this._cpuidMap!,
      this._countersPerCpu!,
    );
    await this.createMetrics(metrics, [...this._cpuidMap!.keys()]);

    if (!canProgress()) {
      restartUpdate();
      return;
    }

    // Retrieve the hardware node in the workspace
    const hardwareGroup = ctx.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(ctx.workspace, 'HARDWARE');

    // Create and attach the main telemetry node
    const telemetryRootNode = new TrackNode({name: 'CPU metrics'});
    hardwareGroup.addChildInOrder(telemetryRootNode);

    this._metricsThrash!.defer(async () => {
      if (ctx === this._ctx) {
        hardwareGroup.removeChild(telemetryRootNode);
      }
    });

    // Iterate over all the metrics to create tracks.
    for (const [metricName, desc] of Object.entries(metrics)) {
      // retrieve CPU this metric applies to
      const cpus = desc
        .map((e) => e.cpus)
        .flat()
        .sort();

      for (const c of cpus) {
        // Note: It is not possible to deregister track explicitely in case of
        // update. For this reason it is important to have a UID in the URI.
        // However the existing nodes are removed from the workspace in case of
        // update.
        const uri = `/arm_telemetry/${metricName}_${c}#${uuidv4()}`;
        const track = new MetricCounterTrack(this._ctx, uri, metricName, c);
        ctx.tracks.registerTrack({
          uri,
          tags: {
            kind: COUNTER_TRACK_KIND,
            cpu: c,
            groupName: `arm_telemetry/${metricName}`,
          },
          renderer: track,
        });

        telemetryRootNode.addChildInOrder(
          new TrackNode({uri, name: `${metricName} for CPU ${c}`}),
        );
      }
    }

    // Update flags and restart the update if required
    this._updating = false;
    if (this._requireUpdate) {
      setTimeout(() => this.updateMetricTracks(), 0);
    }
  }

  private getComputableMetrics(
    cpuids: Map<number, string>,
    cpuCountersMap: Map<number, string[]>,
  ) {
    // Align cpuids to process the counter available
    cpuids = new Map<number, string>(
      [...cpuids.entries()].filter(([k, _]) => cpuCountersMap.has(k)),
    );

    // Resolve the generic/co-processor spec if registered
    const registered = this._ctx!.cpuInfos.registeredCpuids();
    const genericSpec = registered.includes(SME2_SPEC_CPUID)
      ? this._ctx!.cpuInfos.getCpuDesc(SME2_SPEC_CPUID)
      : undefined;

    // Build the list of specs per CPU: CPUID-specific + generic (if present)
    const cpuSpecs = new Map<number, ArmTelemetryCpuSpec[]>();
    for (const [cpu, cpuid] of cpuids.entries()) {
      const specs: ArmTelemetryCpuSpec[] = [];
      if (registered.includes(cpuid)) {
        specs.push(this._ctx!.cpuInfos.getCpuDesc(cpuid));
      }
      if (genericSpec) {
        specs.push(genericSpec);
      }
      if (specs.length > 0) {
        cpuSpecs.set(cpu, specs);
      }
    }

    // For each cpu find applicable metrics based on its available counters
    const allApplicableMetrics: ApplicableMetricDesc[] = [];
    for (const [cpu, specs] of cpuSpecs.entries()) {
      const counters = cpuCountersMap.get(cpu)!;
      for (const spec of specs) {
        for (const [name, metric] of Object.entries(spec.metrics)) {
          if (metric.events.every((e) => counters.includes(e))) {
            allApplicableMetrics.push({
              name,
              cpus: [cpu],
              formula: metric.formula,
              events: metric.events,
            });
          }
        }
      }
    }

    // Merge metrics together
    const applicableMetrics: {[name: string]: ApplicableMetricDesc[]} = {};
    allApplicableMetrics.forEach((m) => {
      const name = m.name;
      const entry = applicableMetrics[name];
      if (exists(entry)) {
        const found = entry.find((candidate) => {
          return (
            m.formula == candidate.formula &&
            m.events.every((e) => candidate.events.includes(e))
          );
        });

        if (exists(found)) {
          const cpu = m.cpus[0];
          if (!found.cpus.includes(cpu)) {
            found.cpus.push(cpu);
          }
        } else {
          entry.push(m);
        }
      } else {
        applicableMetrics[name] = [m];
      }
    });

    return applicableMetrics;
  }

  // Retrieve the cpuid for each CPU
  async getCpuIdMap(): Promise<Map<number, string>> {
    const result = new Map<number, string>();

    const q = await this._ctx!.engine.query(`
      SELECT
        cpu,
        EXTRACT_ARG(ARG_SET_ID, "arm_cpu_implementer") as implementer,
        EXTRACT_ARG(ARG_SET_ID, "arm_cpu_part") as part
      FROM cpu
      WHERE implementer IS NOT NULL and part IS NOT NULL
    `);

    const iter = q.iter({
      cpu: NUM,
      implementer: NUM,
      part: NUM,
    });
    for (; iter.valid(); iter.next()) {
      const cpuid = `0x${iter.implementer.toString(16)}${iter.part.toString(16)}`;
      result.set(iter.cpu, cpuid);
    }

    return result;
  }

  // Compute a given metric from its formula. The metric is created as a table
  // in the database. The name of the table is the name of the metric.
  // The table contains the following fields:
  //   - ts: timestamp of the sample
  //   - cpu: cpu of the sample
  //   - dur: duration of the sampling
  //   - value: value computed from the metric formula
  //   - utid: Thread associated with the sample.
  async createMetrics(
    metricsPerName: {[name: string]: ApplicableMetricDesc[]},
    applicableCpus: number[],
  ) {
    const ctx = this._ctx!;

    const idleSegment = `
      SELECT
        ts,
        cpu,
        dur,
        0 AS value,
        0 AS utid
      FROM cpu_idle_slice
    `;

    for (const [metricName, metrics] of Object.entries(metricsPerName)) {
      const tableName = sqlNameSafe(metricName);

      // Validate that we can continue or should continue the process
      if (ctx !== this._ctx || this._requireUpdate) {
        return;
      }

      // Implementation notes:
      // - If all the CPUs share the same metric definition, just select the
      //   various counters of the metric then use the formula to compute the metric.
      // - If not all the CPUs share the same metric definition then operate at that
      //   metric definition level for each definition and do a UNION.
      // - All the metrics are unioned with the cpu_idle_slice to let the metric drop
      //   to 0 when the CPU is idle.

      // Optimize query if it is applicable to all the CPUS
      if (
        metrics.length == 1 &&
        metrics[0].cpus.length == applicableCpus.length &&
        metrics[0].cpus.every((c) => applicableCpus.includes(c))
      ) {
        const metric = metrics[0];
        const firstTable = metric.events[0];
        const fromClause = metric.events
          .slice(1)
          .reduce(
            (acc, counter) =>
              acc +
              ` LEFT JOIN ${counter} ON ${firstTable}.ROWID = ${counter}.ROWID`,
            `${metric.events[0]}`,
          );

        await ctx.engine.query(`
            DROP TABLE IF EXISTS ${tableName};
            CREATE TABLE ${tableName} AS
            SELECT
              ${firstTable}.ts AS ts,
              ${firstTable}.cpu AS cpu,
              ${firstTable}.dur AS dur,
              ifnull(${metric.formula},0) AS value,
              ${firstTable}.utid AS utid
            FROM ${fromClause}
            UNION ${idleSegment}
            ORDER BY ts;
          `);
      } else {
        await ctx.engine.query(`
          DROP TABLE IF EXISTS ${tableName};
          CREATE TABLE ${tableName} AS
          ${metrics.reduce<string>((acc, metric) => {
            const firstTable = metric.events[0];
            const fromClause = metric.events
              .slice(1)
              .reduce(
                (acc, counter) =>
                  acc +
                  ` LEFT JOIN ${counter} ON ${firstTable}.ROWID = ${counter}.ROWID`,
                `${metric.events[0]}`,
              );
            const whereClause = metric.cpus
              .slice(1)
              .reduce(
                (acc, cpu) => acc + ` OR ${firstTable}.cpu = ${cpu}`,
                `${firstTable}.cpu = ${metric.cpus[0]}`,
              );
            return (
              acc +
              `
              SELECT
                ${firstTable}.ts AS ts,
                ${firstTable}.cpu AS cpu,
                ${firstTable}.dur AS dur,
                ifnull(${metric.formula},0) AS value,
                ${firstTable}.utid AS utid
              FROM ${fromClause}
              WHERE ${whereClause}
              UNION`
            );
          }, '')}
          ${idleSegment}
          ORDER BY ts;
        `);
      }

      // Register cleanup handlers if the context has not changed
      if (ctx === this._ctx) {
        this._metricsThrash!.defer(async () => {
          // only removing the table if a new trace hasn't been loaded
          if (ctx === this._ctx) {
            await ctx.engine.query(`DROP TABLE IF EXISTS ${tableName};`);
          }
        });
      }
    }
  }
}
