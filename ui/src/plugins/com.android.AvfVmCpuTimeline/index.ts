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

import {SliceTrack} from '../../components/tracks/slice_track';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {Engine} from '../../trace_processor/engine';

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.AvfVmCpuTimeline';

  private readonly validTargets = new Map<number, string>();

  async onTraceLoad(ctx: Trace): Promise<void> {
    this.validTargets.clear();
    await this.findValidTargets(ctx.engine);

    if (this.validTargets.size === 0) {
      alert('The loaded trace does not contain any valid Avf VM targets!');
    } else {
      const defaultTargetId = this.validTargets.keys().next().value;
      await this.createTargetVmTrack(ctx, defaultTargetId);

      ctx.commands.registerCommand({
        id: `com.android.SelectAvfVmUtid`,
        name: 'Select Avf VM utid to add track',
        callback: async () => {
          if (this.validTargets.size === 0) {
            alert('Available ValidTargets set exhausted! Do Refresh...');
          } else {
            const utid = await this.selectValidTarget(ctx);
            await this.createTargetVmTrack(ctx, utid);
          }
        },
        defaultHotkey: 'Shift+V',
      });
    }
  }

  async createTargetVmTrack(ctx: Trace, targetUtid: number) {
    const name = `Avf VM CPU Timeline utid:${targetUtid}`;
    const uri = `com.android.AvfVmCpuTimeline#AvfVmCpuTimeline${targetUtid}`;

    this.validTargets.delete(targetUtid);

    const query = `
      SELECT
        sched.id AS id,
        ts,
        dur,
        cpu,
        priority,
        utid,
        name,
        cpu AS depth
      FROM sched
      JOIN thread
        USING (utid)
      WHERE
        utid == ${targetUtid}
    `;

    ctx.tracks.registerTrack({
      uri,
      renderer: SliceTrack.create({
        trace: ctx,
        uri,
        dataset: new SourceDataset({
          src: query,
          schema: {
            id: NUM,
            ts: LONG,
            dur: LONG,
            cpu: NUM,
            priority: NUM,
            utid: NUM,
            name: STR,
            depth: NUM,
          },
        }),
        // Blank details panel - overrides details panel that assumes slices are
        // from the slice table.
        detailsPanel: () => {
          return {
            render: () => undefined,
          };
        },
      }),
    });

    const trackNode = new TrackNode({uri, name, sortOrder: -90});
    ctx.defaultWorkspace.addChildInOrder(trackNode);
  }

  async findValidTargets(engine: Engine) {
    const queryResult = await engine.query(`
      SELECT
        sched.id as id,
        utid,
        thread.name as threadName
      FROM sched
      JOIN thread
        USING (utid)
      WHERE threadName LIKE '%vhost%' OR threadName LIKE '%vcpu%'
    `);

    const qRow = queryResult.iter({
      id: NUM,
      utid: NUM,
      threadName: STR,
    });
    while (qRow.valid()) {
      if (!this.validTargets.has(qRow.utid)) {
        // collect unique thread.utid in the available targets map
        this.validTargets.set(qRow.utid, qRow.threadName);
      }
      qRow.next();
    }
  }

  async selectValidTarget(ctx: Trace): Promise<number> {
    const input = await ctx.omnibox.prompt(this.prepareSelectMessage());
    if (input !== undefined) {
      const checkId = Number(input);
      if (!isNaN(checkId) && this.validTargets.has(checkId)) {
        return checkId;
      }
    }

    const defaultTarget = this.validTargets.keys().next().value;
    alert(`Invalid Target selected! Using default value: ${defaultTarget}`);
    return defaultTarget;
  }

  private prepareSelectMessage(): string {
    let message = 'Available target IDs are:\n';
    this.validTargets.forEach((id, name) => {
      message += `${id} : ${name}\n`;
    });
    message += `\nEnter targetID to add track:`;
    return message;
  }
}
