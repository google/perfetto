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
import type {Trace} from '../../../../public/trace';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {Intent} from '../../../../widgets/common';
import {RadioGroup} from '../../../../widgets/radio_group';
import {Stack} from '../../../../widgets/stack';
import {BillboardStrip} from '../../components/billboard';
import {Callout} from '../../components/callout';
import {Page, SubPage} from '../../components/page';
import {Panel} from '../../components/panel';
import {ProgressBar} from '../../components/progress_bar';
import {Ratio} from '../../components/ratio';
import {statCard} from '../landing_page/mem_format';
import {loadingPanel, topTable} from '../landing_page/section_widgets';
import {
  fmtNativeBytes,
  loadNativeCallsites,
  loadNativeLibraries,
  loadNativeWindowTotals,
  type NativeCallsiteRow,
  type NativeLibraryRow,
  type NativeProfileMeta,
  type NativeWindow,
  type NativeWindowTotals,
} from './native_queries';
import {renderNativeToolbar} from './native_toolbar';

interface NativeOverviewData {
  readonly totals: NativeWindowTotals;
  readonly libraries: ReadonlyArray<NativeLibraryRow>;
  readonly callsitesByUnrel: ReadonlyArray<NativeCallsiteRow>;
  readonly callsitesByAlloc: ReadonlyArray<NativeCallsiteRow>;
}

export interface NativeOverviewViewAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly meta: NativeProfileMeta;
  readonly window: NativeWindow;
  readonly onWindowChange: (next: NativeWindow) => void;
  readonly onOpenFlamegraph: (filter?: string) => void;
  readonly onOpenCallsites: () => void;
  readonly onOpenGrowth: () => void;
}

export class NativeOverviewView implements m.ClassComponent<NativeOverviewViewAttrs> {
  private readonly memo = new AsyncMemo<NativeOverviewData>();
  private appOnly = false;
  private callsiteMode: 'unreleased' | 'alloc' = 'unreleased';

  onremove() {
    this.memo.dispose();
  }

