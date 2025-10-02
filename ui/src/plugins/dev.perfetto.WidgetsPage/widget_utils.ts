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
import {isString} from '../../base/object_utils';
import {Anchor} from '../../widgets/anchor';
import {Checkbox} from '../../widgets/checkbox';
import {Select} from '../../widgets/select';
import {TextInput} from '../../widgets/text_input';

export type Options = {
  [key: string]: EnumOption | boolean | string | number;
};

export class EnumOption {
  constructor(
    public initial: string,
    public options: string[],
  ) {}
}

interface WidgetTitleAttrs {
  label: string;
}

class WidgetTitle implements m.ClassComponent<WidgetTitleAttrs> {
  view({attrs}: m.CVnode<WidgetTitleAttrs>) {
    const {label} = attrs;
    const id = label.replaceAll(' ', '').toLowerCase();
    const href = `#!/widgets#${id}`;
    return m(Anchor, {id, href}, m('h2', label));
  }
}

export interface WidgetShowcaseAttrs {
  label: string;
  description?: string;
  initialOpts?: Options;
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  renderWidget: (options: any) => any;
  wide?: boolean;
}

// A little helper class to render any vnode with a dynamic set of options
export class WidgetShowcase implements m.ClassComponent<WidgetShowcaseAttrs> {
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  private optValues: any = {};
  private opts?: Options;

  renderOptions(listItems: m.Child[]): m.Child {
    if (listItems.length === 0) {
      return null;
    }
    return m('.pf-widget-controls', m('h3', 'Options'), m('ul', listItems));
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
    const {renderWidget, wide, label, description} = attrs;
    const listItems = [];

    if (this.opts) {
      for (const key in this.opts) {
        if (Object.prototype.hasOwnProperty.call(this.opts, key)) {
          listItems.push(m('li', this.renderControlForOption(key)));
        }
      }
    }

    return [
      m(WidgetTitle, {label}),
      description && m('p', description),
      m(
        '.pf-widget-block',
        m(
          'div',
          {
            class: classNames(
              'pf-widget-container',
              wide && 'pf-widget-container--wide',
            ),
          },
          renderWidget(this.optValues),
        ),
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
      `${key}:`,
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
  label: string;
  description?: string;
  renderWidget(opts: T): m.Children;
  initialOpts?: T;
  wide?: boolean;
}) {
  return m(WidgetShowcase, attrs);
}
