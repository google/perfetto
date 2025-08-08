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
  clocks: ClockInfo[];
  syncMode: 'AUTOMATIC' | 'MANUAL';
  syncConfig: SyncConfig;
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
// Synchronization Configuration
// These types define how a trace should be synchronized with others.
// =============================================================================

// The configuration for a trace that acts as the absolute point of reference
// for all other traces.
export interface ReferenceConfig {
  syncMode: 'REFERENCE';
  referenceClock?: string;
}

// The configuration for a trace that is synchronized by aligning it to another
// trace (the "anchor").
export interface AnchoredConfig {
  syncMode: 'SYNC_TO_OTHER';
  syncClock: AnchorLink;
}

// A temporary state for a trace while its automatic configuration is being
// computed.
export interface CalculatingConfig {
  syncMode: 'CALCULATING';
}

// A union type representing any possible synchronization configuration.
export type SyncConfig = ReferenceConfig | AnchoredConfig | CalculatingConfig;

// =============================================================================
// Synchronization Primitives
// These are the low-level building blocks for the synchronization configuration.
// =============================================================================

// Defines the link between a clock in the current trace and a clock in an
// "anchor" trace.
export interface AnchorLink {
  thisTraceClock?: string;
  anchorTraceUuid?: string;
  anchorClock?: string;
  offset: Offset;
}

// A discriminated union to handle the validation of the time offset input.
// This allows the UI to store the raw string from the input while also storing
// the parsed numeric value and any validation errors.

// Represents a valid, parsed offset.
export interface ValidOffset {
  kind: 'valid';
  raw: string;
  value: number;
}

// Represents an invalid offset, storing the original string and an error message.
export interface InvalidOffset {
  kind: 'invalid';
  raw: string;
  error: string;
}

// A union type for the offset, which can be either valid or invalid.
export type Offset = ValidOffset | InvalidOffset;

// =============================================================================
// Trace Metadata
// Miscellaneous types describing the contents of a trace.
// =============================================================================

// Information about a clock domain found within a trace.
export interface ClockInfo {
  name: string;
  count: number; // The number of clock snapshots.
}
