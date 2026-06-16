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

// Multi-trace baseline pool. Each pooled trace owns its own
// WasmEngineProxy. The active baseline is one (trace, dump) ref;
// diff views read it via `getActiveBaseline()` and re-render on change.

import m from 'mithril';
import type {Engine} from '../../../trace_processor/engine';
import type {HeapDump} from '../queries';
import {clearDiffRows} from '../diff/diff_debug';

export interface BaselineTrace {
  readonly id: string;
  readonly engine: Engine;
  readonly title: string;
  readonly dumps: ReadonlyArray<HeapDump>;
  // False for the synthetic "self" entry whose engine is the primary
  // trace's own — disposing it would tear down the primary. Defaults to
  // true (real pooled baselines own their workers).
  readonly disposable?: boolean;
}

export interface BaselineDumpRef {
  readonly trace: BaselineTrace;
  readonly dump: HeapDump;
}

export type DiffMode = 'diff' | 'current' | 'baseline';

let traces: BaselineTrace[] = [];
let active: BaselineDumpRef | null = null;
let mode: DiffMode = 'diff';
let nextTraceId = 1;

export function getBaselineTraces(): ReadonlyArray<BaselineTrace> {
  return traces;
}

export function getActiveBaseline(): BaselineDumpRef | null {
  return active;
}

export function getMode(): DiffMode {
  return mode;
}

export function setMode(m: DiffMode): void {
  mode = m;
  redraw();
}

export function isDiffActive(): boolean {
  return active !== null && mode === 'diff';
}

// True when the active baseline lives in the primary trace's own
// engine — the same-trace ("self") case. Same-trace diffs can run
// the whole computation as one SQL JOIN against `android_heap_graph_*`
// instead of two filter queries + a JS merge, since both sides are
// served by the same SQLite. `disposable === false` is the singleton
// signal we set in setSelfTraceBaseline.
export function isSelfTraceDiff(): boolean {
  return (
    active !== null && mode === 'diff' && active.trace.disposable === false
  );
}

// "1=1" when no active baseline so callers can splice unconditionally
// into a WHERE clause.
export function baselineDumpFilterSql(alias: string = 'o'): string {
  if (!active) return '1=1';
  const d = active.dump;
  return `${alias}.upid = ${d.upid} AND ${alias}.graph_sample_ts = ${d.ts}`;
}

export function addBaselineTrace(
  engine: Engine,
  title: string,
  dumps: ReadonlyArray<HeapDump>,
): BaselineTrace {
  const t: BaselineTrace = {
    id: `btrace-${nextTraceId++}`,
    engine,
    title,
    dumps,
  };
  traces = [...traces, t];
  redraw();
  return t;
}

// Pick a dump from the primary trace itself as the baseline. Lazily
// registers a singleton self-baseline entry per primary engine — same
// engine is reused for both sides; queries serialize on its single worker
// but independent filter SQL still gives correct (different) results.
//
// The engine here is a per-plugin proxy and is freshly minted on every
// access from `trace.engine`, so we cannot use reference equality to
// dedupe — `disposable === false` is the singleton signal we own.
export function setSelfTraceBaseline(
  engine: Engine,
  title: string,
  dumps: ReadonlyArray<HeapDump>,
  dump: HeapDump,
): void {
  let self = traces.find((t) => t.disposable === false);
  if (!self) {
    self = {
      id: `btrace-${nextTraceId++}`,
      engine,
      title,
      dumps,
      disposable: false,
    };
    traces = [...traces, self];
  }
  setActiveBaseline({trace: self, dump});
}

// Picking a dump flips back into 'diff' mode. To deselect call
// clearActiveBaseline; null is intentionally not accepted here so callers
// can't bypass clearDiffRows().
export function setActiveBaseline(b: BaselineDumpRef): void {
  if (active !== null && active.trace === b.trace && active.dump === b.dump) {
    return;
  }
  active = b;
  mode = 'diff';
  redraw();
}

export function clearActiveBaseline(): void {
  if (!active) return;
  active = null;
  clearDiffRows();
  redraw();
}

export function removeBaselineTrace(traceId: string): void {
  const t = traces.find((x) => x.id === traceId);
  if (!t) return;
  if (active && active.trace === t) {
    active = null;
    clearDiffRows();
  }
  traces = traces.filter((x) => x.id !== traceId);
  disposeEngine(t);
  redraw();
}

// Clears state BEFORE disposing engines so any in-flight fetch sees
// active === null after its await and abandons the merge.
export function dispose(): void {
  if (traces.length === 0 && !active) return;
  const old = traces;
  active = null;
  traces = [];
  clearDiffRows();
  for (const t of old) disposeEngine(t);
  redraw();
}

function disposeEngine(t: BaselineTrace): void {
  if (t.disposable === false) return;
  try {
    (t.engine as unknown as Disposable)[Symbol.dispose]();
  } catch (e) {
    console.error('Error disposing baseline engine:', e);
  }
}

function redraw(): void {
  m.redraw();
}

// window.__heapdumpDebug — Playwright surface, not consumed by app code.

export interface HeapdumpDebugApi {
  hasBaseline(): boolean;
  baselineFilename(): string | null;
  mode(): DiffMode;
  poolSize(): number;
  pickBaseline(title: string): boolean;
}

declare global {
  interface Window {
    __heapdumpDebug?: HeapdumpDebugApi;
  }
}

if (typeof window !== 'undefined') {
  window.__heapdumpDebug = {
    hasBaseline: () => active !== null,
    baselineFilename: () => active?.trace.title ?? null,
    mode: () => mode,
    poolSize: () => traces.length,
    pickBaseline: (title) => {
      const t = traces.find((x) => x.title === title);
      if (!t || t.dumps.length === 0) return false;
      setActiveBaseline({trace: t, dump: t.dumps[0]});
      return true;
    },
  };
}
