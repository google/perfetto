// Copyright (C) 2023 The Android Open Source Project
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

// Initiate download of a resource identified by |url| into |filename|.
export function downloadUrl(fileName: string, url: string) {
  const a = document.createElement('a');
  a.href = url;
  a.download = fileName;
  a.target = '_blank';
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

// Initiate download of |data| a file with a given name.
export function downloadData(fileName: string, ...data: Uint8Array[]) {
  const blob = new Blob(data, {type: 'application/octet-stream'});
  const url = URL.createObjectURL(blob);
  downloadUrl(fileName, url);
}
