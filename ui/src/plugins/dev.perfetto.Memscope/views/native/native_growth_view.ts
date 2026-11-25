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

import './native.scss';
import m from 'mithril';
import {AsyncMemo} from '../../../../base/async_memo';
import {formatBytesIec} from '../../../../base/bytes_format';
import {ProportionBar} from '../../../../components/widgets/charts/proportion_bar';
import {
  LineChartSvg,
  type LineChartSeries,
} from '../../../../components/widgets/charts_svg/line_chart_svg';
import type {Trace} from '../../../../public/trace';
import {Intent} from '../../../../widgets/common';
import {Stack} from '../../../../widgets/stack';
import {BillboardStrip} from '../../components/billboard';
import {Callout} from '../../components/callout';
import {Page, SubPage} from '../../components/page';
import {Panel} from '../../components/panel';
import {deltaText, statCard} from '../landing_page/mem_format';
import {loadingPanel, topTable} from '../landing_page/section_widgets';
import {
  fmtNativeBytes,
  fmtNativeDelta,
  loadNativeGrowthData,
  loadNativeLibraries,
  type NativeGrowthData,
  type NativeLibraryRow,
  type NativeProfileMeta,
  type NativeWindow,
} from './native_queries';
import {renderNativeToolbar} from './native_toolbar';

const SERIES_COLORS = [
  '#4285f4',
  '#ea4335',
  '#fbbc04',
  '#34a853',
  '#a142f4',
  '#24c1e0',
];

export interface NativeGrowthViewAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly meta: NativeProfileMeta;
  readonly window: NativeWindow;
  readonly onWindowChange: (next: NativeWindow) => void;
  readonly onOpenFlamegraph: (filter: string) => void;
}

export class NativeGrowthView implements m.ClassComponent<NativeGrowthViewAttrs> {
  private readonly memo = new AsyncMemo<{
    readonly growth: NativeGrowthData;
    readonly libs: ReadonlyArray<NativeLibraryRow>;
  }>();

  onremove() {
    this.memo.dispose();
  }

