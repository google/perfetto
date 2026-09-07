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
import {showModal} from '../../widgets/modal';
import type {SettingsBindings} from '../settings/tab_bound_setting';
import {
  restoreTabConfig,
  snapshotTabConfig,
  type BigTraceEditorTab,
  type QueryTabsState,
} from './query_tabs_state';
import {QuerySettingsForm} from './query_settings_form';

// Query settings — the trace cap and every setting that isn't trace
// selection — in a modal off the run toolbar. Edits apply to the tab as
// they're made, like the Trace Selection page; leaving decides: Apply keeps
// them, while Cancel — or closing the dialog any other way — puts the
// configuration back to what it was when the modal opened.
export async function openQuerySettingsModal(
  tab: BigTraceEditorTab,
  tabsState: QueryTabsState,
  bindings: SettingsBindings,
): Promise<void> {
  const before = snapshotTabConfig(tab);
  let applied = false;
  await showModal({
    title: 'Advanced Query Settings',
    className: 'pf-bt-query-settings-modal',
    content: () => m(QuerySettingsForm, {bindings, scope: 'query-options'}),
    buttons: [
      {text: 'Cancel'},
      {
        text: 'Apply',
        primary: true,
        action: () => {
          applied = true;
        },
      },
    ],
  });
  if (!applied) {
    restoreTabConfig(tab, before);
  }
  tabsState.markDirty();
  m.redraw();
}
