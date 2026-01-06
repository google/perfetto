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
import {Keycap} from '../../../widgets/hotkey_glyphs';
import {QueryNode} from '../query_node';
import {nodeRegistry} from './node_registry';

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
  readonly onLoadLearningTemplate?: () => void;
  readonly onLoadExploreTemplate?: () => void;
  readonly onLoadExample?: () => void;
  readonly onLoadEmptyTemplate?: () => void;
}

export class NavigationSidePanel
  implements m.ClassComponent<NavigationSidePanelAttrs>
{
  view({attrs}: m.CVnode<NavigationSidePanelAttrs>) {
    const results: m.Children[] = [];

    // Show template buttons when nothing is selected
    if (!attrs.selectedNode) {
      results.push(
        m('h4.pf-starting-section-title', 'Load a graph'),
        m(
          '.pf-template-grid',
          renderTemplateCard({
            icon: 'school',
            title: 'Learning',
            description: 'Educational example',
            ariaLabel: 'Start with learning template',
            onClick: () => attrs.onLoadLearningTemplate?.(),
          }),
          renderTemplateCard({
            icon: 'explore',
            title: 'Preload useful tables',
            description: 'Tailored for your trace data',
            ariaLabel: 'Preload useful tables',
            onClick: () => attrs.onLoadExploreTemplate?.(),
          }),
          renderTemplateCard({
            icon: 'auto_stories',
            title: 'Graphs',
            description: 'Load a predefined graph',
            ariaLabel: 'Load predefined graph',
            onClick: () => attrs.onLoadExample?.(),
          }),
          renderTemplateCard({
            icon: 'delete_sweep',
            title: 'Clear Graph',
            description: 'Start with empty canvas',
            ariaLabel: 'Clear graph',
            onClick: () => attrs.onLoadEmptyTemplate?.(),
          }),
        ),
        m('h4.pf-starting-section-title', 'Add source node'),
      );
    }

    const sourceNodes = nodeRegistry
      .list()
      .filter(([_id, node]) => node.showOnLandingPage === true)
      .map(([id, node]) => {
        const name = node.name ?? 'Unnamed Source';
        const description = node.description ?? '';
        const icon = node.icon ?? '';
        const hotkey =
          node.hotkey && typeof node.hotkey === 'string'
            ? node.hotkey.toUpperCase()
            : undefined;

        return m(
          Card,
          {
            'interactive': true,
            'onclick': () => attrs.onAddSourceNode(id),
            'tabindex': 0,
            'role': 'button',
            'aria-label': `Add ${name} source`,
            'className': 'pf-source-card',
            'onkeydown': createKeyboardHandler(() => attrs.onAddSourceNode(id)),
          },
          m('.pf-source-card-clickable', m(Icon, {icon}), m('h3', name)),
          m('p', description),
          hotkey ? m('.pf-source-card-hotkey', m(Keycap, hotkey)) : null,
        );
      });

    // Wrap source cards in horizontal container
    const sourceCardsContainer = m(
      '.pf-source-cards-horizontal',
      ...sourceNodes,
    );

    return [...results, sourceCardsContainer];
  }
}
