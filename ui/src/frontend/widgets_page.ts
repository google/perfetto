// Copyright (C) 2023 The Android Open Source Project
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

import {classNames} from '../base/classnames';
import {Hotkey, Platform} from '../base/hotkeys';
import {isString} from '../base/object_utils';
import {Icons} from '../base/semantic_icons';
import {raf} from '../core/raf_scheduler';
import {Anchor} from '../widgets/anchor';
import {Button} from '../widgets/button';
import {Callout} from '../widgets/callout';
import {Checkbox} from '../widgets/checkbox';
import {Editor} from '../widgets/editor';
import {EmptyState} from '../widgets/empty_state';
import {Form, FormLabel} from '../widgets/form';
import {HotkeyGlyphs} from '../widgets/hotkey_glyphs';
import {Icon} from '../widgets/icon';
import {Menu, MenuDivider, MenuItem, PopupMenu2} from '../widgets/menu';
import {
  MultiSelect,
  MultiSelectDiff,
  PopupMultiSelect,
} from '../widgets/multiselect';
import {Popup, PopupPosition} from '../widgets/popup';
import {Portal} from '../widgets/portal';
import {FilterableSelect, Select} from '../widgets/select';
import {Spinner} from '../widgets/spinner';
import {Switch} from '../widgets/switch';
import {TextInput} from '../widgets/text_input';
import {MultiParagraphText, TextParagraph} from '../widgets/text_paragraph';
import {LazyTreeNode, Tree, TreeNode} from '../widgets/tree';

import {createPage} from './pages';
import {PopupMenuButton} from './popup_menu';
import {TableShowcase} from './tables/table_showcase';
import {VegaView} from './widgets/vega_view';

const DATA_ENGLISH_LETTER_FREQUENCY = {
  table: [
    {category: 'a', amount: 8.167}, {category: 'b', amount: 1.492},
    {category: 'c', amount: 2.782}, {category: 'd', amount: 4.253},
    {category: 'e', amount: 12.70}, {category: 'f', amount: 2.228},
    {category: 'g', amount: 2.015}, {category: 'h', amount: 6.094},
    {category: 'i', amount: 6.966}, {category: 'j', amount: 0.253},
    {category: 'k', amount: 1.772}, {category: 'l', amount: 4.025},
    {category: 'm', amount: 2.406}, {category: 'n', amount: 6.749},
    {category: 'o', amount: 7.507}, {category: 'p', amount: 1.929},
    {category: 'q', amount: 0.095}, {category: 'r', amount: 5.987},
    {category: 's', amount: 6.327}, {category: 't', amount: 9.056},
    {category: 'u', amount: 2.758}, {category: 'v', amount: 0.978},
    {category: 'w', amount: 2.360}, {category: 'x', amount: 0.250},
    {category: 'y', amount: 1.974}, {category: 'z', amount: 0.074},
  ],
};

const DATA_POLISH_LETTER_FREQUENCY = {
  table: [
    {category: 'a', amount: 8.965}, {category: 'b', amount: 1.482},
    {category: 'c', amount: 3.988}, {category: 'd', amount: 3.293},
    {category: 'e', amount: 7.921}, {category: 'f', amount: 0.312},
    {category: 'g', amount: 1.377}, {category: 'h', amount: 1.072},
    {category: 'i', amount: 8.286}, {category: 'j', amount: 2.343},
    {category: 'k', amount: 3.411}, {category: 'l', amount: 2.136},
    {category: 'm', amount: 2.911}, {category: 'n', amount: 5.600},
    {category: 'o', amount: 7.590}, {category: 'p', amount: 3.101},
    {category: 'q', amount: 0.003}, {category: 'r', amount: 4.571},
    {category: 's', amount: 4.263}, {category: 't', amount: 3.966},
    {category: 'u', amount: 2.347}, {category: 'v', amount: 0.034},
    {category: 'w', amount: 4.549}, {category: 'x', amount: 0.019},
    {category: 'y', amount: 3.857}, {category: 'z', amount: 5.620},
  ],
};

const DATA_EMPTY = {};

