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

import {exists} from './utils';

// Errors in JavaScript are (sadly) extremely free form. Three common
// cases are:
// - the error is a string
// - the error object itself has a 'message' field (normally because
//   its an instance of Error or a subclass of Error).
// - the outer object wraps an error under the 'error' key. As in:
//   https://google.github.io/styleguide/jsoncstyleguide.xml#Reserved_Property_Names_in_the_error_object
// TODO(hjd): Can this last case actually occur for us? Maybe this
// too closely followed the code from flash station?
interface ErrorLikeObject {
  message?: unknown;
  error?: {message?: unknown};
  stack?: unknown;
  code?: unknown;
}

// Attempt to coerce an error object into a string message.
// Sometimes an error message is wrapped in an Error object, sometimes not.
export function getErrorMessage(e: unknown | undefined | null) {
  if (exists(e) && typeof e === 'object') {
    const errorObject = e as ErrorLikeObject;
    if (exists(errorObject.message)) {
      // regular Error Object
      return String(errorObject.message);
    } else if (exists(errorObject.error) && exists(errorObject.error.message)) {
      // API result
      return String(errorObject.error.message);
    }
  }
  const asString = String(e);
  if (asString === '[object Object]') {
    try {
      return JSON.stringify(e);
    } catch (stringifyError) {
      // ignore failures and just fall through
    }
  }
  return asString;
}

// Occasionally operations using the cache API throw:
// 'UnknownError: Unexpected internal error. {}'
// It's not clear under which circumstances this can occur. A dive of
// the Chromium code didn't shed much light:
// https://source.chromium.org/chromium/chromium/src/+/main:third_party/blink/renderer/modules/cache_storage/cache_storage_error.cc;l=26;drc=4cfe86482b000e848009077783ba35f83f3c3cfe
// https://source.chromium.org/chromium/chromium/src/+/main:content/browser/cache_storage/cache_storage_cache.cc;l=1686;drc=ab68c05beb790d04d1cb7fd8faa0a197fb40d399
// Given the error is not actionable at present and caching is 'best
// effort' in any case ignore this error. We will want to throw for
// errors in general though so as not to hide errors we actually could
// fix.
// See b/227785665 for an example.
export function ignoreCacheUnactionableErrors<T>(e: unknown, result: T): T {
  if (getErrorMessage(e).includes('UnknownError')) {
    return result;
  } else {
    throw e;
  }
}
