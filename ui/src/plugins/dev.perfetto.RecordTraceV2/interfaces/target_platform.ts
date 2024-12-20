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

export interface TargetPlatform {
  id: string;
  name: string;
  icon: string;
}

export const TARGET_PLATFORMS = [
  {
    id: 'ANDROID',
    name: 'Android',
    icon: 'android',
  },
  {
    id: 'CHROME',
    name: 'Chrome',
    icon: 'travel_explore',
  },
  {
    id: 'CHROME_OS',
    name: 'ChromeOS',
    icon: 'laptop_chromebook',
  },
  {
    id: 'LINUX',
    name: 'Linux',
    icon: 'dns',
  },
] as const;

export type TargetPlatformId = (typeof TARGET_PLATFORMS)[number]['id'];
