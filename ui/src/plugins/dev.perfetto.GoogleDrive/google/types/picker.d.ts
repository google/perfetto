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

// Minimal ambient types for google.picker, the Google Picker API, which the
// Google API loader's 'picker' module adds at runtime. Only the parts this
// plugin uses are declared.
//
// These are declared here rather than taken from @types/google.picker to keep
// them to what the plugin calls.

declare global {
  namespace google.picker {
    interface DocumentObject {
      readonly id: string;
      readonly name?: string;
    }

    interface ResponseObject {
      readonly action: string;
      readonly docs?: DocumentObject[];
    }

    const Action: {
      readonly PICKED: string;
      readonly CANCEL: string;
      readonly ERROR: string;
    };

    const ViewId: {
      readonly DOCS: string;
      readonly FOLDERS: string;
    };

    const DocsViewMode: {
      readonly LIST: string;
    };

    class DocsView {
      constructor(viewId?: string);
      setMode(mode: string): this;
      setFileIds(fileIds: string): this;
      setIncludeFolders(include: boolean): this;
      setSelectFolderEnabled(enabled: boolean): this;
      setMimeTypes(mimeTypes: string): this;
    }

    interface Picker {
      setVisible(visible: boolean): void;
    }

    class PickerBuilder {
      setAppId(appId: string): this;
      setOAuthToken(token: string): this;
      setDeveloperKey(key: string): this;
      setOrigin(origin: string): this;
      addView(view: DocsView): this;
      setTitle(title: string): this;
      setCallback(callback: (data: ResponseObject) => void): this;
      build(): Picker;
    }
  }
}

// We can only augment the global scope from an external module.
export {};
