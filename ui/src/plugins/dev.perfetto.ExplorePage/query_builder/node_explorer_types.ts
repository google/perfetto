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
import {ButtonVariant} from '../../../widgets/button';

interface CommonButtonAttrs {
  onclick: () => void;
  variant?: ButtonVariant;
  compact?: boolean;
}

interface IconButton extends CommonButtonAttrs {
  icon: string;
  label?: never;
}

interface LabelButton extends CommonButtonAttrs {
  label: string;
  icon?: string;
}

export type NodeModifyButton = IconButton | LabelButton;

export interface NodeModifySection {
  title?: string;
  content: m.Children;
}

export interface NodeModifyAttrs {
  // Info content to display in an InfoBox at the top (can be string or rich content)
  info: m.Children;

  // Sections to display in order
  sections?: NodeModifySection[];

  // Buttons at different corners
  topLeftButtons?: NodeModifyButton[];
  topRightButtons?: NodeModifyButton[];
  bottomLeftButtons?: NodeModifyButton[];
  bottomRightButtons?: NodeModifyButton[];
}

// NodeDetails types - for displaying node information in the graph view
export interface NodeDetailsAttrs {
  content: m.Children;
}
