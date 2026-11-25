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

import './domains.scss';
import m from 'mithril';
import {AsyncMemo} from '../../../../base/async_memo';
import {formatBytesIec} from '../../../../base/bytes_format';
import {classNames} from '../../../../base/classnames';
import {
  LineChartSvg,
  type LineChartData,
} from '../../../../components/widgets/charts_svg/line_chart_svg';
import type {Trace} from '../../../../public/trace';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {Icon} from '../../../../widgets/icon';
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import {BillboardStrip} from '../../components/billboard';
import {Page, SubPage} from '../../components/page';
import {Panel} from '../../components/panel';
import {Table} from '../../components/table';
import {niceKbInterval} from '../../utils';
import {deltaText, statCard} from '../landing_page/mem_format';
import {loadingPanel} from '../landing_page/section_widgets';
import {
  FILE_BUCKETS,
  loadGraphicsCounters,
  loadProcessThreads,
  loadSmapsCategoryDetail,
  type CounterSeriesSummary,
  type FileBucketKey,
  type ProcessThreadsData,
  type SmapsCategoryDetail,
  type SmapsMappingRow,
} from './domain_queries';

const fmtBytes = (n: number) => formatBytesIec(n);
const fmtSignedBytes = (n: number) =>
  n === 0 ? '±0 B' : `${n > 0 ? '+' : '-'}${formatBytesIec(Math.abs(n))}`;
const kpi = (label: string, value: string, sub?: string) =>
  statCard([{label, value, sub}]);

function shortPath(p: string): {name: string; dir?: string} {
  const idx = p.lastIndexOf('/');
  return idx < 0 || idx === p.length - 1
    ? {name: p}
    : {name: p.slice(idx + 1), dir: p.slice(0, idx)};
}

function renderShareBar(share: number, color: string): m.Children {
  return m('.pf-memscope-dom__share-cell', [
    m(
      '.pf-memscope-dom__share-track',
      m('.pf-memscope-dom__share-fill', {
        style: {width: `${share}%`, background: color},
      }),
    ),
    m('span', `${share}%`),
  ]);
}

function renderSnapshotSelect(
  trace: Trace,
  detail: SmapsCategoryDetail,
  onSelect: (ts: bigint) => void,
): m.Children {
  if (detail.snapshots.length <= 1) return null;
  const active =
    detail.selectedTs ?? detail.snapshots[detail.snapshots.length - 1]!.ts;
  return m('.pf-memscope-dom__snap-select', [
    m('span.pf-memscope-dom__snap-label', 'Snapshot:'),
    m(
      Select,
      {
        value: active.toString(),
        onchange: (e: Event) =>
          onSelect(BigInt((e.target as HTMLSelectElement).value)),
      },
      detail.snapshots.map((s, i) => {
        const sec = Number(s.ts - trace.traceInfo.start) / 1e9;
        return m(
          'option',
          {value: s.ts.toString()},
          `#${i + 1} · ${sec.toFixed(1)}s (${fmtBytes(s.rssSwap)})`,
        );
      }),
    ),
  ]);
}

function renderSmapsButton(onOpenSmaps?: () => void): m.Children {
  return (
    onOpenSmaps &&
    m(Button, {
      label: 'Full Smaps',
      icon: 'table_rows',
      variant: ButtonVariant.Outlined,
      compact: true,
      onclick: onOpenSmaps,
    })
  );
}

