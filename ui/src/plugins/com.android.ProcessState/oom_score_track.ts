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
import {Duration, Time, type time} from '../../base/time';
import {CounterTrack} from '../../components/tracks/counter_track';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {TrackEventSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import type {TrackMouseEvent} from '../../public/track';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {oomScoreChanges} from './aggregators';

/** OOM score of one process; clicking a step shows that score change. */
export class OomScoreTrack extends CounterTrack {
  constructor(
    trace: Trace,
    uri: string,
    private readonly upid: number,
    // Process name and pid, shown next to the details panel title.
    private readonly label: string,
    private readonly processEnd: time | undefined,
  ) {
    super({
      trace,
      uri,
      endTs: processEnd,
      sqlSource: `
        SELECT ts, oom_score AS value
        FROM _android_process_oom_scores
        WHERE upid = ${upid}
      `,
    });
  }

  onMouseClick({x, timescale}: TrackMouseEvent): boolean {
    const ts = timescale.pxToHpTime(x).toTime('floor');
    if (this.processEnd !== undefined && ts >= this.processEnd) {
      return false;
    }
    this.engine
      .query(
        `
        SELECT id FROM (${oomScoreChanges([this.upid])})
        WHERE ts <= ${ts}
        ORDER BY ts DESC
        LIMIT 1
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
        SELECT ts, dur FROM (${oomScoreChanges([this.upid])}) WHERE id = ${id}
      `)
    ).firstRow({ts: LONG, dur: LONG});
    return {ts: Time.fromRaw(row.ts), dur: Duration.fromRaw(row.dur)};
  }

  detailsPanel(): TrackEventDetailsPanel {
    return new OomScoreDetailsPanel(this.trace, this.upid, this.label);
  }
}

interface ScoreChange {
  readonly score: number;
  readonly prevScore: number | null;
  readonly prevDur: bigint | null;
  readonly reason: string | null;
}

class OomScoreDetailsPanel implements TrackEventDetailsPanel {
  private sel?: TrackEventSelection;
  private change?: ScoreChange;

  constructor(
    private readonly trace: Trace,
    private readonly upid: number,
    private readonly label: string,
  ) {}

  async load(sel: TrackEventSelection) {
    const result = await this.trace.engine.query(`
      SELECT
        oom_score AS score,
        prev_score AS prevScore,
        prev_dur AS prevDur,
        reason
      FROM (${oomScoreChanges([this.upid])})
      WHERE id = ${sel.eventId}
    `);
    this.change = result.firstRow({
      score: NUM,
      prevScore: NUM_NULL,
      prevDur: LONG_NULL,
      reason: STR_NULL,
    });
    this.sel = sel;
  }

  render() {
    const {trace, sel, change} = this;
    if (sel === undefined || change === undefined) {
      return m(DetailsShell, {title: 'OOM score', description: 'Loading...'});
    }
    const {score, prevScore, prevDur, reason} = change;
    return m(
      DetailsShell,
      {title: 'OOM score', description: this.label},
      m(
        GridLayout,
        m(
          Section,
          {title: 'Details'},
          m(Tree, [
            m(TreeNode, {
              left: 'Start time',
              right: m(Timestamp, {trace, ts: sel.ts}),
            }),
            m(TreeNode, {
              left: 'Duration',
              right: m(DurationWidget, {trace, dur: sel.dur ?? Duration.ZERO}),
            }),
            m(TreeNode, {left: 'Score', right: score}),
            m(TreeNode, {
              left: 'Previous score',
              right:
                prevScore === null || prevDur === null
                  ? 'NULL'
                  : [
                      `${prevScore} (for `,
                      m(DurationWidget, {
                        trace,
                        dur: Duration.fromRaw(prevDur),
                      }),
                      ')',
                    ],
            }),
            m(TreeNode, {left: 'OomAdjuster reason', right: reason ?? 'NULL'}),
          ]),
        ),
      ),
    );
  }
}
