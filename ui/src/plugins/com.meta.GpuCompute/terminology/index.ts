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
// Terminology Registry
// =============================================================================

export class TerminologyRegistry {
  private readonly terminologies = new Map<string, Terminology>();

  register(id: string, terminology: Terminology): void {
    this.terminologies.set(id, terminology);
  }

  getIds(): string[] {
    return Array.from(this.terminologies.keys());
  }

  getAll(): ReadonlyMap<string, Terminology> {
    return this.terminologies;
  }

  getOptions(): Array<{id: string; name: string}> {
    return Array.from(this.terminologies.entries()).map(([id, t]) => ({
      id,
      name: t.name,
    }));
  }

  get(id: string): Terminology {
    const t = this.terminologies.get(id);
    if (t) return t;
    const first = this.terminologies.values().next();
    if (!first.done) return first.value;
    throw new Error('No terminologies registered');
  }
}

// =============================================================================
// Helper for creating new terminologies
// =============================================================================

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
