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
// The schema mirrors the reader in
// src/trace_processor/plugins/perfetto_manifest/perfetto_manifest_reader.cc;
// the diff tests in
// test/trace_processor/diff_tests/parser/trace_manifest/tests.py are the
// contract.

import type {
  AlignMode,
  ClockName,
  MachineRemap,
  TraceTimeConfig,
} from './multi_trace_types';

// A file's "clocks" block: relate this file's own clock (`clock`) to a clock in
// another trace (`sync_to`) at a fixed `offset_ns`. `clock` names the file's
// builtin clock to relate (omit for a clockless file, which pins its private
// per-file clock); sync_to.file is required; sync_to.clock is omitted when the
// reference exposes a single clock (the reader resolves it).
export interface ManifestClocks {
  clock?: ClockName;
  sync_to: {file: string; clock?: ClockName};
  offset_ns?: number;
}

export interface ManifestFileEntry {
  path: string;
  machine?: {name: string};
  machines?: Array<{id: number; name: string}>;
  clocks?: ManifestClocks;
}

export interface PerfettoManifest {
  version: 1;
  trace_time?: {clock: ClockName};
  files?: ManifestFileEntry[];
}

// One file's contribution to the merge, resolved from the UI state by the
// controller: the manual reference is already resolved to a file path + clock,
// and the single-machine assignment to a name.
export interface MergeFile {
  readonly path: string;
  readonly alignMode: AlignMode;
  readonly offsetNs?: number;
  // This file's own builtin clock to relate to the reference (undefined => a
  // clockless file, whose private clock is pinned instead).
  readonly sourceClock?: ClockName;
  // Resolved manual reference: the file (and optionally clock) to offset
  // against. Undefined for `auto` or until a reference resolves.
  readonly reference?: {file: string; clock?: ClockName};
  // Resolved single-machine assignment (trimmed, non-empty), else undefined.
  readonly machineName?: string;
  // Multi-machine proto remap, straight from the config.
  readonly machines?: ReadonlyArray<MachineRemap>;
}

const MANIFEST_FILENAME = 'perfetto_manifest.json';

// The clocks block for a file, or undefined. Only `manual` mode with an offset
// and a resolved reference emits one; everything else auto-aligns.
function fileClocks(file: MergeFile): ManifestClocks | undefined {
  if (
    file.alignMode !== 'manual' ||
    file.offsetNs === undefined ||
    file.reference === undefined
  ) {
    return undefined;
  }
  const {file: refFile, clock} = file.reference;
  const syncTo = clock === undefined ? {file: refFile} : {file: refFile, clock};
  const clocks: ManifestClocks = {sync_to: syncTo, offset_ns: file.offsetNs};
  if (file.sourceClock !== undefined) {
    clocks.clock = file.sourceClock;
  }
  return clocks;
}

// Full file entry: machine identity (machine.name or machines[]) plus clocks.
// machine.name (single-machine) and machines[] (multi-machine) are mutually
// exclusive: a file is one or the other.
function serializeFile(file: MergeFile): ManifestFileEntry {
  const entry: ManifestFileEntry = {path: file.path};

  const name = file.machineName?.trim();
  if (name !== undefined && name.length > 0) {
    entry.machine = {name};
  } else if (file.machines !== undefined) {
    // Emit only when every id is named: the importer rejects a partial
    // machines[] and blank names are not valid.
    const allNamed =
      file.machines.length > 0 &&
      file.machines.every((mm) => mm.name.trim().length > 0);
    if (allNamed) {
      entry.machines = file.machines.map((mm) => ({
        id: mm.id,
        name: mm.name.trim(),
      }));
    }
  }

  const clocks = fileClocks(file);
  if (clocks !== undefined) {
    entry.clocks = clocks;
  }
  return entry;
}

function hasConfig(entry: ManifestFileEntry): boolean {
  return (
    entry.machine !== undefined ||
    entry.machines !== undefined ||
    entry.clocks !== undefined
  );
}

function buildManifest(
  files: ReadonlyArray<MergeFile>,
  traceTime: TraceTimeConfig,
): PerfettoManifest {
  const manifest: PerfettoManifest = {version: 1};
  if (traceTime.clock !== undefined) {
    manifest.trace_time = {clock: traceTime.clock};
  }
  const entries = files.map(serializeFile);
  // Bare {path} entries are no-ops; omit `files` when none carry config.
  if (entries.some(hasConfig)) {
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
    !files.some((f) => hasConfig(serializeFile(f)))
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
