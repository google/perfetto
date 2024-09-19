// Copyright (C) 2022 The Android Open Source Project
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

import {globals} from '../../../frontend/globals';
import {AdbKey} from './adb_auth';

function isPasswordCredential(
  cred: Credential | null,
): cred is PasswordCredential {
  return cred !== null && cred.type === 'password';
}

function hasPasswordCredential() {
  return 'PasswordCredential' in window;
}

// how long we will store the key in memory
const KEY_IN_MEMORY_TIMEOUT = 1000 * 60 * 30; // 30 minutes

// Update credential store with the given key.
export async function maybeStoreKey(key: AdbKey): Promise<void> {
  if (!hasPasswordCredential()) {
    return;
  }
  const credential = new PasswordCredential({
    id: 'webusb-adb-key',
    password: key.serializeKey(),
    name: 'WebUSB ADB Key',
    iconURL: `${globals.root}assets/favicon.png`,
  });
  // The 'Save password?' Chrome dialogue only appears if the key is
  // not already stored in Chrome.
  await navigator.credentials.store(credential);
  // 'preventSilentAccess' guarantees the user is always notified when
  // credentials are accessed. Sometimes the user is asked to click a button
  // and other times only a notification is shown temporarily.
  await navigator.credentials.preventSilentAccess();
}

export class AdbKeyManager {
  private key?: AdbKey;
  // Id of timer used to expire the key kept in memory.
  private keyInMemoryTimerId?: ReturnType<typeof setTimeout>;

  // Finds a key, by priority:
  // - looking in memory (i.e. this.key)
  // - looking in the credential store
  // - and finally creating one from scratch if needed
  async getKey(): Promise<AdbKey> {
    // 1. If we have a private key in memory, we return it.
    if (this.key) {
      return this.key;
    }

    // 2. We try to get the private key from the browser.
    // The mediation is set as 'optional', because we use
    // 'preventSilentAccess', which sometimes requests the user to click
    // on a button to allow the auth, but sometimes only shows a
    // notification and does not require the user to click on anything.
    // If we had set mediation to 'required', the user would have been
    // asked to click on a button every time.
    if (hasPasswordCredential()) {
      const options: PasswordCredentialRequestOptions = {
        password: true,
        mediation: 'optional',
      };
      const credential = await navigator.credentials.get(options);
      if (isPasswordCredential(credential)) {
        return this.assignKey(AdbKey.DeserializeKey(credential.password));
      }
    }

    // 3. We generate a new key pair.
    return this.assignKey(await AdbKey.GenerateNewKeyPair());
  }

  // Assigns the key a new value, sets a timeout for storing the key in memory
  // and then returns the new key.
  private assignKey(key: AdbKey): AdbKey {
    this.key = key;
    if (this.keyInMemoryTimerId) {
      clearTimeout(this.keyInMemoryTimerId);
    }
    this.keyInMemoryTimerId = setTimeout(
      () => (this.key = undefined),
      KEY_IN_MEMORY_TIMEOUT,
    );
    return key;
  }
}
