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

// Covers all key/value pairs, so we don't have to keep updating the HTMLAttrs
// type for every new attribute we want to support. We still maintain HTMLAttrs
// an friends for well-known attributes and we can keep adding to it as needed
// and they will restrict what types can be passed for those attributes, but
// this give us an escape hatch.
type ArbitraryAttrs = {[key: string]: unknown};

export type HTMLAttrs = ArbitraryAttrs & {
  readonly ref?: string; // This is a common attribute used in Perfetto.
  readonly style?: Style;
  readonly id?: string;
  readonly title?: string;
  readonly className?: string;
  readonly onclick?: (e: PointerEvent) => void;
  readonly ondblclick?: (e: PointerEvent) => void;
  readonly onmouseover?: (e: MouseEvent) => void;
  readonly onmouseenter?: (e: MouseEvent) => void;
  readonly onmouseout?: (e: MouseEvent) => void;
  readonly onmousedown?: (e: MouseEvent) => void;
  readonly onmouseup?: (e: MouseEvent) => void;
  readonly onmousemove?: (e: MouseEvent) => void;
  readonly onload?: (e: Event) => void;
};

export interface HTMLFocusableAttrs extends HTMLAttrs {
  readonly onblur?: (e: FocusEvent) => void;
  readonly onfocus?: (e: FocusEvent) => void;
}

export interface HTMLInputAttrs extends HTMLFocusableAttrs {
  readonly disabled?: boolean;
  readonly type?: string;
  readonly onchange?: (e: InputEvent) => void;
  readonly oninput?: (e: InputEvent) => void;
  readonly onkeydown?: (e: KeyboardEvent) => void;
  readonly onkeyup?: (e: KeyboardEvent) => void;
  readonly value?: string | number;
  readonly placeholder?: string;
  readonly min?: number;
  readonly max?: number;
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
  None = 'None',
  Primary = 'Primary',
  Success = 'Success',
  Danger = 'Danger',
  Warning = 'Warning',
}

export function classForIntent(intent: Intent): string | undefined {
  switch (intent) {
    case Intent.None:
      return undefined;
    case Intent.Primary:
      return 'pf-intent-primary';
    case Intent.Success:
      return 'pf-intent-success';
    case Intent.Danger:
      return 'pf-intent-danger';
    case Intent.Warning:
      return 'pf-intent-warning';
    default:
      return assertUnreachable(intent);
  }
}

export type Spacing = 'none' | 'small' | 'medium' | 'large';

export function classForSpacing(spacing: Spacing): string {
  switch (spacing) {
    case 'none':
      return 'pf-spacing-none';
    case 'small':
      return 'pf-spacing-small';
    case 'medium':
      return 'pf-spacing-medium';
    case 'large':
      return 'pf-spacing-large';
    default:
      assertUnreachable(spacing);
  }
}
