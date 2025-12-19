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

/**
 * Allow-list of command IDs that can be executed as startup commands.
 * Only commands in this list will be permitted to run automatically
 * when a trace loads.
 */
export const STARTUP_COMMAND_ALLOWLIST: string[] = [
  // Track manipulation commands
  'dev.perfetto.PinTracksByRegex',
  'dev.perfetto.ExpandTracksByRegex',
  'dev.perfetto.CollapseTracksByRegex',

  // Debug track commands
  'dev.perfetto.AddDebugSliceTrack',
  'dev.perfetto.AddDebugSliceTrackWithPivot',
  'dev.perfetto.AddDebugCounterTrack',
  'dev.perfetto.AddDebugCounterTrackWithPivot',

  // Workspace commands
  'dev.perfetto.CreateWorkspace',
  'dev.perfetto.SwitchWorkspace',
  'dev.perfetto.CopyTracksToWorkspaceByRegex',
  'dev.perfetto.CopyTracksToWorkspaceByRegexWithAncestors',

  // Query commands
  'dev.perfetto.RunQuery',
  'dev.perfetto.RunQueryAndShowTab',

  // Commands will be added here based on user suggestions
];

// Create a set for faster lookups of exact matches
const STARTUP_COMMAND_ALLOWLIST_SET = new Set(STARTUP_COMMAND_ALLOWLIST);

/**
 * Validates whether a command ID is allowed as a startup command.
 * @param commandId The command ID to validate
 * @returns true if the command ID is in the allowlist, false otherwise
 */
export function isStartupCommandAllowed(commandId: string): boolean {
  // First check for exact match (fastest)
  if (STARTUP_COMMAND_ALLOWLIST_SET.has(commandId)) {
    return true;
  }

  // Special case: allow all user-defined macros
  if (commandId.startsWith('dev.perfetto.UserMacro.')) {
    return true;
  }

  return false;
}
