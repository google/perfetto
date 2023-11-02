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


// This file contains interfaces for attributes for various HTML elements.
// They are typically used by widgets which pass attributes down to their
// internal child, to provide a type-safe interface to users of those widgets.
// Note: This is a non-exhaustive list, and is added to when required.
// Feel free to add any missing attributes as they arise.
export type Style = string|Partial<CSSStyleDeclaration>;

export interface HTMLAttrs {
  ref?: string;  // This is a common attribute used in Perfetto.
  style?: Style;
  id?: string;
  title?: string;
  className?: string;
  onclick?: (e: PointerEvent) => void;
  onmouseover?: (e: MouseEvent) => void;
  onmouseout?: (e: MouseEvent) => void;
  onmousedown?: (e: MouseEvent) => void;
  onmouseup?: (e: MouseEvent) => void;
  onmousemove?: (e: MouseEvent) => void;
  onload?: (e: Event) => void;
}

export interface HTMLInputAttrs extends HTMLAttrs {
  disabled?: boolean;
  type?: string;
  onchange?: (e: Event) => void;
  oninput?: (e: KeyboardEvent) => void;
  onkeydown?: (e: KeyboardEvent) => void;
  onkeyup?: (e: KeyboardEvent) => void;
  value?: string;
  placeholder?: string;
}

export interface HTMLCheckboxAttrs extends HTMLInputAttrs {
  checked?: boolean;
}

export interface HTMLButtonAttrs extends HTMLInputAttrs {}

export interface HTMLAnchorAttrs extends HTMLAttrs {
  href?: string;
  target?: string;
}

export interface HTMLLabelAttrs extends HTMLAttrs {
  for?: string;
}
