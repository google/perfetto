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
import {Button, ButtonVariant} from '../../../widgets/button';
import {Intent} from '../../../widgets/common';

export interface GraphTabAttrs {
  // Pretty-printed JSON of the current live graph state.
  readonly liveJson: string;
  // Called when the user clicks Apply. Throws if the JSON is invalid.
  readonly onApply: (json: string) => void;
}

export function GraphTab(
  _initialVnode: m.Vnode<GraphTabAttrs>,
): m.Component<GraphTabAttrs> {
  let editing = false;
  let editValue = '';
  let applyError: string | undefined;

  return {
    view({attrs: {liveJson, onApply}}) {
      const displayJson = editing ? editValue : liveJson;
      return m(
        '',
        {
          style: {
            display: 'flex',
            flexDirection: 'column',
            flex: '1',
            overflow: 'hidden',
            height: '100%',
          },
        },
        [
          m(
            '',
            {
              style: {
                display: 'flex',
                gap: '4px',
                justifyContent: 'flex-end',
                padding: '4px 8px 0',
                flexShrink: '0',
              },
            },
            editing
              ? [
                  m(Button, {
                    variant: ButtonVariant.Filled,
                    label: 'Cancel',
                    onclick: () => {
                      editing = false;
                      applyError = undefined;
                    },
                  }),
                  m(Button, {
                    variant: ButtonVariant.Filled,
                    intent: Intent.Primary,
                    label: 'Apply',
                    onclick: () => {
                      try {
                        onApply(editValue);
                        editing = false;
                        applyError = undefined;
                      } catch (e) {
                        applyError = String(e);
                      }
                    },
                  }),
                ]
              : [
                  m(Button, {
                    variant: ButtonVariant.Filled,
                    icon: 'content_copy',
                    label: 'Copy',
                    onclick: () => navigator.clipboard.writeText(liveJson),
                  }),
                  m(Button, {
                    variant: ButtonVariant.Filled,
                    icon: 'edit',
                    label: 'Edit',
                    onclick: () => {
                      editValue = liveJson;
                      editing = true;
                    },
                  }),
                ],
          ),
          applyError &&
            m(
              'div',
              {
                style: {
                  padding: '4px 8px',
                  color: 'var(--pf-color-error, #c00)',
                  fontSize: '12px',
                  flexShrink: '0',
                },
              },
              applyError,
            ),
          m('textarea.pf-qb-graph-textarea', {
            value: displayJson,
            readonly: !editing,
            spellcheck: false,
            style: {flex: '1', minHeight: '0'},
            oninput: (e: InputEvent) => {
              editValue = (e.target as HTMLTextAreaElement).value;
            },
            ondblclick: () => {
              if (!editing) {
                editValue = liveJson;
                editing = true;
              }
            },
          }),
        ],
      );
    },
  };
}
