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
import {Intent} from '../../../../ui/src/widgets/common';
import {Select} from '../../../../ui/src/widgets/select';
import {pageShell, questionBlock, stat} from '../../../common/ui/kit/blocks';
import {
  fmtBytes,
  fmtDeltaBytes,
  fmtDeltaPct,
  fmtMs,
  fmtPct,
} from '../../../common/ui/kit/format';
import {sysmonState} from '../state';

export function SnapshotCompareView(): m.Component {
  return {
    view() {
      const snaps = sysmonState.snapshots;
      if (snaps.length < 2) {
        return pageShell(
          {title: 'Compare snapshots'},
          m(
            '.pf-sysmon-compare-body',
            m(
              '.pf-prod-landing',
              questionBlock({
                question: 'Compare two snapshots',
                answer:
                  'Capture at least two snapshots (or load a second trace) to compare what changed across CPU, Memory, Storage, and Thermal.',
              }),
            ),
          ),
        );
      }

      const left =
        snaps.find((s) => s.id === sysmonState.compareLeftId) ?? snaps[0]!;
      const right =
        snaps.find((s) => s.id === sysmonState.compareRightId) ??
        snaps[snaps.length - 1]!;
      const a = left.pulse;
      const b = right.pulse;

      const dCpu =
        a?.cpuBusyPct !== null &&
        a?.cpuBusyPct !== undefined &&
        b?.cpuBusyPct !== null &&
        b?.cpuBusyPct !== undefined
          ? b.cpuBusyPct - a.cpuBusyPct
          : null;
      const dMemAvail =
        a?.memAvailMinBytes !== null &&
        a?.memAvailMinBytes !== undefined &&
        b?.memAvailMinBytes !== null &&
        b?.memAvailMinBytes !== undefined
          ? b.memAvailMinBytes - a.memAvailMinBytes
          : null;
      const dIoMb =
        a && b ? b.ioReadMb + b.ioWriteMb - (a.ioReadMb + a.ioWriteMb) : null;
      const dIoWait = a && b ? b.ioDstateMs - a.ioDstateMs : null;
      const dLmkd = a && b ? b.lmkdKills - a.lmkdKills : 0;

      const findings: string[] = [];
      if (dCpu !== null && Math.abs(dCpu) >= 5) {
        findings.push(
          `CPU busy ${dCpu > 0 ? 'increased' : 'decreased'} by ${fmtDeltaPct(dCpu)} (${fmtPct(a?.cpuBusyPct)} → ${fmtPct(b?.cpuBusyPct)}).`,
        );
      }
      if (dMemAvail !== null && Math.abs(dMemAvail) >= 64 * 1024 * 1024) {
        findings.push(
          `Minimum available RAM shifted by ${fmtDeltaBytes(dMemAvail)} (${fmtBytes(a?.memAvailMinBytes)} → ${fmtBytes(b?.memAvailMinBytes)}).`,
        );
      }
      if (dLmkd !== 0) {
        findings.push(
          `LMKD kills changed from ${a?.lmkdKills ?? 0} to ${b?.lmkdKills ?? 0}.`,
        );
      }
      if (dIoMb !== null && Math.abs(dIoMb) >= 10) {
        findings.push(
          `Storage volume shifted by ${dIoMb > 0 ? '+' : ''}${dIoMb.toFixed(1)} MB (${((a?.ioReadMb ?? 0) + (a?.ioWriteMb ?? 0)).toFixed(1)} MB → ${((b?.ioReadMb ?? 0) + (b?.ioWriteMb ?? 0)).toFixed(1)} MB).`,
        );
      }
      if (findings.length === 0) {
        findings.push(
          `Headline CPU, Memory, and Storage metrics look broadly similar between ${left.label} and ${right.label}.`,
        );
      }

      return pageShell(
        {
          title: 'Compare',
          actions: m('.pf-sysmon-compare-controls', [
            m('label', [
              'Baseline',
              m(
                Select,
                {
                  value: left.id,
                  onchange: (e: Event) => {
                    sysmonState.compareLeftId = (
                      e.target as HTMLSelectElement
                    ).value;
                  },
                },
                snaps.map((s) => m('option', {value: s.id}, s.label)),
              ),
            ]),
            m('span.pf-sysmon-compare-vs', 'vs'),
            m('label', [
              'Comparison',
              m(
                Select,
                {
                  value: right.id,
                  onchange: (e: Event) => {
                    sysmonState.compareRightId = (
                      e.target as HTMLSelectElement
                    ).value;
                  },
                },
                snaps.map((s) => m('option', {value: s.id}, s.label)),
              ),
            ]),
          ]),
        },
        m(
          '.pf-sysmon-compare-body',
          m('.pf-prod-landing', [
            questionBlock(
              {
                question: `What changed between ${left.label} and ${right.label}?`,
                description: 'Largest whole-device resource shifts between snapshots.',
                answer: findings,
                answerIntent:
                  dLmkd > 0 || (dCpu ?? 0) >= 20 || (dIoWait ?? 0) >= 1000
                    ? Intent.Warning
                    : Intent.Primary,
                actions: [
                  {
                    label: `Open ${left.label}`,
                    onclick: () => sysmonState.selectSnapshot(left.id),
                  },
                  {
                    label: `Open ${right.label}`,
                    onclick: () => sysmonState.selectSnapshot(right.id),
                  },
                ],
              },
              m('.pf-prod-stats', [
                stat(
                  'CPU busy Δ',
                  dCpu !== null ? fmtDeltaPct(dCpu) : '—',
                  `${fmtPct(a?.cpuBusyPct)} → ${fmtPct(b?.cpuBusyPct)}`,
                  dCpu !== null && dCpu >= 15 ? 'warn' : undefined,
                ),
                stat(
                  'Min RAM avail Δ',
                  dMemAvail !== null ? fmtDeltaBytes(dMemAvail) : '—',
                  `${fmtBytes(a?.memAvailMinBytes)} → ${fmtBytes(b?.memAvailMinBytes)}`,
                  dMemAvail !== null && dMemAvail <= -256 * 1024 * 1024
                    ? 'warn'
                    : undefined,
                ),
                stat(
                  'Storage volume Δ',
                  dIoMb !== null
                    ? `${dIoMb >= 0 ? '+' : ''}${dIoMb.toFixed(1)} MB`
                    : '—',
                  `D-state wait: ${fmtMs(a?.ioDstateMs)} → ${fmtMs(b?.ioDstateMs)}`,
                ),
                stat(
                  'LMKD kills Δ',
                  `${dLmkd >= 0 ? '+' : ''}${dLmkd}`,
                  `${a?.lmkdKills ?? 0} → ${b?.lmkdKills ?? 0}`,
                  dLmkd > 0 ? 'bad' : undefined,
                ),
              ]),
            ),
          ]),
        ),
      );
    },
  };
}