function renderMappingsPanel(opts: {
  readonly title: string;
  readonly rows: ReadonlyArray<SmapsMappingRow>;
  readonly showBucket?: boolean;
  readonly showDelta?: boolean;
  readonly filter: string;
  readonly placeholder?: string;
  readonly onFilter: (v: string) => void;
  readonly extraControls?: m.Children;
  readonly accentColor: string;
  readonly totalBytes: number;
  readonly emptyText: string;
}): m.Children {
  const needle = opts.filter.trim().toLowerCase();
  const filtered =
    needle === ''
      ? opts.rows
      : opts.rows.filter((r) => r.path.toLowerCase().includes(needle));
  const bucketMeta = new Map(FILE_BUCKETS.map((b) => [b.key, b]));

  return m(
    Panel,
    m(Panel.Header, {
      title: opts.title,
      controls: [
        opts.extraControls,
        m(TextInput, {
          placeholder: opts.placeholder ?? 'Filter mapping…',
          leftIcon: 'search',
          value: opts.filter,
          onInput: opts.onFilter,
        }),
      ],
    }),
    m(
      Panel.Body,
      filtered.length === 0
        ? m('.pf-memscope-placeholder', opts.emptyText)
        : m(Table, [
            m(
              'thead',
              m('tr', [
                m('th', 'Mapping Path'),
                opts.showBucket && m('th', 'Type'),
                m('th.num', 'RSS + Swap'),
                m('th.num', 'PSS'),
                m('th.num', 'Private Dirty'),
                m('th.num', 'Clean'),
                opts.showDelta && m('th.num', 'Delta'),
                m('th.num', 'Share'),
              ]),
            ),
            m(
              'tbody',
              filtered.slice(0, 150).map((r) => {
                const sp = shortPath(r.path);
                const share =
                  opts.totalBytes > 0
                    ? Math.min(
                        100,
                        Math.round((r.rssSwap / opts.totalBytes) * 100),
                      )
                    : 0;
                const bm = r.fileBucket
                  ? bucketMeta.get(r.fileBucket)
                  : undefined;
                return m('tr', {key: r.path}, [
                  m('td.pf-memscope-dom__path-cell', {title: r.path}, [
                    m('div.pf-memscope-dom__path-name', sp.name),
                    sp.dir && m('div.pf-memscope-dom__path-dir', sp.dir),
                  ]),
                  opts.showBucket &&
                    m(
                      'td',
                      bm &&
                        m('span.pf-memscope-dom__bucket-pill', [
                          m('span.pf-memscope-dom__dot', {
                            style: {background: bm.color},
                          }),
                          bm.shortLabel,
                        ]),
                    ),
                  m('td.num', fmtBytes(r.rssSwap)),
                  m('td.num', fmtBytes(r.pss)),
                  m('td.num', fmtBytes(r.privateDirty)),
                  m('td.num', fmtBytes(r.clean)),
                  opts.showDelta &&
                    m(
                      'td.num',
                      r.deltaBytes !== undefined
                        ? deltaText(r.deltaBytes)
                        : '—',
                    ),
                  m('td.num', renderShareBar(share, opts.accentColor)),
                ]);
              }),
            ),
          ]),
    ),
  );
}

function renderTrendPanel(
  title: string,
  snapshots: ReadonlyArray<{x: number; rssSwap: number}>,
  label: string,
  color: string,
): m.Children {
  if (snapshots.length < 2) return null;
  const maxBytes = Math.max(0, ...snapshots.map((s) => s.rssSwap));
  return m(
    Panel,
    m(Panel.Header, {title}),
    m(
      Panel.Body,
      m(LineChartSvg, {
        data: {
          series: [
            {
              name: label,
              color,
              points: snapshots.map((s) => ({x: s.x, y: s.rssSwap})),
            },
          ],
        },
        height: 165,
        showLegend: false,
        showPoints: true,
        gridLines: 'both',
        xAxisLabel: 'Time (s)',
        yAxisLabel: 'RSS + Swap',
        formatXValue: (v: number) => `${v.toFixed(1)}s`,
        formatYValue: fmtBytes,
        yAxisMinInterval: niceKbInterval(maxBytes / 1024) * 1024,
      }),
    ),
  );
}

function renderNoSmapsCard(
  title: string,
  color: string,
  description: string,
  processName: string,
  onBack?: () => void,
): m.Children {
  return m('.pf-memscope-domain-empty', [
    m('.pf-memscope-domain-empty__card', [
      m('.pf-memscope-domain-empty__head', [
        m('span.pf-memscope-guide__dot', {style: {background: color}}),
        m('h2', `No ${title.toLowerCase()} snapshots in this trace`),
      ]),
      m('p', description),
      m(
        'code.pf-memscope-domain-empty__cmd',
        `memscope record --app ${processName}`,
      ),
      onBack &&
        m(
          '.pf-memscope-domain-empty__actions',
          m(Button, {
            label: 'Back to Overview',
            icon: 'arrow_back',
            variant: ButtonVariant.Outlined,
            onclick: onBack,
          }),
        ),
    ]),
  ]);
}

// ---------------------------------------------------------------------------
// 1. File-backed Memory Domain (Java .oat/.art, Native .so, APKs/Resources)
// ---------------------------------------------------------------------------

