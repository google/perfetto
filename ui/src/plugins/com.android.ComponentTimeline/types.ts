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

export type ComponentCategory =
  | 'broadcast'
  | 'service'
  | 'provider'
  | 'job'
  | 'activity'
  | 'proc_state'
  | 'all';

export type BucketState = '-' | 'S' | 'R' | 'C' | 'I' | 'K';

export type ActiveBucketState = 'S' | 'R' | 'C' | 'I' | 'K';

export type ColorMode = 'component' | 'proc_state';

export type ProcStateFamily =
  'TOP' | 'FG_SVC' | 'RECEIVER' | 'SERVICE' | 'CACHED' | 'KILLED';

export const ALL_TARGETS_VALUE = '__ALL__';

export interface ComponentTargetOption {
  readonly target: string;
  readonly eventCount: number;
  readonly procCount: number;
  readonly firstTs: bigint;
}

export interface CategorySummary {
  readonly category: ComponentCategory;
  readonly label: string;
  readonly eventCount: number;
  readonly procCount: number;
  readonly targets: ReadonlyArray<ComponentTargetOption>;
}

export interface TraceHeaderMetadata {
  readonly uuid: string;
  readonly device: string;
  readonly build: string;
  readonly trigger: string | null;
  readonly ncpu: number;
  readonly nlmk: number;
  readonly hasFrameworkProcState: boolean;
  readonly windowStartNs: bigint;
  readonly windowEndNs: bigint;
}

export interface ProcessTimelineRow {
  readonly upid: number;
  readonly pid: number;
  readonly name: string;
  readonly appearMs: number;
  readonly started: boolean;
  readonly deathMs: number | null;
  readonly goneMs: number | null;
  readonly lmk: boolean;
  readonly oom: number | null;
  readonly slot: number;
  readonly firstEventTs: bigint;
  readonly states: ReadonlyArray<BucketState>;
  readonly cpuMs: ReadonlyArray<number>;
  readonly rssMb: ReadonlyArray<number | null>;
  readonly anonMb: ReadonlyArray<number | null>;
  readonly swapMb: ReadonlyArray<number>;
  readonly procStates: ReadonlyArray<string | null>;
  readonly oomScores: ReadonlyArray<number | null>;
}

export interface TimelineDataset {
  readonly category: ComponentCategory;
  readonly target: string;
  readonly title: string;
  readonly t0Label: string;
  readonly nounPlural: string;
  readonly activeVerb: string;
  readonly activeShort: string;
  readonly t0Ns: bigint;
  readonly bucketMs: number;
  readonly lingerMs: number;
  readonly idleThresholdMs: number;
  readonly b0: number;
  readonly nb: number;
  readonly preMs: number;
  readonly postMs: number;
  readonly cpuScaleMs: number;
  readonly memScaleMb: number;
  readonly nslots: number;
  readonly rows: ReadonlyArray<ProcessTimelineRow>;
  readonly counts: ReadonlyArray<Readonly<Record<ActiveBucketState, number>>>;
  readonly procStateCounts: ReadonlyArray<
    Readonly<Record<ProcStateFamily, number>>
  >;
  readonly totAnonMb: ReadonlyArray<number>;
  readonly totSwapMb: ReadonlyArray<number>;
  readonly totRssMb: ReadonlyArray<number>;
}

export const STATE_COLORS: Readonly<Record<ActiveBucketState, string>> = {
  S: '#e9a23b',
  R: '#3b7ddd',
  C: '#2fa36b',
  I: '#a4acb7',
  K: '#e04848',
};

export const PROC_STATE_FAMILY_COLORS: Readonly<
  Record<ProcStateFamily, string>
> = {
  TOP: '#2563eb',
  FG_SVC: '#0d9488',
  RECEIVER: '#d97706',
  SERVICE: '#7c3aed',
  CACHED: '#94a3b8',
  KILLED: '#e04848',
};

export const PROC_STATE_FAMILY_LABELS: Readonly<
  Record<ProcStateFamily, string>
> = {
  TOP: 'Top / Foreground UI',
  FG_SVC: 'Foreground Service',
  RECEIVER: 'Receiver / Backup',
  SERVICE: 'Service / Heavy',
  CACHED: 'Cached / Background',
  KILLED: 'Killed (5 s linger)',
};

export const PROC_STATE_FAMILY_SHORT: Readonly<
  Record<ProcStateFamily, string>
> = {
  TOP: 'top/fg',
  FG_SVC: 'fg-svc',
  RECEIVER: 'receiver',
  SERVICE: 'service',
  CACHED: 'cached',
  KILLED: 'killed',
};

export function classifyProcStateFamily(
  procState: string | null,
  bucketState: BucketState,
): ProcStateFamily {
  if (bucketState === 'K') {
    return 'KILLED';
  }
  if (procState === null || procState === '') {
    return 'CACHED';
  }
  const s = procState.toUpperCase();
  if (
    s === 'TOP' ||
    s === 'BOUND_TOP' ||
    s === 'PERSISTENT' ||
    s === 'PERSISTENT_UI' ||
    s === 'IMPORTANT_FOREGROUND' ||
    s === 'TOP_SLEEPING' ||
    s.includes('TOP_APP') ||
    s === 'FOREGROUND' ||
    s === 'FOREGROUND_APP' ||
    s === 'SYSTEM' ||
    s === 'NATIVE' ||
    s === 'PERSISTENT_PROC' ||
    s === 'PERSISTENT_SERVICE'
  ) {
    return 'TOP';
  }
  if (
    s === 'FOREGROUND_SERVICE' ||
    s === 'BOUND_FOREGROUND_SERVICE' ||
    s === 'BFGS' ||
    s === 'SHORT_SERVICE' ||
    s.includes('FG_SERVICE') ||
    s.includes('FG_SVC') ||
    s === 'PERCEPTIBLE_FOREGROUND_APP'
  ) {
    return 'FG_SVC';
  }
  if (s === 'RECEIVER' || s === 'BACKUP') {
    return 'RECEIVER';
  }
  if (
    s === 'SERVICE' ||
    s === 'SERVICE_B' ||
    s === 'JOB' ||
    s === 'BACKGROUND' ||
    s === 'IMPORTANT_BACKGROUND' ||
    s === 'TRANSIENT_BACKGROUND' ||
    s === 'HEAVY_WEIGHT' ||
    s === 'HEAVY_WEIGHT_APP' ||
    s.includes('PERCEPTIBLE') ||
    s.includes('VISIBLE')
  ) {
    return 'SERVICE';
  }
  return 'CACHED';
}

