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
import {Anchor} from '../../widgets/anchor';
import {classNames} from '../../base/classnames';
import {Checkbox} from '../../widgets/checkbox';
import {TextInput} from '../../widgets/text_input';
import {Select} from '../../widgets/select';

// --- TYPE DEFINITIONS ---
interface EnumOption {
  kind: 'enum';
  values: string[];
  defaultValue: string;
}

export function enumOption(defaultValue: string, values: string[]): EnumOption {
  return {
    kind: 'enum',
    values,
    defaultValue,
  };
}

type OptionsSchema = {
  [key: string]: EnumOption | boolean | string | number;
};

export type InferOptionTypes<T extends OptionsSchema> = {
  [K in keyof T]: T[K] extends EnumOption ? T[K]['values'][number] : T[K];
};

// --- ATTRS INTERFACE ---
interface WidgetContainerAttrs<T extends OptionsSchema> {
  label: string;
  description?: string;
  wide?: boolean;
  schema: T;
  render: (options: InferOptionTypes<T>) => m.Children;
}

// --- THE HIDDEN COMPONENT ---
// This is not exported, it's an implementation detail of renderWidgetContainer
function WidgetContainer<T extends OptionsSchema>(): m.Component<
  WidgetContainerAttrs<T>
> {
  let options: InferOptionTypes<T>;

  function getDefaults(schema: T): InferOptionTypes<T> {
    const defaults: {[key: string]: any} = {};
    for (const key in schema) {
      if (Object.prototype.hasOwnProperty.call(schema, key)) {
        const value = schema[key];
        if (
          typeof value === 'object' &&
          value !== null &&
          'kind' in value &&
          (value as any).kind === 'enum'
        ) {
          defaults[key] = (value as EnumOption).defaultValue;
        } else {
          defaults[key] = value;
        }
      }
    }
    return defaults as InferOptionTypes<T>;
  }

  // This onchange function is strictly typed.
  function onchange<K extends keyof T>(key: K, value: InferOptionTypes<T>[K]) {
    options[key] = value;
  }

  return {
    oninit: ({attrs}) => {
      options = getDefaults(attrs.schema);
    },
    view: ({attrs}) => {
      const {label, description, wide, schema, render} = attrs;
      const id = label.replaceAll(' ', '').toLowerCase();
      const href = `#!/widgets#${id}`;

      return m('', [
        m(Anchor, {id, href}, m('h2', label)),
        description && m('p', description),
        m(
          '.pf-widget-block',
          m(
            '',
            {
              class: classNames(
                'pf-widget-container',
                wide && 'pf-widget-container--wide',
              ),
            },
            // `options` is strictly typed here, so `render` is called correctly.
            render(options),
          ),
          renderOptions(schema, options, onchange),
        ),
      ]);
    },
  };
}

// --- THE EXPORTED FUNCTION ---
// This is the public API, matching the original function's purpose.
export function renderWidgetContainer<T extends OptionsSchema>(
  args: WidgetContainerAttrs<T>,
): m.Children {
  return m(WidgetContainer<T>, args);
}

// --- RENDER OPTIONS HELPER ---
function renderOptions<T extends OptionsSchema>(
  schema: T,
  values: InferOptionTypes<T>,
  // It receives the strictly typed onchange function.
  onchange: <K extends keyof T>(key: K, value: InferOptionTypes<T>[K]) => void,
) {
  const listItems = [];
  // Iterate over keys in a way that preserves the keyof T type
  for (const key of Object.keys(schema) as Array<keyof T>) {
    const spec = schema[key];
    const value = values[key];

    // The following `if/else` block has the typing issue.
    // TypeScript can narrow the type of `spec`, but not the corresponding
    // `key` or `value`. This is a known limitation with "correlated unions".
    // To work around this, we cast `onchange` to `any` at the call site.
    // This is safe because the logic ensures we pass the correct value type,
    // and the `onchange` function itself remains strictly typed,
    // preserving the integrity of the `options` state object.

    if (typeof spec === 'boolean') {
      listItems.push(
        m(
          'li',
          m(Checkbox, {
            checked: value as boolean,
            label: key as string,
            onchange: (e: Event) =>
              (onchange as any)(key, (e.target as HTMLInputElement).checked),
          }),
        ),
      );
    } else if (typeof spec === 'string') {
      listItems.push(
        m(
          'li',
          m(
            'label',
            key as string,
            ' ',
            m(TextInput, {
              value: value as string,
              oninput: (e: Event) =>
                (onchange as any)(key, (e.target as HTMLInputElement).value),
            }),
          ),
        ),
      );
    } else if (typeof spec === 'number') {
      listItems.push(
        m(
          'li',
          m(
            'label',
            key as string,
            ' ',
            m('input[type=number]', {
              value: value as number,
              oninput: (e: Event) =>
                (onchange as any)(
                  key,
                  parseFloat((e.target as HTMLInputElement).value),
                ),
            }),
          ),
        ),
      );
    } else if (
      typeof spec === 'object' &&
      spec !== null &&
      'kind' in spec &&
      spec.kind === 'enum'
    ) {
      listItems.push(
        m(
          'li',
          m(
            'label',
            key as string,
            ' ',
            m(
              Select,
              {
                onchange: (e: Event) =>
                  (onchange as any)(key, (e.target as HTMLSelectElement).value),
              },
              spec.values.map((opt) =>
                m(
                  'option',
                  {
                    value: opt,
                    selected: value === opt,
                  },
                  opt,
                ),
              ),
            ),
          ),
        ),
      );
    }
  }

  if (listItems.length === 0) {
    return null;
  }
  return m('.pf-widget-controls', m('h3', 'Options'), m('ul', listItems));
}

// --- EXAMPLE USAGE (now uses the exported function again) ---
renderWidgetContainer({
  label: 'Example Widget',
  schema: {
    foo: 'foo',
    bar: 123,
    baz: true,
    qux: {kind: 'enum', defaultValue: 'foo', values: ['foo', 'bar', 'baz']},
  },
  render: (options) => {
    switch (options.qux) {
      case 'foo':
        return m('div', `Foo: ${options.foo}`);
      case 'bar':
        return m('div', `Bar: ${options.bar}`);
      case 'baz':
        return m('div', `Baz: ${options.baz}`);
      default:
        return m('div', 'Unknown option');
    }
  },
});
