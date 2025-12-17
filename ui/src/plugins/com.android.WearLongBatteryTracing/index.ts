// Copyright (C) 2025 The Android Open Source Project
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

import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {TrackNode} from '../../public/workspace';
import {SliceTrack} from '../../components/tracks/slice_track';
import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {Engine} from '../../trace_processor/engine';
import {STR, LONG, LONG_NULL} from '../../trace_processor/query_result';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';

const VALID_SUBSCRIPTION_IDS = [
  -3150162163527618090n,
  -5078517658656926822n,
  -897258217068735442n,
  -4914190112721760517n,
];

export default class WearLongBatteryTracingPlugin implements PerfettoPlugin {
  static readonly id = 'com.android.WearLongBatteryTracing';
  static readonly dependencies = [StandardGroupsPlugin];

  private async findAtoms(e: Engine): Promise<Set<string>> {
    const atoms = new Set<string>();

    const result = await e.query(`
      select distinct s.name as atom
      from track t join slice s on t.id = s.track_id
      where t.name = 'Statsd Atoms'`);
    const it = result.iter({atom: STR});
    for (; it.valid(); it.next()) {
      atoms.add(it.atom);
    }

    return atoms;
  }

  async addCounterTrack(
    ctx: Trace,
    name: string,
    query: string,
    group: TrackNode,
  ) {
    const uri = `/wear_long_battery_tracing_${name}`;
    const track = await createQueryCounterTrack({
      trace: ctx,
      uri,
      data: {
        sqlSource: query,
        columns: ['ts', 'value'],
      },
    });
    ctx.tracks.registerTrack({
      uri,
      renderer: track,
    });
    const trackNode = new TrackNode({uri, name});
    group.addChildInOrder(trackNode);
  }

  async addSliceTrack(
    ctx: Trace,
    name: string,
    query: string,
    group: TrackNode,
  ) {
    const uri = `/wear_long_battery_tracing_${name}`;
    const track = await SliceTrack.createMaterialized({
      trace: ctx,
      uri,
      dataset: new SourceDataset({
        src: query,
        schema: {
          ts: LONG,
          dur: LONG_NULL,
          name: STR,
        },
      }),
    });
    ctx.tracks.registerTrack({
      uri,
      renderer: track,
    });
    const trackNode = new TrackNode({uri, name});
    group.addChildInOrder(trackNode);
  }

  async addModes(
    ctx: Trace,
    atoms: Set<string>,
    group: TrackNode,
  ): Promise<void> {
    if (!atoms.has('wear_mode_state_changed')) {
      return;
    }

    const modesGroup = new TrackNode({
      name: 'Wear Modes',
      isSummary: true,
    });
    group.addChildInOrder(modesGroup);

    const e = ctx.engine;
    await e.query(
      `INCLUDE PERFETTO MODULE
          google3.wireless.android.wear.tools.pipelines.perfetto.cujs.modes`,
    );
    const slices = await e.query(
      `SELECT DISTINCT mode_id AS track_name FROM wear_mode_slices`,
    );

    const slicesIt = slices.iter({track_name: STR});
    for (; slicesIt.valid(); slicesIt.next()) {
      await this.addSliceTrack(
        ctx,
        slicesIt.track_name,
        `SELECT
          ts,
          dur,
          mode_state AS name
        FROM wear_mode_slices
        WHERE mode_id = '${slicesIt.track_name}' AND mode_state != 'UNKNOWN'
        `,
        modesGroup,
      );
    }
  }

  async addRawBatteryGauge(ctx: Trace, atoms: Set<string>): Promise<void> {
    if (!atoms.has('raw_battery_gauge_stats_reported')) {
      return;
    }

    const e = ctx.engine;
    await e.query(
      `INCLUDE PERFETTO MODULE
          google3.wireless.android.telemetry.trace_extractor.modules.power.battery_gauge_power`,
    );

    const group = new TrackNode({
      name: 'Raw Battery Gauge',
    });
    ctx.plugins
      .getPlugin(StandardGroupsPlugin)
      .getOrCreateStandardGroup(ctx.defaultWorkspace, 'POWER')
      .addChildInOrder(group);

    await this.addCounterTrack(
      ctx,
      'Raw Battery Gauge - mW',
      `SELECT ts, power_mw AS value FROM _battery_gauge_power_samples`,
      group,
    );
  }

  async addCujs(ctx: Trace, atoms: Set<string>): Promise<void> {
    const group = new TrackNode({
      name: 'Wear CUJs',
      isSummary: true,
    });
    const e = ctx.engine;

    await e.query(`INCLUDE PERFETTO MODULE
          google3.wireless.android.wear.tools.pipelines.perfetto.cujs.cuj_tracks`);
    const result = await e.query(
      `select track_name, table_name, source_atom FROM wear_cuj_slice_tracks`,
    );
    const it = result.iter({
      track_name: STR,
      table_name: STR,
      source_atom: STR,
    });

    for (; it.valid(); it.next()) {
      if (!atoms.has(it.source_atom)) {
        continue;
      }
      await this.addSliceTrack(
        ctx,
        it.track_name,
        `SELECT ts, dur, name FROM ${it.table_name}`,
        group,
      );
    }

    await this.addModes(ctx, atoms, group);

    if (group.children.length > 0) {
      ctx.defaultWorkspace.addChildInOrder(group);
    }
  }

  /**
   * This hook is called as the trace is loading. At this point the trace is
   * loaded into trace processor and it's ready to process queries. This hook
   * should be used for adding tracks and commands that depend on the trace.
   *
   * It should not be used for finding tracks from other plugins as there is no
   * guarantee those tracks will have been added yet.
   */
  async onTraceLoad(ctx: Trace): Promise<void> {
    const result = await ctx.engine.query(
      `SELECT int_value FROM metadata WHERE name = 'statsd_triggering_subscription_id'`,
    );
    const row = result.maybeFirstRow({int_value: LONG});
    if (!row) {
      return;
    }
    if (!VALID_SUBSCRIPTION_IDS.includes(row.int_value)) {
      return;
    }

    const atoms = await this.findAtoms(ctx.engine);

    await this.addCujs(ctx, atoms);
    await this.addRawBatteryGauge(ctx, atoms);
  }
}