  view({attrs}: m.CVnode<NativeOverviewViewAttrs>): m.Children {
    const {
      trace,
      upid,
      meta,
      window: win,
      onWindowChange,
      onOpenFlamegraph,
      onOpenCallsites,
      onOpenGrowth,
    } = attrs;

    const data = this.memo.use({
      key: {
        traceId: trace.traceInfo.uuid,
        upid,
        heap: win.heapName,
        from: win.fromTs?.toString() ?? 'start',
        to: win.toTs?.toString() ?? 'end',
      },
      compute: async () => {
        const [totals, libraries, callsitesByUnrel, callsitesByAlloc] =
          await Promise.all([
            loadNativeWindowTotals(trace, upid, win),
            loadNativeLibraries(trace, upid, win, 20),
            loadNativeCallsites(trace, upid, win, {
              sortBy: 'unreleased',
              limit: 25,
            }),
            loadNativeCallsites(trace, upid, win, {
              sortBy: 'alloc',
              limit: 25,
            }),
          ]);
        return {totals, libraries, callsitesByUnrel, callsitesByAlloc};
      },
    }).data;

    const diffing = win.fromTs !== undefined;

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
          loadingPanel({title: 'Native Memory Overview'}),
        ),
      );
    }

    const {totals, libraries} = data;
    const rawSites =
      this.callsiteMode === 'unreleased'
        ? data.callsitesByUnrel
        : data.callsitesByAlloc;
    const shownSites = (
      this.appOnly ? rawSites.filter((s) => s.isApp) : rawSites
    ).slice(0, 8);
    const shownLibs = (
      this.appOnly ? libraries.filter((l) => l.isApp) : libraries
    ).slice(0, 8);

    const nativeRss = meta.nativeSmapsRss;
    const coveragePct =
      !diffing && nativeRss > 0
        ? Math.min(100, (totals.unreleasedBytes / nativeRss) * 100)
        : undefined;
    const overhead =
      !diffing && nativeRss > totals.unreleasedBytes
        ? nativeRss - totals.unreleasedBytes
        : 0;

    const topLib = libraries[0];
    const topSite = data.callsitesByUnrel[0];
    const headlineFinding = buildHeadline(totals, topLib, topSite, diffing);

    return m(
      Page,
      m(
        SubPage,
        renderNativeToolbar({
          trace,
          meta,
          window: win,
          onChange: onWindowChange,
          trailing: [
            m(Button, {
              label: 'Flamegraph',
              icon: 'local_fire_department',
              variant: ButtonVariant.Outlined,
              compact: true,
              onclick: () => onOpenFlamegraph(),
            }),
            meta.dumpTimestamps.length >= 2 &&
              m(Button, {
                label: 'Growth & Churn',
                icon: 'trending_up',
                variant: ButtonVariant.Minimal,
                compact: true,
                onclick: onOpenGrowth,
              }),
          ],
        }),
        m(
          Panel,
          m(Panel.Header, {
            title: diffing
              ? 'Native Memory · Diff Window'
              : 'Native Memory · Overview',
            subtitle:
              'heapprofd samples malloc/new/free callstacks after profiling starts.',
          }),
          m(
            Panel.Body,
            m(Stack, {spacing: 'large'}, [
              headlineFinding !== undefined &&
                m(Callout, {intent: Intent.Primary}, headlineFinding),
              m(BillboardStrip, [
                !diffing &&
                  nativeRss > 0 &&
                  statCard([
                    {
                      label: 'Smaps Native RSS+Swap',
                      value: fmtNativeBytes(nativeRss),
                      sub: 'physical allocator arenas',
                    },
                  ]),
                statCard([
                  {
                    label: diffing
                      ? 'Net Unreleased Δ'
                      : 'Unreleased (Retained)',
                    value: fmtNativeBytes(totals.unreleasedBytes),
                    sub: `${totals.unreleasedCount.toLocaleString()} live allocs`,
                  },
                ]),
                statCard([
                  {
                    label: 'Total Allocated (Churn)',
                    value: fmtNativeBytes(totals.totalAllocBytes),
                    sub: `${totals.totalAllocCount.toLocaleString()} allocs · ${totals.freedPct.toFixed(0)}% freed`,
                  },
                ]),
                !diffing &&
                  overhead > 0 &&
                  statCard([
                    {
                      label: 'Unseen + Overhead',
                      value: fmtNativeBytes(overhead),
                      sub: 'pre-trace allocs & fragmentation',
                    },
                  ]),
                meta.threadStackBytes > 0 &&
                  statCard([
                    {
                      label: 'Thread Stacks',
                      value: fmtNativeBytes(meta.threadStackBytes),
                      sub: `${meta.threadCount} threads`,
                    },
                  ]),
              ]),
              coveragePct !== undefined &&
                m(Ratio, {
                  label: 'Profiler coverage vs Smaps Native RSS',
                  tooltip:
                    'heapprofd only sees allocations made after profiling started. Unseen bytes are pre-trace allocations plus allocator metadata/fragmentation.',
                  pct: coveragePct,
                  headline: 'of native RSS explained by heapprofd',
                  color: '#4285f4',
                  aLabel: 'Seen by profiler',
                  aBytes: totals.unreleasedBytes,
                  bLabel: 'Pre-trace + overhead',
                  bBytes: overhead,
                }),
              meta.heaps.length > 1 &&
                topTable({
                  title: 'Recorded Native Heaps',
                  cols: [
                    {label: 'Heap name'},
                    {label: 'Dumps', num: true},
                    {label: 'Unreleased', num: true},
                    {label: 'Total Allocated', num: true},
                    {label: 'Freed %', num: true},
                  ],
                  rows: meta.heaps.map((h) => [
                    m('code', h.heapName),
                    `${h.dumps}`,
                    fmtNativeBytes(h.unreleasedBytes),
                    fmtNativeBytes(h.totalAllocBytes),
                    `${(h.totalAllocBytes > 0 ? (Math.max(0, h.totalAllocBytes - h.unreleasedBytes) / h.totalAllocBytes) * 100 : 0).toFixed(0)}%`,
                  ]),
                }),
            ]),
          ),
        ),

        m('.pf-memscope-native-grid2', [
          // Top Libraries (.so)
          m(
            Panel,
            m(Panel.Header, {
              title: 'Top Libraries (.so)',
              controls: m(Button, {
                label: 'All libraries & callsites',
                rightIcon: 'arrow_forward',
                variant: ButtonVariant.Minimal,
                compact: true,
                onclick: onOpenCallsites,
              }),
            }),
            m(
              Panel.Body,
              shownLibs.length === 0
                ? m(
                    '.pf-memscope-placeholder',
                    this.appOnly
                      ? 'No /data/app libraries in this window. Switch to "All frames" to include framework & system .so libraries.'
                      : 'No allocations captured in this window.',
                  )
                : topTable({
                    title: '',
                    cols: [
                      {label: 'Library'},
                      {label: 'Unreleased', num: true},
                      {label: 'Total Alloc', num: true},
                      {label: 'Freed', num: true},
                      {label: 'Share', num: true},
                    ],
                    rows: shownLibs.map((l) => [
                      m(
                        'a',
                        {
                          href: 'javascript:void(0)',
                          title: l.mapping,
                          onclick: (e: Event) => {
                            e.preventDefault();
                            onOpenFlamegraph(l.shortName);
                          },
                        },
                        m('code', l.shortName),
                      ),
                      fmtNativeBytes(l.unreleasedBytes),
                      fmtNativeBytes(l.totalAllocBytes),
                      `${l.freedPct.toFixed(0)}%`,
                      m(ProgressBar, {
                        pct:
                          totals.unreleasedBytes > 0
                            ? (l.unreleasedBytes / totals.unreleasedBytes) * 100
                            : 0,
                      }),
                    ]),
                  }),
            ),
          ),

          // Top Callsites (Unreleased vs Churn, All vs App)
          m(
            Panel,
            m(Panel.Header, {
              title: 'Top Allocation Callsites',
              controls: m(
                Stack,
                {orientation: 'horizontal', spacing: 'small'},
                [
                  m(
                    RadioGroup,
                    {
                      selectedValue: this.callsiteMode,
                      onValueChange: (id: string) => {
                        this.callsiteMode = id as 'unreleased' | 'alloc';
                      },
                    },
                    [
                      m(RadioGroup.Button, {value: 'unreleased'}, 'Unreleased'),
                      m(RadioGroup.Button, {value: 'alloc'}, 'Churn'),
                    ],
                  ),
                  m(Button, {
                    label: this.appOnly ? 'App only' : 'All frames',
                    icon: this.appOnly ? 'filter_alt' : 'layers',
                    variant: ButtonVariant.Outlined,
                    compact: true,
                    onclick: () => {
                      this.appOnly = !this.appOnly;
                    },
                  }),
                ],
              ),
            }),
            m(
              Panel.Body,
              shownSites.length === 0
                ? m(
                    '.pf-memscope-placeholder',
                    this.appOnly
                      ? 'No /data/app callsites in this window — click "App only" to show all frames.'
                      : 'No callsites in this window.',
                  )
                : topTable({
                    title: '',
                    cols: [
                      {label: 'Function'},
                      {
                        label:
                          this.callsiteMode === 'unreleased'
                            ? 'Unreleased'
                            : 'Total Alloc',
                        num: true,
                      },
                      {label: 'Allocs', num: true},
                      {label: 'Freed', num: true},
                    ],
                    rows: shownSites.map((s) => [
                      m('div', {style: {maxWidth: '280px', minWidth: 0}}, [
                        m(
                          'a',
                          {
                            href: 'javascript:void(0)',
                            title: `${s.leafFrame}\n${s.mapping}`,
                            style: {
                              display: 'block',
                              overflow: 'hidden',
                              textOverflow: 'ellipsis',
                              whiteSpace: 'nowrap',
                            },
                            onclick: (e: Event) => {
                              e.preventDefault();
                              onOpenFlamegraph(s.leafFrame);
                            },
                          },
                          m('code', s.leafFrame),
                        ),
                        m('.pf-memscope-nlist__sub', s.shortMapping),
                      ]),
                      fmtNativeBytes(
                        this.callsiteMode === 'unreleased'
                          ? s.unreleasedBytes
                          : s.totalAllocBytes,
                      ),
                      (this.callsiteMode === 'unreleased'
                        ? s.unreleasedCount
                        : s.totalAllocCount
                      ).toLocaleString(),
                      `${s.freedPct.toFixed(0)}%`,
                    ]),
                  }),
            ),
          ),
        ]),
      ),
    );
  }
}

