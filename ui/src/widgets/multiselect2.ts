// Copyright (C) 2024 The Android Open Source Project
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
import {HTMLAttrs} from './common';
import {Icon} from './icon';
import {findRef} from '../base/dom_utils';
import {Popup, PopupPosition} from './popup';
import {EmptyState} from './empty_state';
import {classNames} from '../base/classnames';

export interface Multiselect2Attrs<T> extends HTMLAttrs {
  readonly options: ReadonlyArray<T>;
  readonly getLabel: (option: T) => string;
  readonly getKey: (option: T) => string;
  readonly selectedOptions: ReadonlyArray<T>;
  readonly onOptionAdd: (option: T) => void;
  readonly onOptionRemove: (option: T) => void;
  readonly placeholder?: string;
}

const INPUT_REF = 'input';

export class Multiselect2<T> implements m.ClassComponent<Multiselect2Attrs<T>> {
  private currentTextValue = '';
  private popupShowing = false;
  private selectedItemIndex = 0;

  view({attrs}: m.CVnode<Multiselect2Attrs<T>>) {
    const {getLabel, selectedOptions, placeholder, ...htmlAttrs} = attrs;

    return m(
      Popup,
      {
        className: 'pf-multiselect2__popup',
        position: PopupPosition.Bottom,
        // isOpen: this.popupShowing,
        onChange: (shouldOpen) => {
          this.popupShowing = shouldOpen;
        },
        matchWidth: true,
        trigger: m(
          '.pf-multiselect2',
          {
            onclick: (ev: Event) => {
              const target = ev.currentTarget as HTMLElement;
              const inputElement = findRef(target, INPUT_REF);
              if (inputElement) {
                (inputElement as HTMLInputElement).focus();
              }
            },
            ...htmlAttrs,
          },
          // Render the selected options as tags in the text field
          selectedOptions.map((option) => {
            const label = getLabel(option);
            return renderTag({
              label,
              onChange: () => {},
              onRemove: () => attrs.onOptionRemove(option),
            });
          }),
          m('input', {
            ref: INPUT_REF,
            onfocus: () => (this.popupShowing = true),
            value: this.currentTextValue,
            placeholder,
            oninput: (ev: InputEvent) => {
              // If we're typing we wanna make sure the popup appears
              this.popupShowing = true;
              const el = ev.target as HTMLInputElement;
              this.currentTextValue = el.value;
              this.selectedItemIndex = 0;
            },
            onkeydown: (ev: KeyboardEvent) => {
              if (ev.key === 'Enter') {
                const filtered = this.filterOptions(attrs);
                if (filtered.length > 0) {
                  const option = filtered[this.selectedItemIndex];
                  attrs.onOptionAdd(option);
                  this.currentTextValue = '';
                }
              }
            },
          }),
        ),
      },
      this.renderOptions(attrs),
    );
  }

  onupdate({dom}: m.VnodeDOM<Multiselect2Attrs<T>>) {
    if (!this.popupShowing) {
      const inputElement = findRef(dom, INPUT_REF);
      if (inputElement) {
        (inputElement as HTMLInputElement).blur();
      }
    }
  }

  private renderOptions(attrs: Multiselect2Attrs<T>) {
    const {getKey, getLabel, onOptionAdd} = attrs;
    const filtered = this.filterOptions(attrs);

    if (filtered.length === 0) {
      return m(EmptyState);
    }

    return m(
      '.pf-multiselect2__scroller',
      filtered.map((o, index) => {
        const key = getKey(o);
        const label = getLabel(o);
        return m(
          '.pf-multiselect2__option-row',
          {
            key,
            className: classNames(
              this.selectedItemIndex === index &&
                'pf-multiselect2__option-row--selected',
            ),
            onclick: () => {
              onOptionAdd?.(o);
            },
          },
          label,
        );
      }),
    );
  }

  private filterOptions({options, getLabel}: Multiselect2Attrs<T>) {
    return options.filter((o) => {
      return getLabel(o)
        .toLowerCase()
        .includes(this.currentTextValue.toLowerCase());
    });
  }
}

interface TagAttrs {
  readonly label: string;
  readonly onChange?: (value: string) => void;
  readonly onRemove?: () => void;
}

function renderTag({label, onChange, onRemove}: TagAttrs): m.Children {
  return m(
    'span.pf-multiselect2__tag',
    {
      ondblclick: onChange
        ? () => {
            onChange(label);
            onRemove?.();
          }
        : undefined,
    },
    label,
    m(Icon, {
      icon: 'close',
      onclick: () => onRemove?.(),
    }),
  );
}
