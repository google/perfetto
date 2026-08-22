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
import {Select} from '../../widgets/select';
import {Spinner} from '../../widgets/spinner';
import {Intent} from '../../widgets/common';
import type {TracePreset} from '../query/bigtrace_query_client';
import {presetStore} from '../query/preset_store';
import {
  DEFAULT_LOCAL_CATEGORY,
  isLocalPreset,
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
import {groupPresetsByCuj, renderCujSelector} from './preset_groups';
import {
  deleteLocalPreset,
  editLocalPreset,
  promptForPreset,
} from './preset_dialogs';
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
// that does: a Presets section, then the trace-selection form. Presets are
// picked from a category selector plus a dropdown of that category's presets.
// Picking fills the page — the whole preset (query included) on a new tab,
// the trace selection only on an existing query — and the footer commits.
// Browsing never applies anything: switching category just changes what the
// dropdown offers, and the dropdown's value is always the preset the page is
// actually filled from ("—" when it isn't from the visible category, or from
// any). Query options live elsewhere (the gear in the run toolbar).
export class QueryLauncher implements m.ClassComponent<QueryLauncherAttrs> {
  // Category being browsed; defaults to the applied (else last-used)
  // preset's, so the picker opens where the relevant entry lives.
  private activeCuj?: string;
  // The preset last applied per category while this page is up, so clicking
  // back to a category restores the one that was chosen there.
  private readonly lastByCategory = new Map<string, string>();

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

  // The Presets section, headed like the form's own sections. What picking
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
      // With no presets to offer, an existing query's page goes straight to
      // the trace selection; only a new tab explains where presets come from.
      (presets.length > 0 || !editing) &&
        m('.pf-bt-settings-page__plugin-section', [
          m('h2.pf-bt-settings-page__plugin-title', 'Presets'),
          presets.length === 0
            ? this.renderNoPresets()
            : this.renderPresetPicker(tab, tabsState, presets, editing),
        ]),
    ];
  }

  // Only reachable on a new tab (an existing query's section is skipped when
  // there is nothing to offer).
  private renderNoPresets(): m.Children {
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

  // Category selector + a dropdown of that category's presets. Clicking a
  // category applies one of its presets — the one last applied there while
  // this page is up, else this tab's last preset when it belongs, else the
  // first — and the dropdown refines the choice within it, so the picker
  // always names what the page is filled from once it has been touched.
  // Before that, the value is "—" and the last-used preset is marked inside
  // the list as a suggestion. Descriptions ride as tooltips — on each option
  // while choosing, on the control once something is chosen.
  private renderPresetPicker(
    tab: BigTraceEditorTab,
    tabsState: QueryTabsState,
    presets: ReadonlyArray<TracePreset>,
    editing: boolean,
  ): m.Children {
    const appliedId = editing
      ? matchingSetupPresetId(tab, presets, tab.lastPresetId)
      : selectedPresetId(tab, presets, tab.lastPresetId);
    const applied = presets.find((p) => p.id === appliedId);
    const suggestedId =
      appliedId === undefined
        ? preselectedPresetId(presets, lastPresetIdState.get())
        : undefined;
    const anchor =
      applied ?? presets.find((p) => p.id === suggestedId) ?? undefined;

    const {groups, byCuj} = groupPresetsByCuj(presets);
    // Same bucketing as groupPresetsByCuj, so an uncategorised preset's
    // category resolves to its ("Other") group.
    const anchorCuj =
      anchor === undefined ? undefined : anchor.category || 'Other';
    const active =
      this.activeCuj !== undefined && byCuj.has(this.activeCuj)
        ? this.activeCuj
        : anchorCuj !== undefined && byCuj.has(anchorCuj)
          ? anchorCuj
          : groups[0][0];
    const options = byCuj.get(active) ?? [];
    const appliedInActive = options.some((p) => p.id === appliedId);
    const shown = appliedInActive ? applied : undefined;

    const pick = (preset: TracePreset) => {
      if (editing) {
        applyPresetSetup(tab, preset);
      } else {
        applyPresetToTab(tab, preset);
      }
      this.lastByCategory.set(preset.category || 'Other', preset.id);
      tabsState.markDirty();
    };

    return m('.pf-bt-preset-picker', [
      renderCujSelector(
        groups.map(([cuj]) => cuj),
        active,
        (cuj) => {
          this.activeCuj = cuj;
          const candidates = byCuj.get(cuj) ?? [];
          const remembered =
            candidates.find((p) => p.id === this.lastByCategory.get(cuj)) ??
            candidates.find((p) => p.id === tab.lastPresetId) ??
            candidates[0];
          if (remembered !== undefined) pick(remembered);
        },
      ),
      m(
        Select,
        {
          className: 'pf-bt-preset-picker__select',
          value: appliedInActive ? appliedId : '',
          title: shown?.description || undefined,
          onchange: (e: Event) => {
            const id = (e.target as HTMLSelectElement).value;
            const preset = options.find((p) => p.id === id);
            if (preset === undefined) return;
            pick(preset);
          },
        },
        [
          m('option', {value: '', disabled: true}, '— choose a preset —'),
          options.map((p) =>
            m(
              'option',
              {value: p.id, title: p.description || undefined},
              presetOptionLabel(p, suggestedId),
            ),
          ),
        ],
      ),
      // Manage the applied preset when it's the user's own.
      shown !== undefined &&
        isLocalPreset(shown) && [
          m(Button, {
            icon: 'edit',
            title: 'Edit this preset: name, group, description',
            onclick: () => void editLocalPreset(shown as LocalPreset),
          }),
          m(Button, {
            icon: 'delete',
            intent: Intent.Danger,
            title: 'Delete this preset',
            onclick: () => void deleteLocalPreset(shown as LocalPreset),
          }),
        ],
    ]);
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

// Option text for the dropdown: the name, with what a card used to say in
// its tags — setup-only, and the last-used suggestion while nothing is
// applied yet.
export function presetOptionLabel(
  p: TracePreset,
  suggestedId: string | undefined,
): string {
  let label = p.name;
  if (p.perfettoSql.trim() === '') label += ' · setup only';
  if (p.id === suggestedId) label += ' · last used';
  return label;
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
