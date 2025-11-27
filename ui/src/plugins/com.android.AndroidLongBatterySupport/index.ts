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
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import {PerfettoPlugin} from '../../public/plugin';
import {Engine} from '../../trace_processor/engine';
import {SliceTrack, RowSchema} from '../../components/tracks/slice_track';
import {CounterOptions} from '../../components/tracks/base_counter_track';
import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {STR} from '../../trace_processor/query_result';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidLongBatterySupport';
  static readonly dependencies = [StandardGroupsPlugin];

  readonly groups = new Map<string, TrackNode>();

  getOrCreateGroup(
    ctx: Trace,
    groupName: string,
    groupCollapsed = true,
  ): TrackNode {
    const existingGroup = this.groups.get(groupName);
    if (existingGroup) {
      return existingGroup;
    }

    const group = new TrackNode({
      name: groupName,
      isSummary: true,
      collapsed: groupCollapsed,
    });
    this.groups.set(groupName, group);
    ctx.defaultWorkspace.addChildInOrder(group);
    return group;
  }

  private addTrack(
    ctx: Trace,
    track: TrackNode,
    groupName?: string,
    groupCollapsed = true,
  ): void {
    if (groupName) {
      const group = this.getOrCreateGroup(ctx, groupName, groupCollapsed);
      group.addChildInOrder(track);
    } else {
      ctx.defaultWorkspace.addChildInOrder(track);
    }
  }

  async addSliceTrack<T extends RowSchema>(
    ctx: Trace,
    name: string,
    dataset: SourceDataset<T>,
    groupName: string,
    groupCollapsed = true,
  ) {
    const uri = `/long_battery_tracing_${name}`;
    const track = await SliceTrack.createMaterialized({
      trace: ctx,
      uri,
      dataset,
    });
    ctx.tracks.registerTrack({
      uri,
      renderer: track,
    });
    const trackNode = new TrackNode({uri, name});
    this.addTrack(ctx, trackNode, groupName, groupCollapsed);
  }

  async addCounterTrack(
    ctx: Trace,
    name: string,
    query: string,
    groupName: string,
    options?: Partial<CounterOptions>,
    groupCollapsed = true,
  ) {
    const uri = `/long_battery_tracing_${name}`;
    const track = await createQueryCounterTrack({
      trace: ctx,
      uri,
      data: {
        sqlSource: query,
        columns: ['ts', 'value'],
      },
      options,
    });
    ctx.tracks.registerTrack({
      uri,
      renderer: track,
    });
    const trackNode = new TrackNode({uri, name});
    this.addTrack(ctx, trackNode, groupName, groupCollapsed);
  }

  private _features: Promise<Set<string>> | undefined;

  async features(e: Engine): Promise<Set<string>> {
    if (!this._features) {
      this._features = this.findFeatures(e);
    }
    return this._features;
  }

  private async findFeatures(e: Engine): Promise<Set<string>> {
    const features = new Set<string>();

    const addFeatures = async (q: string) => {
      const result = await e.query(q);
      const it = result.iter({feature: STR});
      for (; it.valid(); it.next()) {
        features.add(it.feature);
      }
    };

    await addFeatures(`
      select distinct 'atom.' || s.name as feature
      from track t join slice s on t.id = s.track_id
      where t.name = 'Statsd Atoms'`);

    await addFeatures(`
      select distinct
        case when name like '%wlan%' then 'net.wifi'
            when name like '%rmnet%' then 'net.modem'
            else 'net.other'
        end as feature
      from track
      where name like '%Transmitted' or name like '%Received'`);

    await addFeatures(`
      select distinct 'track.' || lower(name) as feature
      from track where name in ('RIL', 'suspend_backoff') or name like 'battery_stats.%'`);

    await addFeatures(`
      select distinct 'track.battery_stats.*' as feature
      from track where name like 'battery_stats.%'`);

    try {
      await e.query(
        `INCLUDE PERFETTO MODULE
              google3.wireless.android.telemetry.trace_extractor.modules.atom_counters_slices`,
      );
      features.add('google3');
    } catch (e) {}

    return features;
  }

  async onTraceLoad(): Promise<void> {}
}
