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

// The commands that set up the local server, as the setup guide shows them.

import {LOCAL_PORT} from './server_list';

export const INSTALL_SCRIPT_URL =
  'https://raw.githubusercontent.com/google/perfetto/main/tools/install-bigtrace';

// Pages the local server accepts without --cors-origin (see `Handler` in
// python/perfetto/bigtrace/server.py).
const DEFAULT_ALLOWED_ORIGINS = ['https://ui.perfetto.dev'];
const LOOPBACK_HOSTS = ['localhost', '127.0.0.1', '[::1]'];

function serverAllows(pageOrigin: string): boolean {
  if (DEFAULT_ALLOWED_ORIGINS.includes(pageOrigin)) return true;
  try {
    return LOOPBACK_HOSTS.includes(new URL(pageOrigin).hostname);
  } catch {
    return false;
  }
}

// Installs and starts the always-on local server; allows this UI's origin.
export function installCommand(pageOrigin: string): string {
  const allow = serverAllows(pageOrigin) ? '' : ` --cors-origin ${pageOrigin}`;
  return `curl -fsSL ${INSTALL_SCRIPT_URL} | python3 -${allow}`;
}

// Run where the browser is, to reach the server of `host`.
export function tunnelCommand(host = 'YOUR_WORKSTATION'): string {
  return `ssh -N -L ${LOCAL_PORT}:127.0.0.1:${LOCAL_PORT} ${host}`;
}

// Lets an installed server answer this UI (keeps its other settings).
export function allowOriginCommand(pageOrigin: string): string {
  return `bigtrace install --cors-origin ${pageOrigin}`;
}
