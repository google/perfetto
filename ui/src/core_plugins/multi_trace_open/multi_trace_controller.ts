// Copyright (C) 2023 The Android Open Source Project
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

import {
  SyncConfig,
  TraceFile,
  TraceFileAnalyzed,
  AnchorLink,
} from './multi_trace_types';
import {uuidv4} from '../../base/uuid';
import {assertExists} from '../../base/logging';
import {TraceAnalyzer} from './trace_analyzer';

function getErrorMessage(e: unknown): string {
  const err = e instanceof Error ? e.message : `${e}`;
  if (err.includes('(ERR:fmt)')) {
    return `The file opened doesn't look like a Perfetto trace or any other supported trace format.`;
  }
  return err;
}

// A prioritized list of clock names to be used when selecting a reference
// clock automatically.
const PREFERRED_REFERENCE_CLOCKS = [
  'BOOTTIME',
  'MONOTONIC',
  'MONOTONIC_RAW',
  'MONOTONIC_COARSE',
  'TSC',
  'REALTIME',
  'REALTIME_COARSE',
  'PERF',
];

// A wrapper to associate a TraceFile with its internal state.
interface TraceFileWrapper {
  trace: TraceFile;
}

// The possible error states for the modal, used to show a helpful message
// to the user and disable the "Open Traces" button.
export type LoadingError =
  | 'NO_TRACES'
  | 'ANALYZING'
  | 'SYNC_ERROR'
  | 'TRACE_ERROR'
  | 'INCOMPLETE_CONFIG';

/**
 * The controller for the multi-trace modal.
 * This class manages the state of the traces, their analysis, and their
 * synchronization configuration.
 */
export class MultiTraceController {
  private wrappers: TraceFileWrapper[] = [];
  private selectedUuid?: string;
  private traceAnalyzer: TraceAnalyzer;
  private onStateChanged: () => void;
  syncError?: string;

  constructor(traceAnalyzer: TraceAnalyzer, onStateChanged: () => void) {
    this.traceAnalyzer = traceAnalyzer;
    this.onStateChanged = onStateChanged;
  }

  // ===========================================================================
  // Public Getters
  // ===========================================================================

  get traces(): ReadonlyArray<TraceFile> {
    return this.wrappers.map((x) => x.trace);
  }

  get selectedTrace(): Readonly<TraceFile> | undefined {
    return this.wrappers.find((w) => w.trace.uuid === this.selectedUuid)?.trace;
  }

  get isOpeningAllowed(): boolean {
    return this.getLoadingError() === undefined;
  }

  getLoadingError(): LoadingError | undefined {
    if (this.traces.length === 0) {
      return 'NO_TRACES';
    }
    if (this.isAnalyzing() || this.isSyncing()) {
      return 'ANALYZING';
    }
    if (this.syncError) {
      return 'SYNC_ERROR';
    }
    if (this.hasTraceError()) {
      return 'TRACE_ERROR';
    }
    if (!this.isSyncConfigComplete()) {
      return 'INCOMPLETE_CONFIG';
    }
    return undefined;
  }

  // ===========================================================================
  // Public Actions
  // ===========================================================================

  async addFiles(files: ReadonlyArray<File>) {
    const newTraces: TraceFileWrapper[] = Array.from(files).map((file) => ({
      trace: {
        file,
        uuid: uuidv4(),
        status: 'not-analyzed',
      },
    }));
    for (const {trace} of this.wrappers) {
      if (trace.status === 'analyzed' && trace.syncMode === 'AUTOMATIC') {
        trace.syncConfig = {
          syncMode: 'CALCULATING',
        };
      }
    }
    this.wrappers.push(...newTraces);
    await Promise.all(newTraces.map((trace) => this.analyzeTrace(trace)));
  }

  selectTrace(uuid: string) {
    this.selectedUuid = uuid;
  }

  removeTrace(uuid: string) {
    const index = this.wrappers.findIndex((w) => w.trace.uuid === uuid);
    if (index > -1) {
      this.wrappers.splice(index, 1);
    }
    if (this.selectedUuid === uuid) {
      this.selectedUuid = undefined;
    }
    this.recomputeSync();
    this.onStateChanged();
  }

