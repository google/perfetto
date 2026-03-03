// Copyright (C) 2025 The Android Open Source Project
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
import {classNames} from '../../../base/classnames';
import {Card, CardStack} from '../../../widgets/card';
import {Icon} from '../../../widgets/icon';
import {QueryNode} from '../query_node';
import {EXAMPLE_GRAPHS} from '../example_graphs';
import {RecentGraphsSection} from '../recent_graphs';

// Helper function for keyboard-accessible card interactions
function createKeyboardHandler(callback: () => void) {
  return (e: KeyboardEvent) => {
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      callback();
    }
  };
}

interface ActionCardAttrs {
  readonly icon: string;
  readonly title: string;
  readonly description: string;
  readonly ariaLabel: string;
  readonly onClick: () => void;
  readonly accent?: boolean;
}

// Renders a prominent horizontal action card (for "New graph" / "Smart graph")
function renderActionCard(attrs: ActionCardAttrs): m.Children {
  return m(
    Card,
    {
      'interactive': true,
      'onclick': attrs.onClick,
      'tabindex': 0,
      'role': 'button',
      'aria-label': attrs.ariaLabel,
      'className': classNames(
        'pf-nav-action-card',
        attrs.accent && 'pf-nav-action-card--accent',
      ),
      'onkeydown': createKeyboardHandler(attrs.onClick),
    },
    m('.pf-nav-action-card__icon', m(Icon, {icon: attrs.icon})),
    m(
      '.pf-nav-action-card__text',
      m('.pf-nav-action-card__title', attrs.title),
      m('.pf-nav-action-card__desc', attrs.description),
    ),
  );
}

interface ListItemAttrs {
  readonly icon: string;
  readonly title: string;
  readonly description: string;
  readonly ariaLabel: string;
  readonly onClick: () => void;
}

// Renders a compact horizontal list item (for tutorials/solutions)
function renderListItem(attrs: ListItemAttrs): m.Children {
  return m(
    Card,
    {
      'interactive': true,
      'onclick': attrs.onClick,
      'tabindex': 0,
      'role': 'button',
      'aria-label': attrs.ariaLabel,
      'className': 'pf-nav-list-item',
      'onkeydown': createKeyboardHandler(attrs.onClick),
    },
    m('.pf-nav-list-item__icon', m(Icon, {icon: attrs.icon})),
    m(
      '.pf-nav-list-item__text',
      m('.pf-nav-list-item__title', attrs.title),
      m('.pf-nav-list-item__desc', attrs.description),
    ),
    m('.pf-nav-list-item__arrow', m(Icon, {icon: 'chevron_right'})),
  );
}

function renderSectionHeader(title: string): m.Children {
  return m('.pf-nav-section-header', m('span', title));
}

export interface NavigationSidePanelAttrs {
  readonly selectedNode?: QueryNode;
  readonly onAddSourceNode: (id: string) => void;
  readonly onLoadExampleByPath?: (jsonPath: string) => void;
  readonly onLoadExploreTemplate?: () => void;
  readonly onLoadEmptyTemplate?: () => void;
  readonly onLoadRecentGraph?: (json: string) => void;
}

export class NavigationSidePanel
  implements m.ClassComponent<NavigationSidePanelAttrs>
{
  view({attrs}: m.CVnode<NavigationSidePanelAttrs>) {
    const results: m.Children[] = [];

    // Show template buttons when nothing is selected
    if (!attrs.selectedNode) {
      // Primary action cards
      results.push(
        m(
          '.pf-nav-actions',
          renderActionCard({
            icon: 'draft',
            title: 'New graph',
            description: 'Start with empty canvas',
            ariaLabel: 'New graph',
            onClick: () => attrs.onLoadEmptyTemplate?.(),
          }),
          renderActionCard({
            icon: 'auto_fix_high',
            title: 'Smart graph',
            description: 'Tailored for your trace data',
            ariaLabel: 'Smart graph',
            onClick: () => attrs.onLoadExploreTemplate?.(),
            accent: true,
          }),
        ),
      );

      // Tutorials section
      results.push(
        m(
          '.pf-nav-section',
          renderSectionHeader('Tutorials'),
          m(
            CardStack,
            {className: 'pf-nav-list'},
            renderListItem({
              icon: 'school',
              title: 'Graph 101',
              description:
                'Node docking, filtering, adding nodes, and multi-child workflows',
              ariaLabel: 'Start Graph 101 tutorial',
              onClick: () =>
                attrs.onLoadExampleByPath?.(
                  'assets/explore_page/examples/learning.json',
                ),
            }),
            renderListItem({
              icon: 'join_inner',
              title: 'Joins',
              description: 'Combine data from multiple sources using joins',
              ariaLabel: 'Start Joins tutorial',
              onClick: () =>
                attrs.onLoadExampleByPath?.(
                  'assets/explore_page/examples/joins_learning.json',
                ),
            }),
            renderListItem({
              icon: 'schedule',
              title: 'Time',
              description: 'Filter and analyze data using time-based queries',
              ariaLabel: 'Start Time tutorial',
              onClick: () =>
                attrs.onLoadExampleByPath?.(
                  'assets/explore_page/examples/time_learning.json',
                ),
            }),
          ),
        ),
      );

      // Solutions section
      const solutionExamples = EXAMPLE_GRAPHS.filter(
        (example) => example.name !== 'Learning',
      );

      if (solutionExamples.length > 0) {
        results.push(
          m(
            '.pf-nav-section',
            renderSectionHeader('Solutions'),
            m(
              CardStack,
              {className: 'pf-nav-list'},
              ...solutionExamples.map((example) =>
                renderListItem({
                  icon: 'auto_stories',
                  title: example.name,
                  description: example.description,
                  ariaLabel: `Load ${example.name} example`,
                  onClick: () => attrs.onLoadExampleByPath?.(example.jsonPath),
                }),
              ),
            ),
          ),
        );
      }

      // Recent graphs section
      if (attrs.onLoadRecentGraph !== undefined) {
        results.push(
          m(RecentGraphsSection, {
            onLoadGraph: attrs.onLoadRecentGraph,
          }),
        );
      }
    }

    return [...results];
  }
}
