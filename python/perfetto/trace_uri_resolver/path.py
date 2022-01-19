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

from typing import Type

from perfetto.trace_uri_resolver import util
from perfetto.trace_uri_resolver.resolver import TraceUriResolver


class PathUriResolver(TraceUriResolver):
  PREFIX: str = None

  def __init__(self, path: str):
    self.path = path

  def resolve(self) -> TraceUriResolver.Result:
    return [
        TraceUriResolver.Result(
            trace=util.file_generator(self.path), metadata=dict())
    ]

  @classmethod
  def from_trace_uri(cls: Type['PathUriResolver'],
                     args_str: str) -> 'PathUriResolver':
    return PathUriResolver(args_str)
