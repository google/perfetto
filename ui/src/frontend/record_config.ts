// Copyright (C) 2020 The Android Open Source Project
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

import {RecordConfig} from '../common/state';
import {JsonObject, validateRecordConfig} from '../controller/validate_config';

const LOCAL_STORAGE_RECORD_CONFIGS_KEY = 'recordConfigs';

class NamedRecordConfig {
  title: string;
  config: RecordConfig;
  key: string;

  constructor(title: string, config: RecordConfig, key: string) {
    this.title = title;
    this.config = validateRecordConfig(config as unknown as JsonObject);
    this.key = key;
  }

  static isValid(jsonObject: object): jsonObject is NamedRecordConfig {
    return (jsonObject as NamedRecordConfig).title !== undefined &&
        (jsonObject as NamedRecordConfig).config !== undefined &&
        (jsonObject as NamedRecordConfig).key !== undefined;
  }
}

export class RecordConfigStore {
  recordConfigs: NamedRecordConfig[];
  recordConfigNames: Set<string>;

  constructor() {
    this.recordConfigs = [];
    this.recordConfigNames = new Set();
    this.reloadFromLocalStorage();
  }

  save(recordConfig: RecordConfig, title?: string): void {
    // We reload from local storage in case of concurrent
    // modifications of local storage from a different tab.
    this.reloadFromLocalStorage();

    const savedTitle = title ? title : new Date().toJSON();
    const config =
        new NamedRecordConfig(savedTitle, recordConfig, new Date().toJSON());

    this.recordConfigs.push(config);
    this.recordConfigNames.add(savedTitle);

    window.localStorage.setItem(
        LOCAL_STORAGE_RECORD_CONFIGS_KEY, JSON.stringify(this.recordConfigs));
  }

  delete(key: string): void {
    // We reload from local storage in case of concurrent
    // modifications of local storage from a different tab.
    this.reloadFromLocalStorage();

    let idx = -1;
    for (let i = 0; i < this.recordConfigs.length; ++i) {
      if (this.recordConfigs[i].key === key) {
        idx = i;
        break;
      }
    }

    if (idx !== -1) {
      this.recordConfigNames.delete(this.recordConfigs[idx].title);
      this.recordConfigs.splice(idx, 1);
      window.localStorage.setItem(
          LOCAL_STORAGE_RECORD_CONFIGS_KEY, JSON.stringify(this.recordConfigs));
    } else {
      // TODO(bsebastien): Show a warning message to the user in the UI.
      console.warn('The config selected doesn\'t exist any more');
    }
  }

  private clearRecordConfigs(): void {
    this.recordConfigs = [];
    this.recordConfigNames.clear();
    window.localStorage.setItem(
        LOCAL_STORAGE_RECORD_CONFIGS_KEY, JSON.stringify([]));
  }

  reloadFromLocalStorage(): void {
    const configsLocalStorage =
        window.localStorage.getItem(LOCAL_STORAGE_RECORD_CONFIGS_KEY);

    if (configsLocalStorage) {
      this.recordConfigNames.clear();

      try {
        const validConfigLocalStorage: NamedRecordConfig[] = [];
        const parsedConfigsLocalStorage = JSON.parse(configsLocalStorage);

        // Check if it's an array.
        if (!Array.isArray(parsedConfigsLocalStorage)) {
          this.clearRecordConfigs();
          return;
        }

        for (let i = 0; i < parsedConfigsLocalStorage.length; ++i) {
          if (!NamedRecordConfig.isValid(parsedConfigsLocalStorage[i])) {
            continue;
          }
          this.recordConfigNames.add(parsedConfigsLocalStorage[i].title);
          validConfigLocalStorage.push(new NamedRecordConfig(
              parsedConfigsLocalStorage[i].title,
              parsedConfigsLocalStorage[i].config,
              parsedConfigsLocalStorage[i].key));
        }

        this.recordConfigs = validConfigLocalStorage;
        window.localStorage.setItem(
            LOCAL_STORAGE_RECORD_CONFIGS_KEY,
            JSON.stringify(validConfigLocalStorage));
      } catch (e) {
        this.clearRecordConfigs();
      }
    } else {
      this.clearRecordConfigs();
    }
  }

  canSave(title: string) {
    return !this.recordConfigNames.has(title);
  }
}

// This class is a singleton to avoid many instances
// conflicting as they attempt to edit localStorage.
export const recordConfigStore = new RecordConfigStore();
