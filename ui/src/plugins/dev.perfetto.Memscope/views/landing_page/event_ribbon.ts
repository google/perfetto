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

// Shared Trace Event Time Ribbon: plots smaps snapshots (green sparkline +
// dots), Java heap dumps (interactive blue diamonds), and heapprofd native
// samples (orange ticks) on a single trace-wide time axis so engineers can see
// how all three memory scopes line up in time and jump straight to any dump or
// snapshot.

import m from 'mithril';
import {AsyncMemo} from '../../../../base/async_memo';
import {Time, type time} from '../../../../base/time';
import type {Trace} from '../../../../public/trace';
import {LONG, NUM} from '../../../../trace_processor/query_result';
import type {HeapDump} from '../../../com.android.HeapDumpExplorer/types';
import {formatBytes} from './mem_format';
import {type MemSelection, nearestByTs} from './selection';
import './event_ribbon.scss';

interface SmapsPoint {
  readonly ts: time;
  readonly bytes: number;
}

interface RibbonData {
  readonly smaps: ReadonlyArray<SmapsPoint>;
  readonly nativeTs: ReadonlyArray<time>;
}

async function loadRibbonData(trace: Trace, upid: number): Promise<RibbonData> {
  const smaps: SmapsPoint[] = [];
  const smapsRes = await trace.engine.query(`
    SELECT
      s.ts AS ts,
      CAST(ifnull(SUM(s.rss_kb + s.swap_kb), 0) * 1024 AS INT) AS bytes
    FROM profiler_smaps s
    WHERE s.upid = ${upid}
    GROUP BY s.ts
    ORDER BY s.ts ASC
  `);
  for (
    const it = smapsRes.iter({ts: LONG, bytes: NUM});
    it.valid();
    it.next()
  ) {
    smaps.push({ts: Time.fromRaw(it.ts), bytes: it.bytes});
  }

  const nativeTs: time[] = [];
  const nativeRes = await trace.engine.query(`
    SELECT DISTINCT a.ts AS ts
    FROM heap_profile_allocation a
    WHERE a.upid = ${upid}
    ORDER BY a.ts ASC
  `);
  for (const it = nativeRes.iter({ts: LONG}); it.valid(); it.next()) {
    nativeTs.push(Time.fromRaw(it.ts));
  }

  return {smaps, nativeTs};
}

export interface EventTimeRibbonAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly procDumps: ReadonlyArray<HeapDump>;
  readonly activeDump: HeapDump | null;
  readonly selection?: MemSelection;
  readonly onSelectSmapsTs: (ts: time) => void;
  readonly onSelectHeapDump: (dump: HeapDump) => void;
}

const SVG_W = 340;
const SVG_H = 30;
const PAD_X = 10;
const TRACK_Y = 23;
const TOP_Y = 5;

export class EventTimeRibbon implements m.ClassComponent<EventTimeRibbonAttrs> {
  private readonly slot = new AsyncMemo<RibbonData>();

  onremove() {
    this.slot.dispose();
  }

