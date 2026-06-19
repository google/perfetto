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

import m from 'mithril';
import type {Filter} from '../../components/widgets/datagrid/model';
import type {
  EnumOption,
  Setting as BigTraceSetting,
  SettingFilter,
} from './settings_types';

// Routes SettingsPage reads/writes to the per-query snapshot instead of the
// global state modules. Used by the Query page's chip strip / +Add modal.
export interface SettingsBindings {
  // Effective SettingFilter[] for data source + schema requests.
  readonly getEffectiveSettings: () => ReadonlyArray<SettingFilter>;
  // One setting's `values` by id. undefined = no snapshot entry; falls back to
  // defaultValue.
  readonly getSettingValue: (id: string) => readonly string[] | undefined;
  readonly setSettingValue: (
    id: string,
    values: readonly string[],
    category: string,
  ) => void;
  readonly getTraceFilters: () => readonly Filter[];
  readonly setTraceFilters: (filters: readonly Filter[]) => void;
  // null = unchosen (defaultVisible); set accepts a concrete list or null.
  readonly getTraceMetadataColumns: () => readonly string[] | null;
  readonly setTraceMetadataColumns: (cols: readonly string[] | null) => void;
  // Per-tab AIP-132 ordering (e.g. "size_bytes desc"), shipped as
  // `trace_order_by` on the next Run. Empty defers to the default.
  readonly getTraceOrderBy: () => string;
  readonly setTraceOrderBy: (orderBy: string) => void;
  // Per-tab enable/disable of one setting, independent of global state — a tab
  // can toggle a filter without affecting /settings or other tabs. Effective
  // settings drop per-tab-disabled entries.
  readonly isSettingDisabled: (id: string) => boolean;
  readonly setSettingDisabled: (id: string, disabled: boolean) => void;
  // Called when the trace-list data source reports a fresh filteredTotalRows
  // (traces the current filter selects). undefined = count not yet known.
  readonly onTraceMatchCount?: (count: number | undefined) => void;
  // The tab's current SQL, used only to detect whether the tab matches a
  // preset 1:1 (so the matching preset chip can highlight).
  readonly getSql?: () => string;
  // Load a preset's query text + title into the tab. Present only on the
  // per-tab modal (which has an editor); absent on standalone /settings.
  readonly setQueryAndTitle?: (perfettoSql: string, title: string) => void;
}

// Wraps a globally-registered Setting<T> so reads/writes route through per-tab
// bindings, inheriting the descriptor (type, schema, placeholder, options) so
// renderSetting() picks the right widget.
export class TabBoundSetting<T> implements BigTraceSetting<T> {
  readonly id: string;
  readonly name: string;
  readonly description: string;
  readonly type: BigTraceSetting<T>['type'];
  readonly schema: BigTraceSetting<T>['schema'];
  readonly defaultValue: T;
  readonly category?: string;
  readonly requiresReload?: boolean;
  readonly options?: readonly (string | EnumOption)[];
  readonly placeholder?: string;
  readonly format?: 'sql';
  readonly disabled: boolean;

  constructor(
    private readonly base: BigTraceSetting<T>,
    private readonly bindings: SettingsBindings,
  ) {
    this.id = base.id;
    this.name = base.name;
    this.description = base.description;
    this.type = base.type;
    this.schema = base.schema;
    this.defaultValue = base.defaultValue;
    this.category = base.category;
    this.requiresReload = base.requiresReload;
    this.options = base.options;
    this.placeholder = base.placeholder;
    this.format = base.format;
    this.disabled = base.disabled ?? false;
  }

  get isDefault(): boolean {
    return JSON.stringify(this.get()) === JSON.stringify(this.defaultValue);
  }

  get(): T {
    const raw = this.bindings.getSettingValue(this.id);
    if (raw === undefined) return this.base.get();
    return convertFromWireValues<T>(raw, this.base) ?? this.defaultValue;
  }

  set(value: T): void {
    const wire = Array.isArray(value) ? value.map(String) : [String(value)];
    this.bindings.setSettingValue(this.id, wire, this.category ?? '');
    m.redraw();
  }

  reset(): void {
    this.set(this.defaultValue);
  }

  isDisabled(): boolean {
    // Booleans have no enable/disable concept (the value is the on/off), so
    // they're never disabled per-tab.
    if (this.type === 'boolean') return false;
    return this.bindings.isSettingDisabled(this.id);
  }

  setDisabled(disabled: boolean): void {
    this.bindings.setSettingDisabled(this.id, disabled);
    m.redraw();
  }

  [Symbol.dispose](): void {}
}

// Reads the wire-side `values` string array back into the setting's declared
// type. undefined when the entry doesn't match the type (e.g. "abc" for a
// number); callers fall back to defaultValue.
export function convertFromWireValues<T>(
  raw: readonly string[],
  setting: BigTraceSetting<T>,
): T | undefined {
  switch (setting.type) {
    case 'number': {
      if (raw.length === 0) return undefined;
      const n = parseFloat(raw[0]);
      return Number.isFinite(n) ? (n as unknown as T) : undefined;
    }
    case 'boolean':
      return (raw[0] === 'true') as unknown as T;
    case 'string':
    case 'enum':
      return (raw[0] ?? '') as unknown as T;
    case 'multi-select':
    case 'string-array':
      return [...raw] as unknown as T;
  }
  return undefined;
}
