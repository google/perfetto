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

import type {Filter} from '../../components/widgets/datagrid/model';
import type {SettingFilter} from './settings_types';
import {SingleFieldStorage} from './single_field_storage';

// The trace selection + options a query last ran with. Not a global setting:
// nothing reads it at run time. It exists so the next query doesn't start from
// a blank trace source — a preset from a shared catalog can't know where this
// user's traces live.
export interface QuerySetup {
  readonly settings: ReadonlyArray<SettingFilter>;
  readonly traceFilters: ReadonlyArray<Filter>;
  readonly traceMetadataColumns: ReadonlyArray<string> | null;
  readonly traceOrderBy: string;
}

function parseSetup(raw: unknown): QuerySetup | null {
  if (typeof raw !== 'object' || raw === null) return null;
  const s = raw as Partial<QuerySetup>;
  if (!Array.isArray(s.settings)) return null;
  return {
    settings: s.settings,
    traceFilters: Array.isArray(s.traceFilters) ? s.traceFilters : [],
    traceMetadataColumns: Array.isArray(s.traceMetadataColumns)
      ? s.traceMetadataColumns.filter((c): c is string => typeof c === 'string')
      : null,
    traceOrderBy: typeof s.traceOrderBy === 'string' ? s.traceOrderBy : '',
  };
}

export const lastSetupState = new SingleFieldStorage<QuerySetup | null>(
  'bigtraceLastSetup',
  'setup',
  parseSetup,
  null,
);

// Id of the preset the last new query started from, so the launcher can
// preselect it. Empty when the last query was configured by hand.
export const lastPresetIdState = new SingleFieldStorage<string>(
  'bigtraceLastPreset',
  'id',
  (raw) => (typeof raw === 'string' ? raw : ''),
  '',
);

// Settings describing WHERE the traces come from, as opposed to what to run
// over them. These stick across queries (and survive applying a preset); a
// preset only overrides them when it names them explicitly.
export const TRACE_SOURCE_CATEGORY = 'TRACE_ADDRESS';

export function traceSourceSettings(
  setup: QuerySetup | null,
  exclude: ReadonlyArray<string> = [],
): ReadonlyArray<SettingFilter> {
  if (setup === null) return [];
  return setup.settings.filter(
    (s) =>
      s.category === TRACE_SOURCE_CATEGORY && !exclude.includes(s.settingId),
  );
}
