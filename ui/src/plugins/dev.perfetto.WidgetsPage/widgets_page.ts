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
import {Hotkey, Platform} from '../../base/hotkeys';
import {parseAndPrintTree} from '../../base/perfetto_sql_lang/language';
import {Icons} from '../../base/semantic_icons';
import {
  DataGrid,
  DataGridAttrs,
} from '../../components/widgets/data_grid/data_grid';
import {SQLDataSource} from '../../components/widgets/data_grid/sql_data_source';
import {TreeTable, TreeTableAttrs} from '../../components/widgets/treetable';
import {App} from '../../public/app';
import {Engine} from '../../trace_processor/engine';
import {Anchor} from '../../widgets/anchor';
import {
  Button,
  ButtonAttrs,
  ButtonBar,
  ButtonGroup,
  ButtonVariant,
} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {Card, CardStack} from '../../widgets/card';
import {Checkbox} from '../../widgets/checkbox';
import {Chip} from '../../widgets/chip';
import {CodeSnippet} from '../../widgets/code_snippet';
import {Intent} from '../../widgets/common';
import {CopyToClipboardButton} from '../../widgets/copy_to_clipboard_button';
import {CopyableLink} from '../../widgets/copyable_link';
import {CursorTooltip} from '../../widgets/cursor_tooltip';
import {Editor} from '../../widgets/editor';
import {EmptyState} from '../../widgets/empty_state';
import {Form, FormLabel} from '../../widgets/form';
import {
  Grid,
  GridBody,
  GridDataCell,
  GridHeader,
  GridHeaderCell,
  GridRow,
} from '../../widgets/grid';
import {HotkeyGlyphs} from '../../widgets/hotkey_glyphs';
import {Icon} from '../../widgets/icon';
import {
  Menu,
  MenuDivider,
  MenuItem,
  MenuTitle,
  PopupMenu,
} from '../../widgets/menu';
import {MiddleEllipsis} from '../../widgets/middle_ellipsis';
import {showModal} from '../../widgets/modal';
import {
  MultiSelect,
  MultiSelectDiff,
  PopupMultiSelect,
} from '../../widgets/multiselect';
import {MultiselectInput} from '../../widgets/multiselect_input';
import {Popup, PopupPosition} from '../../widgets/popup';
import {Portal} from '../../widgets/portal';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {Select} from '../../widgets/select';
import {Spinner} from '../../widgets/spinner';
import {SplitPanel, Tab} from '../../widgets/split_panel';
import {Stack} from '../../widgets/stack';
import {Switch} from '../../widgets/switch';
import {TabStrip} from '../../widgets/tabs';
import {TagInput} from '../../widgets/tag_input';
import {TextInput} from '../../widgets/text_input';
import {MultiParagraphText, TextParagraph} from '../../widgets/text_paragraph';
import {Tooltip} from '../../widgets/tooltip';
import {TrackShell} from '../../widgets/track_shell';
import {LazyTreeNode, Tree, TreeNode} from '../../widgets/tree';
import {VirtualOverlayCanvas} from '../../widgets/virtual_overlay_canvas';
import {
  VirtualTable,
  VirtualTableAttrs,
  VirtualTableRow,
} from '../../widgets/virtual_table';
import {ButtonDemo} from './demos/button_demo';
import {VegaDemo} from './demos/vega_demo';
import {enumOption, renderWidgetContainer} from './widget_container';

