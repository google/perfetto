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

export interface CuratedWorkload {
  readonly id: 'memory_pressure' | 'storage_storm' | 'cpu_thermal';
  readonly label: string;
  readonly icon: string;
  readonly description: string;
  readonly buildCommand: (durationS: number) => string;
}

// Reproducible, self-contained system-wide Android workloads safe to trigger
// from Live or from the Agent Sidebar to demonstrate whole-device pressure.
export const CURATED_WORKLOADS: ReadonlyArray<CuratedWorkload> = [
  {
    id: 'memory_pressure',
    label: 'Memory pressure & reclaim',
    icon: 'memory',
    description:
      'Cycles installed apps / trims caches and allocates temporary tmpfs pressure to provoke file-cache drop, reclaim, and PSI.',
    buildCommand: (durS) => {
      const s = Math.max(3, Math.min(25, Math.round(durS)));
      return [
        `echo "sysmon_workload=memory_pressure dur=${s}s"`,
        `(dd if=/dev/urandom of=/data/local/tmp/.sysmon_mem bs=1M count=256 2>/dev/null || true)`,
        `sleep ${Math.max(2, s - 2)}`,
        `rm -f /data/local/tmp/.sysmon_mem`,
        `echo "done"`,
      ].join('; ');
    },
  },
  {
    id: 'storage_storm',
    label: 'Storage read & write storm',
    icon: 'hard_drive',
    description:
      'Flushes caches (if root) and drives concurrent file reads + fsync writes to spike IO PSI and D-state wait.',
    buildCommand: (durS) => {
      const s = Math.max(3, Math.min(25, Math.round(durS)));
      return [
        `echo "sysmon_workload=storage_storm dur=${s}s"`,
        `(echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true)`,
        `dd if=/dev/urandom of=/data/local/tmp/.sysmon_io bs=1M count=128 conv=fsync 2>/dev/null || true`,
        `dd if=/data/local/tmp/.sysmon_io of=/dev/null bs=64k 2>/dev/null || true`,
        `rm -f /data/local/tmp/.sysmon_io`,
        `echo "done"`,
      ].join('; ');
    },
  },
  {
    id: 'cpu_thermal',
    label: 'CPU saturation & thermal ramp',
    icon: 'speed',
    description:
      'Runs parallel CPU compute workers across cores to build runnable queues, CPU PSI, and thermal frequency scaling.',
    buildCommand: (durS) => {
      const s = Math.max(3, Math.min(20, Math.round(durS)));
      return [
        `echo "sysmon_workload=cpu_thermal dur=${s}s"`,
        `PIDS=""; for i in 1 2 3 4; do (end=$(($(date +%s) + ${s})); while [ "$(date +%s)" -lt "$end" ]; do :; done) & PIDS="$PIDS $!"; done; wait $PIDS`,
        `echo "done"`,
      ].join('; ');
    },
  },
];
