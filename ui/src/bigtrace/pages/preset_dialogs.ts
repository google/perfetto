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
import {TextInput} from '../../widgets/text_input';
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
import type {BigTraceEditorTab} from './query_tabs_state';

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
export async function promptPresetDetails(opts: {
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

// Edit what a saved preset shows in the gallery; what it runs is untouched.
export async function editLocalPreset(preset: LocalPreset): Promise<void> {
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

export async function deleteLocalPreset(preset: LocalPreset): Promise<void> {
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
