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

import m from 'mithril';
import type {Trace} from '../../public/trace';
import StandardGroupsPlugin from '../dev.perfetto.StandardGroups';
import type {PerfettoPlugin} from '../../public/plugin';
import {
  STR,
  STR_NULL,
  LONG,
  LONG_NULL,
} from '../../trace_processor/query_result';
import {SourceDataset, type Dataset} from '../../trace_processor/dataset';
import SupportPlugin from '../com.android.AndroidLongBatterySupport';
import {AsyncMemo} from '../../base/async_memo';
import {Duration, Time, type duration} from '../../base/time';
import {DurationWidget} from '../../components/widgets/duration';
import {selectTracksAndGetDataset} from '../../components/aggregation_adapter';
import {Icons} from '../../base/semantic_icons';
import {Icon} from '../../widgets/icon';
import {Button} from '../../widgets/button';
import {Checkbox} from '../../widgets/checkbox';
import {Anchor} from '../../widgets/anchor';
import {Grid, GridCell, GridHeaderCell} from '../../widgets/grid';
import type {
  AreaSelection,
  AreaSelectionTab,
  ContentWithLoadingFlag,
} from '../../public/selection';

interface ContainedTrace {
  uuid: string;
  subscription: string;
  trigger: string;
  // NB: these are millis.
  ts: number;
  dur: number;
}

interface MergeRow {
  uuid: string;
  name: string;
  track: string;
  dur: duration;
  isSelf: boolean;
}

const TRACE_UUID_URL = 'http://go/trace-uuid/';
const TRACE_UUIDS_URL = 'http://go/trace-uuids/';

const CONTAINED_TRACE_SPEC = {
  ts: LONG,
  dur: LONG_NULL,
  name: STR,
  track: STR,
  link: STR,
};

function traceName(t: ContainedTrace): string {
  return t.trigger === '' ? 'Trace' : t.trigger;
}

class ContainedTracesTab implements AreaSelectionTab {
  readonly id = 'contained_traces_merge';
  readonly name = 'Contained traces';

  private readonly memo = new AsyncMemo<{
    rows: MergeRow[];
    selected: Set<string>;
  }>();

  constructor(
    private readonly trace: Trace,
    private readonly selfUuid?: string,
  ) {}

  render(selection: AreaSelection): ContentWithLoadingFlag | undefined {
    const dataset = selectTracksAndGetDataset(
      selection.tracks,
      CONTAINED_TRACE_SPEC,
    );
    if (dataset === undefined) return undefined;

    const {data, isPending} = this.memo.use({
      key: {
        start: selection.start,
        end: selection.end,
        trackUris: selection.trackUris,
      },
      compute: async () => {
        const rows = await this.queryRows(dataset, selection);
        return {rows, selected: new Set(rows.map((r) => r.uuid))};
      },
    });

    return {
      isLoading: isPending,
      buttons: m(Button, {
        label: 'Merge selected traces',
        icon: Icons.ExternalLink,
        disabled: data === undefined || data.selected.size === 0,
        onclick: () => {
          if (data !== undefined) {
            window.open(
              TRACE_UUIDS_URL + [...data.selected].join(','),
              '_blank',
            );
          }
        },
      }),
      content:
        data === undefined
          ? undefined
          : this.renderGrid(data.rows, data.selected),
    };
  }

  private async queryRows(
    dataset: Dataset,
    selection: AreaSelection,
  ): Promise<MergeRow[]> {
    const rows: MergeRow[] = [];
    if (this.selfUuid !== undefined) {
      const {start, end} = this.trace.traceInfo;
      rows.push({
        uuid: this.selfUuid,
        name: 'This trace',
        track: '',
        dur: Time.durationBetween(start, end),
        isSelf: true,
      });
    }

    const result = await this.trace.engine.query(`
      select
        replace(link, '${TRACE_UUID_URL}', '') as uuid,
        name,
        track,
        coalesce(dur, 0) as dur
      from (${dataset.query(CONTAINED_TRACE_SPEC)})
      where ts < ${selection.end} and ts + coalesce(dur, 0) > ${selection.start}
      order by ts
    `);
    const it = result.iter({uuid: STR, name: STR, track: STR, dur: LONG});
    for (; it.valid(); it.next()) {
      rows.push({
        uuid: it.uuid,
        name: it.name,
        track: it.track,
        dur: Duration.fromRaw(it.dur),
        isSelf: false,
      });
    }
    return rows;
  }

  private renderGrid(
    rows: ReadonlyArray<MergeRow>,
    selected: Set<string>,
  ): m.Children {
    const allSelected = rows.length > 0 && selected.size === rows.length;
    return m(Grid, {
      columns: [
        {
          key: 'select',
          header: m(
            GridHeaderCell,
            m(Checkbox, {
              checked: allSelected,
              onchange: () => {
                if (allSelected) {
                  selected.clear();
                } else {
                  rows.forEach((r) => selected.add(r.uuid));
                }
              },
            }),
          ),
        },
        {key: 'self', header: m(GridHeaderCell, 'Self')},
        {key: 'uuid', header: m(GridHeaderCell, 'Trace')},
        {key: 'name', header: m(GridHeaderCell, 'Name')},
        {key: 'track', header: m(GridHeaderCell, 'Track')},
        {key: 'dur', header: m(GridHeaderCell, 'Duration')},
      ],
      rowData: rows.map((r) => [
        m(
          GridCell,
          m(Checkbox, {
            checked: selected.has(r.uuid),
            onchange: () => {
              if (selected.has(r.uuid)) {
                selected.delete(r.uuid);
              } else {
                selected.add(r.uuid);
              }
            },
          }),
        ),
        m(GridCell, r.isSelf ? m(Icon, {icon: Icons.Check}) : undefined),
        m(
          GridCell,
          m(
            Anchor,
            {href: TRACE_UUID_URL + r.uuid, target: '_blank'},
            TRACE_UUID_URL + r.uuid,
          ),
        ),
        m(GridCell, r.name),
        m(GridCell, r.track),
        m(GridCell, m(DurationWidget, {trace: this.trace, dur: r.dur})),
      ]),
    });
  }
}

async function selfTraceUuid(ctx: Trace): Promise<string | undefined> {
  const result = await ctx.engine.query(
    `select str_value from metadata where name = 'trace_uuid'`,
  );
  const it = result.iter({str_value: STR_NULL});
  return it.valid() ? (it.str_value ?? undefined) : undefined;
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
                '${traceName(t)}' AS name,
                '${t.subscription}' AS track,
                '${TRACE_UUID_URL}${t.uuid}' AS link
              `,
            )
            .join(' UNION ALL '),
          schema: {
            ts: LONG,
            dur: LONG_NULL,
            name: STR,
            track: STR,
            link: STR,
          },
        }),
        'Other traces',
      );
    }

    ctx.selection.registerAreaSelectionTab(
      new ContainedTracesTab(ctx, await selfTraceUuid(ctx)),
    );
  }
}
