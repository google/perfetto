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
import {Card} from '../../widgets/card';
import {Icon} from '../../widgets/icon';
import {Switch} from '../../widgets/switch';
import {classNames} from '../../base/classnames';
import type {Setting as BigTraceSetting} from '../settings/settings_types';
import {renderSetting} from '../settings/settings_widgets';

export interface BigTraceSettingsCardAttrs extends m.Attributes {
  id?: string;
  title: string;
  controls: m.Children;
  description?: m.Children;
  disabled?: boolean;
  onChange?: (disabled: boolean) => void;
  fullWidthControls?: boolean;
}

export class BigTraceSettingsCard
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
            className: 'pf-settings-card__toggle pf-bt-settings-toggle',
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

export function renderBigTraceSettingCard(
  setting: BigTraceSetting<unknown>,
): m.Children {
  const disabled = setting.isDisabled();
  const fullWidth =
    setting.type === 'string-array' ||
    (setting.type === 'string' && setting.format === 'sql');
  // Flag enabled-but-empty filters upfront. Numeric settings are
  // excluded because 0 is legit (= unlimited).
  const needsValue =
    !disabled && (setting.type === 'string' || setting.type === 'string-array');
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
          '.pf-settings-card__warning.pf-bt-settings-warning',
          m(Icon, {
            icon: 'warning',
            className: 'pf-bt-settings-warning-icon',
          }),
          ' ',
          warning,
        ),
      ]
    : hint
      ? [
          setting.description,
          ' ',
          m('span.pf-settings-card__hint.pf-bt-settings-hint', hint),
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
