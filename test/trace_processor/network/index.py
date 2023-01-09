#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License a
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import Path
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import DiffTestModule


class DiffTestModule_Network(DiffTestModule):

  def test_netif_receive_skb(self):
    return DiffTestBlueprint(
        trace=Path('netif_receive_skb.textproto'),
        query=Path('netif_receive_skb_test.sql'),
        out=Path('netif_receive_skb.out'))

  def test_net_dev_xmit(self):
    return DiffTestBlueprint(
        trace=Path('net_dev_xmit.textproto'),
        query=Path('net_dev_xmit_test.sql'),
        out=Path('net_dev_xmit.out'))

  def test_netperf_metric(self):
    return DiffTestBlueprint(
        trace=Path('netperf_metric.textproto'),
        query=Path('android_netperf'),
        out=Path('netperf_metric.out'))

  def test_inet_sock_set_state(self):
    return DiffTestBlueprint(
        trace=Path('inet_sock_set_state.textproto'),
        query=Path('inet_sock_set_state_test.sql'),
        out=Path('inet_sock_set_state.out'))

  def test_tcp_retransmit_skb(self):
    return DiffTestBlueprint(
        trace=Path('tcp_retransmit_skb.textproto'),
        query=Path('tcp_retransmit_skb_test.sql'),
        out=Path('tcp_retransmit_skb.out'))

  def test_napi_gro_receive(self):
    return DiffTestBlueprint(
        trace=Path('napi_gro_receive.textproto'),
        query=Path('napi_gro_receive_test.sql'),
        out=Path('napi_gro_receive.out'))

  def test_kfree_skb(self):
    return DiffTestBlueprint(
        trace=Path('kfree_skb.textproto'),
        query=Path('kfree_skb_test.sql'),
        out=Path('kfree_skb.out'))
