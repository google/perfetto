--
-- Copyright 2019 The Android Open Source Project
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--     https://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.
--
SELECT o.id,
       o.type,
       o.upid,
       o.graph_sample_ts,
       o.self_size,
       o.reference_set_id,
       o.reachable,
       c.name AS type_name,
       c.deobfuscated_name AS deobfuscated_type_name,
       o.root_type
FROM heap_graph_object o JOIN heap_graph_class c ON o.type_id = c.id;
