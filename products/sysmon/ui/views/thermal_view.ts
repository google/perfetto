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
import type {ColumnSchema} from '../../../../ui/src/components/widgets/datagrid/datagrid_schema';
import {Intent} from '../../../../ui/src/widgets/common';
import {Spinner} from '../../../../ui/src/widgets/spinner';
import {
  collapsiblePanel,
  disclosureMore,
  limitCallout,
  questionBlock,
  stat,
} from '../../../common/ui/kit/blocks';
import {proportionBar} from '../../../common/ui/kit/charts';
import {fmtPowerMw, fmtTempC} from '../../../common/ui/kit/format';
import {powerRailsChart, thermalZonesChart} from '../charts';
import {loadThermalBundle, type ThermalBundle} from '../queries';
import {sysmonState, type CapturedSnapshot} from '../state';
import {codeCell, countCell, powerMwCell, simpleGrid, tempCell} from '../tables';

const RAIL_COLORS = [
  '#ea580c',
  '#2563eb',
  '#7c3aed',
  '#0891b2',
  '#16a34a',
  '#d97706',
  '#64748b',
];

export function SnapshotThermalView(): m.Component<{
  readonly snap: CapturedSnapshot;
}> {
  const memo = new AsyncMemo<ThermalBundle>();
  const panelToggles = new Map<string, boolean>();
  let showAllRails = false;
  let showAllZones = false;

  const isOpen = (id: string, defaultOpen: boolean) =>
    panelToggles.get(id) ?? defaultOpen;
  const togglePanel = (id: string, defaultOpen: boolean) => {
    panelToggles.set(id, !isOpen(id, defaultOpen));
  };

  return {
    view({attrs: {snap}}) {
      const win = sysmonState.getWindowArgs(snap);
      const d = memo.use({
        key: {
          id: snap.id,
          s: win?.startTs?.toString() ?? 'all',
          e: win?.endTs?.toString() ?? 'all',
        },
        compute: () => loadThermalBundle(snap.engine, win, 25),
      }).data;

      if (!d) {
        return m('.pf-sysmon-wait', [
          m(Spinner, {easing: true}),
          m('span', 'Loading thermal & power view…'),
        ]);
      }

      const hot = d.zones[0];
      const vsysRail = d.rails.find((r) => r.rail.includes('VSYS_ALL'));
      const subRails = d.rails.filter((r) => !r.rail.includes('VSYS_ALL'));
      const topRail = subRails[0] ?? d.rails[0];
      const totalMw = vsysRail
        ? vsysRail.avgMw
        : subRails.reduce((a, r) => a + r.avgMw, 0);
      const totalMj = vsysRail
        ? vsysRail.energyMj
        : subRails.reduce((a, r) => a + r.energyMj, 0);
      const activeCooling = d.cooling.filter((c) => c.lastState > 0 || c.maxState > 0);

      const zoneSchema: ColumnSchema = {
        zone: {title: 'Thermal Zone', columnType: 'text', cellRenderer: codeCell},
        min_c: {title: 'Min', columnType: 'quantitative', cellRenderer: tempCell},
        avg_c: {title: 'Avg', columnType: 'quantitative', cellRenderer: tempCell},
        peak_c: {title: 'Peak', columnType: 'quantitative', cellRenderer: tempCell},
      };

      const railSchema: ColumnSchema = {
        rail: {title: 'Power Rail', columnType: 'text', cellRenderer: codeCell},
        avg_mw: {title: 'Avg Power', columnType: 'quantitative', cellRenderer: powerMwCell},
        energy_mj: {
          title: 'Total Energy',
          columnType: 'quantitative',
          cellRenderer: (v: unknown) => ({
            content: typeof v === 'number' ? `${v.toFixed(1)} mJ` : '—',
            align: 'right' as const,
          }),
        },
      };

      return m('.pf-prod-landing', [
        // Executive Thermal & Power Verdict
        questionBlock(
          {
            question: 'Thermal & power diagnosis',
            answer: [
              topRail
                ? `Total power draw averaged ${fmtPowerMw(totalMw)} (${totalMj.toFixed(0)} mJ total), led by \`${topRail.rail.replace(/^power\.rails\./, '')}\` at ${fmtPowerMw(topRail.avgMw)}.`
                : 'No ODPM power rail counters recorded in this trace.',
              hot
                ? `Hottest thermal zone: \`${hot.zone}\` peaked at ${fmtTempC(hot.peakC)} (avg ${fmtTempC(hot.avgC)}).`
                : undefined,
              activeCooling.length > 0
                ? `${activeCooling.length} of ${d.cooling.length} thermal cooling devices engaged throttling during this window.`
                : undefined,
            ],
            answerIntent:
              activeCooling.length > 0 || (hot?.peakC ?? 0) >= 48
                ? Intent.Warning
                : Intent.Primary,
          },
          m('.pf-prod-stats', [
            stat(
              'Total avg power',
              d.rails.length > 0 ? fmtPowerMw(totalMw) : '—',
              d.rails.length > 0 ? `${totalMj.toFixed(0)} mJ across ${d.rails.length} rails` : 'No ODPM rails',
            ),
            stat(
              'Top subsystem rail',
              topRail ? fmtPowerMw(topRail.avgMw) : '—',
              topRail?.rail.replace(/^power\.rails\./, ''),
            ),
            stat(
              'Peak temperature',
              hot ? fmtTempC(hot.peakC) : '—',
              hot ? hot.zone : 'No thermal zones',
              (hot?.peakC ?? 0) >= 48 ? 'warn' : undefined,
            ),
            stat(
              'Cooling / Throttling',
              d.cooling.length > 0 ? `${activeCooling.length} active` : 'None',
              d.cooling.length > 0 ? `of ${d.cooling.length} cooling devices` : 'No cooling counters',
              activeCooling.length > 0 ? 'warn' : undefined,
            ),
          ]),
        ),

        // Panel 1: ODPM Power Rails & Subsystem Share
        d.rails.length > 0 &&
          collapsiblePanel(
            {
              title: 'ODPM Power Rails & Subsystem Share',
              icon: 'bolt',
              color: '#ea580c',
              metric: fmtPowerMw(totalMw),
              metricSub: `${d.rails.length} rails`,
              summary: subRails
                .slice(0, 3)
                .map((r) => `${r.rail.replace(/^power\.rails\./, '')}: ${fmtPowerMw(r.avgMw)}`)
                .join(' · '),
              open: isOpen('power', true),
              onToggle: () => togglePanel('power', true),
            },
            () => [
              subRails.length > 0 &&
                proportionBar(
                  subRails.slice(0, 7).map((r, idx) => ({
                    label: r.rail.replace(/^power\.rails\./, ''),
                    value: r.avgMw,
                    display: fmtPowerMw(r.avgMw),
                    color: RAIL_COLORS[idx % RAIL_COLORS.length]!,
                  })),
                ),
              powerRailsChart(d.powerSeries, 220),
              disclosureMore(
                {
                  open: showAllRails,
                  label: `Show all power rails (${d.rails.length} rails)`,
                  openLabel: 'Hide power rails table',
                  onToggle: () => {
                    showAllRails = !showAllRails;
                  },
                },
                () =>
                  simpleGrid(
                    railSchema,
                    d.rails.map((r) => ({
                      rail: r.rail,
                      avg_mw: r.avgMw,
                      energy_mj: r.energyMj,
                    })),
                  ),
              ),
            ],
          ),

        // Panel 2: Thermal Zones & Temperature Trends
        d.zones.length > 0 &&
          collapsiblePanel(
            {
              title: 'Thermal Zones & Temperature Trends',
              icon: 'thermostat',
              color: '#dc2626',
              metric: hot ? fmtTempC(hot.peakC) : undefined,
              metricSub: `${d.zones.length} zones`,
              badge:
                (hot?.peakC ?? 0) >= 48
                  ? {label: 'Warm', tone: 'warn'}
                  : {label: 'Normal', tone: 'ok'},
              summary: d.zones
                .slice(0, 3)
                .map((z) => `${z.zone} peak ${fmtTempC(z.peakC)}`)
                .join(' · '),
              quiet: (hot?.peakC ?? 0) < 42,
              open: isOpen('zones', (hot?.peakC ?? 0) >= 42 || d.rails.length === 0),
              onToggle: () =>
                togglePanel('zones', (hot?.peakC ?? 0) >= 42 || d.rails.length === 0),
            },
            () => [
              thermalZonesChart(d.zoneSeries, 200),
              disclosureMore(
                {
                  open: showAllZones,
                  label: `Show all thermal zones (${d.zones.length} zones)`,
                  openLabel: 'Hide thermal zones table',
                  onToggle: () => {
                    showAllZones = !showAllZones;
                  },
                },
                () =>
                  simpleGrid(
                    zoneSchema,
                    d.zones.map((z) => ({
                      zone: z.zone,
                      min_c: z.minC,
                      avg_c: z.avgC,
                      peak_c: z.peakC,
                    })),
                  ),
              ),
            ],
          ),

        // Panel 3: Cooling Devices & Frequency Throttling (collapsed when idle)
        d.cooling.length > 0 &&
          collapsiblePanel(
            {
              title: 'Cooling Devices & Frequency Throttling',
              icon: 'ac_unit',
              color: '#0891b2',
              metric: `${activeCooling.length} active`,
              metricSub: `of ${d.cooling.length} devices`,
              badge:
                activeCooling.length > 0
                  ? {label: 'Throttling', tone: 'warn'}
                  : {label: 'Quiet', tone: 'ok'},
              summary:
                activeCooling.length > 0
                  ? `Active mitigation: ${activeCooling.slice(0, 3).map((c) => `${c.device} (max state ${c.maxState})`).join(', ')}.`
                  : 'All thermal cooling devices remained at state 0 — no frequency capping observed.',
              quiet: activeCooling.length === 0,
              open: isOpen('cooling', activeCooling.length > 0),
              onToggle: () => togglePanel('cooling', activeCooling.length > 0),
            },
            () =>
              simpleGrid(
                {
                  device: {title: 'Cooling Device', columnType: 'text', cellRenderer: codeCell},
                  last: {title: 'Final State', columnType: 'quantitative', cellRenderer: countCell},
                  max: {title: 'Peak State', columnType: 'quantitative', cellRenderer: countCell},
                  updates: {title: 'Updates', columnType: 'quantitative', cellRenderer: countCell},
                },
                d.cooling.map((c) => ({
                  device: c.device,
                  last: c.lastState,
                  max: c.maxState,
                  updates: c.updates,
                })),
              ),
          ),

        limitCallout({
          badge: 'Power',
          title: 'Need detailed subsystem energy attribution?',
          body: 'Sysmon correlates thermal throttling and ODPM rails with whole-device load. Detailed energy modeling requires a dedicated power tool.',
        }),
      ]);
    },
    onremove() {
      memo.dispose();
    },
  };
}
