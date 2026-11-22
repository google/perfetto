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
import {AsyncMemo} from '../../../../base/async_memo';
import {formatBytesIec} from '../../../../base/bytes_format';
import {classNames} from '../../../../base/classnames';
import type {time} from '../../../../base/time';
import type {Trace} from '../../../../public/trace';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {Intent} from '../../../../widgets/common';
import {Panel} from '../../components/panel';
import {deltaText} from '../landing_page/mem_format';
import {
  loadMemorySunburstData,
  MemorySunburst,
  type MemorySunburstData,
  type SunburstDomainId,
} from './memory_sunburst';
import './domains.scss';

export type MemoryAreaId = SunburstDomainId;

export interface MemoryAreaQuickLink {
  readonly label: string;
  readonly icon?: string;
  readonly onclick: () => void;
}

export interface MemoryAreaCardModel {
  readonly id: MemoryAreaId;
  readonly label: string;
  readonly color: string;
  readonly bytes?: number;
  readonly totalBytes: number;
  readonly deltaBytes?: number;
  readonly summary: string;
  readonly hasDeepProfiler: boolean;
  readonly primaryHighlight?: boolean;
  readonly onOpen: () => void;
  readonly quickLinks?: ReadonlyArray<MemoryAreaQuickLink>;
}

export interface MemoryAreasPanelAttrs {
  readonly trace: Trace;
  readonly upid: number;
  readonly hasSmaps: boolean;
  readonly hasNative: boolean;
  readonly hasJava: boolean;
  readonly selTs?: time;
  readonly areas: ReadonlyArray<MemoryAreaCardModel>;
  readonly onOpenDomain: (id: MemoryAreaId) => void;
}

const SMALL_SHARE_THRESHOLD = 0.02;

// "Where Memory Went" panel on Overview: pairs an interactive 2-ring sunburst
// chart (inner ring = memory domains, outer ring = sub-buckets / libraries /
// arenas) with a ranked domain breakdown list that stays hover-synced and
// jumps straight into each top-level domain tab on click.
export class MemoryAreasPanel implements m.ClassComponent<MemoryAreasPanelAttrs> {
  private readonly sunburstMemo = new AsyncMemo<MemorySunburstData>();
  private hoveredDomain?: MemoryAreaId;

  onremove() {
    this.sunburstMemo.dispose();
  }