export function formatShortProcState(
  procState: string | null,
  oomScore: number | null,
): string | null {
  if (procState !== null && procState !== '') {
    const key = procState.toUpperCase().replace(/^PROCESS_STATE_/, '');
    const map: Readonly<Record<string, string>> = {
      PERSISTENT: 'PERSIST',
      PERSISTENT_PROC: 'PERSIST',
      PERSISTENT_SERVICE: 'PERSIST',
      PERSISTENT_UI: 'PERS_UI',
      SYSTEM: 'SYS',
      NATIVE: 'NATIVE',
      TOP: 'TOP',
      TOP_APP: 'TOP',
      FOREGROUND: 'FG',
      FOREGROUND_APP: 'FG',
      BOUND_TOP: 'BND_TOP',
      FOREGROUND_SERVICE: 'FGS',
      FG_SERVICE: 'FGS',
      FG_SVC: 'FGS',
      BOUND_FOREGROUND_SERVICE: 'BFGS',
      BFGS: 'BFGS',
      PERCEPTIBLE_FOREGROUND_APP: 'FGS',
      SHORT_SERVICE: 'SHORT_SVC',
      IMPORTANT_FOREGROUND: 'IMP_FG',
      IMP_FG: 'IMP_FG',
      PERCEPTIBLE: 'PERCEPT',
      PERCEPTIBLE_APP: 'PERCEPT',
      PERCEPTIBLE_LOW: 'PERCEPT',
      PERCEPTIBLE_LOW_APP: 'PERCEPT',
      PERCEPTIBLE_MEDIUM: 'PERCEPT',
      PERCEPTIBLE_MEDIUM_APP: 'PERCEPT',
      VISIBLE: 'VISIBLE',
      VISIBLE_APP: 'VISIBLE',
      IMPORTANT_BACKGROUND: 'IMP_BG',
      IMP_BG: 'IMP_BG',
      TRANSIENT_BACKGROUND: 'TRANS_BG',
      BACKGROUND: 'BG',
      JOB: 'JOB',
      BACKUP: 'BACKUP',
      SERVICE: 'SVC',
      SERVICE_B: 'SVC_B',
      SERVICES: 'SVC',
      RECEIVER: 'RCVR',
      TOP_SLEEPING: 'TOP_SLP',
      HEAVY_WEIGHT: 'HEAVY',
      HEAVY_WEIGHT_APP: 'HEAVY',
      HOME: 'HOME',
      HOME_APP: 'HOME',
      LAST_ACTIVITY: 'LAST_ACT',
      PREVIOUS: 'PREV',
      PREVIOUS_APP: 'PREV',
      CACHED_ACTIVITY: 'CACHED_ACT',
      CACHED_ACT: 'CACHED_ACT',
      CACHED_ACTIVITY_CLIENT: 'CACHED_CLI',
      CACHED_RECENT: 'CACHED_REC',
      CACHED_EMPTY: 'CACHED',
      CACHED_APP: 'CACHED',
      CACHED_APP_LMK_FIRST: 'CACHED',
      CACHED: 'CACHED',
    };
    return map[key] ?? key.slice(0, 10);
  }
  if (oomScore !== null) {
    return `oom ${oomScore}`;
  }
  return null;
}

export function formatShortTarget(category: string, target: string): string {
  if (!target) return 'unknown';
  if (category === 'broadcast') {
    const prefixes = [
      'android.intent.action.',
      'com.google.android.c2dm.intent.',
      'android.appwidget.action.',
      'android.net.conn.',
      'android.accounts.',
    ];
    for (const p of prefixes) {
      if (target.startsWith(p) && target.length > p.length) {
        return target.slice(p.length);
      }
    }
    const dotIdx = target.lastIndexOf('.');
    if (dotIdx >= 0 && dotIdx + 1 < target.length) {
      return target.slice(dotIdx + 1);
    }
    return target;
  }
  if (category === 'service' || category === 'job' || category === 'activity') {
    const slashIdx = target.indexOf('/');
    const clsPart = slashIdx >= 0 ? target.slice(slashIdx + 1) : target;
    const dotIdx = clsPart.lastIndexOf('.');
    if (dotIdx >= 0 && dotIdx + 1 < clsPart.length) {
      return clsPart.slice(dotIdx + 1);
    }
    return clsPart;
  }
  if (category === 'provider') {
    const semiIdx = target.indexOf(';');
    if (semiIdx > 0) {
      return target.slice(0, semiIdx);
    }
    const dotIdx = target.lastIndexOf('.');
    if (dotIdx >= 0 && dotIdx + 1 < target.length) {
      return target.slice(dotIdx + 1);
    }
    return target;
  }
  return target;
}
