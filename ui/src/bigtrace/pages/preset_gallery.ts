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
import {Button} from '../../widgets/button';
import {Card} from '../../widgets/card';
import {Icon} from '../../widgets/icon';
import {Intent} from '../../widgets/common';
import {classNames} from '../../base/classnames';
import type {TracePreset} from '../query/bigtrace_query_client';
import {isLocalPreset, type LocalPreset} from '../query/local_preset_store';
import {groupPresetsByCuj, renderCujSelector} from './preset_groups';
import {deleteLocalPreset, editLocalPreset} from './preset_dialogs';

export interface PresetGalleryAttrs {
  // Presets on offer, catalog first. Grouped by category behind a selector;
  // the group holding `selectedId` opens first.
  readonly presets: ReadonlyArray<TracePreset>;
  // The card to mark, and what its badge says — "Last used" where a query is
  // started, "Current" where a setup is read back.
  readonly selectedId?: string;
  readonly selectedBadge: string;
  readonly onPick: (preset: TracePreset) => void;
}

// A backend may send an absent or malformed icon name; fall back to a generic
// glyph so it never renders as raw ligature text.
function presetIcon(icon?: string): string {
  return icon !== undefined && /^[a-z0-9_]+$/.test(icon) ? icon : 'bookmark';
}

// Preset cards by category. What picking one does is the caller's: the
// launcher starts a query from it, the settings form takes its setup. Locally
// saved presets carry edit / delete on hover.
export class PresetGallery implements m.ClassComponent<PresetGalleryAttrs> {
  private activeCuj?: string;

  view({attrs}: m.Vnode<PresetGalleryAttrs>): m.Children {
    const {presets, selectedId, selectedBadge, onPick} = attrs;
    if (presets.length === 0) return null;
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
          renderPresetCard(p, p.id === selectedId, selectedBadge, onPick),
        ),
      ),
    );
  }
}

function renderPresetCard(
  preset: TracePreset,
  selected: boolean,
  selectedBadge: string,
  onPick: (preset: TracePreset) => void,
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
      onclick: () => onPick(preset),
    },
    m('.pf-bt-preset-card__icon', m(Icon, {icon: presetIcon(preset.icon)})),
    m(
      '.pf-bt-preset-card__body',
      m('.pf-bt-preset-card__title', preset.name),
      // A preset without a query configures the run and leaves the editor
      // empty; say so, since the other cards hand you SQL.
      preset.perfettoSql.trim() === '' &&
        m('.pf-bt-preset-card__kind', 'Setup only'),
      preset.description && m('.pf-bt-preset-card__desc', preset.description),
    ),
    local &&
      m('.pf-bt-preset-card__tools', [
        m(Button, {
          icon: 'edit',
          title: 'Edit this preset: name, group, description',
          onclick: (e: MouseEvent) => {
            e.stopPropagation();
            void editLocalPreset(preset as LocalPreset);
          },
        }),
        m(Button, {
          icon: 'delete',
          intent: Intent.Danger,
          title: 'Delete this preset',
          onclick: (e: MouseEvent) => {
            e.stopPropagation();
            void deleteLocalPreset(preset as LocalPreset);
          },
        }),
      ]),
    selected && m('.pf-bt-preset-card__badge', selectedBadge),
  );
}
