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

import {EmptyState} from '../../widgets/empty_state';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';
import m from 'mithril';
import {SettingsShell} from '../../widgets/settings_shell';
import {Switch} from '../../widgets/switch';
import {Card, CardStack} from '../../widgets/card';
import {classNames} from '../../base/classnames';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import {Setting as BigTraceSetting} from '../settings/settings_types';
import {renderSetting} from '../settings/settings_widgets';
import {Button, ButtonVariant} from '../../widgets/button';

import {endpointStorage} from '../settings/endpoint_storage';
import {Setting} from '../../public/settings';

import {TextInput} from '../../widgets/text_input';
import {Stack, StackAuto} from '../../widgets/stack';

interface BigTraceSettingsCardAttrs extends m.Attributes {
  id?: string;
  title: string;
  controls: m.Children;
  description?: m.Children;
  disabled?: boolean;
  onChange?: (disabled: boolean) => void;
  fullWidthControls?: boolean;
}

class BigTraceSettingsCard
  implements m.ClassComponent<BigTraceSettingsCardAttrs>
{
  view(vnode: m.Vnode<BigTraceSettingsCardAttrs>) {
    const {
      id,
      title,
      controls,
      description,
      disabled,
      onChange,
      fullWidthControls,
      ...rest
    } = vnode.attrs;

    const details = m(
      '.pf-settings-card__details',
      m('.pf-settings-card__title', [
        disabled !== undefined &&
          m(Switch, {
            className: 'pf-settings-card__toggle',
            style: {marginRight: '8px'},
            checked: !disabled,
            onchange: (e: Event) => {
              const target = e.target as HTMLInputElement;
              onChange?.(!target.checked);
            },
          }),
        title,
      ]),
      id && m('.pf-settings-card__id', id),
      description !== undefined &&
        m('.pf-settings-card__description', description),
    );

    const controlsEl = m(
      '.pf-settings-card__controls',
      {
        className: classNames(
          disabled !== undefined &&
            disabled &&
            'pf-bt-settings-controls--disabled',
        ),
        style: fullWidthControls
          ? {gridColumn: '1 / -1', minWidth: '0'}
          : undefined,
      },
      controls,
    );

    return m(
      'div',
      {
        className: classNames(
          disabled && 'pf-bt-settings-card-wrapper--disabled',
        ),
      },
      m(
        Card,
        {
          id,
          className: classNames('pf-settings-card', disabled && 'pf-disabled'),
          ...rest,
        },
        [details, controlsEl],
      ),
    );
  }
}

export class SettingsPage implements m.ClassComponent {
  private searchQuery = '';

  oninit() {
    bigTraceSettingsStorage.loadSettings();
  }

  private static readonly CATEGORY_DISPLAY_NAMES: ReadonlyMap<string, string> =
    new Map([
      ['General', 'General'],
      ['TRACE_ADDRESS', 'Trace Address'],
      ['TRACE_METADATA', 'Trace Metadata'],
      ['BIGTRACE_QUERY_OPTIONS', 'Query Options'],
    ]);

  private displayCategory(raw: string): string {
    return SettingsPage.CATEGORY_DISPLAY_NAMES.get(raw) ?? raw;
  }

