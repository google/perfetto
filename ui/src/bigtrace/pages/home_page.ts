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

// A preset as a clickable card: leading icon + title with the description
// below. Clicking stashes the preset for QueryPage to seed a fresh tab (query
// + trace-selection settings), then opens the editor.
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
    m('.pf-bt-preset-card__icon', m(Icon, {icon: presetIcon(t.icon)})),
    m(
      '.pf-bt-preset-card__body',
      m('.pf-bt-preset-card__title', t.name),
      t.description && m('.pf-bt-preset-card__desc', t.description),
    ),
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
        // Lead with purpose, make the presets the single focal point, and keep
        // the manual path as a quiet secondary link below them. With no backend
        // the empty state stands alone (no competing intro).
        hasPresets && this.renderIntro(),
        hasPresets
          ? this.renderPresets()
          : presetStore.isLoading
            ? null
            : this.renderEmptyState(),
        hasPresets && this.renderCustomLink(),
      ),
    );
  }

  // Purpose at a glance. The sidebar already carries the product name, so this
  // leads with value (what BigTrace lets you do), not branding.
  private renderIntro(): m.Children {
    return m(
      '.pf-bt-home-intro',
      m(
        '.pf-bt-home-intro__title',
        m('span.pf-bt-home-intro__brand', 'BigTrace:'),
        ' query traces at scale',
      ),
      m(
        '.pf-bt-home-intro__subtitle',
        'Ready-to-run presets for common issues — pick one to start, or build your own.',
      ),
    );
  }

  // No backend configured (or unreachable / empty catalog): a single-message
  // onboarding state — headline, a one-line reason, and one clear action.
  private renderEmptyState(): m.Children {
    return m(
      EmptyState,
      {
        icon: 'cloud_off',
        title: 'Connect a backend to get started',
      },
      m(
        '.pf-bt-home-empty__detail',
        'BigTrace runs your queries against a backend that holds the traces.',
      ),
      homeButton('Configure backend', 'settings', () =>
        setRoute(Routes.SETTINGS),
      ),
    );
  }

  // The presets are the primary action and the page's focal point: a CUJ
  // selector over the ready-to-run preset cards.
  private renderPresets(): m.Children {
    const tpls = presetStore.presets;
    if (tpls.length === 0) return null;

    const {groups, byCuj} = groupPresetsByCuj(tpls);
    const active =
      this.activeCuj !== undefined && byCuj.has(this.activeCuj)
        ? this.activeCuj
        : groups[0][0];

    return m(
      '.pf-bt-home-presets',
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

  // Secondary path, deliberately lower-weight than the presets: the manual
  // route — open the settings page to choose which traces to run over and set
  // query options, the starting point for building your own analysis.
  private renderCustomLink(): m.Children {
    return m(
      'a.pf-bt-home-custom-link',
      {onclick: () => setRoute(Routes.SETTINGS)},
      m(Icon, {icon: 'tune', className: 'pf-left-icon'}),
      'Configure trace selection and options',
    );
  }
}
