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
import {Anchor} from '../../../ui/src/widgets/anchor';
import {Button, ButtonVariant} from '../../../ui/src/widgets/button';
import {Intent} from '../../../ui/src/widgets/common';
import {
  Grid,
  GridCell,
  type GridColumn,
  GridHeaderCell,
  renderSortMenuItems,
  type SortDirection,
} from '../../../ui/src/widgets/grid';
import {MenuDivider, MenuItem, PopupMenu} from '../../../ui/src/widgets/menu';
import {PopupPosition} from '../../../ui/src/widgets/popup';
import {RadioGroup} from '../../../ui/src/widgets/radio_group';
import {TextInput} from '../../../ui/src/widgets/text_input';
import {fmtDeltaBytes, fmtMs, fmtPct} from '../../common/ui/kit/format';
import {
  colorChip,
  formatAgeShort,
  OOM_BUCKETS,
  oomBucketFor,
  sparklineSvg,
} from '../../common/ui/kit/sparkline';
import type {TopProcessRow} from './queries';

export type TaskScope = 'privileged' | 'active' | 'all';

export interface TaskManagerAttrs {
  readonly processes: ReadonlyArray<TopProcessRow>;
  readonly defaultScope?: TaskScope;
  readonly showSnapshotWaits?: boolean;
  readonly initialOomBucketIdx?: number;
  readonly maxRows?: number;
  readonly onOpenProcess: (processName: string) => void;
}

type SortKey =
  | 'process'
  | 'oom'
  | 'pid'
  | 'cpu_pct'
  | 'anon_swap'
  | 'mem_delta'
  | 'dmabuf'
  | 'age'
  | 'runnable'
  | 'io_wait';

function isPrivilegedRow(p: TopProcessRow): boolean {
  if (p.isTop) return true;
  if (p.oomScoreAdj === null) return p.cpuPct >= 0.5 || p.anonSwapMib >= 32;
  // Foreground (0), Visible (1-99), Perceptible (100-299)
  if (p.oomScoreAdj >= 0 && p.oomScoreAdj <= 299) return true;
  // Persistent / system / native (< 0): surface active or significant ones
  // so 150 idle kernel/native daemons don't drown out foreground apps.
  if (p.oomScoreAdj < 0) {
    return (
      p.processName === 'system_server' ||
      p.processName.includes('surfaceflinger') ||
      p.processName.includes('systemui') ||
      p.cpuPct >= 0.5 ||
      p.anonSwapMib >= 40
    );
  }
  return false;
}

export class TaskManagerTable implements m.ClassComponent<TaskManagerAttrs> {
  private scope: TaskScope = 'privileged';
  private search = '';
  private oomFilter = new Set<number>();
  private sortKey: SortKey = 'cpu_pct';
  private sortDir: SortDirection = 'DESC';
  private lastInitialBucket?: number;

  oninit({attrs}: m.Vnode<TaskManagerAttrs>) {
    this.scope = attrs.defaultScope ?? 'privileged';
    if (attrs.initialOomBucketIdx !== undefined) {
      this.scope = 'all';
      this.oomFilter.add(attrs.initialOomBucketIdx);
      this.lastInitialBucket = attrs.initialOomBucketIdx;
    }
  }

  private headerCell(
    key: SortKey,
    label: string,
    hint: SortDirection = 'DESC',
  ): m.Children {
    const current = this.sortKey === key ? this.sortDir : undefined;
    const onSort = (dir: SortDirection | undefined) => {
      this.sortKey = dir !== undefined ? key : 'cpu_pct';
      this.sortDir = dir ?? 'DESC';
    };
    return m(
      GridHeaderCell,
      {
        sort: current,
        onSort,
        hintSortDirection: hint,
        menuItems: renderSortMenuItems(current, onSort),
      },
      label,
    );
  }