const SPEC_BAR_CHART = `
{
  "$schema": "https://vega.github.io/schema/vega/v5.json",
  "description": "A basic bar chart example, with value labels shown upon mouse hover.",
  "width": 400,
  "height": 200,
  "padding": 5,

  "data": [
    {
      "name": "table"
    }
  ],

  "signals": [
    {
      "name": "tooltip",
      "value": {},
      "on": [
        {"events": "rect:mouseover", "update": "datum"},
        {"events": "rect:mouseout",  "update": "{}"}
      ]
    }
  ],

  "scales": [
    {
      "name": "xscale",
      "type": "band",
      "domain": {"data": "table", "field": "category"},
      "range": "width",
      "padding": 0.05,
      "round": true
    },
    {
      "name": "yscale",
      "domain": {"data": "table", "field": "amount"},
      "nice": true,
      "range": "height"
    }
  ],

  "axes": [
    { "orient": "bottom", "scale": "xscale" },
    { "orient": "left", "scale": "yscale" }
  ],

  "marks": [
    {
      "type": "rect",
      "from": {"data":"table"},
      "encode": {
        "enter": {
          "x": {"scale": "xscale", "field": "category"},
          "width": {"scale": "xscale", "band": 1},
          "y": {"scale": "yscale", "field": "amount"},
          "y2": {"scale": "yscale", "value": 0}
        },
        "update": {
          "fill": {"value": "steelblue"}
        },
        "hover": {
          "fill": {"value": "red"}
        }
      }
    },
    {
      "type": "text",
      "encode": {
        "enter": {
          "align": {"value": "center"},
          "baseline": {"value": "bottom"},
          "fill": {"value": "#333"}
        },
        "update": {
          "x": {"scale": "xscale", "signal": "tooltip.category", "band": 0.5},
          "y": {"scale": "yscale", "signal": "tooltip.amount", "offset": -2},
          "text": {"signal": "tooltip.amount"},
          "fillOpacity": [
            {"test": "datum === tooltip", "value": 0},
            {"value": 1}
          ]
        }
      }
    }
  ]
}
`;

const SPEC_BAR_CHART_LITE = `
{
  "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
  "description": "A simple bar chart with embedded data.",
  "data": {
    "name": "table"
  },
  "mark": "bar",
  "encoding": {
    "x": {"field": "category", "type": "nominal", "axis": {"labelAngle": 0}},
    "y": {"field": "amount", "type": "quantitative"}
  }
}
`;

const SPEC_BROKEN = `{
  "description": 123
}
`;

enum SpecExample {
  BarChart = 'Barchart',
  BarChartLite = 'Barchart (Lite)',
  Broken = 'Broken',
}

enum DataExample {
  English = 'English',
  Polish = 'Polish',
  Empty = 'Empty',
}

function getExampleSpec(example: SpecExample): string {
  switch (example) {
    case SpecExample.BarChart:
      return SPEC_BAR_CHART;
    case SpecExample.BarChartLite:
      return SPEC_BAR_CHART_LITE;
    case SpecExample.Broken:
      return SPEC_BROKEN;
    default:
      const exhaustiveCheck: never = example;
      throw new Error(`Unhandled case: ${exhaustiveCheck}`);
  }
}

function getExampleData(example: DataExample) {
  switch (example) {
    case DataExample.English:
      return DATA_ENGLISH_LETTER_FREQUENCY;
    case DataExample.Polish:
      return DATA_POLISH_LETTER_FREQUENCY;
    case DataExample.Empty:
      return DATA_EMPTY;
    default:
      const exhaustiveCheck: never = example;
      throw new Error(`Unhandled case: ${exhaustiveCheck}`);
  }
}


const options: {[key: string]: boolean} = {
  foobar: false,
  foo: false,
  bar: false,
  baz: false,
  qux: false,
  quux: false,
  corge: false,
  grault: false,
  garply: false,
  waldo: false,
  fred: false,
  plugh: false,
  xyzzy: false,
  thud: false,
};

function PortalButton() {
  let portalOpen = false;

  return {
    view: function({attrs}: any) {
      const {
        zIndex = true,
        absolute = true,
        top = true,
      } = attrs;
      return [
        m(Button, {
          label: 'Toggle Portal',
          onclick: () => {
            portalOpen = !portalOpen;
            raf.scheduleFullRedraw();
          },
        }),
        portalOpen &&
            m(Portal,
              {
                style: {
                  position: absolute && 'absolute',
                  top: top && '0',
                  zIndex: zIndex ? '10' : '0',
                  background: 'white',
                },
              },
              m('', `A very simple portal - a div rendered outside of the normal
              flow of the page`)),
      ];
    },
  };
}

