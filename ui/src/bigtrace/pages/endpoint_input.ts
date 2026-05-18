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

import m from 'mithril';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {TextInput} from '../../widgets/text_input';
import {Stack} from '../../widgets/stack';
import type {Setting} from '../../public/settings';
import {endpointStorage} from '../settings/endpoint_storage';

export function renderEndpointControl(setting: Setting<unknown>): m.Children {
  const currentValue = setting.get() as string;
  return m(
    Stack,
    {
      orientation: 'horizontal',
      gap: '8px',
      alignItems: 'center',
      className: 'pf-bt-endpoint-row',
    },
    m(TextInput, {
      value: currentValue,
      placeholder: 'https://your-bigtrace-backend/v1',
      className: 'pf-bt-endpoint-input',
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