  view({attrs}: m.CVnode<TaskManagerAttrs>) {
    if (
      attrs.initialOomBucketIdx !== undefined &&
      attrs.initialOomBucketIdx !== this.lastInitialBucket
    ) {
      this.lastInitialBucket = attrs.initialOomBucketIdx;
      this.scope = 'all';
      this.oomFilter.clear();
      this.oomFilter.add(attrs.initialOomBucketIdx);
    }

    const needle = this.search.trim().toLowerCase();
    const privCount = attrs.processes.filter(isPrivilegedRow).length;
    const activeCount = attrs.processes.filter(
      (p) =>
        p.cpuPct >= 0.2 || Math.abs(p.memDeltaMib) >= 0.5 || p.ioMb >= 0.1,
    ).length;

    const filtered = attrs.processes.filter((p) => {
      if (needle !== '') {
        const match =
          p.processName.toLowerCase().includes(needle) ||
          `${p.pid}`.includes(needle);
        if (!match) return false;
      }
      if (this.oomFilter.size > 0) {
        const b = oomBucketFor(p.oomScoreAdj);
        if (!b || !this.oomFilter.has(b.idx)) return false;
      }
      if (this.scope === 'privileged' && !isPrivilegedRow(p)) return false;
      if (
        this.scope === 'active' &&
        p.cpuPct < 0.2 &&
        Math.abs(p.memDeltaMib) < 0.5 &&
        p.ioMb < 0.1
      ) {
        return false;
      }
      return true;
    });

    const mul = this.sortDir === 'ASC' ? 1 : -1;
    const sorted = [...filtered].sort((a, b) => {
      // Keep top app pinned first when using default privileged view & default sort.
      if (
        this.scope === 'privileged' &&
        this.sortKey === 'cpu_pct' &&
        this.sortDir === 'DESC' &&
        a.isTop !== b.isTop
      ) {
        return a.isTop ? -1 : 1;
      }
      switch (this.sortKey) {
        case 'process':
          return mul * a.processName.localeCompare(b.processName);
        case 'oom':
          return mul * ((a.oomScoreAdj ?? 9999) - (b.oomScoreAdj ?? 9999));
        case 'pid':
          return mul * (a.pid - b.pid);
        case 'cpu_pct':
          return (
            mul * (a.cpuPct - b.cpuPct) || b.anonSwapMib - a.anonSwapMib
          );
        case 'anon_swap':
          return mul * (a.anonSwapMib - b.anonSwapMib);
        case 'mem_delta':
          return mul * (a.memDeltaMib - b.memDeltaMib);
        case 'dmabuf':
          return mul * (a.dmabufMib - b.dmabufMib);
        case 'age':
          return mul * ((a.ageS ?? -1) - (b.ageS ?? -1));
        case 'runnable':
          return mul * (a.runnableMs - b.runnableMs);
        case 'io_wait':
          return mul * (a.ioWaitMs - b.ioWaitMs);
      }
    });

    const shown = sorted.slice(0, attrs.maxRows ?? 60);
    const hasDmabuf = attrs.processes.some((p) => p.dmabufMib > 0.1);
    const hasTrends = attrs.processes.some(
      (p) => (p.ramTrend?.length ?? 0) >= 2 || (p.cpuTrend?.length ?? 0) >= 2,
    );

    const columns: GridColumn[] = [
      {
        key: 'process',
        header: this.headerCell('process', 'Process', 'ASC'),
        maxInitialWidthPx: 235,
      },
      {key: 'oom', header: this.headerCell('oom', 'State / OOM', 'ASC')},
      {key: 'pid', header: this.headerCell('pid', 'PID')},
      {key: 'cpu_pct', header: this.headerCell('cpu_pct', 'CPU %')},
      ...(hasTrends
        ? [{key: 'cpu_trend', header: m(GridCell, 'CPU trend')}]
        : []),
      {key: 'anon_swap', header: this.headerCell('anon_swap', 'Anon + Swap')},
      ...(hasTrends
        ? [{key: 'ram_trend', header: m(GridCell, 'RAM trend')}]
        : []),
      {key: 'mem_delta', header: this.headerCell('mem_delta', 'Δ RAM')},
      ...(hasDmabuf
        ? [{key: 'dmabuf', header: this.headerCell('dmabuf', 'DMA-BUF')}]
        : []),
      ...(attrs.showSnapshotWaits
        ? [
            {key: 'runnable', header: this.headerCell('runnable', 'Runnable')},
            {key: 'io_wait', header: this.headerCell('io_wait', 'I/O Wait')},
          ]
        : []),
      {key: 'age', header: this.headerCell('age', 'Age')},
    ];

    const rowData = shown.map((p) => {
      const bucket = oomBucketFor(p.oomScoreAdj);
      const oomLabel = p.isTop
        ? `TOP (${p.oomScoreAdj ?? 0})`
        : bucket
          ? `${bucket.short} (${p.oomScoreAdj})`
          : p.oomScoreAdj !== null
            ? `${p.oomScoreAdj}`
            : '—';
      const deltaColor =
        p.memDeltaMib >= 1
          ? 'var(--pf-color-danger)'
          : p.memDeltaMib <= -1
            ? 'var(--pf-color-success)'
            : undefined;
      const cells: m.Children[] = [
        m(
          GridCell,
          {
            actionButtons: m(Button, {
              label: 'Open',
              rightIcon: 'arrow_forward',
              minimal: true,
              compact: true,
              onclick: () => attrs.onOpenProcess(p.processName),
            }),
          },
          m('.pf-sysmon-proc-cell', [
            p.frozen && m('span', {title: 'Frozen by cgroup freezer'}, '❄'),
            m(
              Anchor,
              {onclick: () => attrs.onOpenProcess(p.processName)},
              m('code', p.processName),
            ),
          ]),
        ),
        m(
          GridCell,
          bucket || p.isTop
            ? colorChip(
                oomLabel,
                p.isTop ? '#16a34a' : bucket?.color,
                bucket?.name,
              )
            : oomLabel,
        ),
        m(GridCell, {align: 'right'}, `${p.pid}`),
        m(
          GridCell,
          {align: 'right'},
          p.cpuPct >= 0.05 ? fmtPct(p.cpuPct) : '0%',
        ),
      ];
      if (hasTrends) {
        cells.push(
          m(
            GridCell,
            sparklineSvg(p.cpuTrend ?? [], {
              width: 64,
              height: 16,
              color: '#2563eb',
              minSpan: 1,
            }),
          ),
        );
      }
      cells.push(
        m(
          GridCell,
          {align: 'right'},
          p.anonSwapMib > 0 ? `${p.anonSwapMib.toFixed(1)} MiB` : '—',
        ),
      );
      if (hasTrends) {
        cells.push(
          m(
            GridCell,
            sparklineSvg(p.ramTrend ?? [], {
              width: 64,
              height: 16,
              minSpan: 0.5,
            }),
          ),
        );
      }
      cells.push(
        m(
          GridCell,
          {
            align: 'right',
            style: deltaColor ? {color: deltaColor} : undefined,
          },
          Math.abs(p.memDeltaMib) >= 0.05
            ? fmtDeltaBytes(p.memDeltaMib * 1024 * 1024)
            : '0 B',
        ),
      );
      if (hasDmabuf) {
        cells.push(
          m(
            GridCell,
            {align: 'right'},
            p.dmabufMib > 0.05 ? `${p.dmabufMib.toFixed(1)} MiB` : '—',
          ),
        );
      }
      if (attrs.showSnapshotWaits) {
        cells.push(
          m(
            GridCell,
            {align: 'right'},
            p.runnableMs > 0 ? fmtMs(p.runnableMs) : '—',
          ),
          m(
            GridCell,
            {align: 'right'},
            p.ioWaitMs > 0 ? fmtMs(p.ioWaitMs) : '—',
          ),
        );
      }
      cells.push(m(GridCell, {align: 'right'}, formatAgeShort(p.ageS)));
      return cells;
    });

    return m('.pf-sysmon-tm', [
      m('.pf-sysmon-tm__bar', [
        m(
          RadioGroup,
          {
            intent: Intent.Primary,
            selectedValue: this.scope,
            onValueChange: (v) => {
              this.scope = v as TaskScope;
            },
          },
          m(RadioGroup.Button, {value: 'privileged'}, `Privileged (${privCount})`),
          m(RadioGroup.Button, {value: 'active'}, `Active (${activeCount})`),
          m(
            RadioGroup.Button,
            {value: 'all'},
            `All (${attrs.processes.length})`,
          ),
        ),
        m(TextInput, {
          leftIcon: 'search',
          placeholder: 'Filter process or PID…',
          value: this.search,
          onInput: (v: string) => {
            this.search = v;
          },
        }),
        m(
          PopupMenu,
          {
            position: PopupPosition.Bottom,
            trigger: m(Button, {
              label:
                this.oomFilter.size > 0
                  ? `OOM State (${this.oomFilter.size})`
                  : 'OOM State',
              icon: 'filter_list',
              variant: ButtonVariant.Outlined,
              intent: this.oomFilter.size > 0 ? Intent.Primary : Intent.None,
            }),
          },
          m(MenuItem, {
            label: 'Clear filter',
            icon: 'close',
            disabled: this.oomFilter.size === 0,
            onclick: () => this.oomFilter.clear(),
          }),
          m(MenuDivider),
          OOM_BUCKETS.map((b) =>
            m(MenuItem, {
              label: colorChip(b.name, b.color),
              active: this.oomFilter.has(b.idx),
              closePopupOnClick: false,
              onclick: () => {
                if (this.oomFilter.has(b.idx)) this.oomFilter.delete(b.idx);
                else this.oomFilter.add(b.idx);
              },
            }),
          ),
        ),
        m(
          'span.pf-sysmon-tm__count',
          `Showing ${shown.length} of ${filtered.length}`,
        ),
      ]),
      shown.length === 0
        ? m('.pf-sysmon-muted', 'No processes match the current filter.')
        : m(Grid, {columns, rowData, fillHeight: false}),
    ]);
  }
}
