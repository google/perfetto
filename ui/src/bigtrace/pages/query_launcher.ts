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
import {EmptyState} from '../../widgets/empty_state';
import {Spinner} from '../../widgets/spinner';
import {Intent} from '../../widgets/common';
import type {TracePreset} from '../query/bigtrace_query_client';
import {presetStore} from '../query/preset_store';
import {
  DEFAULT_LOCAL_CATEGORY,
  localPresetStore,
  type LocalPreset,
} from '../query/local_preset_store';
import {lastPresetIdState} from '../settings/last_preset_state';
import {
  presetMatches,
  setupEquals,
  type SettingKinds,
} from '../query/preset_match';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import {getBigtraceEndpoint} from '../settings/endpoint_storage';
import type {SettingsBindings} from '../settings/tab_bound_setting';
import {
  applyPresetSetup,
  applyPresetToTab,
  closeSettings,
  effectiveTabSettings,
  isTraceSelectionSetting,
  type BigTraceEditorTab,
  type QueryTabsState,
} from './query_tabs_state';
import {PresetGallery} from './preset_gallery';
import {promptForPreset} from './preset_dialogs';
import {QuerySettingsForm} from './query_settings_form';

export interface QueryLauncherAttrs {
  readonly tab: BigTraceEditorTab;
  readonly tabsState: QueryTabsState;
  readonly bindings: SettingsBindings;
}

// Backend catalog first, then the user's own — so a saved preset never hides
// behind a long catalog, and the grouping still reads by CUJ.
export function launcherPresets(
  backend: ReadonlyArray<TracePreset>,
  local: ReadonlyArray<LocalPreset>,
): TracePreset[] {
  return [...backend, ...local];
}

// The preset a new query should start on: the last one used, when it still
// exists. Falls back to nothing preselected.
export function preselectedPresetId(
  presets: ReadonlyArray<TracePreset>,
  lastId: string,
): string | undefined {
  if (lastId === '') return undefined;
  return presets.some((p) => p.id === lastId) ? lastId : undefined;
}

// One page for a tab that has no query yet and for the Trace Selection of one
// that does: a Presets section, then the trace-selection form — nothing
// folded. Cards fill the page, buttons commit. On a new tab a card fills in
// the whole preset, query included, and Start query opens the editor; on an
// existing query a card lends its setup only, and Apply keeps it. Query
// options live elsewhere (the gear in the run toolbar).
export class QueryLauncher implements m.ClassComponent<QueryLauncherAttrs> {
  oninit() {
    void presetStore.load();
  }

  view({attrs}: m.Vnode<QueryLauncherAttrs>): m.Children {
    const {tab, tabsState, bindings} = attrs;
    const editing = tab.settingsSession !== undefined;
    return m('.pf-bt-launcher', [
      m(
        '.pf-bt-launcher__body',
        m(QuerySettingsForm, {
          bindings,
          scope: 'trace-selection',
          header: this.renderPresetsSection(tab, tabsState, editing),
        }),
      ),
      this.renderFooter(tab, tabsState, editing),
    ]);
  }

  // The Presets section, headed like the form's own sections. What a card
  // does depends on the page: filling a new tab in wholesale, or lending an
  // existing query its setup.
  private renderPresetsSection(
    tab: BigTraceEditorTab,
    tabsState: QueryTabsState,
    editing: boolean,
  ): m.Children {
    const presets = launcherPresets(
      presetStore.presets,
      localPresetStore.list(),
    );
    return [
      !editing &&
        m('.pf-bt-launcher__head', [
          m('.pf-bt-launcher__title', 'Start a query'),
          m(
            '.pf-bt-launcher__subtitle',
            'Pick a preset, or configure the traces and options yourself.',
          ),
        ]),
      // With no presets to offer, an existing query's page goes straight to
      // the trace selection; only a new tab explains where presets come from.
      (presets.length > 0 || !editing) &&
        m('.pf-bt-settings-page__plugin-section', [
          m('h2.pf-bt-settings-page__plugin-title', 'Presets'),
          editing &&
            m(
              '.pf-bt-presets-hint',
              'Pick a preset to use its settings here. The query is not ' +
                'changed.',
            ),
          editing
            ? this.renderSetupPresets(tab, tabsState, presets)
            : this.renderStartPresets(tab, tabsState, presets),
        ]),
    ];
  }

