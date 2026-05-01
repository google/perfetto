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

import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import m from 'mithril';
import {NUM, STR} from '../../trace_processor/query_result';
import {exists, getOrCreate} from '../../base/utils';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {TrackNode} from '../../public/workspace';
import {COUNTER_TRACK_KIND} from '../../public/track_kinds';
import {sqlNameSafe} from '../../base/string_utils';
import {CounterTrack} from '../../components/tracks/counter_track';
import {uuidv4} from '../../base/uuid';
import {CpuPage} from './cpu_page';
import {
  ARM_TELEMETRY_CPU_SPEC_SCHEMA,
  ArmTelemetryCpuSpec,
} from './arm_telemetry_spec';
import {z} from 'zod';
import {ArmTelemetrySpecManager} from './arm_telemetry_spec_manager';
import {ArmTelemetrySpecManagerImpl} from './arm_telemetry_spec_manager_impl';

type ApplicableMetricDesc = {
  name: string;
  cpus: number[];
  events: string[];
  formula: string;
};

type TraceLoadState = {
  disposed: boolean;
};

const ARM_TELEMETRY_CPU_SPECS_SCHEMA = z.array(ARM_TELEMETRY_CPU_SPEC_SCHEMA);
const SME2_SPEC_CPUID = '0x41d8d';

function eventTableName(eventName: string): string {
  return `arm_telemetry_event_${sqlNameSafe(eventName)}`;
}

function metricTableName(metricName: string): string {
  return `arm_telemetry_metric_${sqlNameSafe(metricName)}`;
}

export default class ArmTelemetryPlugin implements PerfettoPlugin {
  static readonly id = 'com.arm.ArmTelemetry';
  static readonly description = `
    Computes Arm CPU telemetry metrics from PMU counters in the loaded trace.
    CPU specification files define the available events, metrics, and formulas
    used to create per-CPU metric tracks.
    Use the Arm CPU page to load and manage CPU specification files.
  `;
  static readonly dependencies = [StandardGroupsPlugin];
  private static specManager: ArmTelemetrySpecManager;

  static onActivate(app: App): void {
    const cpuSpecsSetting = app.settings.register<ArmTelemetryCpuSpec[]>({
      id: `${ArmTelemetryPlugin.id}.cpuSpecs`,
      name: 'Arm telemetry CPU specs',
      description: `CPU telemetry specifications used to compute Arm telemetry metrics.
        To edit, go to SUPPORT -> Arm cpu page.`,
      schema: ARM_TELEMETRY_CPU_SPECS_SCHEMA,
      defaultValue: [],
      requiresReload: true,
    });

    ArmTelemetryPlugin.specManager = new ArmTelemetrySpecManagerImpl(
      cpuSpecsSetting,
    );

    app.pages.registerPage({
      route: '/arm_cpu',
      render: () =>
        m(CpuPage, {
          specManager: ArmTelemetryPlugin.specManager,
        }),
    });
    app.sidebar.addMenuItem({
      section: 'support',
      text: 'Arm cpu',
      href: '#!/arm_cpu',
      icon: 'memory',
    });
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    const state: TraceLoadState = {disposed: false};
    trace.trash.defer(() => {
      state.disposed = true;
    });

    if (!(await this.checkRequirements(trace))) {
      return;
    }

    if (state.disposed) {
      return;
    }

    // Fetch the perf counters to process them in a way that suits
    // metric computation
    const countersPerCpu = await this.getPerfCountersNamePerCpu(trace);
    if (state.disposed) {
      return;
    }

    // Retrieve the CPUID of each CPU
    const cpuidMap = await this.getCpuIdMap(trace);
    if (state.disposed) {
      return;
    }

    // Create activity views required by the counters and metrics
    await this.createCpuActivityViews(trace);
    if (state.disposed) {
      return;
    }

    // Create all the counters view and tables
    const counters = [...new Set([...countersPerCpu.values()].flat())];
    for (const c of counters) {
      await this.createPerfCounterView(trace, c);
      if (state.disposed) {
        return;
      }
    }

    // Update the metric tracks if possible
    await this.updateMetricTracks(trace, state, cpuidMap, countersPerCpu);
  }