function buildHeadline(
  totals: NativeWindowTotals,
  topLib: NativeLibraryRow | undefined,
  topSite: NativeCallsiteRow | undefined,
  diffing: boolean,
): m.Children | undefined {
  if (totals.totalAllocBytes === 0 && totals.unreleasedBytes === 0) {
    return 'No native allocations occurred in this window.';
  }
  const parts: m.Children[] = [];
  if (topLib !== undefined && totals.unreleasedBytes > 0) {
    const share = Math.round(
      (topLib.unreleasedBytes / totals.unreleasedBytes) * 100,
    );
    parts.push(
      m('strong', topLib.shortName),
      ` holds `,
      m('strong', `${share}%`),
      ` (${fmtNativeBytes(topLib.unreleasedBytes)}) of ${diffing ? 'net unreleased growth' : 'profiled unreleased native memory'}`,
    );
    if (topSite !== undefined) {
      parts.push(', led by ', m('code', topSite.leafFrame), '. ');
    } else {
      parts.push('. ');
    }
  }
  if (totals.freedPct >= 85 && totals.totalAllocBytes > 1024 * 1024) {
    parts.push(
      `High allocation churn: ${fmtNativeBytes(totals.totalAllocBytes)} allocated and ${totals.freedPct.toFixed(0)}% freed — check `,
      m('strong', 'Total Malloc Size'),
      ' in Flamegraph for transient pressure.',
    );
  } else if (totals.unreleasedBytes > 0) {
    parts.push(
      `${totals.freedPct.toFixed(0)}% of ${fmtNativeBytes(totals.totalAllocBytes)} allocated was freed.`,
    );
  }
  return parts.length > 0 ? parts : undefined;
}
