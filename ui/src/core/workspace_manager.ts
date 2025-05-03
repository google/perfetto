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

import {assertTrue} from '../base/logging';
import {Workspace, WorkspaceManager} from '../public/workspace';
import {featureFlags} from './feature_flags';

const DEFAULT_WORKSPACE_NAME = 'Default Workspace';
const DEFAULT_WORKSPACE_EDITABLE_FLAG = featureFlags.register({
  id: 'defaultWorkspaceEditable',
  name: 'Enable Default Workspace Editing',
  description:
    'Allows tracks within the default workspace to be removed and rearranged using drag-and-drop operations.',
  defaultValue: false,
});

export class WorkspaceManagerImpl implements WorkspaceManager {
  readonly defaultWorkspace = new Workspace();
  private _workspaces: Workspace[] = [];
  private _currentWorkspace: Workspace;

  constructor() {
    this.defaultWorkspace.title = DEFAULT_WORKSPACE_NAME;
    this.defaultWorkspace.userEditable = DEFAULT_WORKSPACE_EDITABLE_FLAG.get();
    this._currentWorkspace = this.defaultWorkspace;
  }

  createEmptyWorkspace(title: string): Workspace {
    const workspace = new Workspace();
    workspace.title = title;
    this._workspaces.push(workspace);
    return workspace;
  }

  removeWorkspace(ws: Workspace) {
    if (ws === this.currentWorkspace) {
      this._currentWorkspace = this.defaultWorkspace;
    }
    this._workspaces = this._workspaces.filter((w) => w !== ws);
  }

  switchWorkspace(workspace: Workspace): void {
    // If this fails the workspace doesn't come from createEmptyWorkspace().
    assertTrue(
      this._workspaces.includes(workspace) ||
        workspace === this.defaultWorkspace,
    );
    this._currentWorkspace = workspace;
  }

  get all(): ReadonlyArray<Workspace> {
    return [this.defaultWorkspace].concat(this._workspaces);
  }

  get currentWorkspace() {
    return this._currentWorkspace;
  }
}