  private renderStartPresets(
    tab: BigTraceEditorTab,
    tabsState: QueryTabsState,
    presets: ReadonlyArray<TracePreset>,
  ): m.Children {
    if (presets.length === 0) {
      // Don't call it empty while the catalog is still in flight.
      if (presetStore.isLoading) {
        return m(
          EmptyState,
          {title: 'Loading presets…', icon: 'hourglass_empty'},
          m(Spinner),
        );
      }
      // No catalog reached us: say so, and point at the control that fixes it.
      const unreachable =
        getBigtraceEndpoint() === '' ||
        bigTraceSettingsStorage.execConfigLoadError !== undefined;
      return m(
        EmptyState,
        {
          icon: unreachable ? 'cloud_off' : 'bookmark_border',
          title: unreachable
            ? 'Connect a backend to load presets'
            : 'No presets yet',
        },
        m(
          '.pf-bt-launcher__empty-detail',
          unreachable
            ? 'BigTrace runs your queries against a backend that holds the ' +
                'traces. Set the endpoint from the connection button, top right.'
            : 'Configure the traces and options below, then save that setup ' +
                'as a preset to reuse it.',
        ),
      );
    }
    // The page reads back the preset it's filled from; until a card is
    // picked, the last-used one is suggested.
    const selectedId = selectedPresetId(tab, presets, tab.lastPresetId);
    const suggestedId = preselectedPresetId(presets, lastPresetIdState.get());
    return m(PresetGallery, {
      presets,
      selectedId: selectedId ?? suggestedId,
      selectedBadge: selectedId !== undefined ? 'Selected' : 'Last used',
      onPick: (preset) => {
        applyPresetToTab(tab, preset);
        tabsState.markDirty();
      },
    });
  }

  // On an existing query a preset lends its setup only — settings, trace
  // selection, result columns, order, caps and mode — and the query stays the
  // user's: this page is about how a query runs, not what it runs.
  // Provisional like any other edit here: Cancel undoes it. The card marked
  // "Current" is the preset whose setup the tab runs with right now.
  private renderSetupPresets(
    tab: BigTraceEditorTab,
    tabsState: QueryTabsState,
    presets: ReadonlyArray<TracePreset>,
  ): m.Children {
    return m(PresetGallery, {
      presets,
      selectedId: matchingSetupPresetId(tab, presets, tab.lastPresetId),
      selectedBadge: 'Current',
      onPick: (preset) => {
        applyPresetSetup(tab, preset);
        tabsState.markDirty();
      },
    });
  }

  private renderFooter(
    tab: BigTraceEditorTab,
    tabsState: QueryTabsState,
    editing: boolean,
  ): m.Children {
    return m('.pf-bt-launcher__footer', [
      // Edits take effect as they're made, so leaving is where the choice is:
      // Cancel puts the configuration back to what it was, Apply keeps it.
      // Filled like a dialog's Cancel, so the way out is as visible as Apply.
      editing &&
        m(Button, {
          label: 'Cancel',
          variant: ButtonVariant.Filled,
          title: 'Drop the changes made here and return to the query.',
          onclick: () => {
            closeSettings(tab, {keep: false});
            tabsState.markDirty();
          },
        }),
      m('.pf-bt-launcher__footer-spacer'),
      m(Button, {
        label: 'Save as preset…',
        icon: 'bookmark_add',
        onclick: () => void this.saveAsPreset(tab),
      }),
      m(Button, {
        label: editing ? 'Apply' : 'Start query',
        icon: editing ? 'check' : 'arrow_forward',
        intent: Intent.Primary,
        variant: ButtonVariant.Filled,
        title: editing
          ? 'Use these settings for this query from its next run.'
          : 'Start with the settings below — the defaults, unless you ' +
            'changed them.',
        onclick: () => {
          // Starting a query: the preset the page was filled from (or that
          // the setup happens to equal, e.g. one just saved) is what the next
          // tab suggests; otherwise there's no preset to remember. Editing an
          // existing query says nothing about what the next one starts from,
          // so Apply leaves the last-used preset alone.
          if (!editing) {
            lastPresetIdState.set(
              selectedPresetId(
                tab,
                launcherPresets(presetStore.presets, localPresetStore.list()),
                tab.lastPresetId,
              ) ?? '',
            );
          }
          closeSettings(tab, {keep: true});
          tabsState.markDirty();
        },
      }),
    ]);
  }

