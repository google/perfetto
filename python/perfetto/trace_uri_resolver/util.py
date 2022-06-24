# Copyright (C) 2022 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
from typing import Any, BinaryIO, Callable, Dict, List, Optional, Tuple

# Limit parsing file to 1MB to maintain parity with the UI
MAX_BYTES_LOADED = 1 * 1024 * 1024


def file_generator(path: str):
  with open(path, 'rb') as f:
    yield from read_generator(f)


def read_generator(trace: BinaryIO):
  while True:
    chunk = trace.read(MAX_BYTES_LOADED)
    if not chunk:
      break
    yield chunk


def merge_dicts(a: Dict[str, str], b: Dict[str, str]):
  return {**a, **b}


def parse_trace_uri(uri: str) -> Tuple[Optional[str], str]:
  # This is definitely a path and not a URI
  if uri.startswith('/') or uri.startswith('.'):
    return None, uri

  # If there's no colon, it cannot be a URI
  idx = uri.find(':')
  if idx == -1:
    return None, uri

  # If there is only a single character before the colon
  # this is likely a Windows path; throw an error on other platforms
  # to prevent single character names causing issues on Windows.
  if idx == 1:
    if os.name != 'nt':
      raise Exception('Single character resolvers are not allowed')
    return None, uri

  return (uri[:idx], uri[idx + 1:])


def to_list(cs: Any) -> Optional[List[Any]]:
  """Converts input into list if it is not already a list.

  For resolvers that can accept list types it may happen the user inputs just
  a single value, to make the code generic enough we would want to convert those
  input into list of single element. It does not do anything for None or List
  types.
  """
  if cs is None or isinstance(cs, list):
    return cs
  return [cs]


def _cs_list(cs: List[Any], fn: Callable[[Any], str], empty_default: str,
             condition_sep: str) -> str:
  """Converts list of constraints into list of clauses.

  Applies function `fn` over each element in list `cs` and joins the list of
  transformed strings with join string `condition_sep`. `empty_default` string
  is returned incase cs is a list of length 0. 'TRUE' is returned when cs is
  None

  e.g.
  Input:
    cs: ['Android', 'Linux']
    fn: "platform = '{}'".format
    empty_default: FALSE
    condition_sep: 'OR'
  OUTPUT:
    "(platform = 'Android' OR platform = 'Linux')"
  """
  if cs is None:
    return 'TRUE'
  if not cs:
    return empty_default
  return f'({condition_sep.join([fn(c) for c in cs])})'


def and_list(cs: List[Any], fn: Callable[[Any], str],
             empty_default: str) -> str:
  """Converts list of constraints into list of AND clauses.

  Function `fn` is applied over each element of list `cs` and joins the list of
  transformed strings with ' AND ' string. `empty_default` string
  is returned incase cs is a list of length 0. 'TRUE' is returned when cs is
  None.

  e.g.
  Input:
    cs: ['Android', 'Linux']
    fn: "platform != '{}'".format
    empty_default: FALSE
  OUTPUT:
    "(platform != 'Android' AND platform != 'Linux')"
  """
  return _cs_list(cs, fn, empty_default, ' AND ')


def or_list(cs: List[Any], fn: Callable[[Any], str], empty_default: str) -> str:
  """Converts list of constraints into list of OR clauses.

  Similar to and_list method, just the join string is ' OR ' instead of ' AND '.

  e.g.
  Input:
    cs: ['Android', 'Linux']
    fn: "platform = '{}'".format
    empty_default: FALSE
  OUTPUT:
    "(platform = 'Android' OR platform = 'Linux')"
  """
  return _cs_list(cs, fn, empty_default, ' OR ')
