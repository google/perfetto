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
import {App} from '../../public/app';
import {anchor} from './demos/anchor_demo';
import {renderButtonDemo} from './demos/button_demo';
import {renderButtonGroupDemo} from './demos/button_group_demo';
import {renderCallout} from './demos/callout_demo';
import {renderCard} from './demos/card_demo';
import {cardStack} from './demos/cardstack_demo';
import {renderCheckbox} from './demos/checkbox_demo';
import {renderChip} from './demos/chip_demo';
import {renderCodeSnippet} from './demos/code_snippet_demo';
import {renderCopyableLink} from './demos/copyable_link_demo';
import {cursorTooltip} from './demos/cursor_tooltip_demo';
import {renderDataGrid} from './demos/datagrid_demo';
import {renderEditor} from './demos/editor_demo';
import {renderEmptyState} from './demos/empty_state_demo';
import {renderForm} from './demos/form_demo';
import {renderGrid} from './demos/grid_demo';
import {renderHotkey} from './demos/hotkey_demo';
import {renderIcon} from './demos/icon_demo';
import {renderMenu} from './demos/menu_demo';
import {renderMiddleEllipsis} from './demos/middle_ellipsis_demo';
import {renderModal} from './demos/modal_demo';
import {renderMultiselect} from './demos/multiselect_demo';
import {renderNodeGraph} from './demos/nodegraph_demo';
import {renderPopup} from './demos/popup_demo';
import {popupMenuDemo} from './demos/popup_menu_demo';
import {renderPortal} from './demos/portal_demo';
import {renderResizeHandle} from './demos/resize_handle_demo';
import {segmentedButtons} from './demos/segmented_buttons_demo';
import {renderSelect} from './demos/select_demo';
import {renderSpinner} from './demos/spinner_demo';
import {renderSplitPanel} from './demos/split_panel_demo';
import {renderSwitch} from './demos/switch_demo';
import {renderTabStrip} from './demos/tabstrip_demo';
import {renderTagInput} from './demos/tag_input_demo';
import {renderTextInput} from './demos/text_input_demo';
import {renderTextParagraph} from './demos/text_paragraph_demo';
import {renderTooltip} from './demos/tooltip_demo';
import {renderTrackShell} from './demos/track_shell_demo';
import {renderTree} from './demos/tree_demo';
import {renderTreeTable} from './demos/treetable_demo';
import {renderVegaView} from './demos/vega_view_demo';
import {renderVirtualCanvas} from './demos/virtual_canvas_demo';

interface WidgetSection {
  readonly id: string;
  readonly label: string;
  readonly view: (app: App) => m.Children;
}

const WIDGET_SECTIONS: WidgetSection[] = [
  {id: 'anchor', label: 'Anchor', view: anchor},
  {id: 'button', label: 'Button', view: renderButtonDemo},
  {id: 'button-group', label: 'ButtonGroup', view: renderButtonGroupDemo},
  {id: 'callout', label: 'Callout', view: renderCallout},
  {id: 'card-stack', label: 'CardStack', view: cardStack},
  {id: 'card', label: 'Card', view: renderCard},
  {id: 'checkbox', label: 'Checkbox', view: renderCheckbox},
  {id: 'chip', label: 'Chip', view: renderChip},
  {id: 'codesnippet', label: 'CodeSnippet', view: renderCodeSnippet},
  {id: 'copyablelink', label: 'CopyableLink', view: renderCopyableLink},
  {id: 'cursor-tooltip', label: 'CursorTooltip', view: cursorTooltip},
  {id: 'datagrid', label: 'DataGrid', view: renderDataGrid},
  {id: 'editor', label: 'Editor', view: renderEditor},
  {id: 'emptystate', label: 'EmptyState', view: renderEmptyState},
  {id: 'form', label: 'Form', view: renderForm},
  {id: 'grid', label: 'Grid', view: renderGrid},
  {id: 'hotkey', label: 'Hotkey', view: renderHotkey},
  {id: 'icon', label: 'Icon', view: renderIcon},
  {id: 'menu', label: 'Menu', view: renderMenu},
  {id: 'middleellipsis', label: 'MiddleEllipsis', view: renderMiddleEllipsis},
  {id: 'modal', label: 'Modal', view: renderModal},
  {id: 'multiselect', label: 'Multiselect', view: renderMultiselect},
  {id: 'nodegraph', label: 'NodeGraph', view: renderNodeGraph},
  {id: 'popup', label: 'Popup', view: renderPopup},
  {id: 'popup-menu', label: 'PopupMenu', view: popupMenuDemo},
  {id: 'portal', label: 'Portal', view: renderPortal},
  {id: 'resize-handle', label: 'ResizeHandle', view: renderResizeHandle},
  {id: 'segmented-buttons', label: 'SegmentedButtons', view: segmentedButtons},
  {id: 'select', label: 'Select', view: renderSelect},
  {id: 'spinner', label: 'Spinner', view: renderSpinner},
  {id: 'split-panel', label: 'Split Panel', view: renderSplitPanel},
  {id: 'switch', label: 'Switch', view: renderSwitch},
  {id: 'tabstrip', label: 'TabStrip', view: renderTabStrip},
  {id: 'taginput', label: 'TagInput', view: renderTagInput},
  {id: 'textinput', label: 'TextInput', view: renderTextInput},
  {id: 'textparagraph', label: 'TextParagraph', view: renderTextParagraph},
  {id: 'tooltip', label: 'Tooltip', view: renderTooltip},
  {id: 'trackshell', label: 'TrackShell', view: renderTrackShell},
  {id: 'tree', label: 'Tree', view: renderTree},
  {id: 'treetable', label: 'TreeTable', view: renderTreeTable},
  {id: 'vegaview', label: 'VegaView', view: renderVegaView},
  {id: 'virtualcanvas', label: 'VirtualCanvas', view: renderVirtualCanvas},
];

export interface WidgetsPageAttrs {
  readonly app: App;
  readonly subpage?: string;
}

export class WidgetsPage implements m.ClassComponent<WidgetsPageAttrs> {
  view({attrs}: m.Vnode<WidgetsPageAttrs>) {
    const currentSection = WIDGET_SECTIONS.find(
      (s) => `/${s.id}` === attrs.subpage,
    );

    return m(
      '.pf-widgets-page',
      // Left sidebar menu
      m(
        '.pf-widgets-page__menu',
        {key: 'widgets-menu'},
        m(
          'nav',
          WIDGET_SECTIONS.map((sec) =>
            m(
              '.pf-widgets-page__menu-item',
              {
                className: classNames(sec === currentSection && 'pf-active'),
                onclick: () => {
                  window.location.hash = `#!/widgets/${sec.id}`;
                },
              },
              sec.label,
            ),
          ),
        ),
      ),
      // Main content area
      m(
        '.pf-widgets-page__content-container',
        {key: currentSection ? currentSection.id : 'no-section'},
        m(
          '.pf-widgets-page__content',
          currentSection
            ? currentSection.view(attrs.app)
            : m(
                '.pf-widget-intro',
                m('h1', 'Widgets Showcase'),
                m('p', [
                  'This showcase demonstrates the reusable UI components available in the Perfetto UI framework.',
                ]),
                m('p', [
                  'Click on a widget name in the menu on the left to view interactive demos and examples.',
                ]),
              ),
        ),
      ),
    );
  }
}
