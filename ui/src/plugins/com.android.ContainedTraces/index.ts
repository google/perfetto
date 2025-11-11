// Copyright (C) 2023 The Android Open Source Project
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
import {STR, LONG, LONG_NULL} from '../../trace_processor/query_result';
import {SourceDataset} from '../../trace_processor/dataset';
import SupportPlugin from '../com.android.AndroidLongBatterySupport';

interface ContainedTrace {
  uuid: string;
  subscription: string;
  trigger: string;
  // NB: these are millis.
  ts: number;
  dur: number;
}

export default class implements PerfettoPlugin {
  static readonly id = 'com.android.ContainedTraces';
  static readonly dependencies = [StandardGroupsPlugin, SupportPlugin];

  private support(ctx: Trace) {
    return ctx.plugins.getPlugin(SupportPlugin);
  }

  async onTraceLoad(ctx: Trace, args: {[key: string]: unknown}): Promise<void> {
    const support = this.support(ctx);

    const containedTraces = (args?.containedTraces ?? []) as ContainedTrace[];

    const bySubscription = new Map<string, ContainedTrace[]>();
    for (const trace of containedTraces) {
      if (!bySubscription.has(trace.subscription)) {
        bySubscription.set(trace.subscription, []);
      }
      bySubscription.get(trace.subscription)!.push(trace);
    }

    for (const [subscription, traces] of bySubscription) {
      await support.addSliceTrack(
        ctx,
        subscription,
        new SourceDataset({
          src: traces
            .map(
              (t) => `
              SELECT
                CAST(${t.ts} * 1e6 AS int) AS ts,
                CAST(${t.dur} * 1e6 AS int) AS dur,
                '${t.trigger === '' ? 'Trace' : t.trigger}' AS name,
                'http://go/trace-uuid/${t.uuid}' AS link
              `,
            )
            .join(' UNION ALL '),
          schema: {
            ts: LONG,
            dur: LONG_NULL,
            name: STR,
            link: STR,
          },
        }),
        'Other traces',
      );
    }
  }
}
