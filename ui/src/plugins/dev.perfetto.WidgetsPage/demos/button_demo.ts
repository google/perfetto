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
import {Intent} from '../../../widgets/common';
import {
  enumOption,
  InferOptionTypes,
  renderWidgetContainer,
} from '../widget_container';

const optionsSchema = {
  label: true,
  icon: true,
  rightIcon: false,
  disabled: false,
  intent: enumOption(Intent.None, Object.values(Intent)),
  active: false,
  compact: false,
  loading: false,
  variant: enumOption(ButtonVariant.Filled, Object.values(ButtonVariant)),
  showAsGrid: false,
  showInlineWithText: false,
  rounded: false,
};

export class ButtonDemo implements m.ClassComponent {
  view() {
    return renderWidgetContainer({
      label: 'Button',
      schema: optionsSchema,
      render: (opts) =>
        opts.showAsGrid ? this.renderGrid(opts) : this.renderSimple(opts),
    });
  }

  private renderSimple({
    showInlineWithText,
    icon,
    rightIcon,
    label,
    intent,
    variant,
    ...rest
  }: InferOptionTypes<typeof optionsSchema>) {
    return m('', [
      Boolean(showInlineWithText) && 'Inline ',
      m(Button, {
        label: (label ? 'Button' : undefined) as string,
        icon: icon ? 'start' : undefined,
        rightIcon: rightIcon ? 'arrow_forward' : undefined,
        intent: intent as Intent,
        variant: variant as ButtonVariant,
        ...rest,
      }),
      Boolean(showInlineWithText) && ' text',
    ]);
  }

  private renderGrid({
    showInlineWithText,
    icon,
    rightIcon,
    label,
    intent,
    variant,
    ...rest
  }: InferOptionTypes<typeof optionsSchema>) {
    return m(
      '',
      {
        style: {
          display: 'grid',
          gridTemplateColumns: 'auto auto auto',
          gap: '4px',
        },
      },
      Object.values(Intent).map((intent) => {
        return Object.values(ButtonVariant).map((variant) => {
          return m(Button, {
            style: {
              width: '80px',
            },
            ...rest,
            label: variant,
            variant,
            intent,
          });
        });
      }),
    );
  }
}
