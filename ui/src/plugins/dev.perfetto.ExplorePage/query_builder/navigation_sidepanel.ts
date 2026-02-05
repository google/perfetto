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
import {Card} from '../../../widgets/card';
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

// Helper function to render template cards with consistent structure
interface TemplateCardAttrs {
  readonly icon: string;
  readonly title: string;
  readonly description: string;
  readonly ariaLabel: string;
  readonly onClick: () => void;
}

function renderTemplateCard(attrs: TemplateCardAttrs): m.Children {
  return m(
    Card,
    {
      'interactive': true,
      'onclick': attrs.onClick,
      'tabindex': 0,
      'role': 'button',
      'aria-label': attrs.ariaLabel,
      'className': 'pf-template-card',
      'onkeydown': createKeyboardHandler(attrs.onClick),
    },
    m(
      '.pf-source-card-clickable',
      m(Icon, {icon: attrs.icon}),
      m('h3', attrs.title),
    ),
    m('p', attrs.description),
  );
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
      // Top two cards: Clear graph and Preloaded tables
      results.push(
        m(
          '.pf-top-cards',
          renderTemplateCard({
            icon: 'draft',
            title: 'New graph',
            description: 'Start with empty canvas',
            ariaLabel: 'New graph',
            onClick: () => attrs.onLoadEmptyTemplate?.(),
          }),
          renderTemplateCard({
            icon: 'auto_fix_high',
            title: 'Smart graph',
            description: 'Tailored for your trace data',
            ariaLabel: 'Smart graph',
            onClick: () => attrs.onLoadExploreTemplate?.(),
          }),
        ),
      );

      // Tutorials section
      results.push(
        m('h4.pf-starting-section-title', 'Tutorials'),
        m(
          '.pf-tutorial-cards',
          renderTemplateCard({
            icon: 'school',
            title: 'Graph 101',
            description:
              'Interactive tutorial covering node docking, filtering, adding nodes, and multi-child workflows',
            ariaLabel: 'Start Graph 101 tutorial',
            onClick: () =>
              attrs.onLoadExampleByPath?.(
                'assets/explore_page/examples/learning.json',
              ),
          }),
          renderTemplateCard({
            icon: 'join_inner',
            title: 'Joins',
            description:
              'Learn how to combine data from multiple sources using joins',
            ariaLabel: 'Start Joins tutorial',
            onClick: () =>
              attrs.onLoadExampleByPath?.(
                'assets/explore_page/examples/joins_learning.json',
              ),
          }),
          renderTemplateCard({
            icon: 'schedule',
            title: 'Time',
            description:
              'Learn how to filter and analyze data using time-based queries',
            ariaLabel: 'Start Time tutorial',
            onClick: () =>
              attrs.onLoadExampleByPath?.(
                'assets/explore_page/examples/time_learning.json',
              ),
          }),
        ),
      );

      // Solutions section - filter out the Learning example since it's now Graph 101
      const solutionExamples = EXAMPLE_GRAPHS.filter(
        (example) => example.name !== 'Learning',
      );

      if (solutionExamples.length > 0) {
        results.push(m('h4.pf-starting-section-title', 'Solutions'));
        const solutionCards = solutionExamples.map((example) =>
          renderTemplateCard({
            icon: 'auto_stories',
            title: example.name,
            description: example.description,
            ariaLabel: `Load ${example.name} example`,
            onClick: () => attrs.onLoadExampleByPath?.(example.jsonPath),
          }),
        );
        results.push(m('.pf-solution-cards', ...solutionCards));
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
