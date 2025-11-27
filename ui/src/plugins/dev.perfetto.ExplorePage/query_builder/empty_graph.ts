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
import {Button, ButtonVariant} from '../../../widgets/button';
import {Card} from '../../../widgets/card';
import {Keycap} from '../../../widgets/hotkey_glyphs';
import {Icon} from '../../../widgets/icon';
import {nodeRegistry} from './node_registry';

interface SourceCardAttrs {
  title: string;
  description: string;
  icon: string;
  hotkey: string;
  onclick: () => void;
}

const SourceCard: m.Component<SourceCardAttrs> = {
  view({attrs}) {
    const {title, description, icon, hotkey, onclick} = attrs;
    return m(
      Card,
      {
        interactive: true,
        onclick,
        tabindex: 0,
        role: 'button',
        className: 'pf-source-card',
        onkeydown: (e: KeyboardEvent) => {
          if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            onclick();
          }
        },
      },
      m('.pf-source-card-clickable', m(Icon, {icon}), m('h3', title)),
      m('p', description),
      hotkey ? m('.pf-source-card-hotkey', m(Keycap, hotkey)) : null,
    );
  },
};

export interface EmptyGraphAttrs {
  readonly onAddSourceNode: (id: string) => void;
  readonly onImport: () => void;
}

export class EmptyGraph implements m.ClassComponent<EmptyGraphAttrs> {
  view({attrs}: m.CVnode<EmptyGraphAttrs>) {
    const sourceNodes = nodeRegistry
      .list()
      .filter(([_id, node]) => node.type === 'source')
      .map(([id, node]) => {
        return m(SourceCard, {
          title: node.name,
          description: node.description,
          icon: node.icon,
          hotkey: node.hotkey?.toUpperCase() || '',
          onclick: () => attrs.onAddSourceNode(id),
        });
      });

    return m(
      '.pf-exp-node-graph-add-button-container.pf-empty-graph-hero',
      m('h2.pf-empty-graph-hero__title', 'Welcome to the Explore Page'),
      m(
        'p.pf-empty-graph-hero__subtitle',
        'Build and execute SQL queries on your trace data using a visual ' +
          'node-based editor. Get started by adding a source node below.',
      ),
      m('.pf-exp-node-graph-add-buttons', sourceNodes),
      m(Button, {
        label: 'Import from json',
        onclick: attrs.onImport,
        variant: ButtonVariant.Filled,
        icon: 'file_upload',
      }),
    );
  }
}