export interface CompositionDomainViewAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly processName: string;
  readonly hasJavaDump?: boolean;
  readonly onOpenJavaBitmaps?: () => void;
  readonly onOpenSmaps?: () => void;
  readonly onOpenOverview?: () => void;
}

export class FileBackedDomainView implements m.ClassComponent<CompositionDomainViewAttrs> {
  private readonly memo = new AsyncMemo<SmapsCategoryDetail>();
  private selectedTs?: bigint;
  private activeBucket: 'all' | FileBucketKey = 'all';
  private filter = '';

  onremove() {
    this.memo.dispose();
  }

  view({attrs}: m.CVnode<CompositionDomainViewAttrs>): m.Children {
    const {trace, upid, onOpenSmaps} = attrs;
    const data = this.memo.use({
      key: {
        traceId: trace.traceInfo.uuid,
        upid,
        ts: this.selectedTs?.toString() ?? 'latest',
      },
      compute: () =>
        loadSmapsCategoryDetail(trace, upid, 'file', this.selectedTs),
    }).data;

    if (data === undefined) {
      return m(Page, m(SubPage, loadingPanel({title: 'File-backed Memory'})));
    }
    if (data.snapshots.length === 0) {
      return m(
        Page,
        m(
          SubPage,
          renderNoSmapsCard(
            'File-backed Memory',
            '#34a853',
            'File-backed memory (mapped .so libraries, .oat/.vdex/.art compiled Java code, and APKs) is measured from /proc/<pid>/smaps snapshots.',
            attrs.processName,
            attrs.onOpenOverview,
          ),
        ),
      );
    }

    const {totals, rows, snapshots} = data;
    const bucketTotals = new Map<FileBucketKey, number>();
    for (const r of rows) {
      if (r.fileBucket) {
        bucketTotals.set(
          r.fileBucket,
          (bucketTotals.get(r.fileBucket) ?? 0) + r.rssSwap,
        );
      }
    }
    const filteredRows =
      this.activeBucket === 'all'
        ? rows
        : rows.filter((r) => r.fileBucket === this.activeBucket);

    const chartData: LineChartData | undefined =
      snapshots.length >= 2
        ? {
            series: FILE_BUCKETS.map((b) => ({
              name: b.shortLabel,
              color: b.color,
              points: snapshots.map((s) => ({
                x: s.x,
                y: s.byFileBucket?.get(b.key) ?? 0,
              })),
            })).filter((s) => s.points.some((p) => p.y > 0)),
          }
        : undefined;

    return m(
      Page,
      m(SubPage, [
        m(
          Panel,
          m(Panel.Header, {
            title: 'File-backed Memory',
            subtitle:
              'Mapped code & files (.oat/.vdex/.art Java code, .so libraries, APKs, mmap files). Clean pages are kernel-evictable; Private Dirty pages consume dedicated RAM.',
            controls: [
              renderSnapshotSelect(trace, data, (ts) => {
                this.selectedTs = ts;
              }),
              renderSmapsButton(onOpenSmaps),
            ],
          }),
          m(Panel.Body, [
            m(BillboardStrip, [
              kpi(
                'File RSS + Swap',
                fmtBytes(totals.rssSwap),
                `${rows.length} mapped files`,
              ),
              kpi(
                'Clean (Evictable)',
                fmtBytes(totals.clean),
                totals.rssSwap > 0
                  ? `${Math.round((totals.clean / totals.rssSwap) * 100)}% reclaimable`
                  : 'page cache',
              ),
              kpi(
                'Private Dirty',
                fmtBytes(totals.privateDirty),
                'non-evictable private cost',
              ),
              kpi(
                'Proportional (PSS)',
                fmtBytes(totals.pss),
                'shared-adjusted RAM',
              ),
              totals.deltaBytes !== undefined &&
                kpi(
                  'Delta (First → Sel)',
                  fmtSignedBytes(totals.deltaBytes),
                  `${snapshots.length} snapshots`,
                ),
            ]),
            totals.rssSwap > 0 &&
              m('.pf-memscope-dom__bucket-strip', [
                m(
                  '.pf-memscope-dom__bar',
                  FILE_BUCKETS.filter(
                    (b) => (bucketTotals.get(b.key) ?? 0) > 0,
                  ).map((b) => {
                    const v = bucketTotals.get(b.key) ?? 0;
                    const pct = (v / totals.rssSwap) * 100;
                    return m('.pf-memscope-dom__bar-seg', {
                      key: b.key,
                      style: {width: `${pct}%`, background: b.color},
                      title: `${b.label}: ${fmtBytes(v)} (${pct.toFixed(1)}%)`,
                    });
                  }),
                ),
                m(
                  '.pf-memscope-dom__legend',
                  FILE_BUCKETS.map((b) =>
                    m(
                      'button.pf-memscope-dom__legend-item',
                      {
                        key: b.key,
                        type: 'button',
                        className: classNames(
                          this.activeBucket === b.key &&
                            'pf-memscope-dom__legend-item--active',
                        ),
                        onclick: () => {
                          this.activeBucket =
                            this.activeBucket === b.key ? 'all' : b.key;
                        },
                      },
                      [
                        m('span.pf-memscope-dom__dot', {
                          style: {background: b.color},
                        }),
                        m('span', b.shortLabel),
                        m('strong', fmtBytes(bucketTotals.get(b.key) ?? 0)),
                      ],
                    ),
                  ),
                ),
              ]),
          ]),
        ),
        chartData !== undefined &&
          chartData.series.length > 0 &&
          m(
            Panel,
            m(Panel.Header, {
              title: 'File-backed Breakdown Over Time',
              subtitle:
                'Stacked RSS + Swap by file type across smaps snapshots.',
            }),
            m(
              Panel.Body,
              m(LineChartSvg, {
                data: chartData,
                height: 165,
                stacked: true,
                showLegend: true,
                showPoints: true,
                gridLines: 'vertical',
                xAxisLabel: 'Time (s)',
                yAxisLabel: 'Size',
                formatXValue: (v: number) => `${v.toFixed(1)}s`,
                formatYValue: fmtBytes,
                yAxisMinInterval:
                  niceKbInterval(
                    Math.max(0, ...snapshots.map((s) => s.rssSwap)) / 1024,
                  ) * 1024,
              }),
            ),
          ),
        renderMappingsPanel({
          title: `Mapped Files (${filteredRows.length})`,
          rows: filteredRows,
          showBucket: true,
          showDelta: snapshots.length >= 2,
          filter: this.filter,
          placeholder: 'Filter file path…',
          onFilter: (v) => {
            this.filter = v;
          },
          extraControls: m('.pf-memscope-dom__filter-pills', [
            m(
              'button.pf-memscope-dom__fpill',
              {
                key: 'all',
                type: 'button',
                className: classNames(
                  this.activeBucket === 'all' &&
                    'pf-memscope-dom__fpill--active',
                ),
                onclick: () => {
                  this.activeBucket = 'all';
                },
              },
              `All (${rows.length})`,
            ),
            ...FILE_BUCKETS.filter((b) =>
              rows.some((r) => r.fileBucket === b.key),
            ).map((b) =>
              m(
                'button.pf-memscope-dom__fpill',
                {
                  key: b.key,
                  type: 'button',
                  className: classNames(
                    this.activeBucket === b.key &&
                      'pf-memscope-dom__fpill--active',
                  ),
                  onclick: () => {
                    this.activeBucket = b.key;
                  },
                },
                `${b.shortLabel} (${rows.filter((r) => r.fileBucket === b.key).length})`,
              ),
            ),
          ]),
          accentColor: '#34a853',
          totalBytes: totals.rssSwap,
          emptyText: 'No file-backed mappings match the current filter.',
        }),
      ]),
    );
  }
}

