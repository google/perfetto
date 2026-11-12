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

import m from 'mithril';
import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import {GoogleOAuthClient} from './gdrive/oauth_client';
import {loadGDriveConfig} from './gdrive_config';
import {installGDriveSupport} from './install';
import {alertModal} from '../../widgets/modal';

export interface GDriveClients {
  readonly oauth: GoogleOAuthClient;
}

export default class GoogleDrivePlugin implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.GoogleDrive';
  static readonly description = 'Connect Perfetto to Google Drive';

  static onActivate(app: App) {
    // Google clients are odd - we must run them as external scripts so loading
    // their code naturally is asynchronous. We don't want to delay startup so
    // kick off the load async.
    const clientsPromise = initGDriveClients();

    // Adds Perfetto to the "Open with" menu in the user's Google Drive.
    app.commands.registerCommand({
      id: `dev.perfetto.GoogleDriveInstall`,
      name: 'Install Google Drive support',
      callback: async () => {
        const clients = await clientsPromise;
        if (!clients) {
          alertGDriveNotAvailable();
          return;
        }
        await installGDriveSupport(clients.oauth);
      },
    });
  }
}

async function alertGDriveNotAvailable() {
  await alertModal({
    key: 'GoogleDriveUnavailable',
    title: 'Google Drive unavailable',
    content: m('p', 'Google Drive support could not be loaded.'),
  });
}

// Loads everything Google Drive support needs, in order: the config, then
// Google's sign-in library (for the OAuth client). This runs once, at startup,
// and nothing in it opens a popup.
//
// Returns undefined if any step fails. This is deliberately quiet (a console
// error only): it runs on every page load, including for users who are offline
// or block Google's scripts. The command reports that Google Drive is
// unavailable.
async function initGDriveClients(): Promise<GDriveClients | undefined> {
  try {
    const config = await loadGDriveConfig();
    const oauth = await GoogleOAuthClient.create({
      clientId: config.clientId,
    });
    return {oauth};
  } catch (e) {
    console.error('Failed to initialize Google Drive', e);
    return undefined;
  }
}
