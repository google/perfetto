// Copyright (C) 2019 The Android Open Source Project
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

import {Draft, produce} from 'immer';
import * as m from 'mithril';

import {assertExists} from '../base/logging';
import {Actions} from '../common/actions';
import {RecordConfig} from '../controller/record_config_types';

import {copyToClipboard} from './clipboard';
import {globals} from './globals';

declare type Setter<T> = (draft: Draft<RecordConfig>, val: T) => void;
declare type Getter<T> = (cfg: RecordConfig) => T;

function defaultSort(a: string, b: string) {
  return a.localeCompare(b);
}

// +---------------------------------------------------------------------------+
// | Docs link with 'i' in circle icon.                                        |
// +---------------------------------------------------------------------------+

interface DocsChipAttrs {
  href: string;
}

class DocsChip implements m.ClassComponent<DocsChipAttrs> {
  view({attrs}: m.CVnode<DocsChipAttrs>) {
    return m(
        'a.inline-chip',
        {href: attrs.href, title: 'Open docs in new tab', target: '_blank'},
        m('i.material-icons', 'info'),
        ' Docs');
  }
}

// +---------------------------------------------------------------------------+
// | Probe: the rectangular box on the right-hand-side with a toggle box.      |
// +---------------------------------------------------------------------------+

export interface ProbeAttrs {
  title: string;
  img: string|null;
  compact?: boolean;
  descr: m.Children;
  isEnabled: Getter<boolean>;
  setEnabled: Setter<boolean>;
}

export class Probe implements m.ClassComponent<ProbeAttrs> {
  view({attrs, children}: m.CVnode<ProbeAttrs>) {
    const onToggle = (enabled: boolean) => {
      const traceCfg = produce(globals.state.recordConfig, (draft) => {
        attrs.setEnabled(draft, enabled);
      });
      globals.dispatch(Actions.setRecordConfig({config: traceCfg}));
    };

    const enabled = attrs.isEnabled(globals.state.recordConfig);

    return m(
        `.probe${attrs.compact ? '.compact' : ''}${enabled ? '.enabled' : ''}`,
        attrs.img && m('img', {
          src: `${globals.root}assets/${attrs.img}`,
          onclick: () => onToggle(!enabled),
        }),
        m('label',
          m(`input[type=checkbox]`, {
            checked: enabled,
            oninput: (e: InputEvent) => {
              onToggle((e.target as HTMLInputElement).checked);
            },
          }),
          m('span', attrs.title)),
        attrs.compact ?
            '' :
            m('div', m('div', attrs.descr), m('.probe-config', children)));
  }
}

export function CompactProbe(args: {
  title: string,
  isEnabled: Getter<boolean>,
  setEnabled: Setter<boolean>
}) {
  return m(Probe, {
    title: args.title,
    img: null,
    compact: true,
    descr: '',
    isEnabled: args.isEnabled,
    setEnabled: args.setEnabled,
  } as ProbeAttrs);
}

// +-------------------------------------------------------------+
// | Toggle: an on/off switch.
// +-------------------------------------------------------------+

export interface ToggleAttrs {
  title: string;
  descr: string;
  cssClass?: string;
  isEnabled: Getter<boolean>;
  setEnabled: Setter<boolean>;
}

export class Toggle implements m.ClassComponent<ToggleAttrs> {
  view({attrs}: m.CVnode<ToggleAttrs>) {
    const onToggle = (enabled: boolean) => {
      const traceCfg = produce(globals.state.recordConfig, (draft) => {
        attrs.setEnabled(draft, enabled);
      });
      globals.dispatch(Actions.setRecordConfig({config: traceCfg}));
    };

    const enabled = attrs.isEnabled(globals.state.recordConfig);

    return m(
        `.toggle${enabled ? '.enabled' : ''}${attrs.cssClass || ''}`,
        m('label',
          m(`input[type=checkbox]`, {
            checked: enabled,
            oninput: (e: InputEvent) => {
              onToggle((e.target as HTMLInputElement).checked);
            },
          }),
          m('span', attrs.title)),
        m('.descr', attrs.descr));
  }
}

// +---------------------------------------------------------------------------+
// | Slider: draggable horizontal slider with numeric spinner.                 |
// +---------------------------------------------------------------------------+

export interface SliderAttrs {
  title: string;
  icon?: string;
  cssClass?: string;
  isTime?: boolean;
  unit: string;
  values: number[];
  get: Getter<number>;
  set: Setter<number>;
  min?: number;
  description?: string;
  disabled?: boolean;
  zeroIsDefault?: boolean;
}

