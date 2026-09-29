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

import './styles.scss';
import m from 'mithril';
import {classNames} from '../../base/classnames';
import {Button, ButtonBar, ButtonVariant} from '../../widgets/button';
import {Card} from '../../widgets/card';
import {Chip} from '../../widgets/chip';
import {Intent} from '../../widgets/common';
import {DualMetricAxis, DualMetricBar} from '../../widgets/dual_metric_bar';
import {EmptyState} from '../../widgets/empty_state';
import {KpiCard, KpiCardGroup} from '../../widgets/kpi_card';
import {Select} from '../../widgets/select';
import {Spinner} from '../../widgets/spinner';
import {TextInput} from '../../widgets/text_input';
import {
  type SparklineBandSeries,
  TimelineSparkline,
} from '../../widgets/timeline_sparkline';
import type {ComponentTimelineModel} from './model';
import {
  type ActiveBucketState,
  ALL_TARGETS_VALUE,
  classifyProcStateFamily,
  type ColorMode,
  type ComponentCategory,
  formatShortProcState,
  PROC_STATE_FAMILY_COLORS,
  PROC_STATE_FAMILY_LABELS,
  PROC_STATE_FAMILY_SHORT,
  type ProcStateFamily,
  type ProcessTimelineRow,
  STATE_COLORS,
  type TimelineDataset,
} from './types';

const ROW_HEIGHT_PX = 21;

export interface ComponentTimelineViewAttrs {
  readonly model: ComponentTimelineModel;
  readonly compact?: boolean;
}

function fmtRelSec(ms: number, digits = 1): string {
  const sign = ms >= 0 ? '+' : '\u2212';
  return `${sign}${(Math.abs(ms) / 1000).toFixed(digits)} s`;
}

function fmtMb(v: number): string {
  if (v >= 1024) {
    return `${(v / 1024).toFixed(2)} GB`;
  }
  return `${Math.round(v)} MB`;
}

export class ComponentTimelineView implements m.ClassComponent<ComponentTimelineViewAttrs> {
  private keydownHandler: ((e: KeyboardEvent) => void) | null = null;