  view() {
    const endpointSetting = endpointStorage.get('bigtraceEndpoint');

    const query = this.searchQuery.toLowerCase();
    const categories = new Map<string, BigTraceSetting<unknown>[]>();

    // Always show the General section so the endpoint is accessible.
    if (endpointSetting) {
      categories.set('General', []);
    }

    const settings = bigTraceSettingsStorage
      .getAllSettings()
      .filter(
        (s) =>
          s.name.toLowerCase().includes(query) ||
          s.description.toLowerCase().includes(query),
      );

    for (const setting of settings) {
      const categoryName = this.displayCategory(setting.category || 'General');
      if (!categories.has(categoryName)) {
        categories.set(categoryName, []);
      }
      categories.get(categoryName)!.push(setting);
    }

    if (
      this.searchQuery === '' &&
      !categories.has('Trace Metadata') &&
      !bigTraceSettingsStorage.execConfigLoadError
    ) {
      categories.set('Trace Metadata', []);
    }

    return m(
      SettingsShell,
      {
        title: 'Settings',
        className: 'page',
        stickyHeaderContent: m(
          Stack,
          {orientation: 'horizontal'},
          endpointStorage.isReloadRequired() &&
            m(Button, {
              label: 'Reload required',
              icon: 'refresh',
              intent: Intent.Primary,
              variant: ButtonVariant.Filled,
              onclick: () => {
                window.location.reload();
              },
            }),
          m(StackAuto),
          m(TextInput, {
            placeholder: 'Search...',
            value: this.searchQuery,
            leftIcon: 'search',
            oninput: (e: Event) => {
              this.searchQuery = (e.target as HTMLInputElement).value;
            },
          }),
        ),
      },
      m('.pf-settings-page', [
        bigTraceSettingsStorage.isExecConfigLoading &&
          m(EmptyState, {
            title: 'Loading settings...',
            icon: 'hourglass_empty',
            fillHeight: true,
          }),
        bigTraceSettingsStorage.execConfigLoadError &&
          m(
            Callout,
            {
              intent: Intent.Danger,
              icon: 'error',
              title: 'Failed to Load Execution Configuration',
            },
            bigTraceSettingsStorage.execConfigLoadError,
          ),
        Array.from(categories.entries()).map(([category, catSettings]) => {
          let categoryHeader: m.Children = m(
            'h2.pf-settings-page__plugin-title',
            category,
          );
          if (category === 'Trace Metadata') {
            categoryHeader = m(
              'h2.pf-settings-page__plugin-title.pf-bt-settings-category-header',
              [
                m('span', category),
                bigTraceSettingsStorage.isReloadRequired() &&
                !bigTraceSettingsStorage.isMetadataLoading
                  ? m(Button, {
                      label: 'Reload',
                      icon: 'refresh',
                      intent: Intent.Primary,
                      variant: ButtonVariant.Filled,
                      onclick: () =>
                        bigTraceSettingsStorage.reloadMetadataSettings(),
                    })
                  : null,
              ],
            );
          }

          let categoryContent;
          if (
            category === 'Trace Metadata' &&
            bigTraceSettingsStorage.isMetadataLoading
          ) {
            categoryContent = m(EmptyState, {
              title: 'Loading metadata...',
              icon: 'hourglass_empty',
            });
          } else if (
            category === 'Trace Metadata' &&
            bigTraceSettingsStorage.metadataLoadError
          ) {
            categoryContent = m(
              Callout,
              {
                intent: Intent.Danger,
                icon: 'error',
                title: 'Failed to Load Trace Metadata',
              },
              bigTraceSettingsStorage.metadataLoadError,
            );
          } else {
            const cards: m.Children[] = [];
            // Render the endpoint card inside "General".
            if (category === 'General' && endpointSetting) {
              cards.push(
                m(BigTraceSettingsCard, {
                  id: endpointSetting.id,
                  title: endpointSetting.name,
                  description: endpointSetting.description,
                  disabled: undefined,
                  controls: this.renderEndpointControl(endpointSetting),
                }),
              );
            }
            for (const setting of catSettings) {
              cards.push(this.renderBigTraceSettingCard(setting));
            }
            categoryContent = m(CardStack, cards);
          }

          return m(
            '.pf-settings-page__plugin-section',
            categoryHeader,
            categoryContent,
          );
        }),
      ]),
    );
  }

  private renderEndpointControl(setting: Setting<unknown>) {
    const currentValue = setting.get() as string;
    return m(TextInput, {
      value: currentValue,
      style: {width: 'min(300px, 30vw)'},
      oninput: (e: Event) => {
        const target = e.target as HTMLInputElement;
        setting.set(target.value);
      },
    });
  }

  private renderBigTraceSettingCard(setting: BigTraceSetting<unknown>) {
    const disabled = setting.isDisabled();
    const fullWidth =
      setting.type === 'string-array' ||
      (setting.type === 'string' && setting.format === 'sql');
    return m(BigTraceSettingsCard, {
      id: setting.id,
      title: setting.name,
      description: setting.description,
      controls: renderSetting(setting),
      disabled,
      fullWidthControls: fullWidth,
      onChange: (newDisabled: boolean) => {
        setting.setDisabled(newDisabled);
      },
    });
  }
}
