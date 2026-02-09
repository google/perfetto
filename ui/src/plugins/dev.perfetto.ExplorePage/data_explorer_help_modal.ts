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
import {Hotkey} from '../../base/hotkeys';
import {HotkeyGlyphs, Keycap} from '../../widgets/hotkey_glyphs';
import {showModal} from '../../widgets/modal';
import {nodeRegistry} from './query_builder/node_registry';

interface HelpEntry {
  readonly keys: m.Children;
  readonly description: string;
}

interface HelpSection {
  readonly title: string;
  readonly entries: ReadonlyArray<HelpEntry>;
}

export function showDataExplorerHelp() {
  return showModal({
    title: 'Data Explorer Help',
    content: () => m(DataExplorerHelpContent),
  });
}

function keycap(glyph: m.Children): m.Children {
  return m(Keycap, {spacing: 'large'}, glyph);
}

function hotkey(combo: Hotkey): m.Children {
  return m(HotkeyGlyphs, {spacing: 'large', hotkey: combo});
}

function getNodeCreationEntries(): HelpEntry[] {
  return nodeRegistry
    .list()
    .filter(([_, desc]) => desc.type === 'source' && desc.hotkey)
    .sort((a, b) => a[1].name.localeCompare(b[1].name))
    .flatMap(([_, desc]) => {
      const hk = desc.hotkey;
      if (hk === undefined) return [];
      return {
        keys: keycap(hk.toUpperCase()),
        description: `Add ${desc.name} node`,
      };
    });
}

function getHelpSections(): HelpSection[] {
  return [
    {
      title: 'Node Creation',
      entries: getNodeCreationEntries(),
    },
    {
      title: 'Graph Editing',
      entries: [
        {
          keys: [keycap('Delete'), ' / ', keycap('Backspace')],
          description: 'Delete selected node(s)',
        },
        {keys: hotkey('Mod+C'), description: 'Copy selected node(s)'},
        {keys: hotkey('Mod+V'), description: 'Paste copied node(s)'},
      ],
    },
    {
      title: 'Undo / Redo',
      entries: [
        {keys: hotkey('Mod+Z'), description: 'Undo'},
        {keys: hotkey('Mod+Shift+Z'), description: 'Redo'},
        {keys: hotkey('Mod+Y'), description: 'Redo (alternative)'},
      ],
    },
    {
      title: 'Query Execution',
      entries: [
        {keys: hotkey('Mod+Enter'), description: 'Execute selected node'},
      ],
    },
    {
      title: 'Import / Export',
      entries: [
        {keys: keycap('I'), description: 'Import graph from JSON file'},
        {keys: keycap('E'), description: 'Export graph to JSON file'},
      ],
    },
    {
      title: 'Navigation',
      entries: [
        {
          keys: [keycap('W'), keycap('A'), keycap('S'), keycap('D')],
          description: 'Pan canvas',
        },
        {keys: 'Scroll', description: 'Pan canvas'},
        {
          keys: [keycap('Ctrl'), ' / ', keycap('Cmd'), ' + Scroll'],
          description: 'Zoom in/out',
        },
      ],
    },
    {
      title: 'Selection',
      entries: [
        {keys: 'Click', description: 'Select node'},
        {
          keys: [keycap('Ctrl'), ' / ', keycap('Cmd'), ' + Click'],
          description: 'Toggle node in selection',
        },
        {
          keys: [keycap('Shift'), ' + Click'],
          description: 'Add node to selection',
        },
        {
          keys: [keycap('Shift'), ' + Drag'],
          description: 'Rectangle select',
        },
        {keys: 'Click canvas', description: 'Deselect all'},
        {keys: 'Drag node', description: 'Move node'},
      ],
    },
  ];
}

function renderSection(section: HelpSection): m.Children {
  return [
    m('h2', section.title),
    m(
      'table',
      section.entries.map((entry) =>
        m('tr', m('td', entry.keys), m('td', entry.description)),
      ),
    ),
  ];
}

class DataExplorerHelpContent implements m.ClassComponent {
  view(): m.Children {
    return m('.pf-help-modal', getHelpSections().map(renderSection));
  }
}