function arg<T>(
  anyArg: unknown,
  valueIfTrue: T,
  valueIfFalse: T | undefined = undefined,
): T | undefined {
  return Boolean(anyArg) ? valueIfTrue : valueIfFalse;
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

let currentTab: string = 'foo';

function PortalButton() {
  let portalOpen = false;

  return {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    view: function ({attrs}: any) {
      const {zIndex = true, absolute = true, top = true} = attrs;
      return [
        m(Button, {
          label: 'Toggle Portal',
          onclick: () => {
            portalOpen = !portalOpen;
          },
        }),
        portalOpen &&
          m(
            Portal,
            {
              style: {
                position: arg(absolute, 'absolute'),
                top: arg(top, '0'),
                zIndex: arg(zIndex, '10', '0'),
                background: 'white',
              },
            },
            m(
              '',
              `A very simple portal - a div rendered outside of the normal
              flow of the page`,
            ),
          ),
      ];
    },
  };
}

function lorem() {
  const text = `Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod
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
    view: function () {
      return m(
        Popup,
        {
          trigger: m(Button, {label: `${popupOpen ? 'Close' : 'Open'} Popup`}),
          isOpen: popupOpen,
          onChange: (shouldOpen: boolean) => (popupOpen = shouldOpen),
        },
        m(Button, {
          label: 'Close Popup',
          onclick: () => {
            popupOpen = !popupOpen;
          },
        }),
      );
    },
  };
}

function recursiveTreeNode(): m.Children {
  return m(LazyTreeNode, {
    left: 'Recursive',
    right: '...',
    fetchData: async () => {
      // await new Promise((r) => setTimeout(r, 1000));
      return () => recursiveTreeNode();
    },
  });
}

interface File {
  name: string;
  size: string;
  date: string;
  children?: File[];
}

const files: File[] = [
  {
    name: 'foo',
    size: '10MB',
    date: '2023-04-02',
  },
  {
    name: 'bar',
    size: '123KB',
    date: '2023-04-08',
    children: [
      {
        name: 'baz',
        size: '4KB',
        date: '2023-05-07',
      },
      {
        name: 'qux',
        size: '18KB',
        date: '2023-05-28',
        children: [
          {
            name: 'quux',
            size: '4KB',
            date: '2023-05-07',
          },
          {
            name: 'corge',
            size: '18KB',
            date: '2023-05-28',
            children: [
              {
                name: 'grault',
                size: '4KB',
                date: '2023-05-07',
              },
              {
                name: 'garply',
                size: '18KB',
                date: '2023-05-28',
              },
              {
                name: 'waldo',
                size: '87KB',
                date: '2023-05-02',
              },
            ],
          },
        ],
      },
    ],
  },
  {
    name: 'fred',
    size: '8KB',
    date: '2022-12-27',
  },
];

let virtualTableData: {offset: number; rows: VirtualTableRow[]} = {
  offset: 0,
  rows: [],
};

function TagInputDemo() {
  const tags: string[] = ['foo', 'bar', 'baz'];
  let tagInputValue: string = '';

  return {
    view: () => {
      return m(TagInput, {
        tags,
        value: tagInputValue,
        onTagAdd: (tag) => {
          tags.push(tag);
          tagInputValue = '';
        },
        onChange: (value) => {
          tagInputValue = value;
        },
        onTagRemove: (index) => {
          tags.splice(index, 1);
        },
      });
    },
  };
}

function SegmentedButtonsDemo({attrs}: {attrs: {disabled: boolean}}) {
  let selectedIdx = 0;
  return {
    view: () => {
      return m(SegmentedButtons, {
        ...attrs,
        options: [{label: 'Yes'}, {label: 'Maybe'}, {label: 'No'}],
        selectedOption: selectedIdx,
        onOptionSelected: (num) => {
          selectedIdx = num;
        },
      });
    },
  };
}

function RadioButtonGroupDemo() {
  let setting: 'yes' | 'maybe' | 'no' = 'no';
  console.log(setting);
  return {
    view: ({attrs}: m.Vnode<ButtonAttrs>) => {
      return m(ButtonGroup, [
        m(Button, {
          ...attrs,
          label: 'Yes',
          active: setting === 'yes',
          onclick: () => {
            setting = 'yes';
          },
        }),
        m(Button, {
          ...attrs,
          label: 'Maybe',
          active: setting === 'maybe',
          onclick: () => {
            setting = 'maybe';
          },
        }),
        m(Button, {
          ...attrs,
          label: 'No',
          active: setting === 'no',
          onclick: () => {
            setting = 'no';
          },
        }),
      ]);
    },
  };
}

export class WidgetsPage implements m.ClassComponent<{app: App}> {
  view({attrs}: m.Vnode<{app: App}>) {
    const trace = attrs.app.trace;
    return m(
      '.pf-widgets-page',
      m('h1', 'Widgets'),
      m(ButtonDemo),
      renderWidgetContainer({
        label: 'Segmented Buttons',
        description: `
          Segmented buttons are a group of buttons where one of them is
          'selected'; they act similar to a set of radio buttons.
        `,
        render: (opts) => m(SegmentedButtonsDemo, opts),
        schema: {
          disabled: false,
        },
      }),
      renderWidgetContainer({
        label: 'ButtonGroup',
        render: ({variant, disabled, intent}) =>
          m(Stack, [
            m(ButtonGroup, [
              m(Button, {
                label: 'Commit',
                variant: variant as ButtonVariant,
                disabled,
                intent: intent as Intent,
              }),
              m(Button, {
                icon: Icons.ContextMenu,
                variant: variant as ButtonVariant,
                disabled,
                intent: intent as Intent,
              }),
            ]),
            m(RadioButtonGroupDemo, {
              variant: variant as ButtonVariant,
              disabled,
              intent: intent as Intent,
              label: '',
            }),
          ]),
        schema: {
          variant: enumOption(
            ButtonVariant.Filled,
            Object.values(ButtonVariant),
          ),
          disabled: false,
          intent: enumOption(Intent.None, Object.values(Intent)),
        },
      }),
      renderWidgetContainer({
        label: 'Checkbox',
        render: (opts) => m(Checkbox, {label: 'Checkbox', ...opts}),
        schema: {
          disabled: false,
        },
      }),
      renderWidgetContainer({
        label: 'Switch',
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        render: ({label, labelLeft, ...rest}: any) =>
          m(Switch, {
            label: arg(label, 'Switch'),
            labelLeft: arg(labelLeft, 'Left Label'),
            ...rest,
          }),
        schema: {
          label: true,
          labelLeft: false,
          disabled: false,
        },
      }),
      renderWidgetContainer({
        label: 'Anchor',
        render: ({icon, showInlineWithText, long}) =>
          m('', [
            Boolean(showInlineWithText) && 'Inline',
            m(
              Anchor,
              {
                icon: arg(icon, 'open_in_new'),
                href: 'https://perfetto.dev/docs/',
                target: '_blank',
              },
              Boolean(long)
                ? 'This is some really long text and it will probably overflow the container'
                : 'Link',
            ),
            Boolean(showInlineWithText) && 'text',
          ]),

        schema: {
          icon: true,
          showInlineWithText: false,
          long: false,
        },
      }),
      renderWidgetContainer({
        label: 'Text Input',
        render: ({placeholder, leftIcon, ...rest}) =>
          m(TextInput, {
            placeholder: arg(placeholder, 'Placeholder...', ''),
            leftIcon: arg(leftIcon, 'search'),
            ...rest,
          }),
        schema: {
          placeholder: true,
          disabled: false,
          leftIcon: true,
        },
      }),
      renderWidgetContainer({
        label: 'Select',
        render: (opts) =>
          m(Select, opts, [
            m('option', {value: 'foo', label: 'Foo'}),
            m('option', {value: 'bar', label: 'Bar'}),
            m('option', {value: 'baz', label: 'Baz'}),
          ]),
        schema: {
          disabled: false,
        },
      }),
      renderWidgetContainer({
        label: 'Empty State',
        render: ({header, content}) =>
          m(
            EmptyState,
            {
              title: arg(header, 'No search results found...'),
            },
            arg(content, m(Button, {label: 'Try again'})),
          ),
        schema: {
          header: true,
          content: true,
        },
      }),
      renderWidgetContainer({
        label: 'Card',
        description: `A card is a simple container with a shadow and rounded
          corners. It can be used to display grouped content in a visually
          appealing way.`,
        render: ({interactive}) =>
          m(Card, {interactive}, [
            m('h1', {style: {margin: 'unset'}}, 'Welcome!'),
            m('p', 'Would you like to start your journey?'),
            m(Stack, {orientation: 'horizontal'}, [
              m(Button, {
                variant: ButtonVariant.Filled,
                label: 'No thanks...',
              }),
              m(Button, {
                intent: Intent.Primary,
                variant: ButtonVariant.Filled,
                label: "Let's go!",
              }),
            ]),
          ]),
        schema: {interactive: true},
      }),
      renderWidgetContainer({
        label: 'CardStack',
        description: `A container component that can be used to display
          multiple Card elements in a vertical stack. Cards placed in this list
          automatically have their borders adjusted to appear as one continuous
          card with thin borders between them.`,
        schema: {
          direction: enumOption('vertical', ['vertical', 'horizontal']),
          interactive: true,
        },
        render: ({direction, interactive}) =>
          m(CardStack, {direction: direction as 'vertical' | 'horizontal'}, [
            m(Card, {interactive}, m(Switch, {label: 'Option 1'})),
            m(Card, {interactive}, m(Switch, {label: 'Option 2'})),
            m(Card, {interactive}, m(Switch, {label: 'Option 3'})),
          ]),
      }),
      renderWidgetContainer({
        label: 'CopyableLink',
        render: ({noicon}) =>
          m(CopyableLink, {
            noicon: arg(noicon, true),
            url: 'https://perfetto.dev/docs/',
          }),
        schema: {
          noicon: false,
        },
      }),
      renderWidgetContainer({
        label: 'CopyToClipboardButton',
        render: (opts) =>
          m(CopyToClipboardButton, {
            textToCopy: 'Text to copy',
            variant: opts.variant as ButtonVariant,
          }),
        schema: {
          label: 'Copy',
          variant: enumOption(
            ButtonVariant.Outlined,
            Object.values(ButtonVariant),
          ),
        },
      }),
      renderWidgetContainer({
        label: 'Portal',
        description: `A portal is a div rendered out of normal flow
          of the hierarchy.`,
        render: (opts) => m(PortalButton, opts),
        schema: {
          absolute: true,
          zIndex: true,
          top: true,
        },
      }),
      renderWidgetContainer({
        label: 'Popup',
        description: `A popup is a nicely styled portal element whose position is
        dynamically updated to appear to float alongside a specific element on
        the page, even as the element is moved and scrolled around.`,
        render: ({position, closeOnEscape, closeOnOutsideClick}) =>
          m(
            Popup,
            {
              trigger: m(Button, {label: 'Toggle Popup'}),
              position: position as PopupPosition,
              closeOnEscape,
              closeOnOutsideClick,
            },
            lorem(),
          ),
        schema: {
          position: enumOption(
            PopupPosition.Auto,
            Object.values(PopupPosition),
          ),
          closeOnEscape: true,
          closeOnOutsideClick: true,
        },
      }),
      renderWidgetContainer({
        label: 'Controlled Popup',
        description: `The open/close state of a controlled popup is passed in via
        the 'isOpen' attribute. This means we can get open or close the popup
        from wherever we like. E.g. from a button inside the popup.
        Keeping this state external also means we can modify other parts of the
        page depending on whether the popup is open or not, such as the text
        on this button.
        Note, this is the same component as the popup above, but used in
        controlled mode.`,
        render: (opts) => m(ControlledPopup, opts),
        schema: {},
      }),
      renderWidgetContainer({
        label: 'Icon',
        render: ({filled, intent}) =>
          m(Icon, {icon: 'star', filled, intent: intent as Intent}),
        schema: {
          filled: false,
          intent: enumOption(Intent.None, Object.values(Intent)),
        },
      }),
      renderWidgetContainer({
        label: 'Tooltip',
        description: `A tooltip is a hover-only, useful as an alternative to the browser's inbuilt 'title' tooltip.`,
        render: ({position, showArrow, offset, edgeOffset}) =>
          m(
            Tooltip,
            {
              trigger: m(Icon, {icon: 'Warning'}),
              position: position as PopupPosition,
              showArrow,
              offset,
              edgeOffset,
            },
            lorem(),
          ),
        schema: {
          position: enumOption(
            PopupPosition.Auto,
            Object.values(PopupPosition),
          ),
          showArrow: true,
          offset: 0,
          edgeOffset: 0,
        },
      }),
      renderWidgetContainer({
        label: 'MultiSelect panel',
        render: ({...rest}) =>
          m(MultiSelect, {
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
            },
            ...rest,
          }),
        schema: {
          repeatCheckedItemsAtTop: false,
          fixedSize: false,
        },
      }),
      renderWidgetContainer({
        label: 'Popup with MultiSelect',
        render: ({icon, ...rest}) =>
          m(PopupMultiSelect, {
            options: Object.entries(options).map(([key, value]) => {
              return {
                id: key,
                name: key,
                checked: value,
              };
            }),
            popupPosition: PopupPosition.Top,
            label: 'Multi Select',
            icon: arg(icon, Icons.LibraryAddCheck),
            onChange: (diffs: MultiSelectDiff[]) => {
              diffs.forEach(({id, checked}) => {
                options[id] = checked;
              });
            },
            ...rest,
          }),
        schema: {
          icon: true,
          showNumSelected: true,
          repeatCheckedItemsAtTop: false,
        },
      }),
      renderWidgetContainer({
        label: 'MultiselectInput',
        description: `Tag input with options`,
        render: () => {
          return m(MultiselectInputDemo);
        },
        schema: {},
      }),
      renderWidgetContainer({
        label: 'Menu',
        schema: {},
        render: () =>
          m(
            Menu,
            m(MenuItem, {label: 'New', icon: 'add'}),
            m(MenuItem, {label: 'Open', icon: 'folder_open'}),
            m(MenuItem, {label: 'Save', icon: 'save', disabled: true}),
            m(MenuDivider),
            m(MenuItem, {label: 'Delete', icon: 'delete'}),
            m(MenuDivider),
            m(MenuTitle, {label: 'Sharing'}),
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
      // renderWidgetContainer({
      //   label: 'PopupMenu',
      //   render: (opts) =>
      //     m(
      //       PopupMenu,
      //       {
      //         trigger: m(Button, {
      //           label: 'Menu',
      //           rightIcon: Icons.ContextMenu,
      //         }),
      //         ...opts,
      //       },
      //       m(MenuTitle, {label: 'File'}),
      //       m(MenuItem, {label: 'New', icon: 'add'}),
      //       m(MenuItem, {label: 'Open', icon: 'folder_open'}),
      //       m(MenuItem, {label: 'Save', icon: 'save', disabled: true}),
      //       m(MenuItem, {label: 'Delete', icon: 'delete'}),
      //       m(MenuDivider),
      //       m(MenuTitle, {label: 'Sharing'}),
      //       m(
      //         MenuItem,
      //         {label: 'Share', icon: 'share'},
      //         m(MenuItem, {label: 'Everyone', icon: 'public'}),
      //         m(MenuItem, {label: 'Friends', icon: 'group'}),
      //         m(
      //           MenuItem,
      //           {label: 'Specific people', icon: 'person_add'},
      //           m(MenuItem, {label: 'Alice', icon: 'person'}),
      //           m(MenuItem, {label: 'Bob', icon: 'person'}),
      //         ),
      //       ),
      //       m(
      //         MenuItem,
      //         {label: 'More', icon: 'more_horiz'},
      //         m(MenuItem, {label: 'Query', icon: 'database'}),
      //         m(MenuItem, {label: 'Download', icon: 'download'}),
      //         m(MenuItem, {label: 'Clone', icon: 'copy_all'}),
      //       ),
      //     ),
      //   schema: {
      //     popupPosition: enumOption(
      //       PopupPosition.Bottom,
      //       Object.values(PopupPosition),
      //     ),
      //   },
      // }),
      renderWidgetContainer({
        label: 'CursorTooltip',
        description: 'A tooltip that follows the mouse around.',
        schema: {},
        render: () => m(CursorTooltipShowcase),
      }),
      renderWidgetContainer({
        label: 'Spinner',
        description: `Simple spinner, rotates forever.
            Width and height match the font size.`,
        render: ({fontSize, easing}) =>
          m('', {style: {fontSize}}, m(Spinner, {easing})),
        schema: {
          fontSize: enumOption('16px', [
            '12px',
            '16px',
            '24px',
            '32px',
            '64px',
            '128px',
          ]),
          easing: false,
        },
      }),
      renderWidgetContainer({
        label: 'Tree',
        description: `Hierarchical tree with left and right values aligned to
        a grid.`,
        schema: {},
        render: (opts) =>
          m(
            Tree,
            opts,
            m(TreeNode, {left: 'Name', right: 'my_event', icon: 'badge'}),
            m(TreeNode, {left: 'CPU', right: '2', icon: 'memory'}),
            m(TreeNode, {
              left: 'Start time',
              right: '1s 435ms',
              icon: 'schedule',
            }),
            m(TreeNode, {left: 'Duration', right: '86ms', icon: 'timer'}),
            m(TreeNode, {
              left: 'SQL',
              right: m(
                PopupMenu,
                {
                  popupPosition: PopupPosition.RightStart,
                  trigger: m(
                    Anchor,
                    {
                      icon: Icons.ContextMenu,
                    },
                    'SELECT * FROM ftrace_event WHERE id = 123',
                  ),
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
            recursiveTreeNode(),
          ),
        wide: true,
      }),
      renderWidgetContainer({
        label: 'Form',
        render: () => renderForm('form'),
        schema: {},
      }),
      renderWidgetContainer({
        label: 'Nested Popups',
        schema: {},
        render: () =>
          m(
            Popup,
            {
              trigger: m(Button, {label: 'Open the popup'}),
            },
            m(ButtonBar, [
              m(
                PopupMenu,
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
            ]),
          ),
      }),
      renderWidgetContainer({
        label: 'Callout',
        schema: {
          intent: enumOption(Intent.None, Object.values(Intent)),
          dismissable: false,
          icon: true,
        },
        render: ({icon, intent, ...rest}) =>
          m(
            Callout,
            {
              ...rest,
              icon: icon ? 'info' : undefined,
              intent: intent as Intent,
            },
            'Lorem ipsum dolor sit amet, consectetur adipiscing elit. ' +
              'Nulla rhoncus tempor neque, sed malesuada eros dapibus vel. ' +
              'Aliquam in ligula vitae tortor porttitor laoreet iaculis ' +
              'finibus est.',
          ),
      }),
      renderWidgetContainer({
        label: 'Editor',
        schema: {},
        render: () =>
          m(Editor, {
            language: 'perfetto-sql',
            onUpdate: (text) => {
              parseAndPrintTree(text);
            },
          }),
      }),
      m(VegaDemo),
      renderWidgetContainer({
        label: 'Form within PopupMenu',
        description: `A form placed inside a popup menu works just fine,
              and the cancel/submit buttons also dismiss the popup. A bit more
              margin is added around it too, which improves the look and feel.`,
        schema: {},
        render: () =>
          m(
            PopupMenu,
            {
              trigger: m(Button, {label: 'Popup!'}),
            },
            m(
              MenuItem,
              {
                label: 'Open form...',
              },
              renderForm('popup-form'),
            ),
          ),
      }),
      renderWidgetContainer({
        label: 'Hotkey',

        render: (opts) => {
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
        schema: {
          hotkey: 'Mod+Shift+P',
          platform: enumOption('auto', ['auto', 'Mac', 'PC']),
        },
      }),
      renderWidgetContainer({
        label: 'Text Paragraph',
        description: `A basic formatted text paragraph with wrapping. If
              it is desirable to preserve the original text format/line breaks,
              set the compressSpace attribute to false.`,
        render: (opts) => {
          return m(TextParagraph, {
            text: `Lorem ipsum dolor sit amet, consectetur adipiscing
                         elit. Nulla rhoncus tempor neque, sed malesuada eros
                         dapibus vel. Aliquam in ligula vitae tortor porttitor
                         laoreet iaculis finibus est.`,
            compressSpace: opts.compressSpace,
          });
        },
        schema: {
          compressSpace: true,
        },
      }),
      renderWidgetContainer({
        label: 'Multi Paragraph Text',
        description: `A wrapper for multiple paragraph widgets.`,
        schema: {},
        render: () => {
          return m(
            MultiParagraphText,
            m(TextParagraph, {
              text: `Lorem ipsum dolor sit amet, consectetur adipiscing
                         elit. Nulla rhoncus tempor neque, sed malesuada eros
                         dapibus vel. Aliquam in ligula vitae tortor porttitor
                         laoreet iaculis finibus est.`,
              compressSpace: true,
            }),
            m(TextParagraph, {
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
      renderWidgetContainer({
        label: 'Modal',
        description: `Shows a dialog box in the center of the screen over the
                      top of other elements.`,
        schema: {},
        render: () =>
          m(Button, {
            label: 'Show Modal',
            onclick: () => {
              showModal({
                title: 'Attention',
                icon: Icons.Help,
                content: () => [
                  m('', 'This is a modal dialog'),
                  m(
                    Popup,
                    {
                      trigger: m(Button, {
                        variant: ButtonVariant.Filled,
                        label: 'Open Popup',
                      }),
                    },
                    'Popup content',
                  ),
                ],
                buttons: [
                  {
                    text: 'Cancel',
                  },
                  {
                    text: 'OK',
                    primary: true,
                  },
                ],
              });
            },
          }),
      }),
      renderWidgetContainer({
        label: 'Advanced Modal',
        description: `A helper for modal dialog.`,
        schema: {},
        render: () => m(ModalShowcase),
      }),
      renderWidgetContainer({
        label: 'TreeTable',
        description: `Hierarchical tree with multiple columns`,
        schema: {},
        render: () => {
          const attrs: TreeTableAttrs<File> = {
            rows: files,
            getChildren: (file) => file.children,
            columns: [
              {name: 'Name', getData: (file) => file.name},
              {name: 'Size', getData: (file) => file.size},
              {name: 'Date', getData: (file) => file.date},
            ],
          };
          return m(TreeTable<File>, attrs);
        },
      }),
      renderWidgetContainer({
        label: 'VirtualTable',
        description: `Virtualized table for efficient rendering of large datasets`,
        schema: {},
        render: () => {
          const attrs: VirtualTableAttrs = {
            columns: [
              {header: 'x', width: '4em'},
              {header: 'x^2', width: '8em'},
            ],
            rows: virtualTableData.rows,
            firstRowOffset: virtualTableData.offset,
            rowHeight: 20,
            numRows: 500_000,
            style: {height: '200px'},
            onReload: (rowOffset, rowCount) => {
              const rows = [];
              for (let i = rowOffset; i < rowOffset + rowCount; i++) {
                rows.push({id: i, cells: [i, i ** 2]});
              }
              virtualTableData = {
                offset: rowOffset,
                rows,
              };
            },
          };
          return m(VirtualTable, attrs);
        },
      }),
      renderWidgetContainer({
        label: 'Tag Input',
        description: `
          TagInput displays Tag elements inside an input, followed by an
          interactive text input. The container is styled to look like a
          TextInput, but the actual editable element appears after the last tag.
          Clicking anywhere on the container will focus the text input.`,
        schema: {},
        render: () => m(TagInputDemo),
      }),
      renderWidgetContainer({
        label: 'Middle Ellipsis',
        description: `
          Sometimes the start and end of a bit of text are more important than
          the middle. This element puts the ellipsis in the midde if the content
          is too wide for its container.`,
        render: (opts) =>
          m(
            'div',
            {style: {width: Boolean(opts.squeeze) ? '150px' : '450px'}},
            m(MiddleEllipsis, {
              text: 'Lorem ipsum dolor sit amet, consectetur adipiscing elit',
            }),
          ),
        schema: {
          squeeze: false,
        },
      }),
      renderWidgetContainer({
        label: 'Chip',
        description: `A little chip or tag`,
        render: (opts) => {
          const {icon, intent, ...rest} = opts;
          return m(
            Stack,
            {orientation: 'horizontal'},
            m(Chip, {
              label: 'Foo',
              icon: icon === true ? 'info' : undefined,
              intent: intent as Intent,
              ...rest,
            }),
            m(Chip, {
              label: 'Bar',
              icon: icon === true ? 'warning' : undefined,
              intent: intent as Intent,
              ...rest,
            }),
            m(Chip, {
              label: 'Baz',
              icon: icon === true ? 'error' : undefined,
              intent: intent as Intent,
              ...rest,
            }),
          );
        },
        schema: {
          intent: enumOption(Intent.None, Object.values(Intent)),
          icon: true,
          compact: false,
          rounded: false,
          disabled: false,
          removable: true,
        },
      }),
      renderWidgetContainer({
        label: 'TrackShell',
        description: `The Mithril parts of a track (the shell, mainly).`,
        render: (opts) => {
          const {buttons, chips, multipleTracks, error, ...rest} = opts;
          const dummyButtons = () => [
            m(Button, {icon: 'info', compact: true}),
            m(Button, {icon: 'settings', compact: true}),
          ];
          const dummyChips = () => ['foo', 'bar'];

          const renderTrack = (children?: m.Children) =>
            m(
              TrackShell,
              {
                buttons: Boolean(buttons) ? dummyButtons() : undefined,
                chips: Boolean(chips) ? dummyChips() : undefined,
                error: Boolean(error)
                  ? new Error('An error has occurred')
                  : undefined,
                ...rest,
              },
              children,
            );

          return m(
            '',
            {
              style: {width: '500px', boxShadow: '0px 0px 1px 1px lightgray'},
            },
            Boolean(multipleTracks)
              ? [renderTrack(), renderTrack(), renderTrack()]
              : renderTrack(),
          );
        },
        schema: {
          title: 'This is the title of the track',
          subtitle: 'This is the subtitle of the track',
          buttons: true,
          chips: true,
          heightPx: 32,
          collapsible: true,
          collapsed: true,
          summary: false,
          highlight: false,
          error: false,
          multipleTracks: false,
          reorderable: false,
          depth: 0,
          lite: false,
        },
      }),
      renderWidgetContainer({
        label: 'Virtual Overlay Canvas',
        description: `A scrolling container that draws a virtual canvas over
          the top of it's content and keeps it in the viewport to make it appear
          like there is one big canvas over the top of the content.`,
        render: () => {
          const width = 200;
          const rowCount = 65536;
          const rowHeight = 20;
          return m(
            VirtualOverlayCanvas,
            {
              className: 'pf-virtual-canvas',
              overflowY: 'auto',
              onCanvasRedraw({ctx, canvasRect}) {
                ctx.strokeStyle = 'red';
                ctx.lineWidth = 1;

                ctx.font = '20px Arial';
                ctx.fillStyle = 'black';
                ctx.textAlign = 'left';
                ctx.textBaseline = 'top';

                for (let i = 0; i < rowCount; i++) {
                  const rect = {
                    left: 0,
                    top: i * rowHeight,
                    right: width,
                    bottom: i * rowHeight + rowHeight,
                  };
                  if (canvasRect.overlaps(rect)) {
                    ctx.strokeRect(0, i * rowHeight, width, rowHeight);
                    ctx.fillText(`Row: ${i}`, 0, i * rowHeight);
                  }
                }
              },
            },
            m('', {
              style: {height: `${rowCount * rowHeight}px`, width: `${width}px`},
            }),
          );
        },
        schema: {},
      }),

      renderWidgetContainer({
        label: 'SplitPanel',
        description: `Resizeable split panel with optional tabs.`,
        render: (opts) => {
          return m(
            '',
            {
              style: {
                height: '400px',
                width: '400px',
                border: 'solid 2px gray',
              },
            },
            m(
              SplitPanel,
              {
                leftHandleContent: [
                  Boolean(opts.leftContent) && m(Button, {icon: 'Menu'}),
                ],
                drawerContent: 'Drawer Content',
                tabs:
                  Boolean(opts.tabs) &&
                  m(
                    '.pf-split-panel__tabs',
                    m(
                      Tab,
                      {active: true, hasCloseButton: opts.showCloseButtons},
                      'Foo',
                    ),
                    m(Tab, {hasCloseButton: opts.showCloseButtons}, 'Bar'),
                  ),
              },
              'Main Content',
            ),
          );
        },
        schema: {
          leftContent: true,
          tabs: true,
          showCloseButtons: true,
        },
      }),

      renderWidgetContainer({
        label: 'Grid',
        description: `
          Presentation layer for grid/table elements. Defines a consistent look
          and feel for grids but leaves the data and interaction handling to the
          user. For instance, it provides slots and callbacks for sorting, column
          reordering and column level aggregations, but doesn't have any
          opinions about the data or how they should be manipulated.
        `,
        render: ({reorderable, ...rest}) =>
          m(
            '',
            {style: {height: '400px', width: '400px', overflow: 'hidden'}},
            m(Grid, rest, [
              m(GridHeader, [
                m(GridRow, [
                  m(
                    GridHeaderCell,
                    {
                      key: 'id',
                      sort: 'ASC',
                      onSort: () => {},
                      aggregation: {
                        left: 'Σ',
                        right: 15,
                      },
                      reorderable: reorderable ? {handle: 'left'} : undefined,
                    },
                    'ID',
                  ),
                  m(
                    GridHeaderCell,
                    {
                      key: 'lang',
                      onSort: () => {},
                      menuItems: [
                        m(MenuItem, {label: 'Filter nulls'}),
                        m(MenuItem, {label: 'Show only nulls'}),
                      ],
                      reorderable: reorderable ? {handle: 'left'} : undefined,
                      thickRightBorder: true,
                    },
                    'Language',
                  ),
                  m(
                    GridHeaderCell,
                    {
                      key: 'year',
                      aggregation: {
                        left: 'AVG',
                        right: 1998.3,
                      },
                      reorderable: reorderable ? {handle: 'right'} : undefined,
                    },
                    'Year',
                  ),
                  m(
                    GridHeaderCell,
                    {
                      key: 'creator',
                      reorderable: reorderable ? {handle: 'right'} : undefined,
                    },
                    'Creator',
                  ),
                  m(
                    GridHeaderCell,
                    {
                      key: 'typing',
                      reorderable: reorderable ? {handle: 'right'} : undefined,
                    },
                    'Typing',
                  ),
                ]),
              ]),
              m(GridBody, [
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 1),
                  m(
                    GridDataCell,
                    {
                      menuItems: [
                        m(MenuItem, {label: 'Filter to "TypeScript"'}),
                        m(MenuItem, {label: 'Exclude "TypeScript"'}),
                      ],
                      thickRightBorder: true,
                    },
                    'TypeScript',
                  ),
                  m(GridDataCell, {align: 'right'}, 2012),
                  m(GridDataCell, 'Microsoft'),
                  m(GridDataCell, 'Static'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 2),
                  m(GridDataCell, {thickRightBorder: true}, 'JavaScript'),
                  m(GridDataCell, {align: 'right'}, 1995),
                  m(GridDataCell, 'Brendan Eich'),
                  m(GridDataCell, 'Dynamic'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 3),
                  m(GridDataCell, {thickRightBorder: true}, 'Python'),
                  m(GridDataCell, {align: 'right'}, 1991),
                  m(GridDataCell, 'Guido van Rossum'),
                  m(GridDataCell, 'Dynamic'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 4),
                  m(GridDataCell, {thickRightBorder: true}, 'Java'),
                  m(GridDataCell, {align: 'right'}, 1995),
                  m(GridDataCell, 'James Gosling'),
                  m(GridDataCell, 'Static'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 5),
                  m(GridDataCell, {thickRightBorder: true}, 'C++'),
                  m(GridDataCell, {align: 'right'}, 1985),
                  m(GridDataCell, 'Bjarne Stroustrup'),
                  m(GridDataCell, 'Static'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 6),
                  m(GridDataCell, {thickRightBorder: true}, 'Go'),
                  m(GridDataCell, {align: 'right'}, 2009),
                  m(GridDataCell, 'Google'),
                  m(GridDataCell, 'Static'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 7),
                  m(GridDataCell, {thickRightBorder: true}, 'Rust'),
                  m(GridDataCell, {align: 'right'}, 2010),
                  m(GridDataCell, 'Graydon Hoare'),
                  m(GridDataCell, 'Static'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 8),
                  m(GridDataCell, {thickRightBorder: true}, 'Ruby'),
                  m(GridDataCell, {align: 'right'}, 1995),
                  m(GridDataCell, 'Yukihiro Matsumoto'),
                  m(GridDataCell, 'Dynamic'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 9),
                  m(GridDataCell, {thickRightBorder: true}, 'Swift'),
                  m(GridDataCell, {align: 'right'}, 2014),
                  m(GridDataCell, 'Apple'),
                  m(GridDataCell, 'Static'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 10),
                  m(GridDataCell, {thickRightBorder: true}, 'Kotlin'),
                  m(GridDataCell, {align: 'right'}, 2011),
                  m(GridDataCell, 'JetBrains'),
                  m(GridDataCell, 'Static'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 11),
                  m(GridDataCell, {thickRightBorder: true}, 'PHP'),
                  m(GridDataCell, {align: 'right'}, 1995),
                  m(GridDataCell, 'Rasmus Lerdorf'),
                  m(GridDataCell, 'Dynamic'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 12),
                  m(GridDataCell, {thickRightBorder: true}, 'C#'),
                  m(GridDataCell, {align: 'right'}, 2000),
                  m(GridDataCell, 'Microsoft'),
                  m(GridDataCell, 'Static'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 13),
                  m(GridDataCell, {thickRightBorder: true}, 'Perl'),
                  m(GridDataCell, {align: 'right'}, 1987),
                  m(GridDataCell, 'Larry Wall'),
                  m(GridDataCell, 'Dynamic'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 14),
                  m(GridDataCell, {thickRightBorder: true}, 'Scala'),
                  m(GridDataCell, {align: 'right'}, 2004),
                  m(GridDataCell, 'Martin Odersky'),
                  m(GridDataCell, 'Static'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 15),
                  m(GridDataCell, {thickRightBorder: true}, 'Haskell'),
                  m(GridDataCell, {align: 'right'}, 1990),
                  m(GridDataCell, 'Lennart Augustsson, et al.'),
                  m(GridDataCell, 'Static'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 16),
                  m(GridDataCell, {thickRightBorder: true}, 'Lua'),
                  m(GridDataCell, {align: 'right'}, 1993),
                  m(GridDataCell, 'Roberto Ierusalimschy, et al.'),
                  m(GridDataCell, 'Dynamic'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 17),
                  m(GridDataCell, {thickRightBorder: true}, 'Dart'),
                  m(GridDataCell, {align: 'right'}, 2011),
                  m(GridDataCell, 'Google'),
                  m(GridDataCell, 'Static'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 18),
                  m(GridDataCell, {thickRightBorder: true}, 'Elixir'),
                  m(GridDataCell, {align: 'right'}, 2012),
                  m(GridDataCell, 'José Valim'),
                  m(GridDataCell, 'Dynamic'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 19),
                  m(GridDataCell, {thickRightBorder: true}, 'Clojure'),
                  m(GridDataCell, {align: 'right'}, 2007),
                  m(GridDataCell, 'Rich Hickey'),
                  m(GridDataCell, 'Dynamic'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 20),
                  m(GridDataCell, {thickRightBorder: true}, 'F#'),
                  m(GridDataCell, {align: 'right'}, 2005),
                  m(GridDataCell, 'Microsoft'),
                  m(GridDataCell, 'Static'),
                ]),
                m(GridRow, [
                  m(GridDataCell, {align: 'right'}, 21),
                  m(GridDataCell, {thickRightBorder: true}, 'Lisp'),
                  m(GridDataCell, {align: 'right'}, 1958),
                  m(GridDataCell, 'John McCarthy'),
                  m(GridDataCell, 'Dynamic'),
                ]),
              ]),
            ]),
          ),
        schema: {
          fillHeight: true,
          reorderable: true,
        },
      }),

      renderWidgetContainer({
        label: 'DataGrid (memory backed)',
        description: `An interactive data explorer and viewer.`,
        render: ({readonlyFilters, readonlySorting, aggregation, ...rest}) =>
          m(DataGrid, {
            ...rest,
            filters: readonlyFilters ? [] : undefined,
            sorting: readonlySorting ? {direction: 'UNSORTED'} : undefined,
            columns: [
              {
                name: 'id',
                title: 'ID',
                aggregation: aggregation ? 'COUNT' : undefined,
              },
              {name: 'ts', title: 'Timestamp'},
              {
                name: 'dur',
                aggregation: aggregation ? 'SUM' : undefined,
                title: 'Duration',
              },
              {name: 'name', title: 'Name'},
              {name: 'data', title: 'Data'},
              {name: 'maybe_null', title: 'Maybe Null?'},
              {name: 'category', title: 'Category'},
            ],
            data: [
              {
                id: 1,
                name: 'foo',
                ts: 123n,
                dur: 16n,
                data: new Uint8Array(),
                maybe_null: null,
                category: 'aaa',
              },
              {
                id: 2,
                name: 'bar',
                ts: 185n,
                dur: 4n,
                data: new Uint8Array([1, 2, 3]),
                maybe_null: 'Non null',
                category: 'aaa',
              },
              {
                id: 3,
                name: 'baz',
                ts: 575n,
                dur: 12n,
                data: new Uint8Array([1, 2, 3]),
                maybe_null: null,
                category: 'aaa',
              },
            ],
          }),
        schema: {
          showFiltersInToolbar: true,
          readonlyFilters: false,
          readonlySorting: false,
          aggregation: false,
        },
      }),

      renderWidgetContainer({
        label: 'DataGrid (query backed)',
        description: `An interactive data explorer and viewer - fetched from SQL.`,
        render: ({readonlyFilters, readonlySorting, aggregation, ...rest}) => {
          if (trace) {
            return m(QueryDataGrid, {
              ...rest,
              engine: trace.engine,
              query: `
                SELECT
                  ts.id as id,
                  dur,
                  state,
                  thread.name as thread_name,
                  dur,
                  io_wait,
                  ucpu
                FROM thread_state ts
                JOIN thread USING(utid)
              `,
              filters: readonlyFilters ? [] : undefined,
              sorting: readonlySorting ? {direction: 'UNSORTED'} : undefined,
              columns: [
                {
                  name: 'id',
                  title: 'ID',
                  aggregation: aggregation ? 'COUNT' : undefined,
                },
                {
                  name: 'dur',
                  title: 'Duration',
                  aggregation: aggregation ? 'SUM' : undefined,
                },
                {name: 'state', title: 'State'},
                {name: 'thread_name', title: 'Thread'},
                {name: 'ucpu', title: 'CPU'},
                {name: 'io_wait', title: 'IO Wait'},
              ],
              maxRowsPerPage: 10,
            });
          } else {
            return 'Load a trace to start';
          }
        },
        schema: {
          showFiltersInToolbar: true,
          readonlyFilters: false,
          readonlySorting: false,
          aggregation: false,
        },
      }),

      renderWidgetContainer({
        label: 'TabStrip',
        description: `A simple tab strip`,
        render: () => {
          return m(TabStrip, {
            tabs: [
              {key: 'foo', title: 'Foo'},
              {key: 'bar', title: 'Bar'},
              {key: 'baz', title: 'Baz'},
            ],
            currentTabKey: currentTab,
            onTabChange: (key) => {
              currentTab = key;
            },
          });
        },
        schema: {},
      }),

      renderWidgetContainer({
        label: 'CodeSnippet',
        render: ({wide}) =>
          m(CodeSnippet, {
            language: 'SQL',
            text: Boolean(wide)
              ? 'SELECT a_very_long_column_name, another_super_long_column_name, yet_another_ridiculously_long_column_name FROM a_table_with_an_unnecessarily_long_name WHERE some_condition_is_true AND another_condition_is_also_true;'
              : 'SELECT * FROM slice LIMIT 10;',
          }),
        schema: {
          wide: false,
        },
      }),
    );
  }
}

function CursorTooltipShowcase() {
  let show = false;
  return {
    view() {
      return m(
        '',
        {
          style: {
            width: '150px',
            height: '150px',
            border: '1px dashed gray',
            userSelect: 'none',
            color: 'gray',
            textAlign: 'center',
            lineHeight: '150px',
          },
          onmouseover: () => (show = true),
          onmouseout: () => (show = false),
        },
        'Hover here...',
        show && m(CursorTooltip, 'Hi!'),
      );
    },
  };
}

function MultiselectInputDemo() {
  const options = [
    'foo',
    'bar',
    'baz',
    'qux',
    'quux',
    'corge',
    'grault',
    'garply',
    'waldo',
    'fred',
  ];
  let selectedOptions: string[] = [];
  return {
    view() {
      return m(MultiselectInput, {
        options: options.map((o) => ({key: o, label: o})),
        selectedOptions,
        onOptionAdd: (key) => selectedOptions.push(key),
        onOptionRemove: (key) => {
          selectedOptions = selectedOptions.filter((x) => x !== key);
        },
      });
    },
  };
}

type QueryDataGridAttrs = Omit<DataGridAttrs, 'data'> & {
  readonly query: string;
  readonly engine: Engine;
};

function QueryDataGrid(vnode: m.Vnode<QueryDataGridAttrs>) {
  const dataSource = new SQLDataSource(vnode.attrs.engine, vnode.attrs.query);

  return {
    view({attrs}: m.Vnode<QueryDataGridAttrs>) {
      return m(DataGrid, {...attrs, data: dataSource});
    },
  };
}

class ModalShowcase implements m.ClassComponent {
  private static counter = 0;

  private static log(txt: string) {
    const mwlogs = document.getElementById('mwlogs');
    if (!mwlogs || !(mwlogs instanceof HTMLTextAreaElement)) return;
    const time = new Date().toLocaleTimeString();
    mwlogs.value += `[${time}] ${txt}\n`;
    mwlogs.scrollTop = mwlogs.scrollHeight;
  }

  private static showModalDialog(staticContent = false) {
    const id = `N=${++ModalShowcase.counter}`;
    ModalShowcase.log(`Open ${id}`);
    const logOnClose = () => ModalShowcase.log(`Close ${id}`);

    let content;
    if (staticContent) {
      content = m(
        '.pf-modal-pre',
        'Content of the modal dialog.\nEnd of content',
      );
    } else {
      // The humble counter is basically the VDOM 'Hello world'!
      function CounterComponent() {
        let counter = 0;
        return {
          view: () => {
            return m(
              '',
              `Counter value: ${counter}`,
              m(Button, {
                label: 'Increment Counter',
                onclick: () => ++counter,
              }),
            );
          },
        };
      }
      content = () => m(CounterComponent);
    }
    const closePromise = showModal({
      title: `Modal dialog ${id}`,
      buttons: [
        {text: 'OK', action: () => ModalShowcase.log(`OK ${id}`)},
        {text: 'Cancel', action: () => ModalShowcase.log(`Cancel ${id}`)},
        {
          text: 'Show another now',
          action: () => ModalShowcase.showModalDialog(),
        },
        {
          text: 'Show another in 2s',
          action: () => setTimeout(() => ModalShowcase.showModalDialog(), 2000),
        },
      ],
      content,
    });
    closePromise.then(logOnClose);
  }

  view() {
    return m(
      'div',
      {
        style: {
          'display': 'flex',
          'flex-direction': 'column',
          'width': '100%',
        },
      },
      m('textarea', {
        id: 'mwlogs',
        readonly: 'readonly',
        rows: '8',
        placeholder: 'Logs will appear here',
      }),
      m('input[type=button]', {
        value: 'Show modal (static)',
        onclick: () => ModalShowcase.showModalDialog(true),
      }),
      m('input[type=button]', {
        value: 'Show modal (dynamic)',
        onclick: () => ModalShowcase.showModalDialog(false),
      }),
    );
  }
} // class ModalShowcase

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
    m(FormLabel, {for: `${id}-foo`}, 'Foo'),
    m(TextInput, {id: `${id}-foo`}),
    m(FormLabel, {for: `${id}-bar`}, 'Bar'),
    m(Select, {id: `${id}-bar`}, [
      m('option', {value: 'foo', label: 'Foo'}),
      m('option', {value: 'bar', label: 'Bar'}),
      m('option', {value: 'baz', label: 'Baz'}),
    ]),
  );
}