  setTraceOffset(anchorLink: AnchorLink, rawOffset: string) {
    const offset = parseInt(rawOffset, 10);
    if (Number.isNaN(offset) || !Number.isInteger(Number(rawOffset))) {
      anchorLink.offset = {
        kind: 'invalid',
        raw: rawOffset,
        error: 'Offset must be a valid integer.',
      };
    } else {
      anchorLink.offset = {
        kind: 'valid',
        raw: rawOffset,
        value: offset,
      };
    }
    this.onStateChanged();
  }

  // ===========================================================================
  // Core Synchronization Logic
  // ===========================================================================

  findBestClock(trace: TraceFileAnalyzed): string | undefined {
    const availableClocks = new Set(trace.clocks.map((c) => c.name));
    const bestClock = PREFERRED_REFERENCE_CLOCKS.find((c) =>
      availableClocks.has(c),
    );
    return bestClock ?? trace.clocks[0]?.name;
  }

  recomputeSync() {
    this.syncError = undefined;

    if (this.traces.some((trace) => trace.status === 'analyzing')) {
      return;
    }
    const analyzedTraces = this.wrappers
      .map(({trace}) => trace)
      .filter((trace): trace is TraceFileAnalyzed => {
        return trace.status === 'analyzed';
      });

    const manualTraces = analyzedTraces.filter(
      (trace) => trace.syncMode === 'MANUAL',
    );
    const automaticTraces = analyzedTraces.filter(
      (trace) => trace.syncMode === 'AUTOMATIC',
    );

    const manualReferences = manualTraces.filter(
      (trace) => trace.syncConfig.syncMode === 'REFERENCE',
    );

    if (manualReferences.length > 1) {
      this.syncError = 'Only one reference clock can be chosen.';
      this.onStateChanged();
      return;
    }

    const adj = new Map<string, string[]>();
    const clocksByUuid = new Map<string, Set<string>>();
    for (const trace of analyzedTraces) {
      adj.set(trace.uuid, []);
      clocksByUuid.set(trace.uuid, new Set(trace.clocks.map((c) => c.name)));
    }

    for (let i = 0; i < analyzedTraces.length; i++) {
      for (let j = i + 1; j < analyzedTraces.length; j++) {
        const traceA = analyzedTraces[i];
        const traceB = analyzedTraces[j];
        const clocksA = assertExists(clocksByUuid.get(traceA.uuid));
        const clocksB = assertExists(clocksByUuid.get(traceB.uuid));
        if ([...clocksA].some((clock) => clocksB.has(clock))) {
          adj.get(traceA.uuid)!.push(traceB.uuid);
          adj.get(traceB.uuid)!.push(traceA.uuid);
        }
      }
    }

    let referenceUuid: string | undefined =
      manualReferences.length === 1 ? manualReferences[0].uuid : undefined;

    if (!referenceUuid && automaticTraces.length > 0) {
      for (const clock of PREFERRED_REFERENCE_CLOCKS) {
        const traceWithClock = automaticTraces.find((t) =>
          clocksByUuid.get(t.uuid)!.has(clock),
        );
        if (traceWithClock) {
          referenceUuid = traceWithClock.uuid;
          break;
        }
      }
      if (!referenceUuid && automaticTraces.length > 0) {
        referenceUuid = automaticTraces.reduce((a, b) =>
          adj.get(a.uuid)!.length > adj.get(b.uuid)!.length ? a : b,
        ).uuid;
      }
    }

    const newConfigs = new Map<string, SyncConfig>();
    if (referenceUuid) {
      const queue: string[] = [referenceUuid];
      const visited = new Set<string>([referenceUuid]);
      const referenceTrace = analyzedTraces.find(
        (t) => t.uuid === referenceUuid,
      )!;
      if (referenceTrace.syncMode === 'AUTOMATIC') {
        const referenceTraceClocks = clocksByUuid.get(referenceUuid)!;
        const bestClock = PREFERRED_REFERENCE_CLOCKS.find((c) =>
          referenceTraceClocks.has(c),
        );
        newConfigs.set(referenceUuid, {
          syncMode: 'REFERENCE',
          referenceClock: bestClock ?? referenceTrace.clocks[0]?.name,
        });
      }

      while (queue.length > 0) {
        const parentUuid = queue.shift()!;
        for (const childUuid of adj.get(parentUuid)!) {
          if (visited.has(childUuid)) continue;
          visited.add(childUuid);
          const childTrace = analyzedTraces.find((t) => t.uuid === childUuid)!;
          if (childTrace.syncMode === 'MANUAL') continue;

          const parentClocks = clocksByUuid.get(parentUuid)!;
          const childClocks = clocksByUuid.get(childUuid)!;
          const bestCommonClock = PREFERRED_REFERENCE_CLOCKS.find(
            (c) => parentClocks.has(c) && childClocks.has(c),
          );

          if (bestCommonClock) {
            newConfigs.set(childUuid, {
              syncMode: 'SYNC_TO_OTHER',
              syncClock: {
                thisTraceClock: bestCommonClock,
                anchorTraceUuid: parentUuid,
                anchorClock: bestCommonClock,
                offset: {kind: 'valid', raw: '0', value: 0},
              },
            });
            queue.push(childUuid);
          }
        }
      }
    }

    for (const trace of automaticTraces) {
      const newConfig = newConfigs.get(trace.uuid);
      if (newConfig) {
        trace.syncConfig = newConfig;
      } else {
        trace.syncConfig = {
          syncMode: 'REFERENCE',
          referenceClock: trace.clocks[0]?.name,
        };
      }
    }
    this.onStateChanged();
  }

