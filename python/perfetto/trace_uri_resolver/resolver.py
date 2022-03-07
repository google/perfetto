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
from typing import BinaryIO, Dict, Generator, List, Type, Union

from perfetto.trace_uri_resolver import util

TraceUri = str
TraceGenerator = Generator[bytes, None, None]
TraceContent = Union[BinaryIO, TraceGenerator]


class TraceUriResolver:
  """"Resolves a trace URI (e.g. 'ants:trace_id=1234') into a list of traces.

  This class can be subclassed to provide a pluggable mechanism for looking
  up traces using URI strings.

  For example:
    class CustomTraceResolver(TraceResolver):
      PREFIX = 'custom'

      def __init__(self, build_branch: List[str] = None, id: str = None):
        self.build_branch = build_branch
        self.id = id
        self.db = init_db()

      def resolve(self):
        traces = self.db.lookup(
          id=self.id, build_branch=self.build_branch)['path']
        return [
          TraceResolver.Result(
            trace=t['path'],
            args={'iteration': t['iteration'], 'device': t['device']}
          )
          for t in traces
        ]

  Trace resolvers can be passed to trace processor directly:
    with TraceProcessor(CustomTraceResolver(id='abcdefg')) as tp:
      tp.query('select * from slice')

  Alternatively, a trace addesses can be passed:
    config = TraceProcessorConfig(
      resolver_registry=ResolverRegistry(resolvers=[CustomTraceResolver])
    )
    with TraceProcessor('custom:id=abcdefg', config=config) as tp:
      tp.query('select * from slice')
  """

  # Subclasses should set PREFIX to match the trace address prefix they
  # want to handle.
  PREFIX: str = None

  @dc.dataclass
  class Result:
    # TraceUri is present here because it allows recursive lookups (i.e.
    # a resolver which returns a path to a trace).
    trace: Union[TraceUri, TraceContent]

    # metadata allows additional key-value pairs to be provided which are
    # associated for trace. For example, test names and iteration numbers
    # could be provivded for traces originating from lab tests.
    metadata: Dict[str, str]

    def __init__(self,
                 trace: Union[TraceUri, TraceContent],
                 metadata: Dict[str, str] = dict()):
      self.trace = trace
      self.metadata = metadata

  def resolve(self) -> List['TraceUriResolver.Result']:
    """Resolves a list of traces.

    Subclasses should implement this method and resolve the parameters
    specified in the constructor to a list of traces."""
    raise Exception("resolve is unimplemented for this resolver")

  @classmethod
  def from_trace_uri(cls: Type['TraceUriResolver'],
                     uri: TraceUri) -> 'TraceUriResolver':
    """Creates a resolver from a URI.

    URIs have the form:
    android_ci:day=2021-01-01;devices=blueline,crosshatch

    This is converted to a dictionary of the form:
    {'day': '2021-01-01', 'id': ['blueline', 'crosshatch']}

    and passed as kwargs to the constructor of the trace resolver (see class
    documentation for info).

    Generally, sublcasses should not override this method as the standard
    trace address format should work for most usecases. Instead, simply
    define your constructor with the parameters you expect to see in the
    trace address."""
    return cls(**_args_dict_from_uri(uri))


def _args_dict_from_uri(uri: str) -> Dict[str, str]:
  """Creates an the args dictionary from a trace URI.

    URIs have the form:
    android_ci:day=2021-01-01;devices=blueline,crosshatch

    This is converted to a dictionary of the form:
    {'day': '2021-01-01', 'id': ['blueline', 'crosshatch']}
  """
  _, args_str = util.parse_trace_uri(uri)
  if not args_str:
    return {}

  args_lst = args_str.split(';')
  args_dict = dict()
  for arg in args_lst:
    (key, value) = arg.split('=')
    lst = value.split(',')
    if len(lst) > 1:
      args_dict[key] = lst
    else:
      args_dict[key] = value
  return args_dict
