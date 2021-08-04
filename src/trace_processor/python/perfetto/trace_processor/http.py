#!/usr/bin/env python3
# Copyright (C) 2020 The Android Open Source Project
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

import http.client

from .protos import ProtoFactory


class TraceProcessorHttp:

  def __init__(self, url):
    self.protos = ProtoFactory()
    self.conn = http.client.HTTPConnection(url)

  def execute_query(self, query):
    args = self.protos.RawQueryArgs()
    args.sql_query = query
    byte_data = args.SerializeToString()
    self.conn.request('POST', '/query', body=byte_data)
    with self.conn.getresponse() as f:
      result = self.protos.QueryResult()
      result.ParseFromString(f.read())
      return result

  def compute_metric(self, metrics):
    args = self.protos.ComputeMetricArgs()
    args.metric_names.extend(metrics)
    byte_data = args.SerializeToString()
    self.conn.request('POST', '/compute_metric', body=byte_data)
    with self.conn.getresponse() as f:
      result = self.protos.ComputeMetricResult()
      result.ParseFromString(f.read())
      return result

  def parse(self, chunk):
    self.conn.request('POST', '/parse', body=chunk)
    with self.conn.getresponse() as f:
      return f.read()

  def notify_eof(self):
    self.conn.request('GET', '/notify_eof')
    with self.conn.getresponse() as f:
      return f.read()

  def status(self):
    self.conn.request('GET', '/status')
    with self.conn.getresponse() as f:
      result = self.protos.StatusResult()
      result.ParseFromString(f.read())
      return result

  def enable_metatrace(self):
    self.conn.request('GET', '/enable_metatrace')
    with self.conn.getresponse() as f:
      return f.read()

  def disable_and_read_metatrace(self):
    self.conn.request('GET', '/disable_and_read_metatrace')
    with self.conn.getresponse() as f:
      result = self.protos.DisableAndReadMetatraceResult()
      result.ParseFromString(f.read())
      return result
