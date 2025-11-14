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

import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {TrackNode} from '../../public/workspace';

// Name patterns for tracks which will be pinned in the current workspace.
// It is useful when using the default workspace which contains a lot of tracks
// and uses up a lot of vertical space.
const PIN_TRACK_NAME_PATTERNS = [
  /NavigationRequest/g,
  /NavigationStartToBegin/g,
  /Navigation .*To/g,
  /WebContentsImpl*/g,
];

// Name patterns for tracks that are of interest to navigation investigations.
// Those will be copied over to specific Chrome Navigations workspace to allow
// the user to focus only on the tracks of interest.
const INTERESTING_TRACKS_NAME_PATTERNS = [
  /CrBrowserMain*/g,
  /CrRendererMain*/g,
  /Navigation.*/g,
];

const NAVIGATION_WORKSPACE_NAME = 'Chrome Navigations';

// Plugin to facilitate inspecting traces generated for Chromium navigation.
// It allows pinning navigation relevant tracks, focusing on tracks that are
// of interest, and future ideas for increased productivity.
export default class implements PerfettoPlugin {
  static readonly id = 'org.chromium.ChromeNavigation';

  async onTraceLoad(trace: Trace): Promise<void> {
    // Command which pins navigation specific tracks to the top of the UI
    // so they can easily be inspected without scrolling through a lot of
    // vertical space.
    trace.commands.registerCommand({
      id: 'org.chromium.PinNavigationTracks',
      name: 'Chrome Navigation: Pin relevant tracks',
      callback: () => {
        trace.currentWorkspace.flatTracks
          .filter((t) => PIN_TRACK_NAME_PATTERNS.some((p) => t.name.match(p)))
          .forEach((t) => t.pin());
      },
    });

    // Command which creates a "Chrome Navigations" workspace in which tracks of
    // interest are displayed and the others are not present. It allows us to
    // save vertical space and focus the workspace on navigation specific
    // tracks.
    // Note: It is important to ensure that all tracks of interest also include
    // their parent tracks, so we can have the collapsable process/thread
    // tracks and keep the UI consistent.
    trace.commands.registerCommand({
      id: 'org.chromium.CreateWorkspaceWithTracks',
      name: `Chrome Navigation: Go to "${NAVIGATION_WORKSPACE_NAME}" workspace`,
      defaultHotkey: 'Shift+N',
      callback: () => {
        const flatIds = new Set<string>();

        // If the workspace already exists, just switch to it.
        let ws = trace.workspaces.all.find(
          (w) => w.title === NAVIGATION_WORKSPACE_NAME,
        );
        if (ws) {
          trace.workspaces.switchWorkspace(ws);
          return;
        }

        // Find all tracks that we want to be visible.
        trace.currentWorkspace.flatTracks
          .filter((t) =>
            INTERESTING_TRACKS_NAME_PATTERNS.some((p) => t.name.match(p)),
          )
          .forEach((e) => flatIds.add(e.id));

        // A lambda that will be invoked for each TrackNode to check whehter it
        // is of interest or is an ancestor of a TrackNode of interest.
        const visit: (track: TrackNode) => TrackNode | undefined = (
          track: TrackNode,
        ) => {
          // Visit all children and create track nodes for them if necessary.
          const children = track.children
            .map(visit)
            .filter((t) => t !== undefined);

          // We need to create a new node if we have added any children
          // or this track itself should be copied because the name matches.
          const nameMatch = INTERESTING_TRACKS_NAME_PATTERNS.some((p) =>
            track.name.match(p),
          );
          if (children.length === 0 && !nameMatch) {
            return undefined;
          }
          const result = track.clone();
          children.forEach((c) => result.addChildInOrder(c));
          return result;
        };

        // Create the workspace and add all the relevant tracks to it.
        ws = trace.workspaces.createEmptyWorkspace(NAVIGATION_WORKSPACE_NAME);
        for (const track of trace.currentWorkspace.children) {
          const maybeTrack = visit(track);
          if (maybeTrack !== undefined) {
            ws.addChildInOrder(maybeTrack);
          }
        }

        // Expand all the tracks, so they are visible by default. It can be done
        // from the UI easily, but saves the user a mouse move and a click ;).
        ws.flatTracks.forEach((t) => t.expand());

        trace.workspaces.switchWorkspace(ws);
      },
    });
  }
}
