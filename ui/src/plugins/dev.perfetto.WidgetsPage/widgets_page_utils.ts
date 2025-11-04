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
import {isString} from '../../base/object_utils';
import {Checkbox} from '../../widgets/checkbox';
import {Select} from '../../widgets/select';
import {TextInput} from '../../widgets/text_input';

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

export interface WidgetShowcaseAttrs {
  initialOpts?: Options;
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  renderWidget: (options: any) => any;
}

// A little helper class to render any vnode with a dynamic set of options
export class WidgetShowcase implements m.ClassComponent<WidgetShowcaseAttrs> {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  private optValues: any = {};
  private opts?: Options;

  renderOptions(listItems: m.Child[]): m.Child {
    return m(
      '.pf-widget-controls',
      m('.pf-widgets-page__options-title', 'Options'),
      listItems.length === 0
        ? m('.pf-widgets-page__option', 'No options available')
        : m('', listItems),
    );
  }

  oninit({attrs: {initialOpts: opts}}: m.Vnode<WidgetShowcaseAttrs, this>) {
    this.opts = opts;
    if (opts) {
      // Make the initial options values
      for (const key in opts) {
        if (Object.prototype.hasOwnProperty.call(opts, key)) {
          const option = opts[key];
          if (option instanceof EnumOption) {
            this.optValues[key] = option.initial;
          } else {
            this.optValues[key] = option;
          }
        }
      }
    }
  }

  view({attrs}: m.CVnode<WidgetShowcaseAttrs>) {
    const {renderWidget} = attrs;
    const listItems = [];

    if (this.opts) {
      for (const key in this.opts) {
        if (Object.prototype.hasOwnProperty.call(this.opts, key)) {
          listItems.push(
            m('.pf-widgets-page__option', this.renderControlForOption(key)),
          );
        }
      }
    }

    return [
      m(
        '.pf-widget-block',
        m('.pf-widget-container', renderWidget(this.optValues)),
        this.renderOptions(listItems),
      ),
    ];
  }

  private renderControlForOption(key: string) {
    if (!this.opts) return null;
    const value = this.opts[key];
    if (value instanceof EnumOption) {
      return this.renderEnumOption(key, value);
    } else if (typeof value === 'boolean') {
      return this.renderBooleanOption(key);
    } else if (isString(value)) {
      return this.renderStringOption(key);
    } else if (typeof value === 'number') {
      return this.renderNumberOption(key);
    } else {
      return null;
    }
  }

  private renderBooleanOption(key: string) {
    return m(Checkbox, {
      checked: this.optValues[key],
      label: key,
      onchange: () => {
        this.optValues[key] = !Boolean(this.optValues[key]);
      },
    });
  }

  private renderStringOption(key: string) {
    return m(
      'label',
      `${key}:`,
      m(TextInput, {
        placeholder: key,
        value: this.optValues[key],
        oninput: (e: Event) => {
          this.optValues[key] = (e.target as HTMLInputElement).value;
        },
      }),
    );
  }

  private renderNumberOption(key: string) {
    return m(
      'label',
      `${key}:`,
      m(TextInput, {
        type: 'number',
        placeholder: key,
        value: this.optValues[key],
        oninput: (e: Event) => {
          this.optValues[key] = Number.parseInt(
            (e.target as HTMLInputElement).value,
          );
        },
      }),
    );
  }

  private renderEnumOption(key: string, opt: EnumOption) {
    const optionElements = opt.options.map((option: string) => {
      return m('option', {value: option}, option);
    });
    return m(
      'label',
      `${key}: `,
      m(
        Select,
        {
          value: this.optValues[key],
          onchange: (e: Event) => {
            const el = e.target as HTMLSelectElement;
            this.optValues[key] = el.value;
          },
        },
        optionElements,
      ),
    );
  }
}

export function renderWidgetShowcase<T extends Options = {}>(attrs: {
  renderWidget(opts: TransformOptions<T>): m.Children;
  initialOpts?: T;
}) {
  return m(WidgetShowcase, attrs);
}

// Helper to render documentation sections
export function renderDocSection(
  title: string,
  content: m.Children,
): m.Children {
  return m('.pf-widget-doc-section', [m('h2', title), content]);
}