  view({attrs}: m.CVnode<MemoryAreasPanelAttrs>): m.Children {
    const {
      trace,
      upid,
      hasSmaps,
      hasNative,
      hasJava,
      selTs,
      areas,
      onOpenDomain,
    } = attrs;
    if (areas.length === 0) return null;

    const sunburst = this.sunburstMemo.use({
      key: {
        traceId: trace.traceInfo.uuid,
        upid,
        hasSmaps,
        hasNative,
        hasJava,
        selTs: selTs !== undefined ? selTs.toString() : 'latest',
      },
      compute: () =>
        loadMemorySunburstData(trace, upid, {
          hasSmaps,
          hasNative,
          hasJava,
          selTs,
        }),
    }).data;

    // Sort: areas with deep profiler or larger footprint first, unmeasured /
    // zero-byte unprofiled areas last.
    const ranked = [...areas].sort((a, b) => {
      const aBytes = a.bytes ?? 0;
      const bBytes = b.bytes ?? 0;
      const aActive = aBytes > 0 || a.hasDeepProfiler ? 1 : 0;
      const bActive = bBytes > 0 || b.hasDeepProfiler ? 1 : 0;
      if (aActive !== bActive) return bActive - aActive;
      if (bBytes !== aBytes) return bBytes - aBytes;
      return Number(b.hasDeepProfiler) - Number(a.hasDeepProfiler);
    });

    const showSunburst =
      sunburst === undefined ||
      (sunburst.totalBytes > 0 && sunburst.domains.length > 0);

    return m(
      Panel,
      m(Panel.Header, {
        title: 'Where Memory Went',
        subtitle:
          'Inner ring shows top-level memory domains; outer ring breaks each domain into sub-buckets, libraries, or arenas. Click any slice or row to drill in.',
      }),
      m(
        Panel.Body,
        m(
          '.pf-memscope-where-grid',
          {
            className: classNames(
              !showSunburst && 'pf-memscope-where-grid--no-chart',
            ),
          },
          [
            showSunburst &&
              m(
                '.pf-memscope-where-grid__chart',
                m(MemorySunburst, {
                  data: sunburst,
                  height: 330,
                  onOpenDomain,
                  onHoverDomain: (id) => {
                    this.hoveredDomain = id;
                  },
                }),
              ),
            m(
              '.pf-memscope-areas',
              ranked.map((area) => {
                const bytes = area.bytes ?? 0;
                const shareFrac =
                  area.totalBytes > 0 ? bytes / area.totalBytes : 0;
                const sharePct =
                  area.totalBytes > 0 ? Math.round(shareFrac * 100) : undefined;
                const isSmall =
                  !area.hasDeepProfiler &&
                  (bytes === 0 ||
                    (area.totalBytes > 0 && shareFrac < SMALL_SHARE_THRESHOLD));
                const isHighlighted = this.hoveredDomain === area.id;

                if (isSmall) {
                  return m(
                    '.pf-memscope-area.pf-memscope-area--small',
                    {
                      key: area.id,
                      className: classNames(
                        isHighlighted && 'pf-memscope-area--highlighted',
                      ),
                      style: {'--pf-memscope-area-color': area.color},
                      onclick: area.onOpen,
                    },
                    [
                      m('span.pf-memscope-dom__dot', {
                        style: {background: area.color},
                      }),
                      m('span.pf-memscope-area__name', area.label),
                      m(
                        'span.pf-memscope-area__bytes',
                        area.bytes === undefined
                          ? '—'
                          : bytes > 0
                            ? formatBytesIec(bytes)
                            : '0 B',
                      ),
                      sharePct !== undefined &&
                        bytes > 0 &&
                        m(
                          'span.pf-memscope-area__share',
                          shareFrac < 0.01 ? '<1%' : `${sharePct}%`,
                        ),
                      m('span.pf-memscope-area__quiet-summary', area.summary),
                      m(Button, {
                        className: 'pf-memscope-area__small-open',
                        label: 'Open',
                        rightIcon: 'arrow_forward',
                        variant: ButtonVariant.Minimal,
                        compact: true,
                        onclick: (e: Event) => {
                          e.stopPropagation();
                          area.onOpen();
                        },
                      }),
                    ],
                  );
                }

                return m(
                  '.pf-memscope-area',
                  {
                    key: area.id,
                    className: classNames(
                      isHighlighted && 'pf-memscope-area--highlighted',
                    ),
                    style: {'--pf-memscope-area-color': area.color},
                  },
                  [
                    m('.pf-memscope-area__head', [
                      m('.pf-memscope-area__title-group', [
                        m('span.pf-memscope-dom__dot', {
                          style: {background: area.color},
                        }),
                        m('span.pf-memscope-area__name', area.label),
                        sharePct !== undefined &&
                          m('span.pf-memscope-area__share', `${sharePct}%`),
                      ]),
                      m('.pf-memscope-area__metric-group', [
                        area.deltaBytes !== undefined &&
                          area.deltaBytes !== 0 &&
                          m(
                            'span.pf-memscope-area__delta',
                            deltaText(area.deltaBytes),
                          ),
                        m(
                          'span.pf-memscope-area__bytes',
                          bytes > 0 ? formatBytesIec(bytes) : 'Profiled',
                        ),
                      ]),
                    ]),
                    sharePct !== undefined &&
                      m('.pf-memscope-area__bar', [
                        m('.pf-memscope-area__bar-fill', {
                          style: {
                            width: `${Math.max(2, sharePct)}%`,
                            background: area.color,
                          },
                        }),
                      ]),
                    m('.pf-memscope-area__summary', area.summary),
                    m('.pf-memscope-area__foot', [
                      m(
                        '.pf-memscope-area__quick',
                        (area.quickLinks ?? []).map((ql) =>
                          m(Button, {
                            key: ql.label,
                            label: ql.label,
                            icon: ql.icon,
                            variant: ButtonVariant.Minimal,
                            compact: true,
                            onclick: ql.onclick,
                          }),
                        ),
                      ),
                      m(Button, {
                        label: `Open ${area.label}`,
                        rightIcon: 'arrow_forward',
                        variant: area.primaryHighlight
                          ? ButtonVariant.Filled
                          : ButtonVariant.Outlined,
                        intent: area.primaryHighlight
                          ? Intent.Primary
                          : Intent.None,
                        compact: true,
                        onclick: area.onOpen,
                      }),
                    ]),
                  ],
                );
              }),
            ),
          ],
        ),
      ),
    );
  }
}
