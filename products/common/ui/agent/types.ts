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

import type {Engine} from '../../../../ui/src/trace_processor/engine';

export type AgentMode = 'live' | 'snapshot' | 'trace' | 'compare' | 'none';

export interface JsonSchemaProperty {
  readonly type: 'string' | 'number' | 'integer' | 'boolean' | 'array' | 'object';
  readonly description?: string;
  readonly enum?: ReadonlyArray<string>;
  readonly items?: JsonSchemaProperty;
  readonly default?: unknown;
}

export interface JsonSchemaObject {
  readonly type: 'object';
  readonly properties?: Readonly<Record<string, JsonSchemaProperty>>;
  readonly required?: ReadonlyArray<string>;
}

export interface ProductToolSpec {
  readonly name: string;
  readonly kind: 'sql' | 'sql_multi' | 'sql_adhoc' | 'note' | 'device' | 'ui';
  readonly modes: ReadonlyArray<AgentMode>;
  readonly description: string;
  readonly parameters: JsonSchemaObject;
  readonly sql?: string;
  readonly queries?: Readonly<Record<string, string>>;
  readonly default_limit?: number;
}

export interface ProductToolManifest {
  readonly version: number;
  readonly product?: string;
  readonly tools: ReadonlyArray<ProductToolSpec>;
}

export interface EvidenceRecord {
  readonly id: string;
  readonly tool: string;
  readonly args: Record<string, unknown>;
  readonly scope: string;
  readonly sql?: string;
  readonly data: unknown;
  readonly atMs: number;
}

export class EvidenceStore {
  private readonly records: EvidenceRecord[] = [];

  record(
    tool: string,
    args: Record<string, unknown>,
    scope: string,
    data: unknown,
    sql?: string,
  ): EvidenceRecord {
    const id = `E${this.records.length + 1}`;
    const rec: EvidenceRecord = {
      id,
      tool,
      args,
      scope,
      sql,
      data,
      atMs: Date.now(),
    };
    this.records.push(rec);
    return rec;
  }

  get(id: string): EvidenceRecord | undefined {
    return this.records.find((r) => r.id === id);
  }

  all(): ReadonlyArray<EvidenceRecord> {
    return this.records;
  }

  clear(): void {
    this.records.length = 0;
  }
}

export interface InvestigationNote {
  readonly title: string;
  readonly summary: string;
  readonly evidenceIds: ReadonlyArray<string>;
  readonly missingEvidenceIds: ReadonlyArray<string>;
  readonly verified: boolean;
  readonly atMs: number;
}

export interface HandoffAction {
  readonly product: string;
  readonly label: string;
  readonly description: string;
  readonly url: string;
  readonly onOpen?: () => void;
}

export interface SuggestedPrompt {
  readonly label: string;
  readonly prompt: string;
  readonly icon?: string;
}

export interface TimeWindow {
  readonly startTs: bigint;
  readonly endTs: bigint;
  readonly label: string;
}

export interface AgentContextSnapshot {
  readonly mode: AgentMode;
  readonly label: string;
  readonly subtab?: string;
  readonly deviceName?: string;
  readonly focusProcess?: string;
  readonly engine?: Engine;
  readonly secondaryEngine?: Engine;
  readonly window?: TimeWindow;
  readonly extraContext?: string;
}

export interface DeviceActionAdapter {
  getStatus(): Promise<unknown>;
  takeSnapshot(label?: string): Promise<unknown>;
  runWorkload(opts: {
    readonly workload: string;
    readonly command?: string;
    readonly durationS?: number;
    readonly snapshotAfter?: boolean;
  }): Promise<unknown>;
}

export interface UiActionAdapter {
  readonly navTargets: ReadonlyArray<string>;
  navigate(target: string, subtab?: string): string;
  buildHandoff(req: {
    readonly product: string;
    readonly process?: string;
    readonly reason: string;
  }): HandoffAction | undefined;
}

export interface ProductAgentAdapter {
  readonly productId: string;
  readonly productName: string;
  readonly manifestKey: string; // e.g. 'sysmon/tools.json'
  getContext(): AgentContextSnapshot;
  getSystemPrompt(ctx: AgentContextSnapshot): string;
  getSuggestions(ctx: AgentContextSnapshot): ReadonlyArray<SuggestedPrompt>;
  readonly device?: DeviceActionAdapter;
  readonly ui?: UiActionAdapter;
}