// ---------------------------------------------------------------------------
// 2. Graphics & GPU Memory Domain (DMA-BUF, GPU driver mappings, Bitmap link)
// ---------------------------------------------------------------------------

interface GraphicsBundle {
  readonly smaps: SmapsCategoryDetail;
  readonly counters: ReadonlyArray<CounterSeriesSummary>;
}

export class GraphicsDomainView implements m.ClassComponent<CompositionDomainViewAttrs> {
  private readonly memo = new AsyncMemo<GraphicsBundle>();
  private selectedTs?: bigint;
  private filter = '';

  onremove() {
    this.memo.dispose();
  }

  view({attrs}: m.CVnode<CompositionDomainViewAttrs>): m.Children {
    const {trace, upid, hasJavaDump, onOpenJavaBitmaps} = attrs;
    const data = this.memo.use({
      key: {
        traceId: trace.traceInfo.uuid,
        upid,
        ts: this.selectedTs?.toString() ?? 'latest',
      },
      compute: async () => {
        const [smaps, counters] = await Promise.all([
          loadSmapsCategoryDetail(trace, upid, 'graphics', this.selectedTs),
          loadGraphicsCounters(trace, upid),
        ]);
        return {smaps, counters};
      },
    }).data;

    if (data === undefined) {
      return m(
        Page,
        m(SubPage, loadingPanel({title: 'Graphics & GPU Memory'})),
      );
    }

    const {smaps, counters} = data;
    const hasAnyGfx = smaps.totals.rssSwap > 0 || counters.length > 0;
    const dmabuf = counters.find((c) => c.name === 'mem.dmabuf_rss');
    const gpuMem = counters.find((c) => c.name === 'gpu_memory');
    const counterChart: LineChartData | undefined = counters.some(
      (c) => c.points.length >= 2,
    )
      ? {
          series: counters
            .filter((c) => c.points.length >= 2)
            .map((c) => ({name: c.label, color: c.color, points: c.points})),
        }
      : undefined;

    return m(
      Page,
      m(SubPage, [
        m(
          Panel,
          m(Panel.Header, {
            title: 'Graphics & GPU Memory',
            subtitle:
              'GPU driver mappings (/dev/kgsl, /dev/mali, /dev/dri), DMA-BUF surfaces, and shared graphics buffers. Often invisible to heapprofd/java_hprof.',
            controls: [
              renderSnapshotSelect(trace, smaps, (ts) => {
                this.selectedTs = ts;
              }),
              hasJavaDump &&
                onOpenJavaBitmaps &&
                m(Button, {
                  label: 'Inspect Java Bitmaps',
                  icon: 'image',
                  rightIcon: 'arrow_forward',
                  variant: ButtonVariant.Outlined,
                  compact: true,
                  onclick: onOpenJavaBitmaps,
                }),
            ],
          }),
          m(Panel.Body, [
            m(BillboardStrip, [
              kpi(
                'Graphics Smaps',
                fmtBytes(smaps.totals.rssSwap),
                `${smaps.rows.length} GPU / dmabuf VMAs`,
              ),
              dmabuf &&
                kpi(
                  'DMA-BUF RSS',
                  fmtBytes(dmabuf.latest),
                  `peak ${fmtBytes(dmabuf.peak)}`,
                ),
              gpuMem &&
                kpi(
                  'GPU Memory',
                  fmtBytes(gpuMem.latest),
                  `peak ${fmtBytes(gpuMem.peak)}`,
                ),
              kpi(
                'Private Dirty',
                fmtBytes(smaps.totals.privateDirty),
                'private GPU pages',
              ),
              smaps.totals.deltaBytes !== undefined &&
                kpi(
                  'Smaps Delta',
                  fmtSignedBytes(smaps.totals.deltaBytes),
                  `${smaps.snapshots.length} snapshots`,
                ),
            ]),
            !hasAnyGfx &&
              m('.pf-memscope-dom__callout', [
                m(Icon, {icon: 'info'}),
                m('div', [
                  m(
                    'strong',
                    'No Graphics / DMA-BUF memory recorded for this process. ',
                  ),
                  'GPU driver mappings appear when the app allocates Vulkan/EGL/SurfaceFlinger surfaces and ',
                  m('code', 'profiler_smaps'),
                  ' or ',
                  m('code', 'android.gpu.memory'),
                  ' / ',
                  m('code', 'mem.dmabuf_rss'),
                  ' counters are enabled.',
                  hasJavaDump &&
                    ' Note: Android Bitmap pixel buffers backed by native/ashmem can also be inspected under Java → Bitmaps.',
                ]),
              ]),
          ]),
        ),
        counterChart &&
          m(
            Panel,
            m(Panel.Header, {
              title: 'Graphics & DMA-BUF Counters Over Time',
              subtitle: 'Process-level GPU & DMA-BUF resident memory counters.',
            }),
            m(
              Panel.Body,
              m(LineChartSvg, {
                data: counterChart,
                height: 165,
                showLegend: true,
                showPoints: false,
                gridLines: 'both',
                xAxisLabel: 'Time (s)',
                yAxisLabel: 'Size',
                formatXValue: (v: number) => `${v.toFixed(1)}s`,
                formatYValue: fmtBytes,
                yAxisMinInterval:
                  niceKbInterval(
                    Math.max(0, ...counters.map((c) => c.peak)) / 1024,
                  ) * 1024,
              }),
            ),
          ),
        smaps.totals.rssSwap > 0 &&
          renderTrendPanel(
            'Graphics Smaps Over Time',
            smaps.snapshots,
            'Graphics',
            '#a142f4',
          ),
        smaps.rows.length > 0 &&
          renderMappingsPanel({
            title: `Graphics Mappings (${smaps.rows.length})`,
            rows: smaps.rows,
            showDelta: smaps.snapshots.length >= 2,
            filter: this.filter,
            onFilter: (v) => {
              this.filter = v;
            },
            accentColor: '#a142f4',
            totalBytes: smaps.totals.rssSwap,
            emptyText: 'No graphics mappings match the filter.',
          }),
      ]),
    );
  }
}