  view({attrs}: m.Vnode<EventTimeRibbonAttrs>): m.Children {
    const {
      trace,
      upid,
      procDumps,
      activeDump,
      selection,
      onSelectSmapsTs,
      onSelectHeapDump,
    } = attrs;

    const data = this.slot.use({
      key: {traceId: trace.traceInfo.uuid, upid},
      compute: () => loadRibbonData(trace, upid),
    }).data;

    const smaps = data?.smaps ?? [];
    const nativeTs = data?.nativeTs ?? [];
    const hasAnything =
      smaps.length > 0 || procDumps.length > 0 || nativeTs.length > 0;
    if (!hasAnything) return null;

    const traceStart = trace.traceInfo.start;
    const traceEnd = trace.traceInfo.end;
    const spanNs = Math.max(1, Number(traceEnd - traceStart));
    const durationS = spanNs / 1e9;

    const xOf = (ts: time): number => {
      const frac = Math.min(1, Math.max(0, Number(ts - traceStart) / spanNs));
      return PAD_X + frac * (SVG_W - 2 * PAD_X);
    };

    const offsetLabel = (ts: time): string =>
      `${(Number(ts - traceStart) / 1e9).toFixed(1)}s`;

    const maxBytes = smaps.reduce((mVal, p) => Math.max(mVal, p.bytes), 0);
    const yOfBytes = (bytes: number): number => {
      if (maxBytes <= 0) return TRACK_Y - 6;
      const frac = bytes / maxBytes;
      return TRACK_Y - frac * (TRACK_Y - TOP_Y);
    };

    // Build smaps area + line sparkline paths when >= 2 snapshots exist.
    let linePath = '';
    let areaPath = '';
    if (smaps.length >= 2) {
      const coords = smaps.map((p) => ({
        x: xOf(p.ts),
        y: yOfBytes(p.bytes),
      }));
      const first = coords[0];
      const last = coords[coords.length - 1];
      if (first !== undefined && last !== undefined) {
        linePath = coords
          .map(
            (c, i) =>
              `${i === 0 ? 'M' : 'L'}${c.x.toFixed(1)},${c.y.toFixed(1)}`,
          )
          .join(' ');
        areaPath =
          `${linePath} L${last.x.toFixed(1)},${TRACK_Y} ` +
          `L${first.x.toFixed(1)},${TRACK_Y} Z`;
      }
    }

    const selSnap =
      selection !== undefined && smaps.length > 0
        ? nearestByTs(smaps, selection.sel)
        : undefined;
    const baseSnap =
      selection?.base !== undefined && smaps.length > 0
        ? nearestByTs(smaps, selection.base)
        : undefined;

    const showCompareBand =
      selSnap !== undefined &&
      baseSnap !== undefined &&
      selSnap.ts !== baseSnap.ts;
    const bandX1 = showCompareBand
      ? Math.min(xOf(baseSnap.ts), xOf(selSnap.ts))
      : 0;
    const bandX2 = showCompareBand
      ? Math.max(xOf(baseSnap.ts), xOf(selSnap.ts))
      : 0;

    return m('.pf-memwb-ribbon', [
      m('span.pf-memwb-ribbon__bound', '0s'),
      m(
        'svg.pf-memwb-ribbon__svg',
        {
          'viewBox': `0 0 ${SVG_W} ${SVG_H}`,
          'role': 'img',
          'aria-label': 'Process memory capture timeline',
        },
        [
          // Baseline axis
          m('line.pf-memwb-ribbon__axis', {
            x1: PAD_X,
            y1: TRACK_Y,
            x2: SVG_W - PAD_X,
            y2: TRACK_Y,
          }),

          // Comparison range band (when brushing two snapshots on overview)
          showCompareBand &&
            m('rect.pf-memwb-ribbon__band', {
              x: bandX1,
              y: TOP_Y - 2,
              width: Math.max(2, bandX2 - bandX1),
              height: TRACK_Y - TOP_Y + 3,
              rx: 2,
            }),

          // Smaps area + line sparkline
          areaPath !== '' && m('path.pf-memwb-ribbon__area', {d: areaPath}),
          linePath !== '' && m('path.pf-memwb-ribbon__line', {d: linePath}),

          // Native heapprofd sample ticks along bottom of axis
          nativeTs.map((ts, i) => {
            const x = xOf(ts);
            return m(
              'line.pf-memwb-ribbon__native-tick',
              {
                key: `n-${ts.toString()}`,
                x1: x,
                y1: TRACK_Y + 1,
                x2: x,
                y2: TRACK_Y + 5,
              },
              m('title', `heapprofd sample #${i + 1} · t=${offsetLabel(ts)}`),
            );
          }),

          // Smaps snapshot markers (green dots)
          smaps.map((p, i) => {
            const x = xOf(p.ts);
            const y = yOfBytes(p.bytes);
            const isSel = selSnap?.ts === p.ts;
            const isBase = showCompareBand && baseSnap?.ts === p.ts;
            return m(
              'circle.pf-memwb-ribbon__smaps-dot',
              {
                key: `s-${p.ts.toString()}`,
                cx: x,
                cy: y,
                r: isSel || isBase ? 4 : 2.6,
                class: isSel
                  ? 'pf-memwb-ribbon__smaps-dot--sel'
                  : isBase
                    ? 'pf-memwb-ribbon__smaps-dot--base'
                    : undefined,
                onclick: () => onSelectSmapsTs(p.ts),
              },
              m(
                'title',
                `Smaps #${i + 1} · t=${offsetLabel(p.ts)} · ${formatBytes(p.bytes)} — click to pin`,
              ),
            );
          }),

          // Java heap dump markers (interactive blue diamond pins)
          procDumps.map((d, i) => {
            const x = xOf(d.ts);
            const y = 13;
            const isActive =
              activeDump !== null &&
              activeDump.upid === d.upid &&
              activeDump.ts === d.ts;
            const r = isActive ? 5.5 : 4.2;
            const points = `${x},${y - r} ${x + r},${y} ${x},${y + r} ${x - r},${y}`;
            return m(
              'g.pf-memwb-ribbon__dump-pin',
              {
                key: `d-${d.ts.toString()}`,
                class: isActive
                  ? 'pf-memwb-ribbon__dump-pin--active'
                  : undefined,
                onclick: () => onSelectHeapDump(d),
              },
              [
                m('line.pf-memwb-ribbon__dump-stem', {
                  x1: x,
                  y1: y + r,
                  x2: x,
                  y2: TRACK_Y,
                }),
                isActive &&
                  m('circle.pf-memwb-ribbon__dump-halo', {
                    cx: x,
                    cy: y,
                    r: r + 2.5,
                  }),
                m('polygon.pf-memwb-ribbon__dump-diamond', {points}),
                m(
                  'title',
                  `Java Heap Dump #${i + 1} · t=${offsetLabel(d.ts)} — click to inspect`,
                ),
              ],
            );
          }),
        ],
      ),
      m('span.pf-memwb-ribbon__bound', `${durationS.toFixed(1)}s`),
    ]);
  }
}