  oncreate({attrs}: m.CVnodeDOM<ComponentTimelineViewAttrs>): void {
    const {model} = attrs;
    if (!model.isInitialized && !model.isLoading) {
      void model.initialize();
    }
    this.keydownHandler = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement | null;
      if (
        target !== null &&
        (target.tagName === 'INPUT' ||
          target.tagName === 'SELECT' ||
          target.tagName === 'TEXTAREA' ||
          target.isContentEditable)
      ) {
        return;
      }
      if (e.code === 'Space') {
        e.preventDefault();
        model.togglePlay();
      } else if (e.code === 'ArrowRight') {
        e.preventDefault();
        model.stop();
        model.goToFrame(model.frame + 1);
      } else if (e.code === 'ArrowLeft') {
        e.preventDefault();
        model.stop();
        model.goToFrame(model.frame - 1);
      } else if (e.code === 'Home') {
        e.preventDefault();
        model.stop();
        model.goToFrame(0);
      }
    };
    document.addEventListener('keydown', this.keydownHandler);
  }

  onremove(): void {
    if (this.keydownHandler !== null) {
      document.removeEventListener('keydown', this.keydownHandler);
      this.keydownHandler = null;
    }
  }

  view({attrs}: m.CVnode<ComponentTimelineViewAttrs>): m.Children {
    const {model, compact = false} = attrs;

    if (model.isLoading && model.dataset === null) {
      return m(
        '.pf-comp-timeline',
        {className: classNames(compact && 'pf-comp-timeline--compact')},
        m(
          EmptyState,
          {
            title: 'Computing 100 ms component & process state timeline...',
            fillHeight: true,
          },
          m(Spinner),
        ),
      );
    }

    if (model.errorMessage !== null) {
      return m(
        '.pf-comp-timeline',
        {className: classNames(compact && 'pf-comp-timeline--compact')},
        m(
          EmptyState,
          {
            title: 'Failed to load Component Timeline',
            fillHeight: true,
          },
          m('p', model.errorMessage),
        ),
      );
    }

    const ds = model.dataset;
    if (ds === null) {
      return m(
        '.pf-comp-timeline',
        {className: classNames(compact && 'pf-comp-timeline--compact')},
        m(EmptyState, {
          title: 'No Android component or process state events found in trace.',
          fillHeight: true,
        }),
      );
    }

    return m(
      '.pf-comp-timeline',
      {className: classNames(compact && 'pf-comp-timeline--compact')},
      this.renderTopBar(model, ds),
      this.renderMainContent(model, ds),
    );
  }

  private renderTopBar(
    model: ComponentTimelineModel,
    ds: TimelineDataset,
  ): m.Children {
    const meta = model.headerMeta;
    const activeCatSummary = model.categories.find(
      (c) => c.category === model.selectedCategory,
    );
    const targets = activeCatSummary?.targets ?? [];

    const frame = Math.max(0, Math.min(ds.nb - 1, model.frame));
    const b = ds.b0 + frame;
    const loMs = b * ds.bucketMs;
    const hiMs = loMs + ds.bucketMs;

    return m(
      '.pf-comp-timeline__bar',
      m(
        '.pf-comp-timeline__top',
        m('h1.pf-comp-timeline__title', ds.title),
        m(
          '.pf-comp-timeline__selectors',
          m(
            Select,
            {
              title: 'Component Category',
              value: model.selectedCategory,
              onchange: (e: Event) => {
                const val = (e.target as HTMLSelectElement)
                  .value as ComponentCategory;
                void model.selectCategory(val);
              },
            },
            model.categories.map((c) =>
              m(
                'option',
                {
                  value: c.category,
                  selected: c.category === model.selectedCategory,
                },
                `${c.label} (${c.procCount} apps · ${c.eventCount})`,
              ),
            ),
          ),
          targets.length > 0 &&
            m(
              Select,
              {
                title: 'Specific Broadcast / Service / Provider / Job / State',
                value: model.selectedTarget,
                onchange: (e: Event) => {
                  const val = (e.target as HTMLSelectElement).value;
                  void model.selectTarget(val);
                },
              },
              m(
                'option',
                {
                  value: ALL_TARGETS_VALUE,
                  selected: model.selectedTarget === ALL_TARGETS_VALUE,
                },
                `All ${activeCatSummary?.label ?? 'Items'} (${activeCatSummary?.procCount ?? 0} apps)`,
              ),
              targets.map((t) =>
                m(
                  'option',
                  {
                    value: t.target,
                    selected: t.target === model.selectedTarget,
                  },
                  `${t.target} (${t.procCount} apps · ${t.eventCount})`,
                ),
              ),
            ),
          m(
            Select,
            {
              title:
                'Color bars and sparkline by Component State or Android Process State',
              value: model.colorMode,
              onchange: (e: Event) => {
                const val = (e.target as HTMLSelectElement).value as ColorMode;
                model.setColorMode(val);
              },
            },
            m(
              'option',
              {
                value: 'component',
                selected: model.colorMode === 'component',
              },
              'Color: Component State (S/R/C/I/K)',
            ),
            m(
              'option',
              {
                value: 'proc_state',
                selected: model.colorMode === 'proc_state',
              },
              'Color: Android Proc State',
            ),
          ),
          m(TextInput, {
            leftIcon: 'search',
            placeholder: 'Filter process / pid...',
            value: model.searchQuery,
            onInput: (val: string) => {
              model.searchQuery = val;
            },
          }),
        ),
        m(
          '.pf-comp-timeline__chips',
          m(Chip, {
            label: `${ds.rows.length} apps`,
            rounded: true,
            compact: true,
          }),
          m(Chip, {
            label: `${meta.ncpu} CPUs`,
            rounded: true,
            compact: true,
          }),
          m(Chip, {
            label: `build ${meta.build}`,
            rounded: true,
            compact: true,
          }),
          m(Chip, {
            label: meta.device,
            rounded: true,
            compact: true,
          }),
          m(Chip, {
            label: `${meta.nlmk} LMK kills in trace`,
            rounded: true,
            compact: true,
            intent: meta.nlmk > 0 ? Intent.Danger : Intent.None,
          }),
          m(Chip, {
            label: meta.hasFrameworkProcState
              ? 'proc_state: framework'
              : 'proc_state: oom_adj',
            rounded: true,
            compact: true,
          }),
          m(Button, {
            label: 'Pin Track',
            icon: 'push_pin',
            compact: true,
            variant: ButtonVariant.Outlined,
            tooltip: 'Pin this component category track and view in Timeline',
            onclick: () => model.pinActiveCategoryTrack(),
          }),
        ),
      ),
      m(
        '.pf-comp-timeline__barrow',
        m(
          '.pf-comp-timeline__clock',
          `t0 ${fmtRelSec(loMs)}`,
          m(
            'small',
            `bucket ${fmtRelSec(loMs)} \u2026 ${fmtRelSec(hiMs)} \u00b7 frame ${frame + 1} / ${ds.nb}`,
          ),
        ),
        this.renderKpis(model, ds, frame),
      ),
      m(
        '.pf-comp-timeline__scrub',
        m(
          ButtonBar,
          m(Button, {
            icon: 'skip_previous',
            iconFilled: true,
            label: 'Start',
            compact: true,
            variant: ButtonVariant.Outlined,
            title: 'Jump to first frame (Home)',
            onclick: () => {
              model.stop();
              model.goToFrame(0);
            },
          }),
          m(Button, {
            icon: 'chevron_left',
            compact: true,
            variant: ButtonVariant.Outlined,
            title: 'Back one 100 ms frame (\u2190)',
            onclick: () => {
              model.stop();
              model.goToFrame(frame - 1);
            },
          }),
          m(Button, {
            icon: model.isPlaying ? 'pause' : 'play_arrow',
            iconFilled: true,
            label: model.isPlaying ? 'Pause' : 'Play',
            compact: true,
            variant: ButtonVariant.Filled,
            intent: Intent.Primary,
            title: 'Play / pause timeline (Space)',
            onclick: () => model.togglePlay(),
          }),
          m(Button, {
            icon: 'chevron_right',
            compact: true,
            variant: ButtonVariant.Outlined,
            title: 'Forward one 100 ms frame (\u2192)',
            onclick: () => {
              model.stop();
              model.goToFrame(frame + 1);
            },
          }),
        ),
        m('input.pf-comp-timeline__slider[type=range]', {
          min: 0,
          max: Math.max(0, ds.nb - 1),
          value: frame,
          oninput: (e: Event) => {
            model.stop();
            model.goToFrame(Number((e.target as HTMLInputElement).value));
          },
        }),
        m(
          'span.pf-comp-timeline__frameno',
          `${fmtRelSec(loMs)} \u00b7 ${frame + 1}/${ds.nb}`,
        ),
        m(
          Select,
          {
            title: 'Wall time per 100 ms frame',
            value: String(model.speedMs),
            onchange: (e: Event) => {
              const v = Number((e.target as HTMLSelectElement).value);
              model.setSpeedMs(v);
            },
          },
          m(
            'option',
            {value: '1000', selected: model.speedMs === 1000},
            '1 s / frame',
          ),
          m(
            'option',
            {value: '500', selected: model.speedMs === 500},
            '0.5 s / frame',
          ),
          m(
            'option',
            {value: '250', selected: model.speedMs === 250},
            '0.25 s / frame',
          ),
          m(
            'option',
            {value: '100', selected: model.speedMs === 100},
            '0.1 s / frame (real time)',
          ),
        ),
      ),
      m(
        '.pf-comp-timeline__spark-wrap',
        m(TimelineSparkline, {
          title: `Processes per state (area) and ${ds.nounPlural}' anon + swap (line) over the trace; click or drag to jump`,
          numFrames: ds.nb,
          currentFrame: frame,
          originFrame: Math.max(0, Math.min(ds.nb - 1, -ds.b0)),
          bands: this.buildSparklineBands(model, ds),
          lineValues: ds.totAnonMb.map((v, i) => v + (ds.totSwapMb[i] ?? 0)),
          onSelectFrame: (targetFrame: number) => {
            model.stop();
            model.goToFrame(targetFrame);
          },
        }),
      ),
      this.renderLegend(model, ds),
    );
  }

  private renderKpis(
    model: ComponentTimelineModel,
    ds: TimelineDataset,
    frame: number,
  ): m.Children {
    const anonPlusSwap =
      (ds.totAnonMb[frame] ?? 0) + (ds.totSwapMb[frame] ?? 0);
    const swapOnly = ds.totSwapMb[frame] ?? 0;

    if (model.colorMode === 'proc_state') {
      const psCounts = ds.procStateCounts[frame] ?? {
        TOP: 0,
        FG_SVC: 0,
        RECEIVER: 0,
        SERVICE: 0,
        CACHED: 0,
        KILLED: 0,
      };
      const keys: ReadonlyArray<ProcStateFamily> = [
        'TOP',
        'FG_SVC',
        'RECEIVER',
        'SERVICE',
        'CACHED',
        'KILLED',
      ];
      return m(
        KpiCardGroup,
        keys.map((k) =>
          m(KpiCard, {
            key: k,
            value: String(psCounts[k]),
            label: PROC_STATE_FAMILY_LABELS[k],
            dotColor: PROC_STATE_FAMILY_COLORS[k],
            active: model.stateFilter === k,
            title: `Click to filter processes in ${PROC_STATE_FAMILY_LABELS[k]}`,
            onclick: () => model.toggleStateFilter(k),
          }),
        ),
        m(KpiCard, {
          wide: true,
          value: fmtMb(anonPlusSwap),
          label: `${ds.nounPlural}' anon + swap (${fmtMb(swapOnly)} swap)`,
          swatchColor: '#344054',
        }),
      );
    }

    const cnt = ds.counts[frame] ?? {S: 0, R: 0, C: 0, I: 0, K: 0};
    const stateLabels: Readonly<Record<ActiveBucketState, string>> = {
      S: 'Awaiting',
      R: ds.activeVerb,
      C: 'Done, on CPU',
      I: 'Done, idle',
      K: 'Killed',
    };
    const order: ReadonlyArray<ActiveBucketState> = ['S', 'R', 'C', 'I', 'K'];

    return m(
      KpiCardGroup,
      order.map((k) =>
        m(KpiCard, {
          key: k,
          value: String(cnt[k]),
          label: stateLabels[k],
          dotColor: STATE_COLORS[k],
          active: model.stateFilter === k,
          title: `Click to filter processes in '${stateLabels[k]}' state`,
          onclick: () => model.toggleStateFilter(k),
        }),
      ),
      m(KpiCard, {
        wide: true,
        value: fmtMb(anonPlusSwap),
        label: `${ds.nounPlural}' anon + swap (${fmtMb(swapOnly)} swap)`,
        swatchColor: '#344054',
      }),
    );
  }

  private buildSparklineBands(
    model: ComponentTimelineModel,
    ds: TimelineDataset,
  ): ReadonlyArray<SparklineBandSeries> {
    if (model.colorMode === 'proc_state') {
      const order: ReadonlyArray<ProcStateFamily> = [
        'CACHED',
        'SERVICE',
        'RECEIVER',
        'FG_SVC',
        'TOP',
        'KILLED',
      ];
      return order.map((k) => ({
        key: k,
        color: PROC_STATE_FAMILY_COLORS[k],
        values: ds.procStateCounts.map((c) => c[k]),
      }));
    }

    const order: ReadonlyArray<ActiveBucketState> = ['I', 'C', 'R', 'S', 'K'];
    return order.map((k) => ({
      key: k,
      color: STATE_COLORS[k],
      values: ds.counts.map((c) => c[k]),
    }));
  }

  private renderLegend(
    model: ComponentTimelineModel,
    ds: TimelineDataset,
  ): m.Children {
    const scaleHint = `\u00b7 CPU bar full = ${ds.cpuScaleMs} ms (1 core); RSS bar full = ${fmtMb(ds.memScaleMb)}`;

    if (model.colorMode === 'proc_state') {
      const order: ReadonlyArray<ProcStateFamily> = [
        'TOP',
        'FG_SVC',
        'RECEIVER',
        'SERVICE',
        'CACHED',
        'KILLED',
      ];
      return m(
        '.pf-comp-timeline__legend',
        order.map((k) =>
          m(
            'span',
            {key: k},
            m('i.pf-comp-timeline__dot', {
              style: {background: PROC_STATE_FAMILY_COLORS[k]},
            }),
            PROC_STATE_FAMILY_LABELS[k],
          ),
        ),
        m('span', m('i.pf-comp-timeline__swatch-m'), 'anon RSS'),
        m('span', m('i.pf-comp-timeline__swatch-f'), 'file + shmem RSS'),
        m('span', m('i.pf-comp-timeline__swatch-w'), 'swap'),
        m('span', scaleHint),
      );
    }

    return m(
      '.pf-comp-timeline__legend',
      m(
        'span',
        m('i.pf-comp-timeline__dot', {style: {background: STATE_COLORS.S}}),
        `Started, awaiting ${ds.activeShort}`,
      ),
      m(
        'span',
        m('i.pf-comp-timeline__dot', {style: {background: STATE_COLORS.R}}),
        ds.activeVerb,
      ),
      m(
        'span',
        m('i.pf-comp-timeline__dot', {style: {background: STATE_COLORS.C}}),
        'Finished, still on CPU (\u2265 1 ms)',
      ),
      m(
        'span',
        m('i.pf-comp-timeline__dot', {style: {background: STATE_COLORS.I}}),
        'Finished, idle',
      ),
      m(
        'span',
        m('i.pf-comp-timeline__dot', {style: {background: STATE_COLORS.K}}),
        'Killed (stays 5 s of trace)',
      ),
      m('span', m('i.pf-comp-timeline__swatch-m'), 'anon RSS'),
      m('span', m('i.pf-comp-timeline__swatch-f'), 'file + shmem RSS'),
      m('span', m('i.pf-comp-timeline__swatch-w'), 'swap'),
      m('span', scaleHint),
    );
  }

  private renderMainContent(
    model: ComponentTimelineModel,
    ds: TimelineDataset,
  ): m.Children {
    if (ds.rows.length === 0) {
      return m(
        'main.pf-comp-timeline__main',
        m(EmptyState, {
          title: 'No matching processes for this component filter.',
        }),
      );
    }

    const frame = Math.max(0, Math.min(ds.nb - 1, model.frame));
    const loMs = (ds.b0 + frame) * ds.bucketMs;
    const query = model.searchQuery.trim().toLowerCase();
    const hasFilter = query !== '' || model.stateFilter !== null;

    const colRows: [
      Array<{row: ProcessTimelineRow; topPx: number; visible: boolean}>,
      Array<{row: ProcessTimelineRow; topPx: number; visible: boolean}>,
    ] = [[], []];

    let col0HeightPx = Math.ceil(ds.nslots / 2) * ROW_HEIGHT_PX;
    let col1HeightPx = Math.ceil(ds.nslots / 2) * ROW_HEIGHT_PX;

    if (hasFilter) {
      let filteredSlot = 0;
      for (const r of ds.rows) {
        const st = r.states[frame];
        const baseVis = st !== '-' && !(r.goneMs !== null && loMs >= r.goneMs);
        if (!baseVis) continue;
        if (
          query !== '' &&
          !r.name.toLowerCase().includes(query) &&
          !String(r.pid).includes(query)
        ) {
          continue;
        }
        if (model.stateFilter !== null) {
          if (model.colorMode === 'proc_state') {
            const fam = classifyProcStateFamily(r.procStates[frame], st);
            if (fam !== model.stateFilter) continue;
          } else {
            if (st !== model.stateFilter) continue;
          }
        }
        const c = filteredSlot % 2;
        const topPx = Math.floor(filteredSlot / 2) * ROW_HEIGHT_PX;
        colRows[c].push({row: r, topPx, visible: true});
        filteredSlot++;
      }
      const halfFiltered = Math.max(1, Math.ceil(filteredSlot / 2));
      col0HeightPx = halfFiltered * ROW_HEIGHT_PX;
      col1HeightPx = halfFiltered * ROW_HEIGHT_PX;
    } else {
      for (const r of ds.rows) {
        const st = r.states[frame];
        const vis = st !== '-' && !(r.goneMs !== null && loMs >= r.goneMs);
        const c = r.slot % 2;
        const topPx = Math.floor(r.slot / 2) * ROW_HEIGHT_PX;
        colRows[c].push({row: r, topPx, visible: vis});
      }
    }

    return m(
      'main.pf-comp-timeline__main',
      m(
        '.pf-comp-timeline__cols',
        [0, 1].map((cIdx) =>
          m(
            Card,
            {key: cIdx, className: 'pf-comp-timeline__col'},
            m(
              '.pf-comp-timeline__axis',
              m('div', 'process'),
              m(DualMetricAxis, {
                topMax: ds.cpuScaleMs,
                topUnit: 'ms',
                bottomMax: ds.memScaleMb,
                bottomUnit: 'MB',
              }),
              m('div', 'CPU \u00b7 RSS \u00b7 state'),
            ),
            m(
              '.pf-comp-timeline__rows',
              {
                style: {
                  height: `${cIdx === 0 ? col0HeightPx : col1HeightPx}px`,
                },
              },
              colRows[cIdx].map(({row, topPx, visible}) =>
                this.renderProcessRow(
                  model,
                  ds,
                  row,
                  frame,
                  loMs,
                  topPx,
                  visible,
                ),
              ),
            ),
          ),
        ),
      ),
      m(
        '.pf-comp-timeline__foot',
        't0 = ',
        m('code', ds.t0Label),
        `. Trace covers ${fmtRelSec(-ds.preMs)} \u2026 ${fmtRelSec(ds.postMs)}. `,
        `One row per app process that matched this component (${ds.rows.length} processes, system_server excluded); rows alternate left / right in order of appearance. `,
        'Top bar = CPU in the 100 ms bucket (black end cap = more than one core). ',
        'Bottom bar = RSS from ',
        m('code', 'rss_stat'),
        ' (last value carried forward): dark = anon (private), light = file + shmem (largely shared with zygote / other apps), striped = swap. ',
        'Click any process name to pin its tracks and jump to it in the Perfetto timeline.',
      ),
    );
  }

  private renderProcessRow(
    model: ComponentTimelineModel,
    ds: TimelineDataset,
    r: ProcessTimelineRow,
    frame: number,
    loMs: number,
    topPx: number,
    visible: boolean,
  ): m.Children {
    const st = r.states[frame];
    const activeSt: ActiveBucketState = st === '-' ? 'I' : st;
    const isNew = visible && model.newlyShownUpids.has(r.upid);

    const cpu = r.cpuMs[frame] ?? 0;
    const rss = r.rssMb[frame] ?? null;
    const anon = r.anonMb[frame] ?? null;
    const swap = r.swapMb[frame] ?? 0;
    const procState = r.procStates[frame] ?? null;
    const oomScore = r.oomScores[frame] ?? r.oom;

    const procFamily = classifyProcStateFamily(procState, st);
    const barColor =
      model.colorMode === 'proc_state'
        ? PROC_STATE_FAMILY_COLORS[procFamily]
        : STATE_COLORS[activeSt];

    const shortStateMap: Readonly<Record<ActiveBucketState, string>> = {
      S: 'awaiting',
      R: ds.activeShort,
      C: 'done',
      I: 'idle',
      K: 'killed',
    };

    const stateText =
      model.colorMode === 'proc_state'
        ? PROC_STATE_FAMILY_SHORT[procFamily]
        : shortStateMap[activeSt];

    let valText: string;
    if (st === 'K' && r.goneMs !== null) {
      const leftSec = Math.max(0, (r.goneMs - loMs) / 1000);
      valText = `${r.lmk ? 'LMK' : 'died'}${r.oom !== null ? ` oom ${r.oom}` : ''} \u00b7 ${leftSec.toFixed(1)} s`;
    } else {
      const rssStr = rss !== null ? `${Math.round(rss)} MB` : '\u2013';
      const anonStr = anon !== null ? ` (a ${anon})` : '';
      valText = `${cpu.toFixed(1)} ms \u00b7 ${rssStr}${anonStr} \u00b7 ${stateText}`;
    }

    const shortBadge = formatShortProcState(procState, oomScore);
    const tooltipTitle = [
      `${r.name} (pid ${r.pid}, upid ${r.upid})`,
      `CPU: ${cpu.toFixed(1)} ms`,
      `RSS: ${rss ?? '\u2013'} MB (anon: ${anon ?? '\u2013'} MB, swap: ${swap} MB)`,
      `Component state: ${activeSt} (${shortStateMap[activeSt]})`,
      procState !== null ? `Android proc_state: ${procState}` : null,
      oomScore !== null ? `oom_score_adj: ${oomScore}` : null,
    ]
      .filter((x) => x !== null)
      .join(' \u00b7 ');

    return m(
      '.pf-comp-timeline__row',
      {
        key: r.upid,
        className: classNames(
          `pf-comp-timeline__row--${activeSt}`,
          isNew && 'pf-comp-timeline__row--new',
        ),
        style: {
          top: `${topPx}px`,
          opacity: visible ? '1' : '0',
          pointerEvents: visible ? 'auto' : 'none',
        },
      },
      m(
        '.pf-comp-timeline__nm',
        {
          title: `${r.name} (pid ${r.pid}) \u2014 Click to pin tracks & view in Timeline`,
          onclick: () => model.focusProcessInTimeline(r),
        },
        r.name,
        m('i', String(r.pid)),
      ),
      m(DualMetricBar, {
        title: tooltipTitle,
        topValue: cpu,
        topMax: ds.cpuScaleMs,
        topColor: barColor,
        topZeroWidth: st === 'I' || st === 'K' || !visible,
        bottomSecondaryValue: st === 'K' ? 0 : rss,
        bottomPrimaryValue: st === 'K' ? 0 : anon,
        bottomOverflowValue: st === 'K' ? 0 : swap,
        bottomMax: ds.memScaleMb,
      }),
      m(
        '.pf-comp-timeline__val',
        {title: tooltipTitle},
        m('span', valText),
        shortBadge !== null &&
          st !== 'K' &&
          m(Chip, {label: shortBadge, compact: true}),
      ),
    );
  }
}