  // Validate that all the requirements are met to execute the plugin.
  // - At least one CPU specification has been uploaded
  // - Existence meta data asociated with the CPU table
  private async checkRequirements(ctx: Trace): Promise<boolean> {
    if (!ArmTelemetryPlugin.specManager.hasSpecs()) {
      return false;
    }

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
  private async getPerfCountersNamePerCpu(
    trace: Trace,
  ): Promise<Map<number, string[]>> {
    const q = await trace.engine.query(
      'SELECT name, cpu FROM perf_counter_track',
    );
    const counters: Map<number, string[]> = new Map();
    const it = q.iter({name: STR, cpu: NUM});
    for (; it.valid(); it.next()) {
      getOrCreate(counters, it.cpu, () => []).push(it.name);
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
  private async createCpuActivityViews(trace: Trace) {
    await trace.engine.query(`
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

    await trace.engine.query(`
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
  // The views is named after the arm_telemetry_event_{counterName}
  //  and contains the following fields:
  // - ts: The timestamp of the sample
  // - cpu: The cpu of the sample
  // - {counter}_counter: The accumulated value of the counter
  // - {counter}: The delta between the counter and the one from the next sample
  // - dur: The duration of the sample
  // The view is also SPAN_JOIN using the cpu_busy_slice table.
  // As a result, no sample happens while the CPU is idle.
  private async createPerfCounterView(trace: Trace, counterName: string) {
    const tableName = eventTableName(counterName);

    await trace.engine.query(`
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
  // This function is called once when the trace is loaded. Specs are expected
  // to be registered before trace load; changing specs requires a trace reload.
  private async updateMetricTracks(
    trace: Trace,
    state: TraceLoadState,
    cpuidMap: Map<number, string>,
    countersPerCpu: Map<number, string[]>,
  ): Promise<void> {
    const metrics = this.getComputableMetrics(cpuidMap, countersPerCpu);
    await this.createMetrics(trace, state, metrics, [...cpuidMap.keys()]);
    if (state.disposed) {
      return;
    }

    // Retrieve the hardware node in the workspace
    const hardwareGroup = trace.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(trace.workspaces.currentWorkspace, 'HARDWARE');

    // Create and attach the main telemetry node
    const telemetryRootNode = new TrackNode({name: 'CPU metrics'});
    hardwareGroup.addChildInOrder(telemetryRootNode);

    trace.trash.defer(() => {
      hardwareGroup.removeChild(telemetryRootNode);
    });

    // Iterate over all the metrics to create tracks.
    for (const [metricName, desc] of Object.entries(metrics)) {
      // retrieve CPU this metric applies to
      const cpus = desc
        .map((e) => e.cpus)
        .flat()
        .sort();

      for (const c of cpus) {
        const tableName = metricTableName(metricName);
        // Note: It is not possible to deregister track explicitly.
        // For this reason it is important to have a UID in the URI.
        const uri = `/arm_telemetry/${metricName}_${c}#${uuidv4()}`;
        const track = new CounterTrack({
          trace,
          uri,
          sqlSource: `select * from ${tableName} where cpu = ${c}`,
        });
        trace.tracks.registerTrack({
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
  }

  private getComputableMetrics(
    cpuids: Map<number, string>,
    cpuCountersMap: Map<number, string[]>,
  ) {
    const telemetrySpecManager = ArmTelemetryPlugin.specManager;

    // Align cpuids to process the counter available
    cpuids = new Map<number, string>(
      [...cpuids.entries()].filter(([k, _]) => cpuCountersMap.has(k)),
    );

    // Resolve the generic/co-processor spec if registered
    const registered = telemetrySpecManager.registeredCpuids();
    const genericSpec = registered.includes(SME2_SPEC_CPUID)
      ? telemetrySpecManager.getCpuDesc(SME2_SPEC_CPUID)
      : undefined;

    // Build the list of specs per CPU: CPUID-specific + generic (if present)
    const cpuSpecs = new Map<number, ArmTelemetryCpuSpec[]>();
    for (const [cpu, cpuid] of cpuids.entries()) {
      const specs: ArmTelemetryCpuSpec[] = [];
      if (registered.includes(cpuid)) {
        specs.push(telemetrySpecManager.getCpuDesc(cpuid));
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
  private async getCpuIdMap(trace: Trace): Promise<Map<number, string>> {
    const result = new Map<number, string>();

    const q = await trace.engine.query(`
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

  // Compute a given metric from its formula. The metric is created as a
  // table in the database. The name of the table arm_telemetry_metric_{metricName}.
  // The table contains the following fields:
  //   - ts: timestamp of the sample
  //   - cpu: cpu of the sample
  //   - dur: duration of the sampling
  //   - value: value computed from the metric formula
  //   - utid: Thread associated with the sample.
  private async createMetrics(
    trace: Trace,
    state: TraceLoadState,
    metricsPerName: {[name: string]: ApplicableMetricDesc[]},
    applicableCpus: number[],
  ) {
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
      if (state.disposed) {
        return;
      }

      const tableName = metricTableName(metricName);

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
        const firstTable = eventTableName(metric.events[0]);
        const fromClause = metric.events.slice(1).reduce((acc, counter) => {
          const counterTable = eventTableName(counter);
          return (
            acc +
            ` LEFT JOIN ${counterTable}` +
            ` ON ${firstTable}.ROWID = ${counterTable}.ROWID`
          );
        }, firstTable);

        await trace.engine.query(`
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
        await trace.engine.query(`
          DROP TABLE IF EXISTS ${tableName};
          CREATE TABLE ${tableName} AS
          ${metrics.reduce<string>((acc, metric) => {
            const firstTable = eventTableName(metric.events[0]);
            const fromClause = metric.events.slice(1).reduce((acc, counter) => {
              const counterTable = eventTableName(counter);
              return (
                acc +
                ` LEFT JOIN ${counterTable}` +
                ` ON ${firstTable}.ROWID = ${counterTable}.ROWID`
              );
            }, firstTable);
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

      if (state.disposed) {
        return;
      }
    }
  }
}
