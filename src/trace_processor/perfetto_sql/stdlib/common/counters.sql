--
-- Copyright 2023 The Android Open Source Project
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

-- No new changes allowed. Will be removed after v45 of Perfetto.
--
-- We decided to move away from the generalised `common` module and migrate the
-- most useful functionality into specialised modules.
INCLUDE PERFETTO MODULE deprecated.v42.common.args;
INCLUDE PERFETTO MODULE deprecated.v42.common.counters;