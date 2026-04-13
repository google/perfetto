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

// Trace source preview panel.
//
// When a TraceSliceSource node is selected on the rack canvas, this
// component renders a timeline visualization of the slices it would
// match over the current render window. The visualization mirrors the
// signal that TP's PopulateTraceSources would produce:
//
// - GATE:    horizontal blocks covering the duration of each slice
// - TRIGGER: a vertical tick at the start of each slice
// - DENSITY: a heat strip proportional to the number of overlapping
//            slices at each column
//
// The query is constrained to the current trace.timeline.visibleWindow
// so we don't pathologically over-fetch on traces with huge ranges.

import m from 'mithril';
import protos from '../../protos';
import {Trace} from '../../public/trace';
import {NUM, LONG} from '../../trace_processor/query_result';

export interface TraceSourcePreviewAttrs {
  trace: Trace;
  module: protos.ISynthModule;
}

interface SliceRow {
  ts: number;       // ns since the visible-window start
  dur: number;      // ns
}

interface PreviewData {
  slices: SliceRow[];
  rangeStart: number;  // ns, zero-based
  rangeEnd: number;    // ns, zero-based
  totalSlices: number;
}

export class TraceSourcePreview
  implements m.ClassComponent<TraceSourcePreviewAttrs> {
  private data: PreviewData | null = null;
  private loading = false;
  private lastQueriedKey = '';
  private error: string | null = null;

  view(vnode: m.Vnode<TraceSourcePreviewAttrs>) {
    const {trace, module} = vnode.attrs;
    const cfg = module.traceSliceSource;
    if (!cfg) {
      return m('.trace-source-preview-empty',
        {style: {padding: '12px', color: '#888'}},
        'Not a trace source.');
    }

    const glob = cfg.trackNameGlob ?? '*';
    const sliceNameGlob = cfg.sliceNameGlob ?? '';
    const maxDepth = cfg.maxDepth ?? 0;
    const signalType = cfg.signalType ?? 0;

    // Query key — re-fetch when any of these change.
    const visible = trace.timeline.visibleWindow.toTimeSpan();
    const startTs = Number(visible.start);
    const endTs = Number(visible.end);
    const key = [
      module.id, glob, sliceNameGlob, maxDepth, startTs, endTs,
    ].join('|');
    if (key !== this.lastQueriedKey) {
      this.lastQueriedKey = key;
      void this.refetch(trace, glob, sliceNameGlob, maxDepth,
                        startTs, endTs);
    }

    return m('.trace-source-preview', {
      style: {
        padding: '10px 16px',
        height: '100%',
        overflowY: 'auto',
        fontSize: '12px',
        fontFamily: 'Roboto, sans-serif',
      },
    },
      this.renderHeader(module, glob, sliceNameGlob, maxDepth, signalType),
      this.renderTimeline(signalType, startTs, endTs),
      this.renderStats(startTs, endTs),
      this.renderLegend(signalType),
    );
  }

  private renderHeader(
    module: protos.ISynthModule,
    glob: string,
    sliceNameGlob: string,
    maxDepth: number,
    signalType: number,
  ): m.Child {
    const signalName = ['Gate', 'Trigger', 'Density'][signalType] ?? '?';
    return m('.trace-source-header', {
      style: {
        display: 'flex',
        alignItems: 'center',
        gap: '10px',
        marginBottom: '10px',
        paddingBottom: '8px',
        borderBottom: '1px solid #e0e0e0',
      },
    },
      m('span', {
        style: {
          fontSize: '14px', fontWeight: 'bold',
          color: 'hsl(140, 60%, 30%)',
        },
      }, 'Trace Source Preview'),
      m('span', {style: {color: '#999'}}, '·'),
      m('span',
        {style: {fontFamily: 'monospace', color: '#333'}},
        module.id ?? ''),
      m('.spacer', {style: {flex: '1'}}),
      m('span', {style: {color: '#666'}},
        `Glob: `,
        m('code', {style: {background: '#f0f0f0', padding: '1px 4px'}},
          glob || '*'),
      ),
      sliceNameGlob
        ? m('span', {style: {color: '#666'}},
            `Slice: `,
            m('code', {style: {background: '#f0f0f0', padding: '1px 4px'}},
              sliceNameGlob))
        : null,
      maxDepth > 0
        ? m('span', {style: {color: '#666'}}, `Max depth: ${maxDepth}`)
        : null,
      m('span', {
        style: {
          color: 'white',
          background: 'hsl(140, 60%, 35%)',
          padding: '2px 8px',
          borderRadius: '3px',
          fontWeight: 'bold',
        },
      }, `Signal: ${signalName}`),
    );
  }

  private renderTimeline(
    signalType: number, startTs: number, endTs: number,
  ): m.Child {
    return m('.trace-source-timeline-wrap', {
      style: {
        position: 'relative',
        background: '#fafafa',
        border: '1px solid #ddd',
        borderRadius: '3px',
        height: '110px',
        marginBottom: '10px',
      },
    },
      m('canvas.trace-source-canvas', {
        style: {
          display: 'block',
          width: '100%',
          height: '100%',
        },
        oncreate: (v: m.VnodeDOM) =>
          this.drawCanvas(v.dom as HTMLCanvasElement,
                          signalType, startTs, endTs),
        onupdate: (v: m.VnodeDOM) =>
          this.drawCanvas(v.dom as HTMLCanvasElement,
                          signalType, startTs, endTs),
      }),
      this.loading
        ? m('.loading-overlay', {
            style: {
              position: 'absolute',
              top: '0', left: '0', right: '0', bottom: '0',
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              color: '#888',
              background: 'rgba(250, 250, 250, 0.85)',
            },
          }, 'Loading slices…')
        : null,
      this.error
        ? m('.error-overlay', {
            style: {
              position: 'absolute',
              top: '0', left: '0', right: '0', bottom: '0',
              display: 'flex',
              alignItems: 'center',
              justifyContent: 'center',
              color: '#c62828',
              background: 'rgba(255, 235, 238, 0.9)',
              padding: '8px',
              fontSize: '11px',
            },
          }, `Error: ${this.error}`)
        : null,
    );
  }

  private renderStats(startTs: number, endTs: number): m.Child {
    const durNs = endTs - startTs;
    const durMs = durNs / 1e6;
    const durStr = durMs >= 1000
      ? `${(durMs / 1000).toFixed(2)} s`
      : `${durMs.toFixed(1)} ms`;
    const sliceCount = this.data?.slices.length ?? 0;
    const totalCount = this.data?.totalSlices ?? sliceCount;
    const rate = durNs > 0 && sliceCount > 0
      ? (sliceCount * 1e9 / durNs).toFixed(1)
      : '-';
    return m('.trace-source-stats', {
      style: {
        display: 'flex',
        gap: '18px',
        fontSize: '11px',
        color: '#555',
        marginBottom: '10px',
      },
    },
      m('span', `Window: `,
        m('strong', {style: {color: '#333'}}, durStr)),
      m('span', `Slices: `,
        m('strong', {style: {color: '#333'}}, `${sliceCount}`),
        totalCount > sliceCount
          ? ` (truncated from ${totalCount})`
          : ''),
      m('span', `Rate: `,
        m('strong', {style: {color: '#333'}}, `${rate} /s`)),
    );
  }

  private renderLegend(signalType: number): m.Child {
    const explanations = [
      // GATE
      'GATE is high (1.0) whenever any matching slice is active. ' +
      'Rising edge = slice starts, falling edge = slice ends. ' +
      'Drives ADSR envelopes so each slice becomes one note whose ' +
      'length matches the slice duration.',
      // TRIGGER
      'TRIGGER is a single-sample 1.0 impulse at each slice START. ' +
      'Duration is ignored. Used for percussion: one hit per slice, ' +
      'regardless of slice length. ' +
      '(Not yet implemented in TP — shown for design reference.)',
      // DENSITY
      'DENSITY is a continuous 0..1 CV proportional to the count of ' +
      'overlapping active slices. Used as a modulation source: filter ' +
      'cutoff, drive, LFO depth, etc. ' +
      '(Not yet implemented in TP — shown for design reference.)',
    ];
    return m('.trace-source-legend', {
      style: {
        fontSize: '11px',
        color: '#555',
        lineHeight: '1.5',
        padding: '8px 10px',
        background: '#f8fafd',
        borderLeft: '3px solid hsl(140, 60%, 50%)',
        borderRadius: '2px',
      },
    },
      m('strong', {style: {color: '#333'}},
        ['Gate', 'Trigger', 'Density'][signalType] ?? 'Signal'),
      ' — ',
      explanations[signalType] ?? '',
    );
  }

  // Redraws the canvas with the current `data` and signal type.
  private drawCanvas(
    canvas: HTMLCanvasElement,
    signalType: number,
    startTs: number,
    endTs: number,
  ) {
    // Size the canvas using its CSS box.
    const rect = canvas.getBoundingClientRect();
    if (rect.width === 0 || rect.height === 0) return;
    const dpr = window.devicePixelRatio || 1;
    const w = Math.round(rect.width * dpr);
    const h = Math.round(rect.height * dpr);
    if (canvas.width !== w) canvas.width = w;
    if (canvas.height !== h) canvas.height = h;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    ctx.clearRect(0, 0, w, h);

    // Background grid.
    ctx.fillStyle = '#fafafa';
    ctx.fillRect(0, 0, w, h);
    ctx.strokeStyle = '#e8e8e8';
    ctx.lineWidth = 1;
    for (let i = 1; i < 10; i++) {
      const x = Math.round((i / 10) * w) + 0.5;
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, h);
      ctx.stroke();
    }
    // Baseline (0.0) and top (1.0).
    const yBase = Math.round(h - 10);
    const yTop = Math.round(10);
    ctx.strokeStyle = '#cccccc';
    ctx.beginPath();
    ctx.moveTo(0, yBase + 0.5);
    ctx.lineTo(w, yBase + 0.5);
    ctx.stroke();

    if (!this.data || this.data.slices.length === 0) {
      ctx.fillStyle = '#999';
      ctx.font = `${12 * dpr}px Roboto, sans-serif`;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText(this.loading
        ? 'Loading…'
        : 'No slices match this glob in the current time window',
        w / 2, h / 2);
      return;
    }

    const durNs = Math.max(1, endTs - startTs);
    const toX = (ns: number) => Math.round((ns / durNs) * w);

    if (signalType === 0) {
      // GATE — paint rectangles from (ts, 0) to (ts+dur, 1).
      ctx.fillStyle = 'hsla(140, 60%, 45%, 0.85)';
      for (const s of this.data.slices) {
        const x1 = toX(s.ts);
        const x2 = Math.max(x1 + 1, toX(s.ts + s.dur));
        ctx.fillRect(x1, yTop, x2 - x1, yBase - yTop);
      }
    } else if (signalType === 1) {
      // TRIGGER — vertical tick at each slice start.
      ctx.strokeStyle = 'hsla(30, 80%, 45%, 0.85)';
      ctx.lineWidth = 2 * dpr;
      for (const s of this.data.slices) {
        const x = toX(s.ts) + 0.5;
        ctx.beginPath();
        ctx.moveTo(x, yTop);
        ctx.lineTo(x, yBase);
        ctx.stroke();
      }
    } else if (signalType === 2) {
      // DENSITY — count overlapping slices per column.
      const cols = Math.max(1, Math.floor(rect.width));
      const counts = new Float32Array(cols);
      for (const s of this.data.slices) {
        const c1 = Math.floor((s.ts / durNs) * cols);
        const c2 = Math.max(c1 + 1,
          Math.floor(((s.ts + s.dur) / durNs) * cols));
        for (let c = c1; c < c2 && c < cols; c++) {
          if (c >= 0) counts[c]++;
        }
      }
      let maxCount = 1;
      for (const c of counts) if (c > maxCount) maxCount = c;
      ctx.fillStyle = 'hsla(220, 60%, 45%, 0.85)';
      for (let c = 0; c < cols; c++) {
        const norm = counts[c] / maxCount;
        const barH = Math.round(norm * (yBase - yTop));
        if (barH > 0) {
          const xp = Math.round((c / cols) * w);
          const xpNext = Math.round(((c + 1) / cols) * w);
          ctx.fillRect(xp, yBase - barH, xpNext - xp, barH);
        }
      }
    }
  }

  private async refetch(
    trace: Trace,
    trackGlob: string,
    sliceNameGlob: string,
    maxDepth: number,
    startTs: number,
    endTs: number,
  ) {
    this.loading = true;
    this.error = null;
    this.data = null;
    m.redraw();

    // Cap the number of slices we fetch so huge windows don't kill us.
    const MAX_SLICES = 20000;

    try {
      const clauses: string[] = [
        's.dur > 0',
        `s.ts >= ${startTs}`,
        `s.ts < ${endTs}`,
      ];
      if (trackGlob && trackGlob !== '*') {
        // Escape single quotes inside the glob value.
        const esc = trackGlob.replace(/'/g, `''`);
        clauses.push(`t.name GLOB '${esc}'`);
      }
      if (sliceNameGlob) {
        const esc = sliceNameGlob.replace(/'/g, `''`);
        clauses.push(`s.name GLOB '${esc}'`);
      }
      if (maxDepth > 0) {
        clauses.push(`s.depth <= ${maxDepth}`);
      }
      const where = clauses.join(' AND ');

      // First: total count (for the "truncated from X" info).
      const totalResult = await trace.engine.query(
        `SELECT count(*) AS cnt FROM slice s
         LEFT JOIN track t ON s.track_id = t.id
         WHERE ${where}`);
      const total = totalResult.firstRow({cnt: NUM}).cnt;

      // Then: up to MAX_SLICES rows.
      const result = await trace.engine.query(
        `SELECT s.ts AS ts, s.dur AS dur FROM slice s
         LEFT JOIN track t ON s.track_id = t.id
         WHERE ${where}
         ORDER BY s.ts
         LIMIT ${MAX_SLICES}`);

      const slices: SliceRow[] = [];
      const it = result.iter({ts: LONG, dur: LONG});
      for (; it.valid(); it.next()) {
        // Normalize: store offsets from startTs so the canvas maps
        // easily to [0, endTs - startTs].
        slices.push({
          ts: Number(it.ts - BigInt(startTs)),
          dur: Number(it.dur),
        });
      }

      this.data = {
        slices,
        rangeStart: 0,
        rangeEnd: endTs - startTs,
        totalSlices: total,
      };
    } catch (e) {
      this.error = String(e);
    } finally {
      this.loading = false;
      m.redraw();
    }
  }
}
