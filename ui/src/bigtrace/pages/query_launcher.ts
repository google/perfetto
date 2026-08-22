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
import {AccordionSection} from '../../widgets/accordion';
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
  TRACE_LIMIT_SETTING_ID,
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

// Whether this tab has something to return to — a query, a run, or results.
// A brand-new tab doesn't, so it gets no way "back" to an empty editor.
export function canReturnToQuery(tab: BigTraceEditorTab): boolean {
  return (
    tab.editorText.trim() !== '' ||
    tab.queryUuid !== undefined ||
    tab.queryResult !== undefined
  );
}

// Shown in a tab until it has a configuration: pick a preset, or configure the
// trace selection and options by hand. Also hosts a configured tab's Settings.
export class QueryLauncher implements m.ClassComponent<QueryLauncherAttrs> {
  oninit() {
    void presetStore.load();
  }

  view({attrs}: m.Vnode<QueryLauncherAttrs>): m.Children {
    const {tab, tabsState, bindings} = attrs;
    if (tab.setupMode === 'custom') {
      return m(
        '.pf-bt-launcher.pf-bt-launcher--custom',
        m(
          '.pf-bt-launcher__custom-body',
          m(QuerySettingsForm, {
            bindings,
            header: this.renderSetupPresets(tab, tabsState),
          }),
        ),
        this.renderCustomFooter(tab, tabsState),
      );
    }
    return m(
      '.pf-bt-launcher',
      m('.pf-bt-launcher__inner', [
        // On a tab that already has a query: the way out that changes nothing.
        canReturnToQuery(tab) &&
          m(
            '.pf-bt-launcher__back',
            m(Button, {
              label: 'Back to query',
              icon: 'arrow_back',
              onclick: () => {
                closeSettings(tab, {keep: false});
                tabsState.markDirty();
              },
            }),
          ),
        m('.pf-bt-launcher__title', 'Start a query'),
        m(
          '.pf-bt-launcher__subtitle',
          'Pick a preset, or configure the traces and options yourself.',
        ),
        this.renderPresets(tab, tabsState),
        m(
          '.pf-bt-launcher__actions',
          m(Button, {
            label: 'Configure custom settings',
            icon: 'tune',
            onclick: () => {
              tab.setupMode = 'custom';
              tabsState.markDirty();
            },
          }),
        ),
      ]),
    );
  }

  // The gallery a query starts from: one click applies the whole preset.
  private renderPresets(
    tab: BigTraceEditorTab,
    tabsState: QueryTabsState,
  ): m.Children {
    const presets = launcherPresets(
      presetStore.presets,
      localPresetStore.list(),
    );
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
    return m(PresetGallery, {
      presets,
      selectedId: preselectedPresetId(presets, lastPresetIdState.get()),
      selectedBadge: 'Last used',
      onPick: (preset) => {
        applyPresetToTab(tab, preset);
        lastPresetIdState.set(preset.id);
        tabsState.markDirty();
        m.redraw();
      },
    });
  }

  // The same gallery atop the settings form, folded away until wanted. From
  // here a preset lends its setup only — settings, trace selection, result
  // columns, order, caps and mode — and the query stays the user's: Settings
  // is where you decide how a query runs, not what it runs. Provisional like
  // any other edit in the form: Cancel undoes it. The card marked "Current" is
  // the preset whose setup the tab runs with right now, and the folded header
  // says the same, so the answer is there without opening it.
  private renderSetupPresets(
    tab: BigTraceEditorTab,
    tabsState: QueryTabsState,
  ): m.Children {
    const presets = launcherPresets(
      presetStore.presets,
      localPresetStore.list(),
    );
    if (presets.length === 0) return null;
    const currentId = matchingSetupPresetId(tab, presets, tab.lastPresetId);
    const current = presets.find((p) => p.id === currentId);
    return m(
      AccordionSection,
      {
        className: 'pf-bt-settings-presets',
        summary: m('.pf-bt-settings-presets__summary', [
          m('span.pf-bt-settings-presets__title', 'Presets'),
          m(
            'span.pf-bt-settings-presets__current',
            current === undefined
              ? 'Custom setup'
              : `Setup from “${current.name}”`,
          ),
        ]),
      },
      m(
        '.pf-bt-settings-presets__hint',
        'Pick a preset to use its settings here. The query is not changed.',
      ),
      m(PresetGallery, {
        presets,
        selectedId: currentId,
        selectedBadge: 'Current',
        onPick: (preset) => {
          applyPresetSetup(tab, preset);
          tabsState.markDirty();
        },
      }),
    );
  }

  private renderCustomFooter(
    tab: BigTraceEditorTab,
    tabsState: QueryTabsState,
  ): m.Children {
    // Opened from Settings on a configured tab (a session is open), or reached
    // from the gallery on a new tab. Presets belong to starting a query, not
    // to configuring one that exists — even an empty one: applying a preset
    // there would replace the tab wholesale. So only the new-tab form offers
    // the way back to the gallery.
    const editing = tab.settingsSession !== undefined;
    return m('.pf-bt-launcher__footer', [
      !editing &&
        m(Button, {
          label: 'Back to presets',
          icon: 'arrow_back',
          onclick: () => {
            tab.setupMode = 'presets';
            tabsState.markDirty();
          },
        }),
      // Edits take effect as they're made, so leaving is where the choice is:
      // Cancel puts the configuration back to what it was, Apply keeps it.
      editing &&
        m(Button, {
          label: 'Cancel',
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
          : undefined,
        onclick: () => {
          // Starting a query by hand: if this setup happens to be one of the
          // presets on offer (e.g. it was just saved as one), the next tab
          // preselects it; otherwise there's no preset to remember. Editing
          // an existing query says nothing about what the next one starts
          // from, so Apply leaves the last-used preset alone.
          if (!editing) {
            lastPresetIdState.set(
              matchingPresetId(
                tab,
                launcherPresets(presetStore.presets, localPresetStore.list()),
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

function comparable(tab: BigTraceEditorTab) {
  return {
    sql: tab.editorText,
    traceFilters: tab.traceFilters,
    traceMetadataColumns: tab.traceMetadataColumns,
    traceOrderBy: tab.traceOrderBy,
    settings: effectiveTabSettings(tab),
  };
}

// Whether the tab, as configured, is exactly one of the presets on offer.
export function matchingPresetId(
  tab: BigTraceEditorTab,
  presets: ReadonlyArray<TracePreset>,
): string | undefined {
  const current = comparable(tab);
  return presets.find((p) => presetMatches(p, current))?.id;
}

// Which registered settings are booleans, and which are run controls a setup
// comparison ignores (the trace cap is defaulted per mode, not configured).
function settingKinds(): SettingKinds {
  return {
    booleanIds: new Set(
      bigTraceSettingsStorage
        .getAllSettings()
        .filter((s) => s.type === 'boolean')
        .map((s) => s.id),
    ),
    ignoredIds: new Set([TRACE_LIMIT_SETTING_ID]),
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
  const preferred = presets.find((p) => p.id === preferId);
  if (preferred !== undefined && setupEquals(preferred, current, kinds)) {
    return preferred.id;
  }
  return presets.find((p) => setupEquals(p, current, kinds))?.id;
}
