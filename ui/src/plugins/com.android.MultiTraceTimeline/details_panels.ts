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
import {Duration, type duration, Time, type time} from '../../base/time';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {TrackEventSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {Anchor} from '../../widgets/anchor';
import {DetailsShell} from '../../widgets/details_shell';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {
  type BreakdownColumn,
  DEVICE_STATE_SPEC,
  type DeviceTraceDatasetSpec,
  type DeviceTraceRow,
  formatBytes,
  parseDeviceTraceRow,
  renderArgValue,
  selectColumnList,
  TABLE_NAME,
  TRACE_UUID_URL,
} from './schema';

interface DeviceStateRow {
  readonly id: number;
  readonly ts: time;
  readonly dur: duration;
  readonly name: string;
}

export class DeviceStateDetailsPanel implements TrackEventDetailsPanel {
  private row?: DeviceStateRow;
  private isLoading = true;

  constructor(
    private readonly trace: Trace,
    private readonly androidId: string,
    private readonly stateTrack: string,
  ) {}

  async load(selection: TrackEventSelection): Promise<void> {
    this.isLoading = true;
    const res = await this.trace.engine.query(`
      SELECT id, ts, dur, depth, name
      FROM slice
      WHERE id = ${selection.eventId}
      LIMIT 1
    `);
    const it = res.iter(DEVICE_STATE_SPEC);
    if (it.valid()) {
      this.row = {
        id: it.id,
        ts: Time.fromRaw(it.ts),
        dur: Duration.fromRaw(it.dur),
        name: it.name,
      };
    }
    this.isLoading = false;
  }

  render() {
    if (this.isLoading) {
      return m(DetailsShell, {
        title: 'Device State',
        description: 'Loading...',
      });
    }
    if (!this.row) {
      return m(DetailsShell, {
        title: 'Device State',
        description: 'State interval not found',
      });
    }
    const r = this.row;
    return m(
      DetailsShell,
      {title: `Device State: ${this.stateTrack}`, description: r.name},
      m(
        Section,
        {title: 'State Interval'},
        m(
          Tree,
          m(TreeNode, {left: 'Track', right: this.stateTrack}),
          m(TreeNode, {left: 'State Value', right: r.name}),
          m(TreeNode, {left: 'android_id', right: this.androidId}),
          m(TreeNode, {
            left: 'Start',
            right: m(Timestamp, {trace: this.trace, ts: r.ts}),
          }),
          m(TreeNode, {
            left: 'Duration',
            right: m(DurationWidget, {trace: this.trace, dur: r.dur}),
          }),
        ),
      ),
    );
  }
}

export class DeviceTraceDetailsPanel implements TrackEventDetailsPanel {
  private row?: DeviceTraceRow;
  private isLoading = true;

  constructor(
    private readonly trace: Trace,
    private readonly breakdownColumns: ReadonlyArray<BreakdownColumn>,
    private readonly datasetSpec: DeviceTraceDatasetSpec,
  ) {}

  async load(selection: TrackEventSelection): Promise<void> {
    this.isLoading = true;
    const res = await this.trace.engine.query(`
      SELECT
        ${selectColumnList(this.breakdownColumns)}
      FROM ${TABLE_NAME}
      WHERE id = ${selection.eventId}
      LIMIT 1
    `);
    const it = res.iter(this.datasetSpec);
    if (it.valid()) {
      this.row = parseDeviceTraceRow(it, this.breakdownColumns);
    }
    this.isLoading = false;
  }

  render() {
    if (this.isLoading) {
      return m(DetailsShell, {
        title: 'Device Trace',
        description: 'Loading...',
      });
    }
    if (!this.row) {
      return m(DetailsShell, {
        title: 'Device Trace',
        description: 'Trace interval not found',
      });
    }
    const r = this.row;
    const traceUuidNode = !r.traceUuid
      ? 'N/A'
      : this.trace.isInternalUser
        ? m(
            Anchor,
            {href: `${TRACE_UUID_URL}${r.traceUuid}`, target: '_blank'},
            r.traceUuid,
          )
        : r.traceUuid;

    return m(
      DetailsShell,
      {
        title: 'Device Trace',
        description: r.name,
      },
      m(
        Section,
        {title: 'Trace Metadata'},
        m(
          Tree,
          m(TreeNode, {
            left: 'Trace UUID',
            right: traceUuidNode,
          }),
          ...this.breakdownColumns.map((col) => {
            const value = r.cols.get(col.sqlCol) || '';
            return m(TreeNode, {
              left: col.label,
              right: renderArgValue(this.trace, col.label, value),
            });
          }),
          m(TreeNode, {
            left: 'Start',
            right: m(Timestamp, {trace: this.trace, ts: r.ts}),
          }),
          m(TreeNode, {
            left: 'Duration',
            right: m(DurationWidget, {trace: this.trace, dur: r.dur}),
          }),
          r.armedDur > r.dur &&
            m(TreeNode, {
              left: 'Armed Start',
              right: m(Timestamp, {trace: this.trace, ts: r.armedStartTs}),
            }),
          r.armedDur > r.dur &&
            m(TreeNode, {
              left: 'Armed Duration',
              right: m(DurationWidget, {trace: this.trace, dur: r.armedDur}),
            }),
          m(TreeNode, {
            left: 'Trace Duration',
            right:
              r.traceDur > 0n
                ? m(DurationWidget, {trace: this.trace, dur: r.traceDur})
                : '-',
          }),
          m(TreeNode, {
            left: 'Sched Duration',
            right:
              r.schedDur > 0n
                ? m(DurationWidget, {trace: this.trace, dur: r.schedDur})
                : '-',
          }),
          m(TreeNode, {
            left: 'Uncompressed Size',
            right: formatBytes(r.traceSizeBytes),
          }),
          m(TreeNode, {
            left: 'Compressed Size',
            right: formatBytes(r.originalTraceSizeBytes),
          }),
        ),
      ),
    );
  }
}
