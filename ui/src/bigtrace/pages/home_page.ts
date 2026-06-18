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
import {Anchor} from '../../widgets/anchor';
import {Card} from '../../widgets/card';
import {EmptyState} from '../../widgets/empty_state';
import {Icon} from '../../widgets/icon';
import {queryState} from '../query/query_state';
import type {TracePreset} from '../query/bigtrace_query_client';
import {presetStore} from '../query/preset_store';
import {groupPresetsByCuj, renderCujSelector} from './preset_groups';
import {setRoute} from '../router';
import {Routes} from '../routes';

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

// A backend may send an absent or malformed icon name; fall back to a generic
// glyph so it never renders as raw ligature text.
function presetIcon(icon?: string): string {
  return icon && /^[a-z0-9_]+$/.test(icon) ? icon : 'bookmark';
}

// Clicking stashes the preset so QueryPage seeds a fresh tab from it, then
// routes to the editor.
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
  // Active CUJ tab; defaults to the catalog's first.
  private activeCuj?: string;

  oninit() {
    void presetStore.load();
  }

  view() {
    const hasPresets = presetStore.presets.length > 0;
    return m(
      '.pf-home-page',
      m(
        '.pf-home-page__center.pf-bt-home-center',
        // Presets are the focal point; the intro and the manual-path link show
        // only alongside them, so with no backend the empty state stands alone.
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

  private renderIntro(): m.Children {
    return m(
      '.pf-bt-home-intro',
      m('.pf-bt-home-intro__title', 'Query traces at scale'),
      m(
        '.pf-bt-home-intro__subtitle',
        'Ready-to-run presets for common issues.',
      ),
    );
  }

  // Shown when no backend is configured (or it's unreachable / has no presets).
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

  // The build-your-own path: a link to the settings page to choose traces and
  // set query options by hand.
  private renderCustomLink(): m.Children {
    return m(
      '.pf-bt-home-custom-link',
      m(
        Anchor,
        {startIcon: 'tune', onclick: () => setRoute(Routes.SETTINGS)},
        'Configure trace selection',
      ),
    );
  }
}
