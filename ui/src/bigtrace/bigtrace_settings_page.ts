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

import {EmptyState} from '../widgets/empty_state';
import {Callout} from '../widgets/callout';
import {Intent} from '../widgets/common';
import m from 'mithril';
import {SettingsShell} from '../widgets/settings_shell';
import {Switch} from '../widgets/switch';
import {Card} from '../widgets/card';
import {Anchor} from '../widgets/anchor';
import {classNames} from '../base/classnames';
import {bigTraceSettingsManager} from './bigtrace_settings_manager';
import {Setting} from './settings_types';
import {renderSetting} from './settings_widgets';
import {Button, ButtonVariant} from '../widgets/button';

import {TextInput} from '../widgets/text_input';
import {Icon} from '../widgets/icon';

interface BigTraceSettingsCardAttrs extends m.Attributes {
  id?: string;
  title: string;
  controls: m.Children;
  description?: m.Children;
  disabled?: boolean;
  onChange?: (disabled: boolean) => void;
}

class BigTraceSettingsCard implements m.ClassComponent<BigTraceSettingsCardAttrs> {
  view(vnode: m.Vnode<BigTraceSettingsCardAttrs>) {
    const { id, title, controls, description, disabled, onChange, ...rest } = vnode.attrs;
    return m(
      'div',
      {
        style: {
          opacity: disabled ? 0.6 : 1,
        },
      },
      m(
        Card,
        {
          id,
          className: classNames('pf-settings-card', disabled && 'pf-disabled'),
          ...rest,
        },
        [
          m(
            '.pf-settings-card__details',
            m('.pf-settings-card__title', [
              disabled !== undefined &&
                m(Switch, {
                  className: 'pf-settings-card__toggle',
                  style: { marginRight: '8px' },
                  checked: !disabled,
                  onchange: (e: Event) => {
                    const target = e.target as HTMLInputElement;
                    onChange?.(!target.checked);
                  },
                }),
              title,
              id &&
                m(Anchor, {
                  href: `#!/bigtrace?setting=${id}`,
                  className: 'pf-settings-card__link',
                  icon: 'link',
                  title: 'Link to this setting',
                }),
            ]),
            id && m('.pf-settings-card__id', id),
            description && m('.pf-settings-card__description', description),
          ),
          m(
            '.pf-settings-card__controls',
            {
              style: {
                pointerEvents: disabled ? 'none' : 'auto',
              },
            },
            controls
          ),
        ],
      )
    );
  }
}

export class BigTraceSettingsPage implements m.ClassComponent {
  private searchQuery = '';

  oninit() {
    bigTraceSettingsManager.loadSettings();
  }

  view() {
    const settings = bigTraceSettingsManager.getAllSettings().filter(
      (s) =>
        s.name.toLowerCase().includes(this.searchQuery.toLowerCase()) ||
        s.description.toLowerCase().includes(this.searchQuery.toLowerCase()),
    );
    const categories = new Map<string, Setting<unknown>[]>();

    for (const setting of settings) {
        let categoryName = setting.category || 'General';
        if (categoryName === 'TRACE_ADDRESS') categoryName = 'Trace Address';
        else if (categoryName === 'TRACE_METADATA') categoryName = 'Trace Metadata';
        else if (categoryName === 'BIGTRACE_QUERY_OPTIONS') categoryName = 'Query Options';

        if (!categories.has(categoryName)) {
            categories.set(categoryName, []);
        }
        categories.get(categoryName)!.push(setting);
    }
    
    if (this.searchQuery === '' && !categories.has('Trace Metadata')) {
        categories.set('Trace Metadata', []);
    }


    return m(
        SettingsShell,
        {
            title: 'BigTrace Settings',
            className: 'page',
            stickyHeaderContent: m('.pf-settings-search', [
              m(Icon, {icon: 'search'}),
              m(TextInput, {
                value: this.searchQuery,
                oninput: (e: Event) => {
                  this.searchQuery = (e.target as HTMLInputElement).value;
                },
                placeholder: 'Search settings...',
              }),
            ]),
        },
        m(
            '.pf-settings-page',
            bigTraceSettingsManager.isLoading ?
            m(EmptyState, {
                title: 'Loading settings...',
                icon: 'hourglass_empty',
                fillHeight: true,
                }) :
            bigTraceSettingsManager.loadError ?
            m(Callout, {
                intent: Intent.Danger,
                icon: 'error',
                title: 'Failed to Load Settings',
            }, bigTraceSettingsManager.loadError) :
            Array.from(categories.entries()).map(([category, settings]) => {
                let categoryHeader: m.Children = m('h2', category);
                if (category === 'Trace Metadata') {
                  categoryHeader = m('h2', { style: { display: 'flex', alignItems: 'center', gap: '16px' } }, [
                    m('span', category),
                    (bigTraceSettingsManager.isReloadRequired() && !bigTraceSettingsManager.isMetadataLoading) ? 
                      m(Button, {
                        label: 'Reload',
                        icon: 'refresh',
                        intent: Intent.Primary,
                        variant: ButtonVariant.Filled,
                        onclick: () => bigTraceSettingsManager.reloadMetadataSettings(),
                      }) : null,
                  ]);
                }

                return m('.pf-settings-category',
                    categoryHeader,
                    (settings.length === 0 && category === 'Trace Metadata' && bigTraceSettingsManager.isMetadataLoading) ?
                      m(EmptyState, {
                        title: 'Loading metadata...',
                        icon: 'hourglass_empty',
                      }) :
                      settings.map((setting) => {
                          return this.renderSettingCard(setting);
                      }),
                );
            }),
        ),
    );
  }

  private renderSettingCard(setting: Setting<unknown>) {
    const disabled = setting.isDisabled();
    return m(BigTraceSettingsCard, {
      id: setting.id,
      title: setting.name,
      description: setting.description,
      controls: renderSetting(setting),
      disabled,
      onChange: (newDisabled: boolean) => {
        setting.setDisabled(newDisabled);
      },
    });
  }
}
