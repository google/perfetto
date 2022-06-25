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

import unittest

from perfetto.trace_uri_resolver.util import parse_trace_uri
from perfetto.trace_uri_resolver.util import to_list
from perfetto.trace_uri_resolver.util import _cs_list
from perfetto.trace_uri_resolver.util import and_list
from perfetto.trace_uri_resolver.util import or_list
from perfetto.trace_uri_resolver.resolver import _args_dict_from_uri
from perfetto.trace_uri_resolver.resolver import Constraint
from perfetto.trace_uri_resolver.resolver import ConstraintClass
from perfetto.trace_uri_resolver.resolver import TraceUriResolver
from perfetto.trace_uri_resolver.registry import ResolverRegistry


class SimpleResolver(TraceUriResolver):
  PREFIX = 'simple'

  def __init__(self, foo=None, bar=None):
    self.foo = foo
    self.bar = bar

  def foo_gen(self):
    yield self.foo.encode() if self.foo else b''

  def bar_gen(self):
    yield self.bar.encode() if self.bar else b''

  def resolve(self):
    return [
        TraceUriResolver.Result(self.foo_gen()),
        TraceUriResolver.Result(
            self.bar_gen(), metadata={
                'foo': self.foo,
                'bar': self.bar
            })
    ]


class RecursiveResolver(SimpleResolver):
  PREFIX = 'recursive'

  def __init__(self, foo=None, bar=None):
    super().__init__(foo=foo, bar=bar)

  def resolve(self):
    return [
        TraceUriResolver.Result(self.foo_gen()),
        TraceUriResolver.Result(
            self.bar_gen(), metadata={
                'foo': 'foo',
                'bar': 'bar'
            }),
        TraceUriResolver.Result(f'simple:foo={self.foo};bar={self.bar}'),
        TraceUriResolver.Result(SimpleResolver(foo=self.foo, bar=self.bar)),
    ]


