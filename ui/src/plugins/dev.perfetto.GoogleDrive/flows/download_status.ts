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

export function formatDownloadStatus(
  loadedBytes?: number,
  totalBytes?: number,
): string {
  if (loadedBytes === undefined || loadedBytes === 0) {
    return `Downloading trace…`;
  }
  const mb = (bytes: number) => (bytes / 1e6).toFixed(1);
  if (totalBytes !== undefined && totalBytes > 0) {
    const percent = Math.min(100, Math.floor((100 * loadedBytes) / totalBytes));
    return `Downloading trace ${percent}% (${mb(loadedBytes)} / ${mb(totalBytes)} MB)`;
  }
  return `Downloading trace ${mb(loadedBytes)} MB`;
}