// ---------------------------------------------------------------------------
// 3. Thread Stacks Domain (smaps [stack]/[anon:stack*] + thread-pool leak view)
// ---------------------------------------------------------------------------

interface StacksBundle {
  readonly smaps: SmapsCategoryDetail;
  readonly threads: ProcessThreadsData;
}

export class StacksDomainView implements m.ClassComponent<CompositionDomainViewAttrs> {
  private readonly memo = new AsyncMemo<StacksBundle>();
  private selectedTs?: bigint;
  private filter = '';

  onremove() {
    this.memo.dispose();
  }

  view({attrs}: m.CVnode<CompositionDomainViewAttrs>): m.Children {
    const {trace, upid} = attrs;
    const data = this.memo.use({
      key: {
        traceId: trace.traceInfo.uuid,
        upid,
        ts: this.selectedTs?.toString() ?? 'latest',
      },
      compute: async () => {
        const [smaps, threads] = await Promise.all([
          loadSmapsCategoryDetail(trace, upid, 'stack', this.selectedTs),
          loadProcessThreads(trace, upid),
        ]);
        return {smaps, threads};
      },
    }).data;

    if (data === undefined) {
      return m(Page, m(SubPage, loadingPanel({title: 'Thread Stacks'})));
    }

    const {smaps, threads} = data;
    const avgStackBytes =
      smaps.totals.vmaCount > 0
        ? Math.round(smaps.totals.rssSwap / smaps.totals.vmaCount)
        : 0;

    return m(
      Page,
      m(SubPage, [
        m(
          Panel,
          m(Panel.Header, {
            title: 'Thread Stacks',
            subtitle:
              'Physical anonymous RAM backing thread stacks ([stack], [anon:stack*]). Growing stack RSS or large thread pools point to leaked threads.',
            controls: renderSnapshotSelect(trace, smaps, (ts) => {
              this.selectedTs = ts;
            }),
          }),
          m(
            Panel.Body,
            m(BillboardStrip, [
              kpi(
                'Stack RSS + Swap',
                fmtBytes(smaps.totals.rssSwap),
                `${smaps.totals.vmaCount} ${smaps.totals.vmaCount === 1 ? 'stack VMA' : 'stack VMAs'}`,
              ),
              kpi(
                'Avg Stack Footprint',
                avgStackBytes > 0 ? fmtBytes(avgStackBytes) : '—',
                'per stack mapping',
              ),
              kpi(
                'Recorded Threads',
                String(threads.totalThreads),
                `${threads.pools.length} ${threads.pools.length === 1 ? 'thread group' : 'thread groups'}`,
              ),
              smaps.totals.deltaBytes !== undefined &&
                kpi(
                  'Stack Delta',
                  fmtSignedBytes(smaps.totals.deltaBytes),
                  `${smaps.snapshots.length} snapshots`,
                ),
            ]),
          ),
        ),
        renderTrendPanel(
          'Thread Stack Memory Over Time',
          smaps.snapshots,
          'Thread stacks',
          '#26c6da',
        ),
        threads.pools.length > 0 &&
          m(
            Panel,
            m(Panel.Header, {
              title: `Thread Pools & Groups (${threads.totalThreads} ${threads.totalThreads === 1 ? 'thread' : 'threads'})`,
              subtitle:
                'Threads from the trace grouped by name prefix to spot unbounded executor or HandlerThread pools.',
            }),
            m(
              Panel.Body,
              m(Table, [
                m(
                  'thead',
                  m('tr', [
                    m('th', 'Thread / Pool'),
                    m('th', 'Sample Names'),
                    m('th.num', 'Threads'),
                    m('th.num', 'Share'),
                  ]),
                ),
                m(
                  'tbody',
                  threads.pools.slice(0, 60).map((p) => {
                    const share =
                      threads.totalThreads > 0
                        ? Math.round((p.count / threads.totalThreads) * 100)
                        : 0;
                    return m('tr', {key: p.poolName}, [
                      m('td', [
                        m('span.pf-memscope-dom__path-name', p.poolName),
                        p.hasMain && m('span.pf-memscope-dom__tag', 'main'),
                      ]),
                      m(
                        'td.pf-memscope-dom__path-dir',
                        p.sampleNames.join(', '),
                      ),
                      m('td.num', String(p.count)),
                      m('td.num', renderShareBar(share, '#26c6da')),
                    ]);
                  }),
                ),
              ]),
            ),
          ),
        renderMappingsPanel({
          title: `Stack Smaps Mappings (${smaps.rows.length})`,
          rows: smaps.rows,
          showDelta: smaps.snapshots.length >= 2,
          filter: this.filter,
          onFilter: (v) => {
            this.filter = v;
          },
          accentColor: '#26c6da',
          totalBytes: smaps.totals.rssSwap,
          emptyText: 'No stack smaps mappings recorded for this process.',
        }),
      ]),
    );
  }
}