class TestResolver(unittest.TestCase):

  def test_simple_resolve(self):
    registry = ResolverRegistry([SimpleResolver])

    res = registry.resolve('simple:foo=x;bar=y')
    self.assertEqual(len(res), 2)

    (foo_res, bar_res) = res
    self._check_resolver_result(foo_res, bar_res)

    (foo_res, bar_res) = registry.resolve(['simple:foo=x;bar=y'])
    self._check_resolver_result(foo_res, bar_res)

    resolver = SimpleResolver(foo='x', bar='y')

    (foo_res, bar_res) = registry.resolve(resolver)
    self._check_resolver_result(foo_res, bar_res)

    (foo_res, bar_res) = registry.resolve([resolver])
    self._check_resolver_result(foo_res, bar_res)

    (foo_a, bar_b, foo_x,
     bar_y) = registry.resolve(['simple:foo=a;bar=b', resolver])
    self._check_resolver_result(foo_a, bar_b, foo='a', bar='b')
    self._check_resolver_result(foo_x, bar_y)

  def test_simple_resolve_missing_arg(self):
    registry = ResolverRegistry([SimpleResolver])

    (foo_res, bar_res) = registry.resolve('simple:foo=x')
    self._check_resolver_result(foo_res, bar_res, bar=None)

    (foo_res, bar_res) = registry.resolve('simple:bar=y')
    self._check_resolver_result(foo_res, bar_res, foo=None)

    (foo_res, bar_res) = registry.resolve('simple:')
    self._check_resolver_result(foo_res, bar_res, foo=None, bar=None)

  def test_recursive_resolve(self):
    registry = ResolverRegistry([SimpleResolver])
    registry.register(RecursiveResolver)

    res = registry.resolve('recursive:foo=x;bar=y')
    self.assertEqual(len(res), 6)

    (non_rec_foo, non_rec_bar, rec_foo_str, rec_bar_str, rec_foo_obj,
     rec_bar_obj) = res

    self._check_resolver_result(
        non_rec_foo, non_rec_bar, foo_metadata='foo', bar_metadata='bar')
    self._check_resolver_result(rec_foo_str, rec_bar_str)
    self._check_resolver_result(rec_foo_obj, rec_bar_obj)

  def test_parse_trace_uri(self):
    self.assertEqual(parse_trace_uri('/foo/bar'), (None, '/foo/bar'))
    self.assertEqual(parse_trace_uri('foo/bar'), (None, 'foo/bar'))
    self.assertEqual(parse_trace_uri('/foo/b:ar'), (None, '/foo/b:ar'))
    self.assertEqual(parse_trace_uri('./foo/b:ar'), (None, './foo/b:ar'))
    self.assertEqual(parse_trace_uri('foo/b:ar'), ('foo/b', 'ar'))

  def test_to_list(self):
    self.assertEqual(to_list(None), None)
    self.assertEqual(to_list(1), [1])
    self.assertEqual(to_list('1'), ['1'])
    self.assertEqual(to_list([]), [])
    self.assertEqual(to_list([1]), [1])

  def test_cs_list(self):
    fn = 'col = {}'.format
    sep = ' || '
    self.assertEqual(_cs_list(None, fn, 'FALSE', sep), 'TRUE')
    self.assertEqual(_cs_list(None, fn, 'TRUE', sep), 'TRUE')
    self.assertEqual(_cs_list([], fn, 'FALSE', sep), 'FALSE')
    self.assertEqual(_cs_list([], fn, 'TRUE', sep), 'TRUE')
    self.assertEqual(_cs_list([1], fn, 'FALSE', sep), '(col = 1)')
    self.assertEqual(_cs_list([1, 2], fn, 'FALSE', sep), '(col = 1 || col = 2)')

  def test_and_list(self):
    fn = 'col != {}'.format
    self.assertEqual(and_list([1, 2], fn, 'FALSE'), '(col != 1 AND col != 2)')

  def test_or_list(self):
    fn = 'col = {}'.format
    self.assertEqual(or_list([1, 2], fn, 'FALSE'), '(col = 1 OR col = 2)')

  def test_args_dict_from_uri(self):
    self.assertEqual(_args_dict_from_uri('foo:', {}), {})
    self.assertEqual(_args_dict_from_uri('foo:bar=baz', {}), {
        'bar': 'baz',
    })
    self.assertEqual(
        _args_dict_from_uri('foo:key=v1,v2', {}), {'key': ['v1', 'v2']})
    self.assertEqual(
        _args_dict_from_uri('foo:bar=baz;key=v1,v2', {}), {
            'bar': 'baz',
            'key': ['v1', 'v2']
        })
    with self.assertRaises(ValueError):
      _args_dict_from_uri('foo:=v1', {})
    with self.assertRaises(ValueError):
      _args_dict_from_uri('foo:key', {})
    with self.assertRaises(ValueError):
      _args_dict_from_uri('foo:key<', {})
    with self.assertRaises(ValueError):
      _args_dict_from_uri('foo:key<v1', {})
    with self.assertRaises(ValueError):
      _args_dict_from_uri('foo:key<v1', {'key': str})

    type_hints = {'key': Constraint[str]}
    self.assertEqual(
        _args_dict_from_uri('foo:key=v1', type_hints),
        {'key': ConstraintClass('v1', ConstraintClass.Op.EQ)})
    self.assertEqual(
        _args_dict_from_uri('foo:key!=v1', type_hints),
        {'key': ConstraintClass('v1', ConstraintClass.Op.NE)})
    self.assertEqual(
        _args_dict_from_uri('foo:key<=v1', type_hints),
        {'key': ConstraintClass('v1', ConstraintClass.Op.LE)})
    self.assertEqual(
        _args_dict_from_uri('foo:key>=v1', type_hints),
        {'key': ConstraintClass('v1', ConstraintClass.Op.GE)})
    self.assertEqual(
        _args_dict_from_uri('foo:key>v1', type_hints),
        {'key': ConstraintClass('v1', ConstraintClass.Op.GT)})
    self.assertEqual(
        _args_dict_from_uri('foo:key<v1', type_hints),
        {'key': ConstraintClass('v1', ConstraintClass.Op.LT)})

  def _check_resolver_result(self,
                             foo_res,
                             bar_res,
                             foo='x',
                             bar='y',
                             foo_metadata=None,
                             bar_metadata=None):
    self.assertEqual(
        tuple(foo_res.generator), (foo.encode() if foo else ''.encode(),))
    self.assertEqual(
        tuple(bar_res.generator), (bar.encode() if bar else ''.encode(),))
    self.assertEqual(
        bar_res.metadata, {
            'foo': foo_metadata if foo_metadata else foo,
            'bar': bar_metadata if bar_metadata else bar
        })
