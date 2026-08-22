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
import {Card} from '../../widgets/card';
import {EmptyState} from '../../widgets/empty_state';
import {Spinner} from '../../widgets/spinner';
import {Icon} from '../../widgets/icon';
import {Intent} from '../../widgets/common';
import {TextInput} from '../../widgets/text_input';
import {classNames} from '../../base/classnames';
import {showModal} from '../../widgets/modal';
import type {TracePreset} from '../query/bigtrace_query_client';
import {presetStore} from '../query/preset_store';
import {
  DEFAULT_LOCAL_CATEGORY,
  localPresetStore,
  presetFromTab,
  presetNamed,
  type LocalPreset,
  type PresetDetails,
} from '../query/local_preset_store';
import {lastPresetIdState} from '../settings/last_preset_state';
import {presetMatches} from '../query/preset_match';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import {getBigtraceEndpoint} from '../settings/endpoint_storage';
import type {SettingsBindings} from '../settings/tab_bound_setting';
import {
  applyPresetToTab,
  closeSettings,
  effectiveTabSettings,
  type BigTraceEditorTab,
  type QueryTabsState,
} from './query_tabs_state';
import {groupPresetsByCuj, renderCujSelector} from './preset_groups';
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

export function isLocalPreset(p: TracePreset): p is LocalPreset {
  return (p as LocalPreset).isLocal === true;
}

// A backend may send an absent or malformed icon name; fall back to a generic
// glyph so it never renders as raw ligature text.
function presetIcon(icon?: string): string {
  return icon !== undefined && /^[a-z0-9_]+$/.test(icon) ? icon : 'bookmark';
}

// Shown in a tab until it has a configuration: pick a preset, or configure the
// trace selection and options by hand.
export class QueryLauncher implements m.ClassComponent<QueryLauncherAttrs> {
  private activeCuj?: string;

  oninit() {
    void presetStore.load();
  }

