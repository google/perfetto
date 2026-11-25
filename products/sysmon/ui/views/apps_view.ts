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
import {AsyncMemo} from '../../../../ui/src/base/async_memo';
import type {Engine} from '../../../../ui/src/trace_processor/engine';
import {Anchor} from '../../../../ui/src/widgets/anchor';
import {Button, ButtonVariant} from '../../../../ui/src/widgets/button';
import {Intent} from '../../../../ui/src/widgets/common';
import {RadioGroup} from '../../../../ui/src/widgets/radio_group';
import {Spinner} from '../../../../ui/src/widgets/spinner';
import {questionBlock, stat} from '../../../common/ui/kit/blocks';
import {
  proportionBar,
  type LiveStripContext,
} from '../../../common/ui/kit/charts';
import {
  appStateSwimlane,
  colorChip,
  oomBucketByIdx,
} from '../../../common/ui/kit/sparkline';
import {
  chartCard,
  oomBucketDrilldownChart,
  oomMemoryStackedChart,
  type OomMemMetric,
} from '../charts';
import {
  loadAppsBundle,
  loadOomBucketDrilldown,
  loadTaskManager,
  type AppEventRow,
  type AppsBundle,
  type OomBucketProcSeriesRow,
  type TopProcessRow,
  type WindowArgs,
} from '../queries';
import {sysmonState, type CapturedSnapshot} from '../state';
import {TaskManagerTable} from '../task_manager';

export interface AppsTabAttrs {
  readonly engine?: Engine;
  readonly tickKey: string;
  readonly win?: WindowArgs;
  readonly durationS: number;
  readonly apps: AppsBundle;
  readonly processes: ReadonlyArray<TopProcessRow>;
  readonly liveCtx?: LiveStripContext;
  readonly showSnapshotWaits?: boolean;
  readonly initialOomBucketIdx?: number;
  readonly onOpenProcess: (processName: string) => void;
  readonly onNavigateTab?: (tab: string) => void;
}

export class AppsTabView implements m.ClassComponent<AppsTabAttrs> {
  private metric: OomMemMetric = 'anon_swap';
  private drillBucketIdx?: number;
  private readonly drillMemo = new AsyncMemo<OomBucketProcSeriesRow[]>();

  onremove() {
    this.drillMemo.dispose();
  }

