// Copyright (C) 2018 The Android Open Source Project
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

import {raf} from '../core/raf_scheduler';
import {SqlPackage} from '../public/extra_sql_packages';
import {CommandInvocation} from '../core/command_manager';

// This controls how long we wait for the script to load before giving up and
// proceeding as if the user is not internal.
const SCRIPT_LOAD_TIMEOUT_MS = 5000;
const SCRIPT_URL =
  'https://storage.cloud.google.com/perfetto-ui-internal/internal-data-v1/amalgamated.js';

// This interface describes the required interface that the script expect to
// find on window.globals.
interface InteralUserScriptParams {
  // This variable is set by the is_internal_user.js script if the user is a
  // googler. This is used to avoid exposing features that are not ready yet for
  // public consumption. The gated features themselves are not secret. If a user
  // has been detected as a Googler once, make that sticky in localStorage, so
  // that we keep treating them as such when they connect over public networks.
  // This is normally undefined is injected in via is_internal_user.js.
  // WARNING: do not change/rename/move without considering impact on the
  // internal_user script.
  isInternalUser: boolean;

  // The script adds to this list, hence why it's readonly.
  // WARNING: do not change/rename/move without considering impact on the
  // internal_user script.
  readonly extraSqlPackages: SqlPackage[];

  // JSON Amalgamator populates this with statsd atom descriptors
  // as a list of base64-encoded strings.
  // WARNING: do not change/rename/move without considering impact on the
  // internal_user script.
  readonly extraParsingDescriptors: string[];

  // The script adds to this list, hence why it's readonly.
  // WARNING: do not change/rename/move without considering impact on the
  // internal_user script.
  readonly extraMacros: Record<string, ReadonlyArray<CommandInvocation>>[];

  // TODO(stevegolton): Check if we actually need to use these.
  // Used when switching to the legacy TraceViewer UI.
  // Most resources are cleaned up by replacing the current |window| object,
  // however pending RAFs and workers seem to outlive the |window| and need to
  // be cleaned up explicitly.
  shutdown(): void;
}

/**
 * Sets up a proxy object on window.globals that forwards property accesses to
 * the given AppImpl instance.
 */
function setupGlobalsProxy(): InteralUserScriptParams {
  const params: InteralUserScriptParams = {
    isInternalUser: false,
    extraSqlPackages: [],
    extraParsingDescriptors: [],
    extraMacros: [],
    shutdown() {
      raf.shutdown();
    },
  };
  // Patch the global window object with a few hooks that point into the app
  // object.
  (window as unknown as {globals?: InteralUserScriptParams}).globals = params;
  return params;
}

/**
 * Loads a script that detects if the user is internal, allowing access to
 * non-public features and SQL packages.
 *
 * Returns a promise that resolves to an object containing the parameters set
 * by the script.
 */
export async function tryLoadIsInternalUserScript(): Promise<InteralUserScriptParams> {
  // Set up the global object and attach it to `window` before loading the
  // script.
  const params = setupGlobalsProxy();

  await new Promise<void>((resolve) => {
    const script = document.createElement('script');
    script.src = SCRIPT_URL;
    script.async = true;
    script.onerror = () => resolve();
    script.onload = () => resolve();
    document.head.append(script);

    // Set a timeout to avoid blocking the UI for too long if the script is slow
    // to load.
    setTimeout(() => resolve(), SCRIPT_LOAD_TIMEOUT_MS);
  });

  // The script has loaded (or timed out). The params object has been mutated
  // by the script if possible.
  return params;
}
