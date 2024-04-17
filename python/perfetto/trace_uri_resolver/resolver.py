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
import enum
from typing import BinaryIO, Dict, Generator, List, Type, Union
from typing import Generic, Tuple, TypeVar, get_type_hints

from perfetto.trace_uri_resolver import util

TraceUri = str
TraceGenerator = Generator[bytes, None, None]
TraceContent = Union[BinaryIO, TraceGenerator]
_T = TypeVar('_T')


@dc.dataclass
class ConstraintClass(Generic[_T]):

  class Op(enum.Enum):
    EQ = '='
    NE = '!='
    LE = '<='
    GE = '>='
    GT = '>'
    LT = '<'

    def __str__(self):
      return self.value

  value: _T
  op: Op = Op.EQ


Constraint = Union[_T, ConstraintClass[_T]]
ConstraintWithList = Union[Constraint[_T], Constraint[List[_T]]]


class TraceUriResolver:
  """"Resolves a trace URI (e.g. 'ants:trace_id=1234') into a list of traces.

  This class can be subclassed to provide a pluggable mechanism for looking
  up traces using URI strings.

  For example:
    class CustomTraceResolver(TraceUriResolver):
      PREFIX = 'custom'

      def __init__(self, build_branch: List[str] = None, id: str = None):
        self.build_branch = build_branch
        self.id = id
        self.db = init_db()

      def resolve(self):
        traces = self.db.lookup(
          id=self.id, build_branch=self.build_branch)['path']
        return [
          TraceUriResolver.Result(
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
    specified in the constructor to a list of traces.
    """
    raise Exception('resolve is unimplemented for this resolver')

  @classmethod
  def from_trace_uri(cls: Type['TraceUriResolver'],
                     uri: TraceUri) -> 'TraceUriResolver':
    """Creates a resolver from a URI.

    URIs have the form:
    android_ci:day=2021-01-01;devices=blueline,crosshatch;key>=value

    This is converted to a dictionary of the form:
    {'day': '2021-01-01', 'id': ['blueline', 'crosshatch'],
    'key': ConstraintClass('value', Op.GE)}

    and passed as kwargs to the constructor of the trace resolver (see class
    documentation for info).

    Generally, sublcasses should not override this method as the standard
    trace address format should work for most usecases. Instead, simply
    define your constructor with the parameters you expect to see in the
    trace address.
    """
    return cls(**_args_dict_from_uri(uri, get_type_hints(cls.__init__)))


def _read_op(arg_str: str, op_start_ind: int) -> ConstraintClass.Op:
  """Parse operator from string.

  Given string and an expected start index for operator it returns Op object or
  raises error if operator was not found.

  For example:
  _read_op('a>4', 1) returns Op.GE
  _read_op('a>4', 0) raises ValueError
  _read_op('a>4', 3) raises ValueError
  """
  first = arg_str[op_start_ind] if op_start_ind < len(arg_str) else None
  second = arg_str[op_start_ind +
                   1] if op_start_ind + 1 < len(arg_str) else None
  Op = ConstraintClass.Op
  if first == '>':
    return Op.GE if second == '=' else Op.GT
  elif first == '<':
    return Op.LE if second == '=' else Op.LT
  elif first == '!' and second == '=':
    return Op.NE
  elif first == '=':
    return Op.EQ
  raise ValueError('Could not find valid operator in uri arg_str: ' + arg_str)


def _parse_arg(arg_str: str) -> Tuple[str, ConstraintClass.Op, str]:
  """Parse argument string and return a tuple (key, operator, value).

  Given a string like 'branch_num>=4000', it returns a tuple ('branch_num',
  Op.GE,'4000'). Raises ValueError exceptions in case ill formed arg_str is
  passed like '>30', 'key>', 'key', 'key--31'
  """
  op_start_ind = 0
  for ind, c in enumerate(arg_str):
    if not c.isalnum() and c != '_':
      op_start_ind = ind
      break
  if op_start_ind == 0:
    raise ValueError('Could not find valid key in arg_str: ' + arg_str)
  key = arg_str[:op_start_ind]
  op = _read_op(arg_str, op_start_ind)
  value = arg_str[op_start_ind + len(str(op)):]
  if not value:
    raise ValueError('Empty value in trace uri arg_str: ' + arg_str)
  return (key, op, value)


def _args_dict_from_uri(uri: str,
                        type_hints) -> Dict[str, ConstraintWithList[str]]:
  """Creates an the args dictionary from a trace URI.

    URIs have the form:
    android_ci:day=2021-01-01;devices=blueline,crosshatch;key>=value;\
    version>=1;version<5

    This is converted to a dictionary of the form:
    {'day': '2021-01-01', 'id': ['blueline', 'crosshatch'],
    'key': ConstraintClass('value', Op.GE),
    'version': [ConstraintClass(1, Op.GE), ConstraintClass(5, Op.LT)]}
  """
  _, args_str = util.parse_trace_uri(uri)
  if not args_str:
    return {}

  args_lst = args_str.split(';')
  args_dict = dict()
  for arg in args_lst:
    (key, op, value) = _parse_arg(arg)
    lst = value.split(',')
    if len(lst) > 1:
      args_dict_value = lst
    else:
      args_dict_value = value

    if key not in type_hints:
      if op != ConstraintClass.Op.EQ:
        raise ValueError(f'{key} only supports "=" operator')
      args_dict[key] = args_dict_value
      continue
    have_constraint = False
    type_hint = type_hints[key]
    type_args = type_hint.__args__ if hasattr(type_hint, '__args__') else ()
    for type_arg in type_args:
      type_origin = type_arg.__origin__ if hasattr(type_arg,
                                                   '__origin__') else None
      if type_origin is ConstraintClass:
        have_constraint = True
        break
    if not have_constraint and op != ConstraintClass.Op.EQ:
      raise ValueError('Operator other than "=" passed to argument which '
                       'does not have constraint type: ' + arg)
    if have_constraint:
      if key not in args_dict:
        args_dict[key] = ConstraintClass(args_dict_value, op)
      else:
        if isinstance(args_dict[key], ConstraintClass):
          args_dict[key] = [args_dict[key]]
        args_dict[key].append(ConstraintClass(args_dict_value, op))
    else:
      args_dict[key] = args_dict_value
  return args_dict
