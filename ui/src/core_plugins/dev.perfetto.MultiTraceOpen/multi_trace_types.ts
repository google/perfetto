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
  format: string;
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
