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

import {Connection, Label, NodePort} from '../../widgets/nodegraph';

/** Graph-level data shared by every node. */
export interface NodeData<C extends object = {}> {
  readonly type: string;
  readonly id: string;
  x: number;
  y: number;
  nextId?: string;
  collapsed?: boolean;
  /** Stored input ports for variable-input nodes. Absent for static-input nodes. */
  inputs?: ManifestPort[];
  config: C;
}

export interface ManifestPort extends NodePort {
  /** Stable identifier used by getInputColumns / getInputRef. */
  readonly name: string;
  /** Direction for connection compatibility and port placement. */
  readonly direction: 'top' | 'left' | 'right' | 'bottom';
  /** User-facing label shown on the port. Not used for programmatic lookup. */
  readonly content: string;
}

export interface NodeQueryBuilderStore {
  readonly nodes: Record<string, NodeData>;
  readonly connections: Connection[];
  readonly labels: Label[];
}
