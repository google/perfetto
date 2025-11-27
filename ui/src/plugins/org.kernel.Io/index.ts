// Copyright (C) 2024 The Android Open Source Project
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

import {TrackNode} from '../../public/workspace';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {createQueryCounterTrack} from '../../components/tracks/query_counter_track';
import {Engine} from '../../trace_processor/engine';
import {NUM} from '../../trace_processor/query_result';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Io';
  async onTraceLoad(ctx: Trace) {
    await ctx.engine.query(`INCLUDE PERFETTO MODULE linux.block_io`);
    const devices = await this.lookupDevices(ctx.engine);
    const group = new TrackNode({
      name: 'Queued IO requests',
      sortOrder: -5,
      isSummary: true,
    });
    for (const device of devices) {
      const uri = `/queued_io_request_count/device_${device['id']}`;
      const name = `dev major:${device['major']} minor:${device['minor']}`;
      const track = await createQueryCounterTrack({
        trace: ctx,
        uri,
        data: {
          sqlSource: `
            SELECT ts, ops_in_queue_or_device as value
            FROM linux_active_block_io_operations_by_device
            WHERE dev = ${String(device['id'])}
          `,
        },
      });
      ctx.tracks.registerTrack({
        uri,
        tags: {
          device: device['id'],
        },
        renderer: track,
      });
      const node = new TrackNode({uri, name});
      group.addChildInOrder(node);
    }
    if (group.children.length) {
      ctx.defaultWorkspace.addChildInOrder(group);
    }
  }

  private async lookupDevices(
    engine: Engine,
  ): Promise<{[key: string]: number}[]> {
    const query = `
      SELECT DISTINCT dev, linux_device_major_id(dev) as major, linux_device_minor_id(dev) as minor
      FROM linux_active_block_io_operations_by_device ORDER BY dev`;
    const result = await engine.query(query);
    const it = result.iter({dev: NUM, major: NUM, minor: NUM});

    const devs: {[key: string]: number}[] = [];

    for (; it.valid(); it.next()) {
      devs.push({id: it.dev, major: it.major, minor: it.minor});
    }

    return devs;
  }
}
