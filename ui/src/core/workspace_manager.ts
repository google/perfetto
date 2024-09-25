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

const DEFAULT_WORKSPACE_NAME = 'Default Workspace';

export class WorkspaceManagerImpl implements WorkspaceManager {
  private _workspaces: Workspace[] = [];
  private _currentWorkspace: Workspace;

  constructor() {
    // TS compiler cannot see that we are indirectly initializing
    // _currentWorkspace via resetWorkspaces(), hence the re-assignment.
    this._currentWorkspace = this.createEmptyWorkspace(DEFAULT_WORKSPACE_NAME);
  }

  createEmptyWorkspace(title: string): Workspace {
    const workspace = new Workspace();
    workspace.title = title;
    this._workspaces.push(workspace);
    return workspace;
  }

  switchWorkspace(workspace: Workspace): void {
    // If this fails the workspace doesn't come from createEmptyWorkspace().
    assertTrue(this._workspaces.includes(workspace));
    this._currentWorkspace = workspace;
  }

  get all(): ReadonlyArray<Workspace> {
    return this._workspaces;
  }

  get currentWorkspace() {
    return this._currentWorkspace;
  }
}
