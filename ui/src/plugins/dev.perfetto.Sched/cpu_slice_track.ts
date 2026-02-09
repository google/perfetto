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
import {colorForThread} from '../../components/colorizer';
import {SliceTrack, ColorVariant} from '../../components/tracks/slice_track';
import {LONG, NUM} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {ThreadMap} from '../dev.perfetto.Thread/threads';
import {SourceDataset} from '../../trace_processor/dataset';
import {RECT_PATTERN_HATCHED} from '../../base/renderer';
import {SchedSliceDetailsPanel} from './sched_details_tab';

const MARGIN_TOP = 3;
const RECT_HEIGHT = 24;

const CPU_SLICE_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  utid: NUM,
  pid: LONG,
  priority: NUM,
  depth: 0,
} as const;

type CpuSliceRow = typeof CPU_SLICE_SCHEMA;

export function createCpuSliceTrack(
  trace: Trace,
  uri: string,
  tableName: string,
  ucpu: number,
  threads: ThreadMap,
): SliceTrack<CpuSliceRow> {
  return SliceTrack.create({
    trace,
    uri,
    rootTableName: 'sched_slice',

    dataset: () =>
      new SourceDataset({
        src: tableName,
        schema: CPU_SLICE_SCHEMA,
        filter: {
          col: 'ucpu',
          eq: ucpu,
        },
      }),

    sliceLayout: {
      padding: MARGIN_TOP,
      sliceHeight: RECT_HEIGHT,
    },

    colorizer(row) {
      const threadInfo = threads.get(row.utid);
      return colorForThread(threadInfo);
    },

    sliceName(row) {
      const threadInfo = threads.get(row.utid);
      if (!threadInfo) {
        return `[utid:${row.utid}]`;
      }
      if (threadInfo.pid !== undefined && threadInfo.pid !== 0n) {
        let procName = threadInfo.procName ?? '';
        if (procName.startsWith('/')) {
          procName = procName.substring(procName.lastIndexOf('/') + 1);
        }
        return `${procName} [${threadInfo.pid}]`;
      }
      return `${threadInfo.threadName} [${threadInfo.tid}]`;
    },

    sliceSubtitle(row) {
      const threadInfo = threads.get(row.utid);
      const isRealtime = row.priority < 100;
      if (!threadInfo) {
        return isRealtime ? '(RT)' : '';
      }
      if (threadInfo.pid !== undefined && threadInfo.pid !== 0n) {
        const suffix = isRealtime ? ' (RT)' : '';
        return `${threadInfo.threadName} [${threadInfo.tid}]${suffix}`;
      }
      return isRealtime ? '(RT)' : '';
    },

    slicePattern(row) {
      return row.priority < 100 ? RECT_PATTERN_HATCHED : 0;
    },

    onUpdatedSlices(slices) {
      const timeline = trace.timeline;
      const hoveredUtid = timeline.hoveredUtid;
      const hoveredPid = timeline.hoveredPid;
      const isHovering = hoveredUtid !== undefined;
      const n = slices.length;
      const variants = new Array<ColorVariant>(n);

      for (let i = 0; i < n; ++i) {
        const row = slices[i].row;
        const isThreadHovered = hoveredUtid === row.utid;
        const isProcessHovered = hoveredPid === row.pid;

        if (isHovering && !isThreadHovered) {
          variants[i] = isProcessHovered
            ? ColorVariant.VARIANT
            : ColorVariant.DISABLED;
        } else {
          variants[i] = ColorVariant.BASE;
        }
      }
      return variants;
    },

    onSliceOver({slice}) {
      const threadInfo = threads.get(slice.row.utid);
      trace.timeline.hoveredUtid = slice.row.utid;
      trace.timeline.hoveredPid = threadInfo?.pid;
    },

    onSliceOut() {
      trace.timeline.hoveredUtid = undefined;
      trace.timeline.hoveredPid = undefined;
    },

    tooltip(slice) {
      const threadInfo = threads.get(slice.row.utid);
      if (!threadInfo) {
        return undefined;
      }

      const tidText = `T: ${threadInfo.threadName} [${threadInfo.tid}]`;
      const countDiv =
        slice.count > 1 && m('div', `and ${slice.count - 1} other events`);

      if (threadInfo.pid !== undefined) {
        const pidText = `P: ${threadInfo.procName} [${threadInfo.pid}]`;
        return m('.tooltip', [m('div', pidText), m('div', tidText), countDiv]);
      }
      return m('.tooltip', tidText, countDiv);
    },

    detailsPanel() {
      return new SchedSliceDetailsPanel(trace, threads);
    },
  });
}
