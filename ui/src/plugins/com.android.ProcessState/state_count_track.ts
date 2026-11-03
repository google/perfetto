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

import m from 'mithril';
import {Icons} from '../../base/semantic_icons';
import {sqliteString} from '../../base/string_utils';
import {Duration, Time, type time} from '../../base/time';
import {CounterTrack} from '../../components/tracks/counter_track';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {Timestamp} from '../../components/widgets/timestamp';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {TrackEventSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import type {TrackMouseEvent} from '../../public/track';
import type {TrackNode} from '../../public/workspace';
import {
  LONG,
  NUM,
  STR_NULL,
  type Row,
} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';
import {Button} from '../../widgets/button';
import {DetailsShell} from '../../widgets/details_shell';
import {durationCellRenderer} from './aggregators';

/** What a count track counts. */
export interface CountSource {
  readonly state: string;
  // Completes "N processes ...", e.g. 'in TOP', 'frozen'.
  readonly label: string;
  readonly concurrencyTable: string;
  readonly intervalsTable: string;
  readonly trackUri: (upid: number) => string;
  readonly createBreakdownNode: (
    upid: number,
    pid: number,
    name: string,
  ) => TrackNode;
}

/** Concurrency counter for one state; clicking it lists the processes in it. */
export class ProcessStateCountTrack extends CounterTrack {
  private breakdownRequestId = 0;

  constructor(
    trace: Trace,
    uri: string,
    private readonly node: TrackNode,
    private readonly source: CountSource,
  ) {
    super({
      trace,
      uri,
      sqlSource: `
        SELECT ts, concurrency AS value
        FROM ${source.concurrencyTable}
        WHERE state = ${sqliteString(source.state)}
      `,
    });
  }

  showBreakdownForRange(start: time, end: time): Promise<void> {
    if (start >= end) {
      return this.showBreakdownAtInstant(start);
    }
    return this.showBreakdownWhere(
      `i.ts < ${end} AND (i.dur < 0 OR i.ts + i.dur > ${start})`,
    );
  }

  showBreakdownAtInstant(ts: time): Promise<void> {
    return this.showBreakdownWhere(
      `i.ts <= ${ts} AND (i.dur < 0 OR i.ts + i.dur > ${ts})`,
    );
  }

  clearBreakdown(): void {
    ++this.breakdownRequestId;
    // TrackNode.clear() skips ancestor URI map cleanup and unpinning.
    for (const child of [...this.node.children]) {
      child.remove();
    }
    this.node.collapse();
  }

  private async showBreakdownWhere(timeFilter: string): Promise<void> {
    const requestId = ++this.breakdownRequestId;
    const result = await this.engine.query(`
      SELECT i.upid, i.pid, i.process_name AS name
      FROM ${this.source.intervalsTable} AS i
      LEFT JOIN android_process_metadata AS m USING (upid)
      WHERE i.state = ${sqliteString(this.source.state)}
        AND (${timeFilter})
      GROUP BY i.upid
      ORDER BY m.uid NULLS LAST, min(i.ts), i.pid
    `);
    if (requestId !== this.breakdownRequestId) {
      return;
    }

    for (const child of [...this.node.children]) {
      child.remove();
    }
    for (
      const it = result.iter({upid: NUM, pid: NUM, name: STR_NULL});
      it.valid();
      it.next()
    ) {
      this.node.addChildLast(
        this.source.createBreakdownNode(
          it.upid,
          it.pid,
          it.name ?? `${it.pid}`,
        ),
      );
    }
    if (this.node.hasChildren) {
      this.node.expand();
    } else {
      this.node.collapse();
    }
  }

  getTrackShellButtons(): m.Children {
    const sel = this.trace.selection.selection;
    return [
      sel.kind === 'area' &&
        sel.trackUris.includes(this.uri) &&
        m(Button, {
          compact: true,
          icon: Icons.UnfoldMore,
          title: 'Show process tracks for selection',
          onclick: () => this.showBreakdownForRange(sel.start, sel.end),
        }),
      sel.kind === 'track_event' &&
        sel.trackUri === this.uri &&
        m(Button, {
          compact: true,
          icon: Icons.UnfoldMore,
          title: 'Show process tracks at selected point',
          onclick: () => this.showBreakdownAtInstant(sel.ts),
        }),
      this.node.hasChildren &&
        m(Button, {
          className: 'pf-visible-on-hover',
          compact: true,
          icon: Icons.Close,
          title: 'Clear process tracks',
          onclick: () => this.clearBreakdown(),
        }),
    ];
  }

  onMouseClick({x, timescale}: TrackMouseEvent): boolean {
    const ts = timescale.pxToHpTime(x).toTime('floor');
    this.engine
      .query(
        `
        SELECT id FROM ${this.source.concurrencyTable}
        WHERE state = ${sqliteString(this.source.state)} AND ts <= ${ts}
        ORDER BY ts DESC LIMIT 1
      `,
      )
      .then((result) => {
        const it = result.iter({id: NUM});
        if (it.valid()) this.trace.selection.selectTrackEvent(this.uri, it.id);
      });
    return true;
  }

  async getSelectionDetails(id: number) {
    const row = (
      await this.engine.query(`
        SELECT ts, dur FROM ${this.source.concurrencyTable} WHERE id = ${id}
      `)
    ).firstRow({ts: LONG, dur: LONG});
    return {ts: Time.fromRaw(row.ts), dur: Duration.fromRaw(row.dur)};
  }

  detailsPanel(): TrackEventDetailsPanel {
    return new ProcessesInStatePanel(this.trace, this.source, (ts) =>
      this.showBreakdownAtInstant(ts),
    );
  }
}

class ProcessesInStatePanel implements TrackEventDetailsPanel {
  private rows: Row[] = [];
  private selTs?: time;

  constructor(
    private readonly trace: Trace,
    private readonly source: CountSource,
    private readonly onShowBreakdown: (ts: time) => void,
  ) {}

  async load(sel: TrackEventSelection) {
    this.selTs = sel.ts;
    const result = await this.trace.engine.query(`
      SELECT id, upid, process_name, pid, ts, iif(dur < 0, trace_end() - ts, dur) AS dur
      FROM ${this.source.intervalsTable}
      WHERE state = ${sqliteString(this.source.state)}
        AND ts <= ${sel.ts}
        AND (dur < 0 OR ts + dur > ${sel.ts})
    `);
    const cols = result.columns();
    const rows: Row[] = [];
    for (const it = result.iter({}); it.valid(); it.next()) {
      rows.push(Object.fromEntries(cols.map((c) => [c, it.get(c)])));
    }
    this.rows = rows;
  }

  render() {
    const {trace, selTs} = this;
    return m(
      DetailsShell,
      {
        title: `${this.rows.length} processes ${this.source.label}`,
        buttons: m(Button, {
          label: 'Show process tracks',
          icon: Icons.UnfoldMore,
          disabled: this.rows.length === 0 || selTs === undefined,
          onclick: () => {
            if (selTs !== undefined) {
              this.onShowBreakdown(selTs);
            }
          },
        }),
        fillHeight: true,
      },
      m(DataGrid, {
        fillHeight: true,
        data: this.rows,
        schema: {
          ts: {
            title: 'Entered at',
            columnType: 'quantitative',
            cellRenderer: (ts) =>
              typeof ts === 'bigint'
                ? m(Timestamp, {trace, ts: Time.fromRaw(ts)})
                : String(ts ?? ''),
          },
          id: {
            title: 'Slice ID',
            columnType: 'identifier',
            cellRenderer: (id) => {
              const upid = this.rows.find((r) => r.id === id)?.upid;
              if (typeof id !== 'bigint' || typeof upid !== 'bigint') {
                return String(id ?? '');
              }
              return m(
                Anchor,
                {
                  title: 'Go to slice',
                  icon: Icons.UpdateSelection,
                  onclick: () =>
                    trace.selection.selectTrackEvent(
                      this.source.trackUri(Number(upid)),
                      Number(id),
                      {scrollToSelection: true},
                    ),
                },
                String(id),
              );
            },
          },
          process_name: {title: 'Process', columnType: 'text'},
          pid: {title: 'PID', columnType: 'identifier'},
          dur: {
            title: 'Time in state',
            columnType: 'quantitative',
            cellRenderer: durationCellRenderer(trace),
          },
        },
        initialColumns: [
          {id: 'ts', field: 'ts'},
          {id: 'id', field: 'id'},
          {id: 'process_name', field: 'process_name'},
          {id: 'pid', field: 'pid'},
          {id: 'dur', field: 'dur', sort: 'DESC'},
        ],
      }),
    );
  }
}
