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

export interface ManifestFileEntry {
  path: string;
  clocks?: {offset_ns: number};
}

export interface PerfettoManifest {
  version: 1;
  trace_time?: {clock: ClockName};
  files?: ManifestFileEntry[];
}

export interface MergeFile {
  readonly path: string;
  readonly config: FileMergeConfig;
}

const MANIFEST_FILENAME = 'perfetto_manifest.json';

function fileClocks(config: FileMergeConfig): {offset_ns: number} | undefined {
  if (config.alignMode === 'offset' && config.offsetNs !== undefined) {
    return {offset_ns: config.offsetNs};
  }
  return undefined;
}

function buildManifest(
  files: ReadonlyArray<MergeFile>,
  traceTime: TraceTimeConfig,
): PerfettoManifest {
  const manifest: PerfettoManifest = {version: 1};
  if (traceTime.clock !== undefined) {
    manifest.trace_time = {clock: traceTime.clock};
  }
  const entries = files.map(({path, config}) => {
    const clocks = fileClocks(config);
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
    !files.some((f) => fileClocks(f.config) !== undefined)
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
