// Copyright (C) 2025 The Android Open Source Project
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

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

export function getUrlParameter(key: string) {
  const url = window.location.href;
  const [_, hashFragment] = url.split('#!');
  const [_1, queryString] = hashFragment
    ? hashFragment.split('?')
    : url.split('?');
  const params = new URLSearchParams(queryString || '');
  return params.get(key);
}

export function changeURLParam(name: string, value: string) {
  removeUrlParameter(name);
  if (value) {
    addUrlParameters(name, value);
  }
}

function removeUrlParameter(paramKey: string) {
  const url = window.location.href;
  const [baseUrl, hashFragment] = url.split('#!');

  let newUrl;
  if (hashFragment) {
    const [path, queryString] = hashFragment.split('?');
    const params = new URLSearchParams(queryString || '');

    params.delete(paramKey);

    const updatedQueryString = params.toString();
    newUrl = `${baseUrl}#!${path}${updatedQueryString ? `?${updatedQueryString}` : ''}`;
  } else {
    const [path, queryString] = url.split('?');
    const params = new URLSearchParams(queryString || '');

    params.delete(paramKey);

    const updatedQueryString = params.toString();
    newUrl = `${path}${updatedQueryString ? `?${updatedQueryString}` : ''}`;
  }

  window.history.replaceState(null, '', newUrl);
}

function addUrlParameters(key: string, value: string) {
  const url = window.location.href;
  const [baseUrl, hashFragment] = url.split('#!');
  let newUrl;
  if (hashFragment) {
    const [path, queryString] = hashFragment.split('?');
    const params = new URLSearchParams(queryString || '');
    params.set(key, value);
    const updatedQueryString = params.toString();
    newUrl = `${baseUrl}#!${path}?${updatedQueryString}`;
  } else {
    const [path, queryString] = url.split('?');
    const params = new URLSearchParams(queryString || '');
    params.set(key, value);
    const updatedQueryString = params.toString();
    newUrl = `${path}?${updatedQueryString}`;
  }
  window.history.replaceState(null, '', newUrl);
}
