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

import {Registry} from '../common/registry';

export interface CustomButtonArgs {
    title: string;
    icon: string;
    callback: () => {};
    /**
     * An optional function determining whether the button should be shown.
     * If absent, the button will always be shown.
     */
    isVisible?: () => boolean;
}

export class CustomButton {
    title: string;
    icon: string;
    callback: () => {};
    isVisible?: () => boolean;
    kind: string;

    constructor(args: CustomButtonArgs) {
        this.title = args.title;
        this.icon = args.icon;
        this.callback = args.callback;
        this.isVisible = args.isVisible;
        this.kind = args.title;
    }
}

export const customButtonRegistry = Registry.kindRegistry<CustomButton>();