function lorem() {
  const text =
      `Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod
      tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim
      veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea
      commodo consequat.Duis aute irure dolor in reprehenderit in voluptate
      velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat
      cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id
      est laborum.`;
  return m('', {style: {width: '200px'}}, text);
}

function ControlledPopup() {
  let popupOpen = false;

  return {
    view: function() {
      return m(
          Popup,
          {
            trigger:
                m(Button, {label: `${popupOpen ? 'Close' : 'Open'} Popup`}),
            isOpen: popupOpen,
            onChange: (shouldOpen: boolean) => popupOpen = shouldOpen,
          },
          m(Button, {
            label: 'Close Popup',
            onclick: () => {
              popupOpen = !popupOpen;
              raf.scheduleFullRedraw();
            },
          }),
      );
    },
  };
}

type Options = {
  [key: string]: EnumOption|boolean|string;
};

class EnumOption {
  constructor(public initial: string, public options: string[]) {}
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

interface WidgetShowcaseAttrs {
  label: string;
  description?: string;
  initialOpts?: Options;
  renderWidget: (options: any) => any;
  wide?: boolean;
}

// A little helper class to render any vnode with a dynamic set of options
class WidgetShowcase implements m.ClassComponent<WidgetShowcaseAttrs> {
  private optValues: any = {};
  private opts?: Options;

