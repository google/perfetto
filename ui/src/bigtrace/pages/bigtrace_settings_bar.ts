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
import {Chip} from '../../widgets/chip';
import {Stack} from '../../widgets/stack';
import {showModal} from '../../widgets/modal';
import type {Filter} from '../../components/widgets/datagrid/model';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import type {Setting as BigTraceSetting} from '../settings/settings_types';
import type {SettingsBindings} from '../settings/tab_bound_setting';
import type {BigTraceEditorTab, QueryTabsState} from './query_tabs_state';
import {SettingsPage} from './settings_page';

export interface BigtraceSettingsBarAttrs {
  readonly tab: BigTraceEditorTab;
  readonly tabsState: QueryTabsState;
  readonly bindings: SettingsBindings;
}

// Chip strip atop each editor tab: one chip per per-tab override (settings,
// trace filters) plus an "+ Add" chip opening the Settings modal. Trace-metadata
// columns aren't shown here — they live only in the modal's Query Result Columns
// card.
export class BigtraceSettingsBar
  implements m.ClassComponent<BigtraceSettingsBarAttrs>
{
  view({attrs}: m.Vnode<BigtraceSettingsBarAttrs>): m.Children {
    const {tab, tabsState, bindings} = attrs;
    return m(
      '.pf-bt-settings-bar',
      m(
        Stack,
        {
          orientation: 'horizontal',
          wrap: true,
          spacing: 'small',
          className: 'pf-bt-settings-bar__chips',
        },
        m(Chip, {
          label: 'Add trace filter',
          icon: 'add',
          className: 'pf-bt-settings-bar__add',
          onclick: () => openAddSettingsModal(bindings),
        }),
        renderSettingChips(bindings),
        renderFilterChips(tab, tabsState, bindings),
      ),
    );
  }
}

// ---------------------------------------------------------------------------
// Chip rendering
// ---------------------------------------------------------------------------

// One read-only chip per effective setting (getEffectiveSettings applies
// per-tab overrides and drops disabled settings). Editing lives in the modal.
function renderSettingChips(bindings: SettingsBindings): m.Children {
  return bindings.getEffectiveSettings().map((entry) => {
    const setting = bigTraceSettingsStorage.get(entry.settingId) as
      | BigTraceSetting<unknown>
      | undefined;
    if (setting === undefined) return null;
    return renderSettingChip(setting, entry.values);
  });
}

function renderSettingChip(
  setting: BigTraceSetting<unknown>,
  values: ReadonlyArray<string>,
): m.Children {
  return m(Chip, {
    label: `${setting.name}: ${formatSettingValue(values)}`,
  });
}

function renderFilterChips(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
  bindings: SettingsBindings,
): m.Children {
  return tab.traceFilters.map((filter, idx) =>
    m(Chip, {
      label: formatFilterChipLabel(filter),
      removable: true,
      onRemove: () => {
        const next = tab.traceFilters.filter((_, i) => i !== idx);
        bindings.setTraceFilters(next);
        tabsState.markDirty();
        m.redraw();
      },
      // Display + remove only; add/refine filters via the modal's trace grid.
    }),
  );
}

// ---------------------------------------------------------------------------
// Label formatting
// ---------------------------------------------------------------------------

function formatSettingValue(values: ReadonlyArray<string>): string {
  if (values.length === 0) return '(empty)';
  if (values.length === 1) return values[0] === '' ? '(empty)' : values[0];
  if (values.length <= 3) return values.join(', ');
  return `${values.slice(0, 2).join(', ')}, +${values.length - 2} more`;
}

function formatFilterChipLabel(f: Filter): string {
  if (f.op === 'is null' || f.op === 'is not null') {
    return `${f.field} ${f.op}`;
  }
  if (f.op === 'in' || f.op === 'not in') {
    const vals = f.value.map(String);
    if (vals.length <= 3) return `${f.field} ${f.op} ${vals.join(', ')}`;
    return `${f.field} ${f.op} ${vals.slice(0, 2).join(', ')}, +${vals.length - 2} more`;
  }
  // Remaining ops are scalar comparisons/patterns. TS doesn't always narrow the
  // discriminant via the early returns, so guard defensively.
  if ('value' in f) return `${f.field} ${f.op} ${String(f.value)}`;
  return `${f.field} ${f.op}`;
}

// ---------------------------------------------------------------------------
// Modal opener
// ---------------------------------------------------------------------------

// Hosts the SettingsPage in embedded (per-tab) mode — the one place editing
// (settings, trace-grid selection, metadata columns) happens.
function openAddSettingsModal(bindings: SettingsBindings): void {
  void showModal({
    title: 'Bigtrace settings',
    className: 'pf-bt-settings-modal',
    vAlign: 'TOP',
    content: () => m(SettingsPage, {bindings}),
    buttons: [{text: 'Done', primary: true}],
  });
}