export class Slider implements m.ClassComponent<SliderAttrs> {
  onValueChange(attrs: SliderAttrs, newVal: number) {
    const traceCfg = produce(globals.state.recordConfig, (draft) => {
      attrs.set(draft, newVal);
    });
    globals.dispatch(Actions.setRecordConfig({config: traceCfg}));
  }

  onTimeValueChange(attrs: SliderAttrs, hms: string) {
    try {
      const date = new Date(`1970-01-01T${hms}.000Z`);
      if (isNaN(date.getTime())) return;
      this.onValueChange(attrs, date.getTime());
    } catch {
    }
  }

  onSliderChange(attrs: SliderAttrs, newIdx: number) {
    this.onValueChange(attrs, attrs.values[newIdx]);
  }

  view({attrs}: m.CVnode<SliderAttrs>) {
    const id = attrs.title.replace(/[^a-z0-9]/gmi, '_').toLowerCase();
    const maxIdx = attrs.values.length - 1;
    const val = attrs.get(globals.state.recordConfig);
    let min = attrs.min || 1;
    if (attrs.zeroIsDefault) {
      min = Math.min(0, min);
    }
    const description = attrs.description;
    const disabled = attrs.disabled;

    // Find the index of the closest value in the slider.
    let idx = 0;
    for (; idx < attrs.values.length && attrs.values[idx] < val; idx++) {
    }

    let spinnerCfg = {};
    if (attrs.isTime) {
      spinnerCfg = {
        type: 'text',
        pattern: '(0[0-9]|1[0-9]|2[0-3])(:[0-5][0-9]){2}',  // hh:mm:ss
        value: new Date(val).toISOString().substr(11, 8),
        oninput: (e: InputEvent) => {
          this.onTimeValueChange(attrs, (e.target as HTMLInputElement).value);
        },
      };
    } else {
      const isDefault = attrs.zeroIsDefault && val === 0;
      spinnerCfg = {
        type: 'number',
        value: isDefault ? '' : val,
        placeholder: isDefault ? '(default)' : '',
        oninput: (e: InputEvent) => {
          this.onValueChange(attrs, +(e.target as HTMLInputElement).value);
        },
      };
    }
    return m(
        '.slider' + (attrs.cssClass || ''),
        m('header', attrs.title),
        description ? m('header.descr', attrs.description) : '',
        attrs.icon !== undefined ? m('i.material-icons', attrs.icon) : [],
        m(`input[id="${id}"][type=range][min=0][max=${maxIdx}][value=${idx}]`, {
          disabled,
          oninput: (e: InputEvent) => {
            this.onSliderChange(attrs, +(e.target as HTMLInputElement).value);
          },
        }),
        m(`input.spinner[min=${min}][for=${id}]`, spinnerCfg),
        m('.unit', attrs.unit));
  }
}

// +---------------------------------------------------------------------------+
// | Dropdown: wrapper around <select>. Supports single an multiple selection. |
// +---------------------------------------------------------------------------+

export interface DropdownAttrs {
  title: string;
  cssClass?: string;
  options: Map<string, string>;
  sort?: (a: string, b: string) => number;
  get: Getter<string[]>;
  set: Setter<string[]>;
}

export class Dropdown implements m.ClassComponent<DropdownAttrs> {
  resetScroll(dom: HTMLSelectElement) {
    // Chrome seems to override the scroll offset on creationa, b without this,
    // even though we call it after having marked the options as selected.
    setTimeout(() => {
      // Don't reset the scroll position if the element is still focused.
      if (dom !== document.activeElement) dom.scrollTop = 0;
    }, 0);
  }

  onChange(attrs: DropdownAttrs, e: Event) {
    const dom = e.target as HTMLSelectElement;
    const selKeys: string[] = [];
    for (let i = 0; i < dom.selectedOptions.length; i++) {
      const item = assertExists(dom.selectedOptions.item(i));
      selKeys.push(item.value);
    }
    const traceCfg = produce(globals.state.recordConfig, (draft) => {
      attrs.set(draft, selKeys);
    });
    globals.dispatch(Actions.setRecordConfig({config: traceCfg}));
  }