  // ===========================================================================
  // Trace Analysis
  // ===========================================================================

  private async analyzeTrace(wrapper: TraceFileWrapper) {
    if (wrapper.trace.status !== 'not-analyzed') {
      return;
    }
    wrapper.trace = {
      ...wrapper.trace,
      status: 'analyzing',
      progress: 0,
    };
    this.onStateChanged();
    try {
      const result = await this.traceAnalyzer.analyze(
        wrapper.trace.file,
        (progress) => {
          if (wrapper.trace.status === 'analyzing') {
            wrapper.trace.progress = progress;
            this.onStateChanged();
          }
        },
      );

      wrapper.trace = {
        ...wrapper.trace,
        status: 'analyzed',
        format: result.format,
        clocks: result.clocks,
        syncMode: 'AUTOMATIC',
        syncConfig: {
          syncMode: 'CALCULATING',
        },
      };
    } catch (e) {
      wrapper.trace = {
        ...wrapper.trace,
        status: 'error',
        error: getErrorMessage(e),
      };
    } finally {
      this.recomputeSync();
      this.onStateChanged();
    }
  }

  // ===========================================================================
  // Internal State Helpers
  // ===========================================================================

  isAnalyzing(): boolean {
    return this.traces.some((trace) => trace.status === 'analyzing');
  }

  private hasTraceError(): boolean {
    return this.traces.some((trace) => trace.status === 'error');
  }

  private isSyncing(): boolean {
    return this.traces.some((trace) => {
      return (
        trace.status === 'analyzed' &&
        trace.syncConfig.syncMode === 'CALCULATING'
      );
    });
  }

  private isSyncConfigComplete(): boolean {
    for (const trace of this.traces) {
      if (trace.status !== 'analyzed') {
        continue;
      }
      const config = trace.syncConfig;
      if (config.syncMode === 'REFERENCE') {
        if (config.referenceClock === undefined) {
          return false;
        }
      } else if (config.syncMode === 'SYNC_TO_OTHER') {
        if (
          config.syncClock.thisTraceClock === undefined ||
          config.syncClock.anchorClock === undefined ||
          config.syncClock.anchorTraceUuid === undefined ||
          config.syncClock.offset.kind === 'invalid'
        ) {
          return false;
        }
      }
    }
    return true;
  }

  // ===========================================================================
  // Testing Helpers
  // ===========================================================================

  // visible for testing
  setTracesForTesting(traces: TraceFile[]) {
    this.wrappers = traces.map((trace) => ({trace}));
  }
}