  renderOptions(listItems: m.Child[]): m.Child {
    if (listItems.length === 0) {
      return null;
    }
    return m(
        '.widget-controls',
        m('h3', 'Options'),
        m('ul', listItems),
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
          } else if (typeof option === 'boolean') {
            this.optValues[key] = option;
          } else if (isString(option)) {
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
          '.widget-block',
          m(
              'div',
              {
                class: classNames(
                    'widget-container',
                    wide && 'widget-container-wide',
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
    } else {
      return null;
    }
  }

  private renderBooleanOption(key: string) {
    return m(Checkbox, {
      checked: this.optValues[key],
      label: key,
      onchange: () => {
        this.optValues[key] = !this.optValues[key];
        raf.scheduleFullRedraw();
      },
    });
  }

  private renderStringOption(key: string) {
    return m(TextInput, {
      placeholder: key,
      value: this.optValues[key],
      oninput: (e: Event) => {
        this.optValues[key] = (e.target as HTMLInputElement).value;
        raf.scheduleFullRedraw();
      },
    });
  }

  private renderEnumOption(key: string, opt: EnumOption) {
    const optionElements = opt.options.map((option: string) => {
      return m('option', {value: option}, option);
    });
    return m(
        Select,
        {
          value: this.optValues[key],
          onchange: (e: Event) => {
            const el = e.target as HTMLSelectElement;
            this.optValues[key] = el.value;
            raf.scheduleFullRedraw();
          },
        },
        optionElements);
  }
}

export const WidgetsPage = createPage({
  view() {
    return m(
        '.widgets-page',
        m('h1', 'Widgets'),
        m(WidgetShowcase, {
          label: 'Button',
          renderWidget: ({label, icon, rightIcon, ...rest}) => m(Button, {
            icon: icon ? 'send' : undefined,
            rightIcon: rightIcon ? 'arrow_forward' : undefined,
            label: label ? 'Button' : '',
            ...rest,
          }),
          initialOpts: {
            label: true,
            icon: true,
            rightIcon: false,
            disabled: false,
            minimal: false,
            active: false,
            compact: false,
          },
        }),
        m(WidgetShowcase, {
          label: 'Checkbox',
          renderWidget: (opts) => m(Checkbox, {label: 'Checkbox', ...opts}),
          initialOpts: {
            disabled: false,
          },
        }),
        m(WidgetShowcase, {
          label: 'Switch',
          renderWidget: ({label, ...rest}: any) =>
              m(Switch, {label: label ? 'Switch' : undefined, ...rest}),
          initialOpts: {
            label: true,
            disabled: false,
          },
        }),
        m(WidgetShowcase, {
          label: 'Text Input',
          renderWidget: ({placeholder, ...rest}) => m(TextInput, {
            placeholder: placeholder ? 'Placeholder...' : '',
            ...rest,
          }),
          initialOpts: {
            placeholder: true,
            disabled: false,
          },
        }),
        m(WidgetShowcase, {
          label: 'Select',
          renderWidget: (opts) =>
              m(Select,
                opts,
                [
                  m('option', {value: 'foo', label: 'Foo'}),
                  m('option', {value: 'bar', label: 'Bar'}),
                  m('option', {value: 'baz', label: 'Baz'}),
                ]),
          initialOpts: {
            disabled: false,
          },
        }),
        m(WidgetShowcase, {
          label: 'Filterable Select',
          renderWidget: () =>
              m(FilterableSelect, {
                values: ['foo', 'bar', 'baz'],
                onSelected: () => {},
              }),
        }),
        m(WidgetShowcase, {
          label: 'Empty State',
          renderWidget: ({header, content}) =>
              m(EmptyState,
                {
                  header: header && 'No search results found...',
                },
                content && m(Button, {label: 'Try again'})),
          initialOpts: {
            header: true,
            content: true,
          },
        }),
        m(WidgetShowcase, {
          label: 'Anchor',
          renderWidget: ({icon}) => m(
              Anchor,
              {
                icon: icon && 'open_in_new',
                href: 'https://perfetto.dev/docs/',
                target: '_blank',
              },
              'Docs',
              ),
          initialOpts: {
            icon: true,
          },
        }),
        m(WidgetShowcase,
          {
            label: 'Table',
            renderWidget: () => m(TableShowcase), initialOpts: {}, wide: true,
        }),
        m(WidgetShowcase, {
          label: 'Portal',
          description: `A portal is a div rendered out of normal flow
          of the hierarchy.`,
          renderWidget: (opts) => m(PortalButton, opts),
          initialOpts: {
            absolute: true,
            zIndex: true,
            top: true,
          },
        }),
        m(WidgetShowcase, {
          label: 'Popup',
          description: `A popup is a nicely styled portal element whose position is
        dynamically updated to appear to float alongside a specific element on
        the page, even as the element is moved and scrolled around.`,
          renderWidget: (opts) => m(
              Popup,
              {
                trigger: m(Button, {label: 'Toggle Popup'}),
                ...opts,
              },
              lorem(),
              ),
          initialOpts: {
            position: new EnumOption(
                PopupPosition.Auto,
                Object.values(PopupPosition),
                ),
            closeOnEscape: true,
            closeOnOutsideClick: true,
          },
        }),
        m(WidgetShowcase, {
          label: 'Controlled Popup',
        description: `The open/close state of a controlled popup is passed in via
        the 'isOpen' attribute. This means we can get open or close the popup
        from wherever we like. E.g. from a button inside the popup.
        Keeping this state external also means we can modify other parts of the
        page depending on whether the popup is open or not, such as the text
        on this button.
        Note, this is the same component as the popup above, but used in
        controlled mode.`,
          renderWidget: (opts) => m(ControlledPopup, opts),
          initialOpts: {},
        }),
        m(WidgetShowcase, {
          label: 'Icon',
          renderWidget: (opts) => m(Icon, {icon: 'star', ...opts}),
          initialOpts: {filled: false},
        }),
        m(WidgetShowcase, {
          label: 'MultiSelect panel',
          renderWidget: ({...rest}) => m(MultiSelect, {
            options: Object.entries(options).map(([key, value]) => {
              return {
                id: key,
                name: key,
                checked: value,
              };
            }),
            onChange: (diffs: MultiSelectDiff[]) => {
              diffs.forEach(({id, checked}) => {
                options[id] = checked;
              });
              raf.scheduleFullRedraw();
            },
            ...rest,
          }),
          initialOpts: {
            repeatCheckedItemsAtTop: false,
            fixedSize: false,
          },
        }),
        m(WidgetShowcase, {
          label: 'Popup with MultiSelect',
          renderWidget: ({icon, ...rest}) => m(PopupMultiSelect, {
            options: Object.entries(options).map(([key, value]) => {
              return {
                id: key,
                name: key,
                checked: value,
              };
            }),
            popupPosition: PopupPosition.Top,
            label: 'Multi Select',
            icon: icon ? Icons.LibraryAddCheck : undefined,
            onChange: (diffs: MultiSelectDiff[]) => {
              diffs.forEach(({id, checked}) => {
                options[id] = checked;
              });
              raf.scheduleFullRedraw();
            },
            ...rest,
          }),
          initialOpts: {
            icon: true,
            showNumSelected: true,
            repeatCheckedItemsAtTop: false,
          },
        }),
        m(WidgetShowcase, {
          label: 'PopupMenu',
          renderWidget: () => {
            return m(PopupMenuButton, {
              icon: 'description',
              items: [
                {itemType: 'regular', text: 'New', callback: () => {}},
                {itemType: 'regular', text: 'Open', callback: () => {}},
                {itemType: 'regular', text: 'Save', callback: () => {}},
                {itemType: 'regular', text: 'Delete', callback: () => {}},
                {
                  itemType: 'group',
                  text: 'Share',
                  itemId: 'foo',
                  children: [
                    {itemType: 'regular', text: 'Friends', callback: () => {}},
                    {itemType: 'regular', text: 'Family', callback: () => {}},
                    {itemType: 'regular', text: 'Everyone', callback: () => {}},
                  ],
                },
              ],
            });
          },
        }),
        m(WidgetShowcase, {
          label: 'Menu',
          renderWidget: () => m(
              Menu,
              m(MenuItem, {label: 'New', icon: 'add'}),
              m(MenuItem, {label: 'Open', icon: 'folder_open'}),
              m(MenuItem, {label: 'Save', icon: 'save', disabled: true}),
              m(MenuDivider),
              m(MenuItem, {label: 'Delete', icon: 'delete'}),
              m(MenuDivider),
              m(
                  MenuItem,
                  {label: 'Share', icon: 'share'},
                  m(MenuItem, {label: 'Everyone', icon: 'public'}),
                  m(MenuItem, {label: 'Friends', icon: 'group'}),
                  m(
                      MenuItem,
                      {label: 'Specific people', icon: 'person_add'},
                      m(MenuItem, {label: 'Alice', icon: 'person'}),
                      m(MenuItem, {label: 'Bob', icon: 'person'}),
                      ),
                  ),
              m(
                  MenuItem,
                  {label: 'More', icon: 'more_horiz'},
                  m(MenuItem, {label: 'Query', icon: 'database'}),
                  m(MenuItem, {label: 'Download', icon: 'download'}),
                  m(MenuItem, {label: 'Clone', icon: 'copy_all'}),
                  ),
              ),

        }),
        m(WidgetShowcase, {
          label: 'PopupMenu2',
          renderWidget: (opts) => m(
              PopupMenu2,
              {
                trigger: m(Button, {
                  label: 'Menu',
                  rightIcon: Icons.ContextMenu,
                }),
                ...opts,
              },
              m(MenuItem, {label: 'New', icon: 'add'}),
              m(MenuItem, {label: 'Open', icon: 'folder_open'}),
              m(MenuItem, {label: 'Save', icon: 'save', disabled: true}),
              m(MenuDivider),
              m(MenuItem, {label: 'Delete', icon: 'delete'}),
              m(MenuDivider),
              m(
                  MenuItem,
                  {label: 'Share', icon: 'share'},
                  m(MenuItem, {label: 'Everyone', icon: 'public'}),
                  m(MenuItem, {label: 'Friends', icon: 'group'}),
                  m(
                      MenuItem,
                      {label: 'Specific people', icon: 'person_add'},
                      m(MenuItem, {label: 'Alice', icon: 'person'}),
                      m(MenuItem, {label: 'Bob', icon: 'person'}),
                      ),
                  ),
              m(
                  MenuItem,
                  {label: 'More', icon: 'more_horiz'},
                  m(MenuItem, {label: 'Query', icon: 'database'}),
                  m(MenuItem, {label: 'Download', icon: 'download'}),
                  m(MenuItem, {label: 'Clone', icon: 'copy_all'}),
                  ),
              ),
          initialOpts: {
            popupPosition: new EnumOption(
                PopupPosition.Bottom,
                Object.values(PopupPosition),
                ),
          },
        }),
        m(WidgetShowcase, {
          label: 'Spinner',
          description: `Simple spinner, rotates forever.
            Width and height match the font size.`,
          renderWidget: ({fontSize, easing}) =>
              m('', {style: {fontSize}}, m(Spinner, {easing})),
          initialOpts: {
            fontSize: new EnumOption(
                '16px',
                ['12px', '16px', '24px', '32px', '64px', '128px'],
                ),
            easing: false,
          },
        }),
        m(WidgetShowcase, {
          label: 'Tree',
          renderWidget: (opts) => m(
            Tree,
            opts,
            m(TreeNode, {left: 'Name', right: 'my_event', icon: 'badge'}),
            m(TreeNode, {left: 'CPU', right: '2', icon: 'memory'}),
            m(TreeNode,
              {left: 'Start time', right: '1s 435ms', icon: 'schedule'}),
            m(TreeNode, {left: 'Duration', right: '86ms', icon: 'timer'}),
            m(TreeNode, {
              left: 'SQL',
              right: m(
                PopupMenu2,
                {
                  popupPosition: PopupPosition.RightStart,
                  trigger: m(Anchor, {
                    icon: Icons.ContextMenu,
                  }, 'SELECT * FROM raw WHERE id = 123'),
                },
                m(MenuItem, {
                  label: 'Copy SQL Query',
                  icon: 'content_copy',
                }),
                m(MenuItem, {
                  label: 'Execute Query in new tab',
                  icon: 'open_in_new',
                }),
                ),
            }),
            m(TreeNode, {
              icon: 'account_tree',
              left: 'Process',
              right: m(Anchor, {icon: 'open_in_new'}, '/bin/foo[789]'),
            }),
            m(TreeNode, {
              left: 'Thread',
              right: m(Anchor, {icon: 'open_in_new'}, 'my_thread[456]'),
            }),
            m(
              TreeNode,
              {
                left: 'Args',
                summary: 'foo: string, baz: string, quux: string[4]',
              },
              m(TreeNode, {left: 'foo', right: 'bar'}),
              m(TreeNode, {left: 'baz', right: 'qux'}),
              m(
                TreeNode,
                {left: 'quux', summary: 'string[4]'},
                m(TreeNode, {left: '[0]', right: 'corge'}),
                m(TreeNode, {left: '[1]', right: 'grault'}),
                m(TreeNode, {left: '[2]', right: 'garply'}),
                m(TreeNode, {left: '[3]', right: 'waldo'}),
                ),
              ),
            m(LazyTreeNode, {
              left: 'Lazy',
              icon: 'bedtime',
              fetchData: async () => {
                await new Promise((r) => setTimeout(r, 1000));
                return () => m(TreeNode, {left: 'foo'});
              },
            }),
            m(LazyTreeNode, {
              left: 'Dynamic',
              unloadOnCollapse: true,
              icon: 'bedtime',
              fetchData: async () => {
                await new Promise((r) => setTimeout(r, 1000));
                return () => m(TreeNode, {left: 'foo'});
              },
            }),
            ),
          wide: true,
        }),
        m(
          WidgetShowcase, {
            label: 'Form',
            renderWidget: () => renderForm('form'),
          }),
        m(WidgetShowcase, {
            label: 'Nested Popups',
            renderWidget: () => m(
              Popup,
              {
                trigger: m(Button, {label: 'Open the popup'}),
              },
              m(PopupMenu2,
                {
                  trigger: m(Button, {label: 'Select an option'}),
                },
                m(MenuItem, {label: 'Option 1'}),
                m(MenuItem, {label: 'Option 2'}),
              ),
              m(Button, {
                label: 'Done',
                dismissPopup: true,
              }),
            ),
          }),
          m(
            WidgetShowcase, {
              label: 'Callout',
              renderWidget: () => m(
                Callout,
                {
                  icon: 'info',
                },
                'Lorem ipsum dolor sit amet, consectetur adipiscing elit. ' +
                'Nulla rhoncus tempor neque, sed malesuada eros dapibus vel. ' +
                'Aliquam in ligula vitae tortor porttitor laoreet iaculis ' +
                'finibus est.',
              ),
            }),
          m(WidgetShowcase, {
            label: 'Editor',
            renderWidget: () => m(Editor),
          }),
          m(WidgetShowcase, {
            label: 'VegaView',
            renderWidget: (opt) => m(VegaView, {
              spec: getExampleSpec(opt.exampleSpec),
              data: getExampleData(opt.exampleData),
            }),
            initialOpts: {
              exampleSpec: new EnumOption(
                SpecExample.BarChart,
                Object.values(SpecExample),
              ),
              exampleData: new EnumOption(
                DataExample.English,
                Object.values(DataExample),
              ),

            },
          }),
          m(
            WidgetShowcase, {
              label: 'Form within PopupMenu2',
              description: `A form placed inside a popup menu works just fine,
              and the cancel/submit buttons also dismiss the popup. A bit more
              margin is added around it too, which improves the look and feel.`,
              renderWidget: () => m(
                PopupMenu2,
                {
                  trigger: m(Button, {label: 'Popup!'}),
                },
                m(MenuItem,
                  {
                    label: 'Open form...',
                  },
                  renderForm('popup-form'),
                ),
              ),
            }),
          m(
            WidgetShowcase, {
              label: 'Hotkey',
              renderWidget: (opts) => {
                if (opts.platform === 'auto') {
                  return m(HotkeyGlyphs, {hotkey: opts.hotkey as Hotkey});
                } else {
                  const platform = opts.platform as Platform;
                  return m(HotkeyGlyphs, {
                    hotkey: opts.hotkey as Hotkey,
                    spoof: platform,
                  });
                }
              },
              initialOpts: {
                hotkey: 'Mod+Shift+P',
                platform: new EnumOption('auto', ['auto', 'Mac', 'PC']),
              },
            }),
          m(
            WidgetShowcase, {
              label: 'Text Paragraph',
              description: `A basic formatted text paragraph with wrapping. If
              it is desirable to preserve the original text format/line breaks,
              set the compressSpace attribute to false.`,
              renderWidget: (opts) => {
                return m(TextParagraph, {
                  text: `Lorem ipsum dolor sit amet, consectetur adipiscing
                         elit. Nulla rhoncus tempor neque, sed malesuada eros
                         dapibus vel. Aliquam in ligula vitae tortor porttitor
                         laoreet iaculis finibus est.`,
                  compressSpace: opts.compressSpace,
                });
              },
              initialOpts: {
                compressSpace: true,
              },
            }),
          m(
            WidgetShowcase, {
              label: 'Multi Paragraph Text',
              description: `A wrapper for multiple paragraph widgets.`,
              renderWidget: () => {
                return m(MultiParagraphText,
                 m(TextParagraph, {
                  text: `Lorem ipsum dolor sit amet, consectetur adipiscing
                         elit. Nulla rhoncus tempor neque, sed malesuada eros
                         dapibus vel. Aliquam in ligula vitae tortor porttitor
                         laoreet iaculis finibus est.`,
                  compressSpace: true,
                }), m(TextParagraph, {
                  text: `Sed ut perspiciatis unde omnis iste natus error sit
                         voluptatem accusantium doloremque laudantium, totam rem
                         aperiam, eaque ipsa quae ab illo inventore veritatis et
                         quasi architecto beatae vitae dicta sunt explicabo.
                         Nemo enim ipsam voluptatem quia voluptas sit aspernatur
                         aut odit aut fugit, sed quia consequuntur magni dolores
                         eos qui ratione voluptatem sequi nesciunt.`,
                  compressSpace: true,
                }),
                );
              },
            }),
    );
  },
});

function renderForm(id: string) {
  return m(
      Form,
      {
        submitLabel: 'Submit',
        submitIcon: 'send',
        cancelLabel: 'Cancel',
        resetLabel: 'Reset',
        onSubmit: () => window.alert('Form submitted!'),
      },
      m(FormLabel,
        {for: `${id}-foo`,
        },
        'Foo'),
      m(TextInput, {id: `${id}-foo`}),
      m(FormLabel,
        {for: `${id}-bar`,
        },
        'Bar'),
      m(Select,
        {id: `${id}-bar`},
        [
          m('option', {value: 'foo', label: 'Foo'}),
          m('option', {value: 'bar', label: 'Bar'}),
          m('option', {value: 'baz', label: 'Baz'}),
        ]),
  );
}
