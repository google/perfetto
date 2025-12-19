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
import {classNames} from '../../base/classnames';
import {Select} from '../../widgets/select';
import {TextInput} from '../../widgets/text_input';
import {Form, FormLabel} from '../../widgets/form';
import {Switch} from '../../widgets/switch';

export type Options = {
  [key: string]: EnumOption | boolean | string | number;
};

export class EnumOption<T extends readonly string[] = string[]> {
  constructor(
    public initial: T[number],
    public options: T,
  ) {}
}

// Type helper to extract the string union from EnumOption
type ExtractEnumValue<T> = T extends EnumOption<infer U> ? U[number] : T;

// Type helper to transform all properties in Options
type TransformOptions<T extends Options> = {
  [K in keyof T]: ExtractEnumValue<T[K]>;
};

export interface WidgetShowcaseAttrs<T extends Options> {
  readonly renderWidget: (options: TransformOptions<T>) => m.Children;
  readonly initialOpts?: T;

  // Options for styling the widget container
  readonly noPadding?: boolean;
}

/**
 * A Mithril component that showcases a widget with dynamic options. It renders
 * the widget along with controls to modify its options.
 *
 * @template T - A type extending the Options interface, representing the widget
 * options.
 *
 * @param renderWidget - A function that takes the current options and returns
 * the widget to be rendered.
 * @param initialOpts - An optional object defining the initial options for the
 * widget.
 * @param noPadding - An optional boolean to control padding around the widget
 * container.
 *
 * @see renderWidgetShowcase Use the helper function to create an instance of
 * this component as using the component directly can be fiddly to get right
 * with the generics.
 */
class WidgetShowcase<T extends Options>
  implements m.ClassComponent<WidgetShowcaseAttrs<T>>
{
  private options?: Options;
  private optionValues: Record<string, unknown> = {};

  oninit({attrs: {initialOpts: opts}}: m.Vnode<WidgetShowcaseAttrs<T>>) {
    this.options = opts;
    if (opts) {
      // Make the initial options values
      for (const key in opts) {
        if (Object.prototype.hasOwnProperty.call(opts, key)) {
          const option = opts[key];
          if (option instanceof EnumOption) {
            this.optionValues[key] = option.initial;
          } else {
            this.optionValues[key] = option;
          }
        }
      }
    }
  }

  view({attrs}: m.CVnode<WidgetShowcaseAttrs<T>>) {
    const {renderWidget, noPadding} = attrs;
    const formInputs = [];

    if (this.options) {
      for (const key in this.options) {
        if (Object.prototype.hasOwnProperty.call(this.options, key)) {
          formInputs.push(this.renderControlForOption(key));
        }
      }
    }

    return [
      m('.pf-widgets-page__showcase', [
        m(
          '.pf-widgets-page__widget-container',
          {
            className: classNames(
              noPadding && 'pf-widgets-page__widget-container--no-padding',
            ),
          },
          renderWidget(this.optionValues as TransformOptions<T>),
        ),
        m(
          '.pf-widgets-page__options',
          m('.pf-widgets-page__options-title', 'Options'),
          formInputs.length === 0
            ? 'No options available'
            : m(Form, formInputs),
        ),
      ]),
    ];
  }

  private renderControlForOption(key: string) {
    if (!this.options) return null;
    const option = this.options[key];
    const currentValue = this.optionValues[key];
    if (option instanceof EnumOption) {
      return this.renderEnumOption(key, option, currentValue as string);
    } else if (typeof currentValue === 'boolean') {
      return this.renderBooleanOption(key, currentValue);
    } else if (typeof currentValue === 'string') {
      return this.renderStringOption(key, currentValue);
    } else if (typeof currentValue === 'number') {
      return this.renderNumberOption(key, currentValue);
    } else {
      return null;
    }
  }

  private renderBooleanOption(key: string, value: boolean) {
    return m(Switch, {
      checked: value,
      label: key,
      onchange: () => {
        this.optionValues[key] = !Boolean(this.optionValues[key]);
      },
    });
  }

  private renderStringOption(key: string, value: string) {
    return [
      m(FormLabel, {for: key}, key),
      m(TextInput, {
        id: key,
        placeholder: key,
        value: value,
        oninput: (e: Event) => {
          this.optionValues[key] = (e.target as HTMLInputElement).value;
        },
      }),
    ];
  }

  private renderNumberOption(key: string, value: number) {
    return [
      m(FormLabel, {for: key}, key),
      m(TextInput, {
        id: key,
        type: 'number',
        placeholder: key,
        value: value,
        oninput: (e: Event) => {
          this.optionValues[key] = Number.parseInt(
            (e.target as HTMLInputElement).value,
          );
        },
      }),
    ];
  }

  private renderEnumOption(key: string, opt: EnumOption, value: string) {
    const optionElements = opt.options.map((option: string) => {
      return m('option', {value: option}, option);
    });
    return [
      m(FormLabel, {for: key}, key),
      m(
        Select,
        {
          id: key,
          value: value,
          onchange: (e: Event) => {
            const el = e.target as HTMLSelectElement;
            this.optionValues[key] = el.value;
          },
        },
        optionElements,
      ),
    ];
  }
}

export function renderWidgetShowcase<T extends Options = {}>(
  attrs: WidgetShowcaseAttrs<T>,
) {
  return m(WidgetShowcase<T>, attrs);
}

// Helper to render documentation sections
export function renderDocSection(
  title: string,
  content: m.Children,
): m.Children {
  return m('.pf-widget-doc-section', [m('h2', title), content]);
}