// ---------------------------------------------------------------------------
// 4 & 5. Simple Smaps Domains (Other, Native smaps fallback, Java smaps fallback)
// ---------------------------------------------------------------------------

interface SimpleSmapsDomainSpec {
  readonly category: 'other' | 'native' | 'java';
  readonly title: string;
  readonly subtitle: string;
  readonly accent: string;
  readonly rssLabel: string;
  readonly trendTitle: string;
  readonly trendLabel: string;
  readonly tableTitle: (count: number) => string;
  readonly emptyText: string;
  readonly callout?: (processName: string) => m.Children;
}

const SIMPLE_DOMAIN_SPECS: Record<
  'other' | 'native' | 'java',
  SimpleSmapsDomainSpec
> = {
  other: {
    category: 'other',
    title: 'Other Mappings',
    subtitle:
      'Anonymous and special kernel VMAs ([anon:...], JIT caches, binder, ashmem, vdso) not bucketed into Native, Java, File-backed, Graphics, or Stacks.',
    accent: '#9aa0a6',
    rssLabel: 'Other RSS + Swap',
    trendTitle: 'Other Memory Over Time',
    trendLabel: 'Other',
    tableTitle: (n) => `Mappings (${n})`,
    emptyText: 'No other mappings recorded for this process.',
  },
  native: {
    category: 'native',
    title: 'Native Memory (Smaps)',
    subtitle:
      'Showing physical smaps native allocator arenas (scudo, libc_malloc, jemalloc, [heap]).',
    accent: '#4285f4',
    rssLabel: 'Native Arena RSS+Swap',
    trendTitle: 'Native Arena Memory Over Time',
    trendLabel: 'Native',
    tableTitle: (n) => `Native Arena Mappings (${n})`,
    emptyText: 'No mappings match the filter.',
    callout: (proc) => [
      m('strong', 'No native heap profile (heapprofd) in this trace. '),
      'Smaps shows total native arena residency below, but callstacks and .so attribution require heapprofd. Record with ',
      m('code', `memscope record --app ${proc}`),
      '.',
    ],
  },
  java: {
    category: 'java',
    title: 'Java Memory (Smaps)',
    subtitle:
      'Showing physical smaps Java/ART heap spaces (dalvik-main, large object, zygote, non-moving).',
    accent: '#f4b400',
    rssLabel: 'Java/ART RSS+Swap',
    trendTitle: 'Java/ART Arena Memory Over Time',
    trendLabel: 'Java',
    tableTitle: (n) => `Java / ART Mappings (${n})`,
    emptyText: 'No mappings match the filter.',
    callout: (proc) => [
      m('strong', 'No Java heap dump (java_hprof) in this trace. '),
      'Smaps shows ART/Dalvik space residency below, but object/dominator/Bitmap inspection requires java_hprof. Record with ',
      m('code', `memscope record --app ${proc}`),
      '.',
    ],
  },
};