  view({attrs}: m.CVnode<DropdownAttrs>) {
    const options: m.Children = [];
    const selItems = attrs.get(globals.state.recordConfig);
    let numSelected = 0;
    const entries = [...attrs.options.entries()];
    const f = attrs.sort === undefined ? defaultSort : attrs.sort;
    entries.sort((a, b) => f(a[1], b[1]));
    for (const [key, label] of entries) {
      const opts = {value: key, selected: false};
      if (selItems.includes(key)) {
        opts.selected = true;
        numSelected++;
      }
      options.push(m('option', opts, label));
    }
    const label = `${attrs.title} ${numSelected ? `(${numSelected})` : ''}`;
    return m(
        `select.dropdown${attrs.cssClass || ''}[multiple=multiple]`,
        {
          onblur: (e: Event) => this.resetScroll(e.target as HTMLSelectElement),
          onmouseleave: (e: Event) =>
              this.resetScroll(e.target as HTMLSelectElement),
          oninput: (e: Event) => this.onChange(attrs, e),
          oncreate: (vnode) => this.resetScroll(vnode.dom as HTMLSelectElement),
        },
        m('optgroup', {label}, options));
  }
}


// +---------------------------------------------------------------------------+
// | Textarea: wrapper around <textarea>.                                      |
// +---------------------------------------------------------------------------+

export interface TextareaAttrs {
  placeholder: string;
  docsLink?: string;
  cssClass?: string;
  get: Getter<string>;
  set: Setter<string>;
  title?: string;
}

export class Textarea implements m.ClassComponent<TextareaAttrs> {
  onChange(attrs: TextareaAttrs, dom: HTMLTextAreaElement) {
    const traceCfg = produce(globals.state.recordConfig, (draft) => {
      attrs.set(draft, dom.value);
    });
    globals.dispatch(Actions.setRecordConfig({config: traceCfg}));
  }

  view({attrs}: m.CVnode<TextareaAttrs>) {
    return m(
        '.textarea-holder',
        m('header',
          attrs.title,
          attrs.docsLink && [' ', m(DocsChip, {href: attrs.docsLink})]),
        m(`textarea.extra-input${attrs.cssClass || ''}`, {
          onchange: (e: Event) =>
              this.onChange(attrs, e.target as HTMLTextAreaElement),
          placeholder: attrs.placeholder,
          value: attrs.get(globals.state.recordConfig),
        }));
  }
}

// +---------------------------------------------------------------------------+
// | CodeSnippet: command-prompt-like box with code snippets to copy/paste.    |
// +---------------------------------------------------------------------------+

export interface CodeSnippetAttrs {
  text: string;
  hardWhitespace?: boolean;
}

export class CodeSnippet implements m.ClassComponent<CodeSnippetAttrs> {
  view({attrs}: m.CVnode<CodeSnippetAttrs>) {
    return m(
        '.code-snippet',
        m('button',
          {
            title: 'Copy to clipboard',
            onclick: () => copyToClipboard(attrs.text),
          },
          m('i.material-icons', 'assignment')),
        m('code', attrs.text),
    );
  }
}


interface CategoriesCheckboxListParams {
  categories: Map<string, string>;
  title: string;
  get: Getter<string[]>;
  set: Setter<string[]>;
}

export class CategoriesCheckboxList implements
    m.ClassComponent<CategoriesCheckboxListParams> {
  updateValue(
      attrs: CategoriesCheckboxListParams, value: string, enabled: boolean) {
    const traceCfg = produce(globals.state.recordConfig, (draft) => {
      const values = attrs.get(draft);
      const index = values.indexOf(value);
      if (enabled && index === -1) {
        values.push(value);
      }
      if (!enabled && index !== -1) {
        values.splice(index, 1);
      }
    });
    globals.dispatch(Actions.setRecordConfig({config: traceCfg}));
  }

  view({attrs}: m.CVnode<CategoriesCheckboxListParams>) {
    const enabled = new Set(attrs.get(globals.state.recordConfig));
    return m(
        '.categories-list',
        m('h3',
          attrs.title,
          m('button.config-button',
            {
              onclick: () => {
                const config = produce(globals.state.recordConfig, (draft) => {
                  attrs.set(draft, Array.from(attrs.categories.keys()));
                });
                globals.dispatch(Actions.setRecordConfig({config}));
              },
            },
            'All'),
          m('button.config-button',
            {
              onclick: () => {
                const config = produce(globals.state.recordConfig, (draft) => {
                  attrs.set(draft, []);
                });
                globals.dispatch(Actions.setRecordConfig({config}));
              },
            },
            'None')),
        m('ul.checkboxes',
          Array.from(attrs.categories.entries()).map(([key, value]) => {
            const id = `category-checkbox-${key}`;
            return m(
                'label',
                {'for': id},
                m('li',
                  m('input[type=checkbox]', {
                    id,
                    checked: enabled.has(key),
                    onclick: (e: InputEvent) => {
                      const target = e.target as HTMLInputElement;
                      this.updateValue(attrs, key, target.checked);
                    },
                  }),
                  value));
          })));
  }
}
