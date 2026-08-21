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

import m from 'mithril';
import {materialColorScheme} from '../../components/colorizer';
import {Time, type time} from '../../base/time';
import {formatBytesIec} from '../../base/bytes_format';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, LONG_NULL, NUM, STR} from '../../trace_processor/query_result';
import type {FlamegraphState} from '../../widgets/flamegraph';
import {profileDescriptor} from './common';
import {HeapProfileFlamegraphDetailsPanel} from './heap_profile_details_panel';

export function createHeapProfileTrack(
  trace: Trace,
  uri: string,
  tableName: string,
  upid: number,
  heapProfileIsIncomplete: boolean,
  detailsPanelState: FlamegraphState | undefined,
  onDetailsPanelStateChange: (state: FlamegraphState) => void,
  onNodeSelected?: (args: {
    pathHashes: string;
    isDominator: boolean;
    upid: number;
    ts: time;
  }) => void,
) {
  return SliceTrack.create({
    trace,
    uri,
    dataset: new SourceDataset({
      src: tableName,
      schema: {
        ts: LONG,
        dur: LONG,
        type: STR,
        id: NUM,
        // Byte totals for the profiling interval (null for non heap-profile
        // events like java heap graphs and OOME callstacks).
        retained: LONG_NULL,
        allocated: LONG_NULL,
        delta: LONG_NULL,
      },
      filter: {col: 'upid', eq: upid},
    }),
    detailsPanel: (row) => {
      const ts = Time.fromRaw(row.ts);
      const tsEnd = Time.fromRaw(row.ts + row.dur);
      const descriptor = profileDescriptor(row.type);
      return new HeapProfileFlamegraphDetailsPanel(
        trace,
        heapProfileIsIncomplete,
        upid,
        descriptor,
        ts,
        tsEnd,
        detailsPanelState,
        onDetailsPanelStateChange,
        onNodeSelected,
      );
    },
    sliceName: (row) => intervalSliceName(row),
    tooltip: (slice) => intervalTooltip(slice.row),
    colorizer: (slice) => {
      return materialColorScheme(slice.ts.toString());
    },
  });
}

interface IntervalRow {
  readonly type: string;
  readonly retained: bigint | null;
  readonly allocated: bigint | null;
  readonly delta: bigint | null;
}

// The label rendered on the interval slice: the three byte totals so the
// retained/allocated/delta breakdown is visible without selecting the slice.
function intervalSliceName(row: IntervalRow): string {
  if (row.retained === null || row.allocated === null || row.delta === null) {
    return '';
  }
  const retained = formatBytesIec(Number(row.retained));
  const allocated = formatBytesIec(Number(row.allocated));
  const delta = formatSignedBytes(Number(row.delta));
  return `Retained ${retained}, Allocated ${allocated}, Delta ${delta}`;
}

function intervalTooltip(row: IntervalRow): m.Children {
  if (row.retained === null || row.allocated === null || row.delta === null) {
    return row.type;
  }
  // Retained first: it is the figure users care about most (live memory
  // attributable to this interval).
  return m('.pf-heap-profile-interval-tooltip', [
    m(
      '.pf-heap-profile-interval-tooltip__heading',
      profileDescriptor(row.type).label,
    ),
    intervalTooltipMetric(
      'Retained',
      formatBytesIec(Number(row.retained)),
      'Allocated in this interval and still live at the dump.',
    ),
    intervalTooltipMetric(
      'Allocated',
      formatBytesIec(Number(row.allocated)),
      'Total bytes allocated during this interval.',
    ),
    intervalTooltipMetric(
      'Delta',
      formatSignedBytes(Number(row.delta)),
      'Allocations minus all frees in the interval, including frees of ' +
        'memory allocated before it, so it can differ from Retained.',
    ),
  ]);
}

function intervalTooltipMetric(
  label: string,
  value: string,
  description: string,
): m.Child {
  return m(
    '.pf-heap-profile-interval-tooltip__metric',
    m(
      'div',
      m('span.pf-heap-profile-interval-tooltip__metric-label', `${label}: `),
      value,
    ),
    m('.pf-heap-profile-interval-tooltip__metric-description', description),
  );
}

function formatSignedBytes(bytes: number): string {
  const formatted = formatBytesIec(bytes);
  return bytes > 0 ? `+${formatted}` : formatted;
}
