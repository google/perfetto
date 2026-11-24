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
import {Duration, Time} from '../../base/time';
import {CounterTrack} from '../../components/tracks/counter_track';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import {Timestamp} from '../../components/widgets/timestamp';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {TrackEventSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import type {TrackMouseEvent} from '../../public/track';
import {LONG, NUM, type Row} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';
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
}

/** Concurrency counter for one state; clicking it lists the processes in it. */
export class ProcessStateCountTrack extends CounterTrack {
  constructor(
    trace: Trace,
    uri: string,
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
    return new ProcessesInStatePanel(this.trace, this.source);
  }
}

class ProcessesInStatePanel implements TrackEventDetailsPanel {
  private rows: Row[] = [];

  constructor(
    private readonly trace: Trace,
    private readonly source: CountSource,
  ) {}

  async load(sel: TrackEventSelection) {
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
    const trace = this.trace;
    return m(
      DetailsShell,
      {
        title: `${this.rows.length} processes ${this.source.label}`,
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
