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

import {Color} from './color';

// |ColorScheme| defines a collection of colors which can be used for various UI
// elements. In the future we would expand this interface to include light and
// dark variants.

export interface ColorScheme {
  // The base color to be used for the bulk of the element.
  readonly base: Color;

  // A variant on the base color, commonly used for highlighting.
  readonly variant: Color;

  // Grayed out color to represent a disabled state.
  readonly disabled: Color;

  // Appropriate colors for text to be displayed on top of the above colors.
  readonly textBase: Color;
  readonly textVariant: Color;
  readonly textDisabled: Color;
}
