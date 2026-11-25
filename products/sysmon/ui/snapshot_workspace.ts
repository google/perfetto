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
import {classNames} from '../../../ui/src/base/classnames';
import {Button, ButtonVariant} from '../../../ui/src/widgets/button';
import {Icon} from '../../../ui/src/widgets/icon';
import {Tabs, type TabsTab} from '../../../ui/src/widgets/tabs';
import {
  pageShell,
  questionBlock,
  stat,
  type PageFact,
} from '../../common/ui/kit/blocks';
import {fmtBytes} from '../../common/ui/kit/format';
import {
  sysmonState,
  type CapturedSnapshot,
  type SnapSubtab,
  type SnapSubtabKind,
} from './state';
import {SnapshotAppsView} from './views/apps_view';
import {SnapshotCpuView, SnapshotWaitExplainerView} from './views/cpu_view';
import {SnapshotMemoryView} from './views/memory_view';
import {SnapshotProcessView} from './views/process_view';
import {SnapshotStartView} from './views/start_view';
import {SnapshotStorageView} from './views/storage_view';
import {SnapshotThermalView} from './views/thermal_view';

const SUBTAB_COLORS: Partial<Record<SnapSubtabKind, string>> = {
  apps: '#16a34a',
  cpu: '#2563eb',
  wait: '#d97706',
  memory: '#7c3aed',
  storage: '#0284c7',
  thermal: '#ea580c',
};

// Progressive workspace inside one Snapshot (modeled on lat/workspace.ts):
// opens on Start only; drilling into an area, wait cause, or process opens
// a closable preview tab.
export function SnapshotWorkspace(): m.Component<{
  readonly snap: CapturedSnapshot;
}> {
  const openAnotherTrace = () => {
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.pftrace,.perfetto-trace,.pb,.gz';
    input.onchange = async () => {
      const f = input.files?.[0];
      if (!f) return;
      const buf = new Uint8Array(await f.arrayBuffer());
      await sysmonState.openTraceBytes(buf, f.name);
    };
    input.click();
  };

  return {
    view({attrs: {snap}}) {
      const p = snap.pulse;
      const facts: PageFact[] = [
        {
          icon: snap.source === 'live' ? 'smartphone' : 'description',
          text: snap.deviceName,
        },
      ];
      if (p) {
        facts.push({icon: 'schedule', text: `${p.durS.toFixed(1)} s`});
        facts.push({icon: 'memory', text: `${p.cpuCount} CPUs`});
        if (p.memTotalBytes) {
          facts.push({icon: 'storage', text: fmtBytes(p.memTotalBytes)});
        }
      }
      facts.push({icon: 'data_object', text: fmtBytes(snap.sizeBytes)});

      const subtabs: TabsTab[] = [
        {
          key: 'start',
          title: m('span.pf-prod-pinned-tab', 'Start'),
          leftIcon: 'flag',
          content: m(SnapshotStartView, {snap}),
        },
        ...snap.subtabs.map((t) => buildSubtab(snap, t)),
      ];

      return pageShell(
        {
          title: snap.label,
          facts,
          actions: [
            m(Button, {
              label: 'Load another trace',
              icon: 'add',
              variant: ButtonVariant.Minimal,
              onclick: openAnotherTrace,
            }),
            m(Button, {
              label: 'Download .pftrace',
              icon: 'download',
              variant: ButtonVariant.Outlined,
              onclick: () => void sysmonState.downloadSnapshot(snap),
            }),
            m(Button, {
              label: 'Recording details',
              icon: 'info',
              variant: ButtonVariant.Minimal,
              onclick: () =>
                sysmonState.openSnapSubtab(snap, 'details', undefined, true),
            }),
          ],
        },
        m(Tabs, {
          className: 'pf-prod-tabs',
          variant: 'underline',
          tabs: subtabs,
          activeTabKey: snap.activeSubtabKey,
          onTabChange: (key: string) => {
            snap.activeSubtabKey = key;
          },
          onTabClose: (key: string) => sysmonState.closeSnapSubtab(snap, key),
        }),
      );
    },
  };
}

function buildSubtab(snap: CapturedSnapshot, t: SnapSubtab): TabsTab {
  const color = SUBTAB_COLORS[t.kind];
  return {
    key: t.key,
    leftIcon: color
      ? m(Icon, {icon: t.icon, style: {color}})
      : t.icon,
    closeButton: true,
    lazy: true,
    title: m(
      'span.pf-prod-scope-tab',
      {
        className: classNames(!t.kept && 'pf-prod-scope-tab--preview'),
        title: t.kept ? t.label : `${t.label} (preview — double-click to keep)`,
        ondblclick: () => {
          t.kept = true;
        },
      },
      m('span.pf-prod-scope-tab__text', t.label),
    ),
    content: renderSubtabBody(snap, t),
  };
}

function renderSubtabBody(snap: CapturedSnapshot, t: SnapSubtab): m.Children {
  switch (t.kind) {
    case 'apps':
      return m(SnapshotAppsView, {snap});
    case 'memory':
      return m(SnapshotMemoryView, {snap});
    case 'cpu':
      return m(SnapshotCpuView, {snap});
    case 'wait':
      return m(SnapshotWaitExplainerView, {
        snap,
        category:
          (t.param as 'runnable' | 'io_wait' | 'uninterruptible') ?? 'runnable',
      });
    case 'storage':
      return m(SnapshotStorageView, {snap});
    case 'thermal':
      return m(SnapshotThermalView, {snap});
    case 'process':
      return m(SnapshotProcessView, {snap, processName: t.param ?? ''});
    case 'details':
      return renderRecordingDetails(snap);
    default:
      return undefined;
  }
}

function renderRecordingDetails(snap: CapturedSnapshot): m.Children {
  const p = snap.pulse;
  return m('.pf-prod-landing', [
    questionBlock(
      {
        question: 'Recording details',
        answer: p
          ? `Captured ${p.durS.toFixed(1)}s on ${snap.deviceName} (${fmtBytes(snap.sizeBytes)}).`
          : `Loaded ${snap.deviceName} (${fmtBytes(snap.sizeBytes)}).`,
      },
      p &&
        m('.pf-prod-stats', [
          stat('Scheduling (sched_switch)', p.hasSched ? 'Present' : 'Not recorded'),
          stat('Pressure Stall Info (PSI)', p.hasPsi ? 'Present' : 'Not recorded'),
          stat('Meminfo / vmstat', p.hasMeminfo ? 'Present' : 'Not recorded'),
          stat(
            'Storage (diskstat / block)',
            p.hasDiskstat || p.hasBlock ? 'Present' : 'Not recorded',
          ),
          stat('Thermal zones', p.hasThermal ? 'Present' : 'Not recorded'),
          stat('ODPM power rails', p.hasPower ? 'Present' : 'Not recorded'),
        ]),
    ),
  ]);
}
