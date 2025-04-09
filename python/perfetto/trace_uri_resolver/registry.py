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

import dataclasses as dc
from typing import Dict, List, Type, Union

from perfetto.trace_uri_resolver import util
from perfetto.trace_uri_resolver.resolver import TraceContent
from perfetto.trace_uri_resolver.resolver import TraceGenerator
from perfetto.trace_uri_resolver.resolver import TraceUri
from perfetto.trace_uri_resolver.resolver import TraceUriResolver

TraceReference = Union[TraceUriResolver, TraceUri, TraceContent]
TraceListReference = Union[TraceReference, List[TraceReference]]


class ResolverRegistry:

  @dc.dataclass
  class Result:
    generator: TraceGenerator
    metadata: Dict[str, str]

  def __init__(self, resolvers: List[Type[TraceUriResolver]] = []):
    self.registry: Dict[str, Type[TraceUriResolver]] = dict()

    for resolver in resolvers:
      self.register(resolver)

  def register(self, provider: Type[TraceUriResolver]):
    self.registry[provider.PREFIX] = provider

  def resolve(self, ref: TraceListReference) -> List['ResolverRegistry.Result']:
    if isinstance(ref, list):
      return [inner for outer in ref for inner in self._resolve_ref(outer)]
    return self._resolve_ref(ref)

  def _resolve_ref(self,
                   ref: TraceReference) -> List['ResolverRegistry.Result']:
    if isinstance(ref, TraceUriResolver):
      return [
          _merge_metadata(outer, inner)
          for outer in ref.resolve()
          for inner in self.resolve(outer.trace)
      ]

    if isinstance(ref, TraceUri):
      return [
          _merge_metadata(outer, inner)
          for outer in self._resolver_from_uri(ref).resolve()
          for inner in self.resolve(outer.trace)
      ]

    if hasattr(ref, 'read'):
      return [ResolverRegistry.Result(util.read_generator(ref), {})]

    return [ResolverRegistry.Result(ref, {})]

  def _resolver_from_uri(self, uri: TraceUri) -> TraceUriResolver:
    resolver_name, _ = util.parse_trace_uri(uri)
    resolver_cls = self.registry[resolver_name]
    return resolver_cls.from_trace_uri(uri)


def _merge_metadata(
    resolver_res: TraceUriResolver.Result,
    registry_res: ResolverRegistry.Result) -> List[ResolverRegistry.Result]:
  return ResolverRegistry.Result(
      generator=registry_res.generator,
      metadata=util.merge_dicts(resolver_res.metadata, registry_res.metadata))