  private async saveAsPreset(tab: BigTraceEditorTab): Promise<void> {
    const saved = await promptForPreset(tab, {
      name: tab.title,
      category: DEFAULT_LOCAL_CATEGORY,
      description: '',
    });
    if (saved === undefined) return;
    lastPresetIdState.set(saved.id);
    m.redraw();
  }
}

// A preset carries trace selection and SQL, so both sides of a match are
// narrowed to that: the tab's effective trace-selection settings, and — for a
// stale catalog entry that still names an option or a cap — the preset's
// trace-selection ones.
function comparable(tab: BigTraceEditorTab) {
  return {
    sql: tab.editorText,
    traceFilters: tab.traceFilters,
    traceMetadataColumns: tab.traceMetadataColumns,
    traceOrderBy: tab.traceOrderBy,
    settings: effectiveTabSettings(tab).filter((s) =>
      isTraceSelectionSetting({id: s.settingId, category: s.category}),
    ),
  };
}

function traceScopePreset(p: TracePreset): TracePreset {
  return {
    ...p,
    settings: (p.settings ?? []).filter((s) =>
      isTraceSelectionSetting({id: s.settingId, category: s.category}),
    ),
  };
}

// The preset the tab is filled from — query and setup exactly, the setup
// judged as strictly as matchingSetupPresetId does — for the launcher to read
// back and to remember as last used; undefined when the tab is nobody's.
// `preferId`, the one last applied, wins among presets that fit alike.
export function selectedPresetId(
  tab: BigTraceEditorTab,
  presets: ReadonlyArray<TracePreset>,
  preferId?: string,
): string | undefined {
  const current = comparable(tab);
  const kinds = settingKinds();
  const fits = (p: TracePreset) => {
    const scoped = traceScopePreset(p);
    return (
      presetMatches(scoped, current) && setupEquals(scoped, current, kinds)
    );
  };
  const preferred = presets.find((p) => p.id === preferId);
  if (preferred !== undefined && fits(preferred)) return preferred.id;
  return presets.find(fits)?.id;
}

// Which trace-selection settings are booleans (off = "false"). Both sides of
// a comparison are already narrowed to trace selection, so nothing needs
// ignoring beyond that.
function settingKinds(): SettingKinds {
  return {
    booleanIds: new Set(
      bigTraceSettingsStorage
        .getAllSettings()
        .filter((s) => isTraceSelectionSetting(s) && s.type === 'boolean')
        .map((s) => s.id),
    ),
    ignoredIds: new Set<string>(),
  };
}

// The preset whose setup — everything but the query — the tab runs with right
// now, for the settings form to read back; undefined reads as "Custom".
// Several presets can share one setup (query-only catalog entries), so
// `preferId`, the one last applied, wins while it still fits.
export function matchingSetupPresetId(
  tab: BigTraceEditorTab,
  presets: ReadonlyArray<TracePreset>,
  preferId?: string,
): string | undefined {
  const current = comparable(tab);
  const kinds = settingKinds();
  const fits = (p: TracePreset) =>
    setupEquals(traceScopePreset(p), current, kinds);
  const preferred = presets.find((p) => p.id === preferId);
  if (preferred !== undefined && fits(preferred)) return preferred.id;
  return presets.find(fits)?.id;
}
