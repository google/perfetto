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

// A categorized searchable preset picker, rendered as an inline panel.
// Used both on the rack canvas (add a preset as a new instrument) and
// on the instrument editor canvas (replace current instrument internals).

import m from 'mithril';
import {PresetEntry, PresetLibrary} from './preset_library';

const CATEGORY_HUES: Record<string, number> = {
  drum: 0,
  bass: 210,
  lead: 35,
  pad: 280,
  fx: 320,
  strings: 180,
  organ: 50,
};

export interface PresetPickerAttrs {
  library: PresetLibrary;
  /** Called when the user picks a preset. */
  onPick: (entry: PresetEntry) => void;
  /** Called when the user closes the picker (Escape, outside click). */
  onClose: () => void;
}

export class PresetPicker implements m.ClassComponent<PresetPickerAttrs> {
  private query = '';
  private selectedCategory: string | null = null;

  view(vnode: m.Vnode<PresetPickerAttrs>) {
    const {library, onPick, onClose} = vnode.attrs;

    const entries = this.query.trim()
      ? library.search(this.query)
      : (this.selectedCategory
          ? library.byCategory().get(this.selectedCategory) ?? []
          : library.all());

    const categories = library.categories();

    return m('.preset-picker-overlay', {
      style: {
        position: 'absolute',
        top: '0',
        left: '0',
        right: '0',
        bottom: '0',
        background: 'rgba(0, 0, 0, 0.35)',
        zIndex: '100',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
      },
      onclick: (e: MouseEvent) => {
        if (e.target === e.currentTarget) onClose();
      },
    },
      m('.preset-picker', {
        style: {
          width: '680px',
          maxHeight: '520px',
          background: 'white',
          borderRadius: '6px',
          boxShadow: '0 4px 24px rgba(0,0,0,0.25)',
          display: 'flex',
          flexDirection: 'column',
          overflow: 'hidden',
        },
      },
        // Header.
        m('.preset-picker-header', {
          style: {
            padding: '12px 16px',
            borderBottom: '1px solid #e0e0e0',
            display: 'flex',
            alignItems: 'center',
            gap: '12px',
          },
        },
          m('span',
            {style: {fontWeight: 'bold', fontSize: '15px'}},
            'Preset Library'),
          m('span',
            {style: {fontSize: '11px', color: '#888'}},
            `${library.all().length} presets`),
          m('.spacer', {style: {flex: '1'}}),
          m('input[type=text]', {
            placeholder: 'Search presets...',
            style: {
              padding: '4px 10px',
              fontSize: '12px',
              width: '220px',
            },
            value: this.query,
            oninput: (e: InputEvent) => {
              this.query = (e.target as HTMLInputElement).value;
            },
          }),
          m('button', {
            style: {
              padding: '4px 12px',
              border: '1px solid #ccc',
              background: 'white',
              borderRadius: '3px',
              cursor: 'pointer',
              fontSize: '11px',
            },
            onclick: onClose,
          }, 'Close'),
        ),
        // Category tabs.
        m('.preset-picker-cats', {
          style: {
            display: 'flex',
            gap: '4px',
            padding: '8px 16px',
            borderBottom: '1px solid #f0f0f0',
            flexWrap: 'wrap',
          },
        },
          this.categoryTab('All', null),
          categories.map((c) => this.categoryTab(c, c)),
        ),
        // Preset list.
        m('.preset-picker-list', {
          style: {
            flex: '1',
            overflowY: 'auto',
            padding: '8px 16px',
          },
        },
          entries.length === 0
            ? m('div',
                {style: {padding: '20px', color: '#999'}},
                'No presets match.')
            : entries.map((e) => this.renderEntry(e, onPick)),
        ),
      ),
    );
  }

  private categoryTab(label: string, value: string | null): m.Child {
    const active = this.selectedCategory === value;
    const hue = value ? (CATEGORY_HUES[value] ?? 180) : 0;
    return m('button', {
      style: {
        padding: '4px 12px',
        border: '1px solid ' + (active ? `hsl(${hue}, 65%, 45%)` : '#ddd'),
        background: active ? `hsl(${hue}, 65%, 92%)` : 'white',
        color: active ? `hsl(${hue}, 65%, 30%)` : '#555',
        fontWeight: active ? 'bold' : 'normal',
        borderRadius: '3px',
        cursor: 'pointer',
        fontSize: '11px',
        textTransform: 'capitalize',
      },
      onclick: () => {
        this.selectedCategory = value;
        this.query = '';
      },
    }, label);
  }

  private renderEntry(
    e: PresetEntry, onPick: (entry: PresetEntry) => void,
  ): m.Child {
    const hue = CATEGORY_HUES[e.category] ?? 180;
    return m('.preset-entry', {
      key: e.name,
      style: {
        display: 'flex',
        alignItems: 'center',
        padding: '6px 8px',
        borderRadius: '3px',
        cursor: 'pointer',
        borderLeft: `3px solid hsl(${hue}, 65%, 50%)`,
        marginBottom: '2px',
        background: '#fafafa',
      },
      onmouseenter: (ev: MouseEvent) => {
        (ev.currentTarget as HTMLElement).style.background = '#eef1f6';
      },
      onmouseleave: (ev: MouseEvent) => {
        (ev.currentTarget as HTMLElement).style.background = '#fafafa';
      },
      onclick: () => onPick(e),
    },
      m('span', {
        style: {
          fontSize: '9px',
          color: `hsl(${hue}, 65%, 30%)`,
          fontWeight: 'bold',
          textTransform: 'uppercase',
          width: '60px',
          flexShrink: '0',
        },
      }, e.category),
      m('span', {
        style: {
          fontSize: '12px',
          fontFamily: 'monospace',
          color: '#222',
          minWidth: '220px',
        },
      }, e.name),
      m('span', {
        style: {
          flex: '1',
          fontSize: '11px',
          color: '#666',
          marginLeft: '12px',
          overflow: 'hidden',
          textOverflow: 'ellipsis',
          whiteSpace: 'nowrap',
        },
      }, e.description),
    );
  }
}