  view({attrs}: m.CVnode<AppsTabAttrs>) {
    const {apps, durationS, onOpenProcess} = attrs;
    const top = apps.topApp;

    const drillBucket =
      this.drillBucketIdx !== undefined
        ? oomBucketByIdx(this.drillBucketIdx)
        : undefined;
    const drillState =
      drillBucket && attrs.engine
        ? this.drillMemo.use({
            key: `${attrs.tickKey}:${drillBucket.idx}:${attrs.win?.startTs?.toString() ?? 'all'}:${attrs.win?.endTs?.toString() ?? 'all'}`,
            compute: () =>
              loadOomBucketDrilldown(attrs.engine!, drillBucket.idx, attrs.win, 8),
          })
        : undefined;

    const countBar = proportionBar(
      apps.buckets.map((b) => ({
        label: oomBucketByIdx(b.bucketIdx)?.short ?? b.bucketName,
        value: b.procCount,
        display: `${b.procCount}`,
        color: oomBucketByIdx(b.bucketIdx)?.color,
      })),
    );

    const ramBar = proportionBar(
      apps.buckets.map((b) => ({
        label: oomBucketByIdx(b.bucketIdx)?.short ?? b.bucketName,
        value: b.anonSwapMib,
        display:
          b.anonSwapMib >= 1024
            ? `${(b.anonSwapMib / 1024).toFixed(1)} GiB`
            : `${b.anonSwapMib.toFixed(0)} MiB`,
        color: oomBucketByIdx(b.bucketIdx)?.color,
      })),
    );

    const privCount =
      (top?.fgCount ?? 0) + (top?.visCount ?? 0) + (top?.percCount ?? 0);

    const shortTopApp = top?.topApp
      ? top.topApp.split('.').slice(-2).join('.')
      : '—';

    return m('.pf-prod-landing', [
      questionBlock(
        {
          question: 'Which app is in front, and how are apps distributed by privilege?',
          answer: top?.topApp
            ? `Top app: \`${top.topApp}\` (${top.topDurS.toFixed(0)}s in window). ${privCount} foreground/visible/perceptible, ${top.svcCount} service, ${top.cachedCount} cached, and ${top.nativeCount} native processes (${top.lmkdKills} LMK kills).`
            : 'Waiting for process OOM state samples.',
          actions:
            top?.topApp
              ? [
                  {
                    label: `Inspect ${shortTopApp}`,
                    icon: 'open_in_new',
                    onclick: () => onOpenProcess(top.topApp!),
                  },
                ]
              : undefined,
        },
        [
          m('.pf-prod-stats', [
            stat('Top app', shortTopApp, top?.topApp ? `${top.topDurS.toFixed(0)}s · ${top.topApp}` : undefined),
            stat('Foreground / Visible', `${(top?.fgCount ?? 0) + (top?.visCount ?? 0)}`, `${top?.percCount ?? 0} perceptible`),
            stat('Service / Cached', `${top?.svcCount ?? 0} / ${top?.cachedCount ?? 0}`, `${top?.nativeCount ?? 0} native`),
            stat(
              'LMK kills',
              `${top?.lmkdKills ?? 0}`,
              (top?.lmkdKills ?? 0) > 0 ? 'Low-memory kills in window' : 'None in window',
              (top?.lmkdKills ?? 0) > 0 ? 'bad' : undefined,
            ),
          ]),
          m('.pf-sysmon-chart-row', [
            countBar && chartCard('Processes by OOM state', countBar),
            ramBar && chartCard('Anon + Swap RAM by OOM state', ramBar),
          ]),
        ],
      ),

      apps.intervals.length > 0 &&
        questionBlock(
          {
            question: 'App states over time',
            answer:
              'Top app, killed apps, transitioning apps, and active privileged processes across the window. Click any lane to open that process in its own tab.',
          },
          appStateSwimlane({
            intervals: apps.intervals,
            durationS,
            onSelectProcess: (_upid, name) => onOpenProcess(name),
          }),
        ),

      apps.oomSeries.length >= 2 &&
        questionBlock(
          {
            question: drillBucket
              ? `Memory over time · ${drillBucket.name}`
              : 'Memory by OOM state over time',
            answer: drillBucket
              ? `Top processes currently in ${drillBucket.name}. Click back to return to all OOM buckets.`
              : 'Click any OOM band in the stacked chart to drill into the top processes inside that state.',
          },
          [
            m('.pf-sysmon-oom-controls', [
              drillBucket &&
                m(Button, {
                  label: 'All OOM buckets',
                  icon: 'arrow_back',
                  variant: ButtonVariant.Outlined,
                  onclick: () => {
                    this.drillBucketIdx = undefined;
                  },
                }),
              m(
                RadioGroup,
                {
                  intent: Intent.Primary,
                  selectedValue: this.metric,
                  onValueChange: (v) => {
                    this.metric = v as OomMemMetric;
                  },
                },
                m(RadioGroup.Button, {value: 'anon_swap'}, 'Anon + Swap'),
                m(RadioGroup.Button, {value: 'file'}, 'File RSS'),
                m(RadioGroup.Button, {value: 'dmabuf'}, 'DMA-BUF'),
              ),
            ]),
            drillBucket
              ? drillState?.data
                ? oomBucketDrilldownChart(
                    drillState.data,
                    this.metric,
                    240,
                    attrs.liveCtx,
                    drillBucket.idx,
                  )
                : m('.pf-sysmon-wait', [
                    m(Spinner, {easing: true}),
                    m('span', `Loading ${drillBucket.name} processes…`),
                  ])
              : oomMemoryStackedChart(
                  apps.oomSeries,
                  this.metric,
                  (idx) => {
                    this.drillBucketIdx = idx;
                  },
                  240,
                  attrs.liveCtx,
                ),
          ],
        ),

      apps.events.length > 0 &&
        questionBlock(
          {
            question: 'Recent state transitions & kills',
            answer: `${apps.events.length} app lifecycle, state-transition, or LMK events in this window.`,
          },
          renderEventFeed(apps.events, onOpenProcess, attrs.onNavigateTab),
        ),

      questionBlock(
        {
          question: 'All processes',
          answer:
            'Search, filter by OOM state, sort by CPU or RAM, and click any process to open its dedicated tab.',
        },
        m(TaskManagerTable, {
          processes: attrs.processes,
          defaultScope: 'all',
          showSnapshotWaits: attrs.showSnapshotWaits,
          initialOomBucketIdx: attrs.initialOomBucketIdx,
          maxRows: 120,
          onOpenProcess,
        }),
      ),
    ]);
  }
}

