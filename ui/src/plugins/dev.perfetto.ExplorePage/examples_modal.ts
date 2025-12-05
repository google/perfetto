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
import {closeModal, showModal} from '../../widgets/modal';
import {EXAMPLE_GRAPHS, ExampleGraph} from './example_graphs';
import {Card} from '../../widgets/card';
import {Icon} from '../../widgets/icon';

export function showExamplesModal(): Promise<ExampleGraph | undefined> {
  return new Promise((resolve) => {
    const handleExampleClick = (example: ExampleGraph) => {
      resolve(example);
      closeModal();
    };

    showModal({
      key: 'examples-modal',
      title: 'Example Graphs',
      content: () => {
        return m(
          '.pf-examples-modal',
          m(
            '.pf-examples-grid',
            EXAMPLE_GRAPHS.map((example) =>
              m(
                Card,
                {
                  'interactive': true,
                  'onclick': () => handleExampleClick(example),
                  'tabindex': 0,
                  'role': 'button',
                  'aria-label': `Load ${example.name} example`,
                  'className': 'pf-example-card',
                  'onkeydown': (e: KeyboardEvent) => {
                    if (e.key === 'Enter' || e.key === ' ') {
                      e.preventDefault();
                      handleExampleClick(example);
                    }
                  },
                },
                m(
                  '.pf-example-card-content',
                  m(Icon, {icon: 'description'}),
                  m('h3', example.name),
                ),
                m('p', example.description),
              ),
            ),
          ),
        );
      },
      buttons: [],
      onClose: () => {
        resolve(undefined);
      },
    });
  });
}
