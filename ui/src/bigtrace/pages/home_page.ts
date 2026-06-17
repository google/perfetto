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

import '../../frontend/home_page.scss';
import m from 'mithril';
import {Card} from '../../widgets/card';
import {EmptyState} from '../../widgets/empty_state';
import {Icon} from '../../widgets/icon';
import {queryState} from '../query/query_state';
import type {TracePreset} from '../query/bigtrace_query_client';
import {presetStore} from '../query/preset_store';
import {groupPresetsByCuj, renderCujSelector} from './preset_groups';
import {setRoute} from '../router';
import {Routes} from '../routes';

// Landing-page action button.
function homeButton(
  label: string,
  icon: string,
  onclick: () => void,
): m.Children {
  return m(
    '.pf-home-page__button',
    {onclick},
    m(Icon, {icon, className: 'pf-left-icon'}),
    m('span.pf-button__label', label),
  );
}

// Material Symbols names are lowercase letters / digits / underscores. An
// absent or malformed value (a backend could send either) falls back to a
// generic glyph rather than rendering as raw ligature text.
function presetIcon(icon?: string): string {
  return icon && /^[a-z0-9_]+$/.test(icon) ? icon : 'bookmark';
}

// A section heading: a small title + one-line description. Shared by the
// presets and custom sections so they read as parallel choices.
function sectionHeader(title: string, subtitle: string): m.Children {
  return m(
    '.pf-bt-home-section__header',
    m('.pf-bt-home-section__title', title),
    m('.pf-bt-home-section__subtitle', subtitle),
  );
}

// A full-width, settings-style card: a leading icon + title with the
// description below. Clicking stashes the preset for QueryPage to seed a
// fresh tab (query + trace-selection settings), then opens the editor.
function presetCard(t: TracePreset): m.Children {
  return m(
    Card,
    {
      className: 'pf-bt-preset-card',
      interactive: true,
      title: t.description || t.name,
      onclick: () => {
        queryState.initialPreset = t;
        setRoute(Routes.QUERY);
      },
    },
    m(
      '.pf-bt-preset-card__head',
      m(Icon, {icon: presetIcon(t.icon)}),
      m('.pf-bt-preset-card__title', t.name),
    ),
    t.description && m('.pf-bt-preset-card__desc', t.description),
  );
}

export class HomePage implements m.ClassComponent {
  // Active CUJ tab; defaults to the first one the catalog returns.
  private activeCuj?: string;

  oninit() {
    // Fetch the catalog on mount; cached after the first load.
    void presetStore.load();
  }

  view() {
    const hasPresets = presetStore.presets.length > 0;
    return m(
      '.pf-home-page',
      m(
        '.pf-home-page__center.pf-bt-home-center',
        // Three states: presets loaded → the picker; still loading → nothing
        // (avoids flashing the empty state); loaded but empty (no backend
        // configured / unreachable / no catalog) → an onboarding empty state.
        hasPresets
          ? this.renderPresets()
          : presetStore.isLoading
            ? null
            : this.renderEmptyState(),
        // The "Custom" section (Advanced settings) sits alongside the picker.
        // The empty state carries its own backend-setup CTA, so it's omitted
        // there to avoid two competing settings links.
        hasPresets && this.renderCustomSection(),
      ),
    );
  }

  // Shown when the catalog is empty — typically a first visit with no backend
  // configured. Points the user at the settings page where the endpoint lives.
  private renderEmptyState(): m.Children {
    return m(
      EmptyState,
      {
        icon: 'cloud_off',
        title: 'Connect a BigTrace backend to see analysis presets',
      },
      homeButton('Configure backend', 'settings', () =>
        setRoute(Routes.SETTINGS),
      ),
    );
  }

  // Presets grouped by CUJ (Memory, CPU, Latency, …). A flat segmented row
  // selects the active CUJ; its presets render below as a card grid. Renders
  // nothing until the catalog loads (or if the backend has none).
  private renderPresets(): m.Children {
    const tpls = presetStore.presets;
    if (tpls.length === 0) return null;

    const {groups, byCuj} = groupPresetsByCuj(tpls);
    const active =
      this.activeCuj !== undefined && byCuj.has(this.activeCuj)
        ? this.activeCuj
        : groups[0][0];

    return m(
      '.pf-bt-home-section',
      // Frame the cards so a first-timer knows what they are and what a click
      // does — otherwise the noun titles read like categories, not analyses.
      sectionHeader(
        'Analysis presets',
        'Ready-made queries for common issues. Pick one to open it across ' +
          'your traces, then run or edit.',
      ),
      renderCujSelector(
        groups.map(([cuj]) => cuj),
        active,
        (cuj) => {
          this.activeCuj = cuj;
        },
      ),
      m('.pf-bt-preset-list', (byCuj.get(active) ?? []).map(presetCard)),
    );
  }

  // The "do it yourself" counterpart to the presets: a titled section whose
  // action drops into the settings page to configure trace selection + options
  // (and write a query) by hand.
  private renderCustomSection(): m.Children {
    return m(
      '.pf-bt-home-section',
      sectionHeader(
        'Custom',
        'Configure trace selection and options yourself, then query.',
      ),
      m(
        '.pf-bt-preset-list',
        m(
          Card,
          {
            className: 'pf-bt-preset-card',
            interactive: true,
            onclick: () => setRoute(Routes.SETTINGS),
          },
          m(
            '.pf-bt-preset-card__head',
            m(Icon, {icon: 'settings'}),
            m('.pf-bt-preset-card__title', 'Advanced settings'),
          ),
        ),
      ),
    );
  }
}