  view({attrs}: m.CVnode<NativeGrowthViewAttrs>): m.Children {
    const {
      trace,
      upid,
      meta,
      window: win,
      onWindowChange,
      onOpenFlamegraph,
    } = attrs;

    const data = this.memo.use({
      key: {
        traceId: trace.traceInfo.uuid,
        upid,
        heap: win.heapName,
      },
      compute: async () => {
        const [growth, libs] = await Promise.all([
          loadNativeGrowthData(trace, upid, win.heapName),
          loadNativeLibraries(trace, upid, {heapName: win.heapName}, 10),
        ]);
        return {growth, libs};
      },
    }).data;

    if (data === undefined) {
      return m(
        Page,
        m(
          SubPage,
          renderNativeToolbar({
            trace,
            meta,
            window: win,
            onChange: onWindowChange,
          }),
          loadingPanel({title: 'Native Growth & Churn'}),
        ),
      );
    }

    const {growth, libs} = data;

    // Single-dump fallback: show library composition + churn breakdown cleanly.
    if (growth.intervals.length < 2) {
      const totalUnrel = libs.reduce((a, l) => a + l.unreleasedBytes, 0);
      return m(
        Page,
        m(
          SubPage,
          renderNativeToolbar({
            trace,
            meta,
            window: win,
            onChange: onWindowChange,
          }),
          m(
            Panel,
            m(Panel.Header, {
              title: 'Native Growth & Churn',
              subtitle:
                'Single heapprofd snapshot — continuous dumps are needed for time-series growth rates.',
            }),
            m(
              Panel.Body,
              m(Stack, {spacing: 'large'}, [
                m(
                  Callout,
                  {intent: Intent.Primary},
                  'This trace contains 1 native heap profile dump. Record with continuous dumps (enabled by default in ',
                  m('code', 'memscope record'),
                  ' at 5s intervals) to plot monotonic leaks and allocation churn rate over time.',
                ),
                totalUnrel > 0 &&
                  m(ProportionBar, {
                    segments: libs.slice(0, 6).map((l, idx) => ({
                      label: l.shortName,
                      weight: l.unreleasedBytes,
                      color: SERIES_COLORS[idx % SERIES_COLORS.length]!,
                      value: `${fmtNativeBytes(l.unreleasedBytes)} (${l.freedPct.toFixed(0)}% freed)`,
                    })),
                  }),
              ]),
            ),
          ),
        ),
      );
    }

    const first = growth.intervals[0]!;
    const last = growth.intervals[growth.intervals.length - 1]!;
    const netDelta = last.cumUnreleasedBytes - first.cumUnreleasedBytes;
    const spanS = Math.max(0.001, last.relS - first.relS);
    const totalAlloc = growth.intervals.reduce((a, iv) => a + iv.allocBytes, 0);
    const totalFree = growth.intervals.reduce((a, iv) => a + iv.freeBytes, 0);
    const ratePerHour = (netDelta / spanS) * 3600;
    const allocRatePerS = totalAlloc / spanS;

    const libChartSeries: LineChartSeries[] = growth.topLibraries.map(
      (lib, idx) => ({
        name: lib.shortName,
        color: SERIES_COLORS[idx % SERIES_COLORS.length],
        points: lib.points.map((p) => ({x: p.x, y: p.y / 1024})),
      }),
    );

    const churnSeries: LineChartSeries[] = [
      {
        name: 'Allocated / dump',
        color: '#ea4335',
        points: growth.intervals.map((iv) => ({
          x: iv.relS,
          y: iv.allocBytes / 1024,
        })),
      },
      {
        name: 'Freed / dump',
        color: '#34a853',
        points: growth.intervals.map((iv) => ({
          x: iv.relS,
          y: iv.freeBytes / 1024,
        })),
      },
      {
        name: 'Cumulative Unreleased',
        color: '#4285f4',
        points: growth.intervals.map((iv) => ({
          x: iv.relS,
          y: iv.cumUnreleasedBytes / 1024,
        })),
      },
    ];

    return m(
      Page,
      m(
        SubPage,
        renderNativeToolbar({
          trace,
          meta,
          window: win,
          onChange: onWindowChange,
        }),
        m(
          Panel,
          m(Panel.Header, {
            title: 'Growth & Churn Over Time',
            subtitle: `${growth.intervals.length} continuous heapprofd snapshots over ${spanS.toFixed(1)}s`,
          }),
          m(
            Panel.Body,
            m(Stack, {spacing: 'large'}, [
              m(BillboardStrip, [
                statCard([
                  {
                    label: 'Net Unreleased Δ',
                    value: deltaText(netDelta, fmtNativeDelta(netDelta)),
                    sub: `${fmtNativeBytes(first.cumUnreleasedBytes)} → ${fmtNativeBytes(last.cumUnreleasedBytes)}`,
                  },
                ]),
                statCard([
                  {
                    label: 'Projected Leak Rate',
                    value: deltaText(
                      netDelta,
                      `${fmtNativeDelta(ratePerHour)}/h`,
                    ),
                    sub: `over ${spanS.toFixed(1)}s`,
                  },
                ]),
                statCard([
                  {
                    label: 'Gross Alloc Rate',
                    value: `${fmtNativeBytes(allocRatePerS)}/s`,
                    sub: `${fmtNativeBytes(totalAlloc)} alloc · ${fmtNativeBytes(totalFree)} freed`,
                  },
                ]),
              ]),
              m('.pf-memscope-native-grid2', [
                libChartSeries.length > 0 &&
                  m(
                    Panel,
                    m(Panel.Header, {
                      title: 'Cumulative Unreleased by Library (.so)',
                    }),
                    m(
                      Panel.Body,
                      m(LineChartSvg, {
                        data: {series: libChartSeries},
                        height: 240,
                        showLegend: true,
                        showPoints: true,
                        gridLines: 'both',
                        formatXValue: (v: number) => `${v.toFixed(1)}s`,
                        formatYValue: (v: number) => formatBytesIec(v * 1024),
                      }),
                    ),
                  ),
                m(
                  Panel,
                  m(Panel.Header, {
                    title: 'Allocation vs Free Churn per Dump',
                  }),
                  m(
                    Panel.Body,
                    m(LineChartSvg, {
                      data: {series: churnSeries},
                      height: 240,
                      showLegend: true,
                      showPoints: true,
                      gridLines: 'both',
                      formatXValue: (v: number) => `${v.toFixed(1)}s`,
                      formatYValue: (v: number) => formatBytesIec(v * 1024),
                    }),
                  ),
                ),
              ]),
              growth.growingCallsites.length > 0 &&
                topTable({
                  title: 'Fastest-Growing Callsites (First → Last Dump)',
                  cols: [
                    {label: 'Callsite'},
                    {label: 'First Dump', num: true},
                    {label: 'Last Dump', num: true},
                    {label: 'Net Δ', num: true},
                    {label: 'Rate / h', num: true},
                    {label: 'Total Alloc', num: true},
                  ],
                  rows: growth.growingCallsites.slice(0, 15).map((c) => [
                    m('div', [
                      m(
                        'a',
                        {
                          href: 'javascript:void(0)',
                          onclick: (e: Event) => {
                            e.preventDefault();
                            onOpenFlamegraph(c.leafFrame);
                          },
                        },
                        m('code', c.leafFrame),
                      ),
                      m('.pf-memscope-nlist__sub', c.shortMapping),
                    ]),
                    fmtNativeBytes(c.firstBytes),
                    fmtNativeBytes(c.lastBytes),
                    deltaText(c.deltaBytes, fmtNativeDelta(c.deltaBytes)),
                    c.ratePerHour !== undefined
                      ? deltaText(
                          c.deltaBytes,
                          `${fmtNativeDelta(c.ratePerHour)}/h`,
                        )
                      : '—',
                    fmtNativeBytes(c.totalAllocBytes),
                  ]),
                }),
            ]),
          ),
        ),
      ),
    );
  }
}
