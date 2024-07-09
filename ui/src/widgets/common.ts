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

import {assertUnreachable} from '../base/logging';

// This file contains interfaces for attributes for various HTML elements.
// They are typically used by widgets which pass attributes down to their
// internal child, to provide a type-safe interface to users of those widgets.
// Note: This is a non-exhaustive list, and is added to when required.
// Feel free to add any missing attributes as they arise.
export type Style = string | Partial<CSSStyleDeclaration>;

export interface HTMLAttrs {
  readonly ref?: string; // This is a common attribute used in Perfetto.
  readonly style?: Style;
  readonly id?: string;
  readonly title?: string;
  readonly className?: string;
  readonly onclick?: (e: PointerEvent) => void;
  readonly onmouseover?: (e: MouseEvent) => void;
  readonly onmouseout?: (e: MouseEvent) => void;
  readonly onmousedown?: (e: MouseEvent) => void;
  readonly onmouseup?: (e: MouseEvent) => void;
  readonly onmousemove?: (e: MouseEvent) => void;
  readonly onload?: (e: Event) => void;
}

export interface HTMLFocusableAttrs extends HTMLAttrs {
  readonly onblur?: (e: FocusEvent) => void;
  readonly onfocus?: (e: FocusEvent) => void;
}

export interface HTMLInputAttrs extends HTMLFocusableAttrs {
  readonly disabled?: boolean;
  readonly type?: string;
  readonly onchange?: (e: Event) => void;
  readonly oninput?: (e: KeyboardEvent) => void;
  readonly onkeydown?: (e: KeyboardEvent) => void;
  readonly onkeyup?: (e: KeyboardEvent) => void;
  readonly value?: string;
  readonly placeholder?: string;
}

export interface HTMLCheckboxAttrs extends HTMLInputAttrs {
  readonly checked?: boolean;
}

export interface HTMLButtonAttrs extends HTMLInputAttrs {}

export interface HTMLAnchorAttrs extends HTMLAttrs {
  readonly href?: string;
  readonly target?: string;
}

export interface HTMLLabelAttrs extends HTMLAttrs {
  readonly for?: string;
}

export enum Intent {
  None = 'none',
  Primary = 'primary',
}

export function classForIntent(intent: Intent): string | undefined {
  switch (intent) {
    case Intent.None:
      return undefined;
    case Intent.Primary:
      return 'pf-intent-primary';
    default:
      return assertUnreachable(intent);
  }
}
