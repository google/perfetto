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
  setupFromTab,
  type LocalPreset,
} from '../query/local_preset_store';
import {lastPresetIdState, lastSetupState} from '../settings/query_setup_state';
import {presetMatches} from '../query/preset_match';
import {getBigtraceEndpoint} from '../settings/endpoint_storage';
import type {SettingsBindings} from '../settings/tab_bound_setting';
import {
  applyPresetToTab,
  effectiveTabSettings,
  TRACE_LIMIT_SETTING_ID,
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
  private mode: 'presets' | 'custom' = 'presets';
  private activeCuj?: string;
  // The custom form seeds from the last setup once per visit, not every redraw.
  private seededFromLastSetup = false;

  oninit() {
    void presetStore.load();
  }

  view({attrs}: m.Vnode<QueryLauncherAttrs>): m.Children {
    const {tab, tabsState, bindings} = attrs;
    if (this.mode === 'custom') {
      return m(
        '.pf-bt-launcher.pf-bt-launcher--custom',
        m('.pf-bt-launcher__custom-body', m(QuerySettingsForm, {bindings})),
        this.renderCustomFooter(tab, tabsState),
      );
    }
    return m(
      '.pf-bt-launcher',
      m('.pf-bt-launcher__inner', [
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
            onclick: () => this.enterCustom(bindings),
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
      return m(
        EmptyState,
        {
          icon: getBigtraceEndpoint() === '' ? 'cloud_off' : 'bookmark_border',
          title:
            getBigtraceEndpoint() === ''
              ? 'Connect a backend to load presets'
              : 'No presets yet',
        },
        m(
          '.pf-bt-launcher__empty-detail',
          getBigtraceEndpoint() === ''
            ? 'BigTrace runs your queries against a backend that holds the traces.'
            : 'Configure a query below, then save it as a preset to reuse it.',
        ),
      );
    }

    const selectedId = preselectedPresetId(presets, lastPresetIdState.get());
    const {groups, byCuj} = groupPresetsByCuj(presets);
    const selectedCuj =
      presets.find((p) => p.id === selectedId)?.category || undefined;
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
            title: 'Rename this preset',
            onclick: (e: MouseEvent) => {
              e.stopPropagation();
              void this.renameLocalPreset(preset as LocalPreset);
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
    lastSetupState.set(setupFromTab(tab));
    tabsState.markDirty();
    m.redraw();
  }

  // Entering the custom path on a fresh tab: start from the selection the last
  // query ran with, so the trace source doesn't have to be retyped every time.
  private enterCustom(bindings: SettingsBindings): void {
    const setup = lastSetupState.get();
    if (setup !== null && !this.seededFromLastSetup) {
      for (const s of setup.settings) {
        // The trace cap follows the execution mode; seeding the last one would
        // freeze it (see MODE_DEFAULTS).
        if (s.settingId === TRACE_LIMIT_SETTING_ID) continue;
        bindings.setSettingValue(s.settingId, s.values, s.category);
      }
      bindings.setTraceFilters(setup.traceFilters);
      bindings.setTraceMetadataColumns(setup.traceMetadataColumns);
      bindings.setTraceOrderBy(setup.traceOrderBy);
    }
    this.seededFromLastSetup = true;
    this.mode = 'custom';
  }

  private renderCustomFooter(
    tab: BigTraceEditorTab,
    tabsState: QueryTabsState,
  ): m.Children {
    return m('.pf-bt-launcher__footer', [
      m(Button, {
        label: 'Back to presets',
        icon: 'arrow_back',
        onclick: () => {
          this.mode = 'presets';
        },
      }),
      m('.pf-bt-launcher__footer-spacer'),
      m(Button, {
        label: 'Save as preset…',
        icon: 'bookmark_add',
        onclick: () => void this.saveAsPreset(tab),
      }),
      m(Button, {
        label: 'Start query',
        icon: 'arrow_forward',
        intent: Intent.Primary,
        variant: ButtonVariant.Filled,
        onclick: () => {
          lastSetupState.set(setupFromTab(tab));
          // If this setup happens to be one of the presets on offer (e.g. it
          // was just saved as one), the next tab preselects it; otherwise
          // there's no preset to remember.
          lastPresetIdState.set(
            matchingPresetId(
              tab,
              launcherPresets(presetStore.presets, localPresetStore.list()),
            ) ?? '',
          );
          tab.configured = true;
          tabsState.markDirty();
        },
      }),
    ]);
  }

  private async saveAsPreset(tab: BigTraceEditorTab): Promise<void> {
    const saved = await promptForPreset(tab, {
      name: tab.title,
      category: DEFAULT_LOCAL_CATEGORY,
    });
    if (saved === undefined) return;
    lastPresetIdState.set(saved.id);
    m.redraw();
  }

  private async renameLocalPreset(preset: LocalPreset): Promise<void> {
    let name = preset.name;
    let confirmed = false;
    await showModal({
      title: 'Rename preset',
      content: () =>
        m(TextInput, {
          value: name,
          placeholder: 'Preset name',
          oninput: (e: Event) => {
            name = (e.target as HTMLInputElement).value;
          },
        }),
      buttons: [
        {text: 'Cancel'},
        {
          text: 'Rename',
          primary: true,
          action: () => {
            confirmed = true;
          },
        },
      ],
    });
    if (!confirmed || name.trim() === '') return;
    localPresetStore.rename(preset.id, name.trim());
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

// Name + category prompt for saving a tab as a local preset. Saving under a
// name that already exists overwrites that preset (after a confirm) rather than
// leaving two entries the user can't tell apart. Resolves undefined if
// cancelled.
export async function promptForPreset(
  tab: BigTraceEditorTab,
  initial: {readonly name: string; readonly category: string},
): Promise<LocalPreset | undefined> {
  let name = initial.name;
  let category = initial.category;
  let confirmed = false;
  await showModal({
    title: 'Save as preset',
    content: () =>
      m('.pf-bt-save-preset', [
        m('label', 'Name'),
        m(TextInput, {
          value: name,
          placeholder: 'Preset name',
          oninput: (e: Event) => {
            name = (e.target as HTMLInputElement).value;
          },
        }),
        m('label', 'Group'),
        m(TextInput, {
          value: category,
          placeholder: 'Group shown in the picker',
          oninput: (e: Event) => {
            category = (e.target as HTMLInputElement).value;
          },
        }),
      ]),
    buttons: [
      {text: 'Cancel'},
      {
        text: 'Save',
        primary: true,
        action: () => {
          confirmed = true;
        },
      },
    ],
  });
  if (!confirmed || name.trim() === '') return undefined;

  const existing = localPresetStore.findByName(name.trim());
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
    presetFromTab(tab, {
      name: name.trim(),
      category: category.trim() || DEFAULT_LOCAL_CATEGORY,
      id: existing?.id,
    }),
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
