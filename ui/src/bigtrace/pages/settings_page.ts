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
import {CardStack} from '../../widgets/card';
import {bigTraceSettingsStorage} from '../settings/bigtrace_settings_storage';
import type {Setting as BigTraceSetting} from '../settings/settings_types';
import {Button, ButtonVariant} from '../../widgets/button';

import {endpointStorage} from '../settings/endpoint_storage';

import {TextInput} from '../../widgets/text_input';
import {Stack, StackAuto} from '../../widgets/stack';

import {BigTraceSettingsCard, renderBigTraceSettingCard} from './settings_card';
import {renderEndpointControl} from './endpoint_input';

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
                  controls: renderEndpointControl(endpointSetting),
                }),
              );
            }
            for (const setting of catSettings) {
              cards.push(renderBigTraceSettingCard(setting));
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
}
