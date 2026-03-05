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

// Represents a term with its various forms for display.
export interface Term {
  readonly name: string;
  readonly plural: string;
  readonly title: string;
  readonly pluralTitle: string;
}

// Terminology for presenting GPU compute concepts.
// Used to adapt metric labels to different GPU architectures.
export interface Terminology {
  readonly name: string;
  readonly gpu: Term;
  readonly thread: Term;
  readonly warp: Term;
  readonly block: Term;
  readonly grid: Term;
  readonly sm: Term;
  readonly streamingMultiprocessor: Term;
  readonly sharedMem: Term;
  readonly tensor: Term;
}

// =============================================================================
// Terminology Registry & State Management
// =============================================================================

// Registry of all available terminologies.
const terminologies = new Map<string, Terminology>();

// Current UI terminology
let currentTerminologyId: string = 'cuda';

// Register a new terminology.
// Called by plugins that provide terminology implementations.
// @param id Unique identifier for the terminology (e.g., 'cuda')
// @param terminology The terminology implementation
export function registerTerminology(
  id: string,
  terminology: Terminology,
): void {
  terminologies.set(id, terminology);
}

// Get all registered terminology IDs.
// @returns Array of terminology IDs in registration order
export function getTerminologyIds(): string[] {
  return Array.from(terminologies.keys());
}

// Get all registered terminologies.
// @returns Map of terminology ID to terminology
export function getTerminologies(): ReadonlyMap<string, Terminology> {
  return terminologies;
}

// Get terminology options for UI dropdown.
// @returns Array of {id, name} objects for dropdown display
export function getTerminologyOptions(): Array<{id: string; name: string}> {
  return Array.from(terminologies.entries()).map(([id, t]) => ({
    id,
    name: t.name,
  }));
}

// =============================================================================
// UI Terminology (for display purposes)
// =============================================================================

// Get the current UI terminology.
// This terminology is used for displaying metric labels in the UI.
export function getTerminology(): Terminology {
  const t = terminologies.get(currentTerminologyId);
  if (t) return t;
  // Fall back to the first registered terminology.
  const first = terminologies.values().next();
  if (!first.done) return first.value;
  throw new Error('No terminologies registered');
}

// Get the current UI terminology ID.
export function getTerminologyId(): string {
  return currentTerminologyId;
}

// Set the current UI terminology by ID.
// @param id The terminology ID to set
export function setTerminologyId(id: string): void {
  if (terminologies.has(id)) {
    currentTerminologyId = id;
  }
}

// =============================================================================
// Helper for creating new terminologies (for use by plugins)
// =============================================================================

// Create a new terminology implementation.
// This helper is provided for plugins that want to register new terminologies.
// @param name Human-readable name for the terminology
// @param terms The term definitions for each GPU compute concept
// @param terms.gpu GPU device term
// @param terms.thread Thread-level execution term
// @param terms.warp SIMD execution group term
// @param terms.block Thread block term
// @param terms.grid Grid of thread blocks term
// @param terms.sm Streaming multiprocessor abbreviation term
// @param terms.streamingMultiprocessor Full streaming multiprocessor term
// @param terms.sharedMem Shared memory term
// @param terms.tensor Tensor core / unit term *
// @returns A Terminology implementation
export function createTerminology(
  name: string,
  terms: {
    gpu: Term;
    thread: Term;
    warp: Term;
    block: Term;
    grid: Term;
    sm: Term;
    streamingMultiprocessor: Term;
    sharedMem: Term;
    tensor: Term;
  },
): Terminology {
  return {name, ...terms};
}
