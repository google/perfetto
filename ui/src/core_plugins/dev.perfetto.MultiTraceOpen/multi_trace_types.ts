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

// This file contains the type definitions for the multi-trace opening feature.

// =============================================================================
// Trace File States
// A trace file can be in one of several states, from not-yet-analyzed to fully
// analyzed or errored.
// =============================================================================

// The base properties for any trace file shown in the UI.
export interface TraceFileBase {
  uuid: string;
  file: File;
}

// A trace file that has been added but the analyzer has not yet started.
export interface TraceFileNotAnalyzed extends TraceFileBase {
  status: 'not-analyzed';
}

// A trace file that is currently being analyzed.
export interface TraceFileAnalyzing extends TraceFileBase {
  status: 'analyzing';
  progress: number; // A value between 0 and 1.
}

// A trace file that has been successfully analyzed.
export interface TraceFileAnalyzed extends TraceFileBase {
  status: 'analyzed';
  analysis: FileAnalysis;
}

// A trace file that failed to be analyzed.
export interface TraceFileError extends TraceFileBase {
  status: 'error';
  error: string;
}

// A union type representing any possible state of a trace file.
export type TraceFile =
  | TraceFileNotAnalyzed
  | TraceFileAnalyzing
  | TraceFileAnalyzed
  | TraceFileError;

// A union of all possible status strings.
export type TraceStatus = TraceFile['status'];

// =============================================================================
// Merge configuration: how each file is placed on the shared timeline.
// Serializes to a perfetto_manifest (see merge_manifest.ts).
// =============================================================================

export type ClockName = 'REALTIME' | 'BOOTTIME' | 'MONOTONIC';

// Builtin clock id<->name, in display order. Single source of truth.
export const BUILTIN_CLOCKS: ReadonlyArray<{id: number; name: ClockName}> = [
  {id: 1, name: 'REALTIME'},
  {id: 3, name: 'MONOTONIC'},
  {id: 6, name: 'BOOTTIME'},
];

// Manual modes supply values the UI doesn't compute (offset only, for now).
export type AlignMode = 'auto' | 'offset';

// Defaults emit nothing beyond the path, so the importer auto-aligns.
export interface FileMergeConfig {
  alignMode: AlignMode;
  offsetNs?: number;
}

export interface TraceTimeConfig {
  clock?: ClockName; // undefined => omit trace_time
}

export function defaultFileMergeConfig(): FileMergeConfig {
  return {alignMode: 'auto'};
}

// Populated by the tokenize-only dry-run; gates which controls each file shows.
export interface FileAnalysis {
  format: string;
  singleClock?: boolean;
  // No real clock, only a private trace-file clock (e.g. a Chrome JSON trace).
  privateClockOnly?: boolean;
  // The builtin clock ids present (a subset of BUILTIN_CLOCKS).
  builtinClockIds?: ReadonlyArray<number>;
}
