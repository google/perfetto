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

// Serializes the merge configurator state into a perfetto_manifest object.
// Contract: test/trace_processor/diff_tests/parser/trace_manifest/tests.py.

import type {
  ClockName,
  FileMergeConfig,
  TraceTimeConfig,
} from './multi_trace_types';

// "Manual" mode: relate this file's clock to a clock in another trace
// (sync_to) at a fixed offset. The reference is the baseline trace (the
// trace-time master, i.e. the first file); its clock is left unnamed so the
// importer uses the baseline's sole clock.
export interface ManifestClocks {
  sync_to: {file: string; clock?: ClockName};
  offset_ns?: number;
}

export interface ManifestFileEntry {
  path: string;
  clocks?: ManifestClocks;
}

export interface PerfettoManifest {
  version: 1;
  trace_time?: {clock: ClockName};
  files?: ManifestFileEntry[];
}

export interface MergeFile {
  readonly path: string;
  readonly config: FileMergeConfig;
  // This file's single real clock, if it has exactly one; undefined for a
  // clockless trace. Names the reference clock when this file is the baseline.
  readonly clock?: ClockName;
}

const MANIFEST_FILENAME = 'perfetto_manifest.json';

// The clocks block for a file, or undefined. The baseline (first) trace is the
// reference others sync to, so it never carries a sync_to of its own; a
// non-baseline file with a fixed offset syncs to the baseline's clock (named
// when the baseline has one, else its private timeline) at that offset.
function entryClocks(
  files: ReadonlyArray<MergeFile>,
  index: number,
): ManifestClocks | undefined {
  const baseline = files[0];
  if (index === 0 || baseline === undefined) {
    return undefined;
  }
  const {alignMode, offsetNs} = files[index].config;
  if (alignMode !== 'offset' || offsetNs === undefined) {
    return undefined;
  }
  const syncTo =
    baseline.clock === undefined
      ? {file: baseline.path}
      : {file: baseline.path, clock: baseline.clock};
  return {sync_to: syncTo, offset_ns: offsetNs};
}

function buildManifest(
  files: ReadonlyArray<MergeFile>,
  traceTime: TraceTimeConfig,
): PerfettoManifest {
  const manifest: PerfettoManifest = {version: 1};
  if (traceTime.clock !== undefined) {
    manifest.trace_time = {clock: traceTime.clock};
  }
  const entries = files.map(({path}, i) => {
    const clocks = entryClocks(files, i);
    return clocks === undefined ? {path} : {path, clocks};
  });
  // Bare {path} entries are no-ops; omit `files` when none carry config.
  if (entries.some((e) => e.clocks !== undefined)) {
    manifest.files = entries;
  }
  return manifest;
}

// Empty beyond auto-align: callers skip the manifest and open plain files.
export function isTrivialManifest(
  files: ReadonlyArray<MergeFile>,
  traceTime: TraceTimeConfig,
): boolean {
  return (
    traceTime.clock === undefined &&
    !files.some((_, i) => entryClocks(files, i) !== undefined)
  );
}

export function manifestToJson(
  files: ReadonlyArray<MergeFile>,
  traceTime: TraceTimeConfig,
): string {
  const manifest = {perfetto_manifest: buildManifest(files, traceTime)};
  return JSON.stringify(manifest, null, 2);
}

export function buildManifestFile(
  files: ReadonlyArray<MergeFile>,
  traceTime: TraceTimeConfig,
): File {
  const json = manifestToJson(files, traceTime);
  return new File([json], MANIFEST_FILENAME, {type: 'application/json'});
}