class SimpleSmapsDomainView implements m.ClassComponent<{
  readonly trace: Trace;
  readonly upid: number;
  readonly domain: 'other' | 'native' | 'java';
  readonly processName: string;
  readonly onOpenSmaps?: () => void;
}> {
  private readonly memo = new AsyncMemo<SmapsCategoryDetail>();
  private selectedTs?: bigint;
  private filter = '';

  onremove() {
    this.memo.dispose();
  }

  view({
    attrs,
  }: m.CVnode<{
    readonly trace: Trace;
    readonly upid: number;
    readonly domain: 'other' | 'native' | 'java';
    readonly processName: string;
    readonly onOpenSmaps?: () => void;
  }>): m.Children {
    const {trace, upid, domain, processName, onOpenSmaps} = attrs;
    const spec = SIMPLE_DOMAIN_SPECS[domain];
    const data = this.memo.use({
      key: {
        traceId: trace.traceInfo.uuid,
        upid,
        domain,
        ts: this.selectedTs?.toString() ?? 'latest',
      },
      compute: () =>
        loadSmapsCategoryDetail(trace, upid, spec.category, this.selectedTs),
    }).data;

    if (data === undefined) {
      return m(Page, m(SubPage, loadingPanel({title: spec.title})));
    }

    return m(
      Page,
      m(SubPage, [
        m(
          Panel,
          m(Panel.Header, {
            title: spec.title,
            subtitle: spec.subtitle,
            controls: [
              renderSnapshotSelect(trace, data, (ts) => {
                this.selectedTs = ts;
              }),
              renderSmapsButton(onOpenSmaps),
            ],
          }),
          m(Panel.Body, [
            spec.callout &&
              m('.pf-memscope-dom__callout', [
                m('span.pf-memscope-dom__dot', {
                  style: {background: spec.accent},
                }),
                m('div', spec.callout(processName)),
              ]),
            m(BillboardStrip, [
              kpi(
                spec.rssLabel,
                fmtBytes(data.totals.rssSwap),
                `${data.rows.length} mappings`,
              ),
              kpi(
                'Private Dirty',
                fmtBytes(data.totals.privateDirty),
                'private anonymous RAM',
              ),
              kpi('Swap', fmtBytes(data.totals.swap), 'swapped / zram'),
              data.totals.deltaBytes !== undefined &&
                kpi(
                  'Delta (First → Sel)',
                  fmtSignedBytes(data.totals.deltaBytes),
                  `${data.snapshots.length} snapshots`,
                ),
            ]),
          ]),
        ),
        renderTrendPanel(
          spec.trendTitle,
          data.snapshots,
          spec.trendLabel,
          spec.accent,
        ),
        renderMappingsPanel({
          title: spec.tableTitle(data.rows.length),
          rows: data.rows,
          showDelta: data.snapshots.length >= 2,
          filter: this.filter,
          onFilter: (v) => {
            this.filter = v;
          },
          accentColor: spec.accent,
          totalBytes: data.totals.rssSwap,
          emptyText: spec.emptyText,
        }),
      ]),
    );
  }
}

export class OtherMemDomainView implements m.ClassComponent<CompositionDomainViewAttrs> {
  view({attrs}: m.CVnode<CompositionDomainViewAttrs>): m.Children {
    return m(SimpleSmapsDomainView, {
      trace: attrs.trace,
      upid: attrs.upid,
      domain: 'other',
      processName: attrs.processName,
      onOpenSmaps: attrs.onOpenSmaps,
    });
  }
}

export interface SmapsFallbackDomainViewAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly domain: 'native' | 'java';
  readonly processName: string;
  readonly onOpenSmaps?: () => void;
}

export class SmapsFallbackDomainView implements m.ClassComponent<SmapsFallbackDomainViewAttrs> {
  view({attrs}: m.CVnode<SmapsFallbackDomainViewAttrs>): m.Children {
    return m(SimpleSmapsDomainView, attrs);
  }
}
