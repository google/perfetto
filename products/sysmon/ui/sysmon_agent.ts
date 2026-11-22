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

import type {
  AgentContextSnapshot,
  ProductAgentAdapter,
  SuggestedPrompt,
} from '../../common/ui/agent/types';
import {sysmonState, type LiveSubtab, type SnapSubtabKind} from './state';

export function buildSysmonAgentAdapter(): ProductAgentAdapter {
  return {
    productId: 'sysmon',
    productName: 'Sysmon',
    manifestKey: 'sysmon/tools.json',

    getContext(): AgentContextSnapshot {
      const view = sysmonState.activeView;
      if (view.kind === 'snapshot') {
        const snap = sysmonState.activeSnapshot;
        if (snap) {
          const winArgs = sysmonState.getWindowArgs(snap);
          const sub = snap.subtabs.find((t) => t.key === snap.activeSubtabKey);
          return {
            mode: 'snapshot',
            label: snap.label,
            subtab: sub?.label ?? 'Start',
            deviceName: snap.deviceName,
            focusProcess: sub?.kind === 'process' ? sub.param : undefined,
            engine: snap.engine,
            window:
              winArgs?.startTs !== undefined && winArgs?.endTs !== undefined
                ? {
                    startTs: winArgs.startTs,
                    endTs: winArgs.endTs,
                    label: `brushed slice (${((Number(winArgs.endTs - winArgs.startTs)) / 1e9).toFixed(1)}s)`,
                  }
                : undefined,
            extraContext:
              sysmonState.snapshots.length >= 2
                ? `Available snapshots in session: ${sysmonState.snapshots.map((s) => s.label).join(', ')}.`
                : undefined,
          };
        }
      }

      if (view.kind === 'compare') {
        const left = sysmonState.snapshots.find(
          (s) => s.id === sysmonState.compareLeftId,
        );
        const right = sysmonState.snapshots.find(
          (s) => s.id === sysmonState.compareRightId,
        );
        return {
          mode: 'compare',
          label:
            left && right ? `Compare: ${left.label} vs ${right.label}` : 'Compare',
          engine: right?.engine ?? left?.engine,
          secondaryEngine: left?.engine,
          extraContext:
            left?.pulse && right?.pulse
              ? `Baseline (${left.label}): CPU ${left.pulse.cpuBusyPct}%, min RAM avail ${Math.round((left.pulse.memAvailMinBytes ?? 0) / 1048576)} MiB, IO ${(left.pulse.ioReadMb + left.pulse.ioWriteMb).toFixed(1)} MB, LMKD ${left.pulse.lmkdKills}. Comparison (${right.label}): CPU ${right.pulse.cpuBusyPct}%, min RAM avail ${Math.round((right.pulse.memAvailMinBytes ?? 0) / 1048576)} MiB, IO ${(right.pulse.ioReadMb + right.pulse.ioWriteMb).toFixed(1)} MB, LMKD ${right.pulse.lmkdKills}.`
              : undefined,
        };
      }

      if (sysmonState.liveSession) {
        return {
          mode: 'live',
          label: `Live · ${sysmonState.liveSession.deviceName}`,
          subtab: sysmonState.liveSubtab,
          deviceName: sysmonState.liveSession.deviceName,
          engine: sysmonState.liveSession.engine,
          extraContext: sysmonState.liveSession.snapshotFlightRecorderReady
            ? 'Silent 64 MB flight recorder (compact_sched + binder + block) is armed: call take_snapshot whenever thread-wait or per-process block attribution is needed.'
            : undefined,
        };
      }

      return {
        mode: 'none',
        label: 'No device or snapshot open',
      };
    },

    getSystemPrompt(ctx: AgentContextSnapshot): string {
      return [
        'You are the Sysmon Assistant — a knowledgeable, broad-based Android/Linux systems engineer inside Perfetto Sysmon.',
        'Always call structured `sysmon_*` SQL tools before making claims, and cite verifiable evidence IDs inline as `[E1]`, `[E2]`.',
        'Never invent metrics or report unsupported counters as zero — check the availability flags in `sysmon_pulse`.',
        ctx.mode === 'live'
          ? 'You are on a LIVE connected device. You can inspect live Tier-1 counters, run curated workloads (`run_workload`), and call `take_snapshot` whenever you need full scheduling/wait/block detail from the 60s flight recorder.'
          : 'You are analyzing an immutable Snapshot. Use `ui_navigate` to guide the user to the relevant explainer tab (`start`, `memory`, `cpu`, `storage`, `thermal`, `wait`, `process`) without ever telling them to go stare at a raw timeline.',
        'Know the specialist boundaries: if a single app dominates anonymous RAM, call `suggest_handoff` for `memscope`. If the root cause requires per-file page-cache/readahead waste or 12-layer VFS/F2FS/dm-crypt/UFS storage breakdown, state clearly that this crosses into the storage specialist (`Ioscope`).',
        'Be concise, direct, and quantitative.',
      ].join('\n');
    },

    getSuggestions(ctx: AgentContextSnapshot): ReadonlyArray<SuggestedPrompt> {
      if (ctx.mode === 'live') {
        return [
          {
            icon: 'monitoring',
            label: 'What is under pressure right now?',
            prompt:
              'Check the live device pulse and tell me whether CPU, Memory, Storage, or Thermal is under pressure right now.',
          },
          {
            icon: 'photo_camera',
            label: 'Take a snapshot & explain bottlenecks',
            prompt:
              'Take an instant 60s snapshot right now, analyze CPU waits, memory pressure, and storage I/O, and navigate me to the most relevant explainer tab.',
          },
          {
            icon: 'bolt',
            label: 'Run memory pressure workload & diagnose',
            prompt:
              'Run the memory_pressure workload with snapshot_after=true, then diagnose the resulting memory & reclaim behavior.',
          },
          {
            icon: 'hard_drive',
            label: 'Run storage storm & inspect I/O waits',
            prompt:
              'Run the storage_storm workload with snapshot_after=true, then analyze storage throughput, D-state I/O waits, and top I/O processes.',
          },
        ];
      }

      if (ctx.mode === 'compare') {
        return [
          {
            icon: 'compare_arrows',
            label: 'Summarize what regressed between snapshots',
            prompt:
              'Summarize what changed most between the two snapshots and explain the likely root cause.',
          },
        ];
      }

      if (ctx.mode === 'snapshot') {
        return [
          {
            icon: 'summarize',
            label: 'Summarize what is bottlenecked here',
            prompt:
              'Run sysmon_pulse, explain what resource is bottlenecked in this snapshot, and open the most relevant explainer tab.',
          },
          {
            icon: 'hourglass_top',
            label: 'Why are threads waiting / stalling?',
            prompt:
              'Inspect CPU wait categories and explain the biggest runnable or D-state wait clusters and their wakers.',
          },
          {
            icon: 'memory',
            label: 'Audit memory ledger, reclaim & top apps',
            prompt:
              'Inspect the MemTotal physical ledger, reclaim/swap/LMKD pressure, and top memory-holding processes. Suggest Memscope if one app clearly stands out.',
          },
          {
            icon: 'hard_drive',
            label: 'Check storage I/O & specialist boundary',
            prompt:
              'Inspect storage throughput, IO PSI, D-state I/O wait, and top I/O processes, and tell me if we need a deeper storage specialist.',
          },
        ];
      }

      return [
        {
          icon: 'help_outline',
          label: 'How does Sysmon work?',
          prompt:
            'Briefly explain how Sysmon separates Live monitoring, instant 60s Snapshots, and handoffs to Memscope.',
        },
      ];
    },

    device: {
      async getStatus() {
        const live = sysmonState.liveSession;
        return {
          connected: live !== undefined,
          device_name: live?.deviceName ?? null,
          live_status: live?.status ?? 'not connected',
          flight_recorder_armed: live?.snapshotFlightRecorderReady ?? false,
          snapshots: sysmonState.snapshots.map((s) => ({
            id: s.id,
            label: s.label,
            source: s.source,
            size_bytes: s.sizeBytes,
            dur_s: s.pulse?.durS ?? null,
          })),
        };
      },
      async takeSnapshot(label?: string) {
        const snap = await sysmonState.takeInstantSnapshot(label);
        return {
          snapshot_id: snap.id,
          label: snap.label,
          dur_s: snap.pulse?.durS ?? null,
          size_bytes: snap.sizeBytes,
          has_sched: snap.pulse?.hasSched ?? false,
          has_block: snap.pulse?.hasBlock ?? false,
        };
      },
      async runWorkload(opts) {
        return sysmonState.runWorkload(opts);
      },
    },

    ui: {
      navTargets: [
        'live',
        'start',
        'memory',
        'cpu',
        'wait',
        'storage',
        'thermal',
        'process',
        'compare',
      ],
      navigate(target: string, subtab?: string): string {
        if (target === 'live') {
          sysmonState.selectLive((subtab as LiveSubtab) ?? 'overview');
          return `Switched to Live (${subtab ?? 'overview'}).`;
        }
        if (target === 'compare') {
          sysmonState.selectCompare();
          return 'Switched to Snapshot Compare view.';
        }
        const snap =
          sysmonState.activeSnapshot ??
          sysmonState.snapshots[sysmonState.snapshots.length - 1];
        if (!snap) {
          if (
            sysmonState.liveSession &&
            ['memory', 'cpu', 'storage', 'thermal'].includes(target)
          ) {
            sysmonState.selectLive(target as LiveSubtab);
            return `Switched Live tab to ${target}.`;
          }
          return 'No snapshot open to navigate.';
        }
        sysmonState.selectSnapshot(snap.id);
        if (target === 'start') {
          snap.activeSubtabKey = 'start';
          return `Opened Start tab on ${snap.label}.`;
        }
        sysmonState.openSnapSubtab(
          snap,
          target as Exclude<SnapSubtabKind, 'start'>,
          subtab,
        );
        return `Opened ${target}${subtab ? ` (${subtab})` : ''} explainer on ${snap.label}.`;
      },
      buildHandoff({product, process, reason}) {
        if (product !== 'memscope') return undefined;
        const url = sysmonState.buildMemscopeHandoffUrl(process);
        return {
          product: 'Memscope',
          label: process
            ? `Inspect ${process} in Memscope`
            : 'Open Memscope (Single-Process Memory)',
          description: reason,
          url,
          onOpen: () => void sysmonState.openInMemscope(process),
        };
      },
    },
  };
}