  view({attrs}: m.Vnode<QueryLauncherAttrs>): m.Children {
    const {tab, tabsState, bindings} = attrs;
    if (tab.setupMode === 'custom') {
      return m(
        '.pf-bt-launcher.pf-bt-launcher--custom',
        m('.pf-bt-launcher__custom-body', m(QuerySettingsForm, {bindings})),
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

    const selectedId = preselectedPresetId(presets, lastPresetIdState.get());
    const {groups, byCuj} = groupPresetsByCuj(presets);
    // Same bucketing as groupPresetsByCuj, so an uncategorised preset opens
    // its ("Other") group rather than falling back to the first one.
    const selected = presets.find((p) => p.id === selectedId);
    const selectedCuj =
      selected === undefined ? undefined : selected.category || 'Other';
    const active =
      this.activeCuj !== undefined && byCuj.has(this.activeCuj)
        ? this.activeCuj
        : selectedCuj !== undefined && byCuj.has(selectedCuj)
          ? selectedCuj
          : groups[0][0];
    const cards = byCuj.get(active) ?? [];

    return m(
      '.pf-bt-launcher__presets',
      renderCujSelector(
        groups.map(([cuj]) => cuj),
        active,
        (cuj) => {
          this.activeCuj = cuj;
        },
      ),
      m(
        `.pf-bt-preset-list.pf-bt-preset-list--cols-${Math.min(cards.length, 3)}`,
        cards.map((p) =>
          this.renderPresetCard(p, p.id === selectedId, tab, tabsState),
        ),
      ),
    );
  }

  private renderPresetCard(
    preset: TracePreset,
    selected: boolean,
    tab: BigTraceEditorTab,
    tabsState: QueryTabsState,
  ): m.Children {
    const local = isLocalPreset(preset);
    return m(
      Card,
      {
        className: classNames(
          'pf-bt-preset-card',
          selected && 'pf-bt-preset-card--selected',
        ),
        interactive: true,
        title: preset.description || preset.name,
        onclick: () => this.applyPreset(preset, tab, tabsState),
      },
      m('.pf-bt-preset-card__icon', m(Icon, {icon: presetIcon(preset.icon)})),
      m(
        '.pf-bt-preset-card__body',
        m('.pf-bt-preset-card__title', preset.name),
        preset.description && m('.pf-bt-preset-card__desc', preset.description),
      ),
      local &&
        m('.pf-bt-preset-card__tools', [
          m(Button, {
            icon: 'edit',
            title: 'Edit this preset: name, group, description',
            onclick: (e: MouseEvent) => {
              e.stopPropagation();
              void this.editLocalPreset(preset as LocalPreset);
            },
          }),
          m(Button, {
            icon: 'delete',
            intent: Intent.Danger,
            title: 'Delete this preset',
            onclick: (e: MouseEvent) => {
              e.stopPropagation();
              void this.deleteLocalPreset(preset as LocalPreset);
            },
          }),
        ]),
      selected && m('.pf-bt-preset-card__badge', 'Last used'),
    );
  }

  private applyPreset(
    preset: TracePreset,
    tab: BigTraceEditorTab,
    tabsState: QueryTabsState,
  ): void {
    applyPresetToTab(tab, preset);
    lastPresetIdState.set(preset.id);
    tab.setupMode = undefined;
    tabsState.markDirty();
    m.redraw();
  }

  private renderCustomFooter(
    tab: BigTraceEditorTab,
    tabsState: QueryTabsState,
  ): m.Children {
    const hasQuery = canReturnToQuery(tab);
    return m('.pf-bt-launcher__footer', [
      // Presets belong to starting a query, not to configuring one that
      // exists: from a tab with a query, applying one would replace its SQL
      // and settings wholesale. New tab → the gallery is where you came from.
      !hasQuery &&
        m(Button, {
          label: 'Back to presets',
          icon: 'arrow_back',
          onclick: () => {
            tab.setupMode = 'presets';
            tabsState.markDirty();
          },
        }),
      // Edits apply as they're made, so leaving is where the choice is: Back
      // puts the configuration back to what it was, Done keeps it.
      hasQuery &&
        m(Button, {
          label: 'Back to query',
          icon: 'arrow_back',
          title:
            'Return to the query as it was; changes made here are dropped.',
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
        label: hasQuery ? 'Done' : 'Start query',
        icon: 'arrow_forward',
        intent: Intent.Primary,
        variant: ButtonVariant.Filled,
        title: hasQuery ? 'Keep these settings for the next run.' : undefined,
        onclick: () => {
          // Starting a query by hand: if this setup happens to be one of the
          // presets on offer (e.g. it was just saved as one), the next tab
          // preselects it; otherwise there's no preset to remember. Editing
          // an existing query says nothing about what the next one starts
          // from, so Done leaves the last-used preset alone.
          if (!hasQuery) {
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

  private async editLocalPreset(preset: LocalPreset): Promise<void> {
    const details = await promptPresetDetails({
      title: 'Edit preset',
      button: 'Save',
      initial: preset,
      own: preset,
      allowOverwrite: false,
    });
    if (details === undefined) return;
    localPresetStore.update(preset.id, details);
    m.redraw();
  }

  private async deleteLocalPreset(preset: LocalPreset): Promise<void> {
    let confirmed = false;
    await showModal({
      title: 'Delete preset?',
      content: m('div', `"${preset.name}" will be removed from this browser.`),
      buttons: [
        {text: 'Cancel'},
        {
          text: 'Delete',
          primary: true,
          action: () => {
            confirmed = true;
          },
        },
      ],
    });
    if (!confirmed) return;
    localPresetStore.remove(preset.id);
    m.redraw();
  }
}

// A preset name that would collide with one already on offer. Catalog names
// are reserved outright: a local twin couldn't be told apart in the gallery. A
// local match is left to the caller — overwrite when saving, refuse when editing
// another preset into it. `own` is the preset being edited, which naturally
// keeps its own name.
export type PresetNameConflict =
  | {readonly kind: 'catalog'; readonly preset: TracePreset}
  | {readonly kind: 'local'; readonly preset: LocalPreset}
  | undefined;

export function presetNameConflict(
  name: string,
  catalog: ReadonlyArray<TracePreset>,
  local: ReadonlyArray<LocalPreset>,
  own?: LocalPreset,
): PresetNameConflict {
  if (own !== undefined && presetNamed(name, [own]) !== undefined) {
    return undefined;
  }
  const inCatalog = presetNamed(name, catalog);
  if (inCatalog !== undefined) return {kind: 'catalog', preset: inCatalog};
  const inLocal = presetNamed(
    name,
    local.filter((p) => p.id !== own?.id),
  );
  if (inLocal !== undefined) return {kind: 'local', preset: inLocal};
  return undefined;
}

// Name / group / description dialog shared by "Save as preset" and "Edit
// preset". The name is checked as it's typed against everything in the
// gallery; while it's empty or would shadow another preset the primary button
// stays off, and the message under the field says why. Resolves undefined if
// cancelled; otherwise the trimmed details, group defaulted.
async function promptPresetDetails(opts: {
  readonly title: string;
  readonly button: string;
  readonly initial: PresetDetails;
  // The preset being edited, if any: it may keep its own name.
  readonly own?: LocalPreset;
  // Whether a local preset of the same name may be replaced (the caller then
  // confirms the overwrite) rather than blocking the name.
  readonly allowOverwrite: boolean;
}): Promise<PresetDetails | undefined> {
  let name = opts.initial.name;
  let category = opts.initial.category;
  let description = opts.initial.description;
  let confirmed = false;
  const problem = (): string | undefined => {
    const conflict = presetNameConflict(
      name,
      presetStore.presets,
      localPresetStore.list(),
      opts.own,
    );
    if (conflict === undefined) return undefined;
    if (conflict.kind === 'catalog') {
      return `The catalog already has a preset called "${conflict.preset.name}". Pick another name.`;
    }
    if (!opts.allowOverwrite) {
      return `You already have a preset called "${conflict.preset.name}".`;
    }
    return undefined;
  };
  await showModal({
    title: opts.title,
    content: () => {
      const message = problem();
      return m('.pf-bt-save-preset', [
        m('label', 'Name'),
        m(TextInput, {
          value: name,
          placeholder: 'Preset name',
          oninput: (e: Event) => {
            name = (e.target as HTMLInputElement).value;
          },
        }),
        message !== undefined &&
          m('.pf-bt-save-preset__message', {role: 'alert'}, message),
        m('label', 'Group'),
        m(TextInput, {
          value: category,
          placeholder: 'Group shown in the picker',
          oninput: (e: Event) => {
            category = (e.target as HTMLInputElement).value;
          },
        }),
        m('label', 'Description'),
        m(TextInput, {
          value: description,
          placeholder: 'One line, shown on the card',
          oninput: (e: Event) => {
            description = (e.target as HTMLInputElement).value;
          },
        }),
      ]);
    },
    buttons: [
      {text: 'Cancel'},
      {
        text: opts.button,
        primary: true,
        disabled: () => name.trim() === '' || problem() !== undefined,
        action: () => {
          confirmed = true;
        },
      },
    ],
  });
  if (!confirmed || name.trim() === '') return undefined;
  return {
    name: name.trim(),
    category: category.trim() || DEFAULT_LOCAL_CATEGORY,
    description: description.trim(),
  };
}

// Save a tab as a local preset. Saving under a name the user already has
// overwrites that preset (after a confirm) rather than leaving two entries they
// can't tell apart; a catalog name can't be used at all. Resolves undefined if
// cancelled.
export async function promptForPreset(
  tab: BigTraceEditorTab,
  initial: PresetDetails,
): Promise<LocalPreset | undefined> {
  const details = await promptPresetDetails({
    title: 'Save as preset',
    button: 'Save',
    initial,
    allowOverwrite: true,
  });
  if (details === undefined) return undefined;

  const existing = localPresetStore.findByName(details.name);
  if (existing !== undefined) {
    let overwrite = false;
    await showModal({
      title: 'Overwrite preset?',
      content: m('div', `"${existing.name}" already exists. Replace it?`),
      buttons: [
        {text: 'Cancel'},
        {
          text: 'Overwrite',
          primary: true,
          action: () => {
            overwrite = true;
          },
        },
      ],
    });
    if (!overwrite) return undefined;
  }
  return localPresetStore.save(
    presetFromTab(tab, {...details, id: existing?.id}),
  );
}

// Whether the tab, as configured, is exactly one of the presets on offer.
export function matchingPresetId(
  tab: BigTraceEditorTab,
  presets: ReadonlyArray<TracePreset>,
): string | undefined {
  const current = {
    sql: tab.editorText,
    traceFilters: tab.traceFilters,
    traceMetadataColumns: tab.traceMetadataColumns,
    traceOrderBy: tab.traceOrderBy,
    settings: effectiveTabSettings(tab),
  };
  return presets.find((p) => presetMatches(p, current))?.id;
}