export function renderEventFeed(
  events: ReadonlyArray<AppEventRow>,
  onOpenProcess: (name: string) => void,
  onNavigateTab?: (tab: string) => void,
  maxItems = 15,
): m.Child {
  const shown = events.slice(0, maxItems);
  if (shown.length === 0) return undefined;
  return m(
    '.pf-prod-events',
    shown.map((e) => {
      const chipColor =
        e.severity === 'danger'
          ? '#dc2626'
          : e.kind === 'top'
            ? '#16a34a'
            : e.severity === 'warn'
              ? '#d97706'
              : '#2563eb';
      const kindLabel =
        e.kind === 'lmk'
          ? 'LMK'
          : e.kind === 'top'
            ? 'TOP'
            : e.kind === 'state'
              ? 'STATE'
              : 'PROC';
      return m('.pf-prod-events__item', [
        m('span.pf-prod-events__time', `${e.relS.toFixed(1)}s`),
        colorChip(kindLabel, chipColor),
        m('.pf-prod-events__body', [
          m(
            Anchor,
            {onclick: () => onOpenProcess(e.processName)},
            m('code', e.processName),
          ),
          m('span', e.title),
          e.detail && m('span.pf-prod-events__detail', e.detail),
        ]),
        onNavigateTab &&
          e.targetTab !== 'apps' &&
          m(Button, {
            label: e.targetTab === 'memory' ? 'Memory' : 'Open',
            minimal: true,
            compact: true,
            onclick: () => onNavigateTab(e.targetTab),
          }),
      ]);
    }),
  );
}

// Snapshot wrapper for Apps & States subtab.
export function SnapshotAppsView(): m.Component<{
  readonly snap: CapturedSnapshot;
}> {
  const memo = new AsyncMemo<{
    readonly apps: AppsBundle;
    readonly procs: TopProcessRow[];
  }>();

  return {
    view({attrs: {snap}}) {
      const win = sysmonState.getWindowArgs(snap);
      const key = `${snap.id}:${win?.startTs?.toString() ?? 'all'}:${win?.endTs?.toString() ?? 'all'}`;
      const state = memo.use({
        key,
        compute: async () => {
          const [apps, procs] = await Promise.all([
            loadAppsBundle(snap.engine, win, 20),
            loadTaskManager(snap.engine, win, 120, 15),
          ]);
          return {apps, procs};
        },
      });
      if (!state.data) {
        return m('.pf-sysmon-wait', [
          m(Spinner, {easing: true}),
          m('span', 'Loading app states…'),
        ]);
      }
      return m(AppsTabView, {
        engine: snap.engine,
        tickKey: key,
        win,
        durationS: snap.pulse?.durS ?? 60,
        apps: state.data.apps,
        processes: state.data.procs,
        showSnapshotWaits: true,
        onOpenProcess: (name) => sysmonState.openSnapSubtab(snap, 'process', name),
        onNavigateTab: (tab) => {
          if (tab === 'memory') sysmonState.openSnapSubtab(snap, 'memory');
        },
      });
    },
    onremove() {
      memo.dispose();
    },
  };
}
