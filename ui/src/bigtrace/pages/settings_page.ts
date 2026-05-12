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
import {Icon} from '../../widgets/icon';
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
            title:
              'Turn off to skip this filter — its value will not be ' +
              'sent to the backend with subsequent queries.',
            onchange: (e: Event) => {
              const target = e.target as HTMLInputElement;
              onChange?.(!target.checked);
            },
          }),
        title,
      ]),
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

    // Show a "no matches" hint when search hides everything except
    // the always-shown General card.
    const hasOtherMatches = settings.length > 0;
    const showNoMatchesHint =
      this.searchQuery !== '' &&
      !hasOtherMatches &&
      !bigTraceSettingsStorage.execConfigLoadError;

    // Only force-create the Trace Metadata section while loading or on error;
    // an empty metadata response collapses the section entirely.
    if (
      this.searchQuery === '' &&
      !categories.has('Trace Metadata') &&
      !bigTraceSettingsStorage.execConfigLoadError &&
      (bigTraceSettingsStorage.isMetadataLoading ||
        bigTraceSettingsStorage.metadataLoadError)
    ) {
      categories.set('Trace Metadata', []);
    }

    return m(
      SettingsShell,
      {
        title: 'Settings',
        className: 'page',
        // Reload-required affordance lives next to the endpoint
        // input (renderEndpointControl), not in the header.
        stickyHeaderContent: m(
          Stack,
          {orientation: 'horizontal'},
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
        // After the General card, so the callout's "Set the
        // Endpoint above" copy points at a field above it.
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
        showNoMatchesHint &&
          m(EmptyState, {
            title: `No settings match "${this.searchQuery}"`,
            icon: 'search_off',
          }),
      ]),
    );
  }

  private renderEndpointControl(setting: Setting<unknown>) {
    const currentValue = setting.get() as string;
    return m(
      Stack,
      {
        orientation: 'horizontal',
        gap: '8px',
        alignItems: 'center',
        style: {flexWrap: 'wrap', justifyContent: 'flex-end'},
      },
      m(TextInput, {
        value: currentValue,
        placeholder: 'https://your-bigtrace-backend/v1',
        style: {width: 'min(300px, 30vw)'},
        oninput: (e: Event) => {
          const target = e.target as HTMLInputElement;
          setting.set(target.value);
        },
      }),
      // Endpoint is cached at module init; force a reload to apply
      // changes.
      endpointStorage.isReloadRequired() &&
        m(Button, {
          label: 'Reload to apply',
          icon: 'refresh',
          intent: Intent.Primary,
          variant: ButtonVariant.Filled,
          onclick: () => window.location.reload(),
        }),
    );
  }

  private renderBigTraceSettingCard(setting: BigTraceSetting<unknown>) {
    const disabled = setting.isDisabled();
    const fullWidth =
      setting.type === 'string-array' ||
      (setting.type === 'string' && setting.format === 'sql');
    // Flag enabled-but-empty filters upfront. Numeric settings are
    // excluded because 0 is legit (= unlimited).
    const needsValue =
      !disabled &&
      (setting.type === 'string' || setting.type === 'string-array');
    let warning: string | undefined;
    if (needsValue) {
      const value = setting.get();
      if (setting.type === 'string') {
        if (typeof value === 'string' && value.trim() === '') {
          warning = 'Required when this filter is enabled.';
        }
      } else if (setting.type === 'string-array') {
        if (
          !Array.isArray(value) ||
          value.length === 0 ||
          value.every((v) => typeof v === 'string' && v.trim() === '')
        ) {
          warning = 'Required when this filter is enabled.';
        }
      }
    }
    // "(unlimited)" hint on numeric settings whose description says
    // "ignored if 0" — works for any setting following the convention.
    let hint: string | undefined;
    if (
      !disabled &&
      setting.type === 'number' &&
      setting.get() === 0 &&
      /ignored if 0/i.test(setting.description)
    ) {
      hint = '(unlimited)';
    }
    const description: m.Children = warning
      ? [
          setting.description,
          m(
            '.pf-settings-card__warning',
            {
              style: {
                color: 'var(--pf-color-danger, #b00020)',
                marginTop: '4px',
              },
            },
            m(Icon, {
              icon: 'warning',
              style: {fontSize: '14px', verticalAlign: 'middle'},
            }),
            ' ',
            warning,
          ),
        ]
      : hint
        ? [
            setting.description,
            ' ',
            m(
              'span.pf-settings-card__hint',
              {style: {opacity: 0.7, fontStyle: 'italic'}},
              hint,
            ),
          ]
        : setting.description;
    return m(BigTraceSettingsCard, {
      id: setting.id,
      title: setting.name,
      description,
      controls: renderSetting(setting),
      disabled,
      fullWidthControls: fullWidth,
      onChange: (newDisabled: boolean) => {
        setting.setDisabled(newDisabled);
      },
    });
  }
}
