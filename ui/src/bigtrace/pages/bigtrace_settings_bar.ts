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
import {Button, ButtonVariant} from '../../widgets/button';
import {Chip} from '../../widgets/chip';
import {Stack} from '../../widgets/stack';
import type {Filter} from '../../components/widgets/datagrid/model';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import type {Setting as BigTraceSetting} from '../settings/settings_types';
import type {SettingsBindings} from '../settings/tab_bound_setting';
import {
  isTraceSelectionSetting,
  openSettings,
  type BigTraceEditorTab,
  type QueryTabsState,
} from './query_tabs_state';

export interface BigtraceSettingsBarAttrs {
  readonly tab: BigTraceEditorTab;
  readonly tabsState: QueryTabsState;
  readonly bindings: SettingsBindings;
}

// Chip strip atop each editor tab: a Trace Selection button that opens the
// tab's trace-selection page in place, one read-only chip per active
// trace-selection setting, one removable chip per trace filter, and Clone.
// The strip summarises which traces the query runs over — query options live
// behind the toolbar's gear and are not chipped. The two actions are buttons,
// not chips, so they look pressable and answer to hover.
export class BigtraceSettingsBar implements m.ClassComponent<BigtraceSettingsBarAttrs> {
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
        m(Button, {
          label: 'Trace Selection',
          icon: 'tune',
          compact: true,
          variant: ButtonVariant.Filled,
          title:
            'Choose which traces this query runs over. Apply keeps the ' +
            'changes; Cancel drops them.',
          onclick: () => openSettings(tab),
        }),
        renderSettingChips(bindings),
        renderFilterChips(tab, tabsState, bindings),
      ),
      // Pinned to the right edge: this one acts on the tab as a whole, not on
      // what it runs with, so it sits apart from the configuration chips.
      m(Button, {
        label: 'Clone',
        icon: 'content_copy',
        compact: true,
        variant: ButtonVariant.Filled,
        className: 'pf-bt-settings-bar__end',
        title:
          'Open a clone of this query in a new tab — same SQL and settings, ' +
          'its own run.',
        onclick: () => {
          tabsState.cloneTab(tab.id);
          m.redraw();
        },
      }),
    );
  }
}

// ---------------------------------------------------------------------------
// Chip rendering
// ---------------------------------------------------------------------------

// One read-only chip per effective trace-selection setting
// (getEffectiveSettings applies per-tab overrides and drops disabled
// settings). Editing lives on the Trace Selection page; everything else — the
// cap, the options — belongs to the toolbar's settings modal and isn't
// summarised here.
function renderSettingChips(bindings: SettingsBindings): m.Children {
  return bindings.getEffectiveSettings().map((entry) => {
    if (
      !isTraceSelectionSetting({id: entry.settingId, category: entry.category})
    ) {
      return null;
    }
    const setting = bigTraceSettingsStorage.get(entry.settingId) as
      BigTraceSetting<unknown> | undefined;
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
      // Display + remove only; add/refine filters via the trace grid.
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
