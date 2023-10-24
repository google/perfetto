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

-- This file specifies common metrics/tables for critical user interactions. It
-- is expected to be in flux as metrics are added across different CUI types.
-- Currently we only track Chrome page loads and their associated metrics.

INCLUDE PERFETTO MODULE chrome.page_loads;

-- All critical user interaction events, including type and table with
-- associated metrics.
--
-- @column scoped_id                 Identifier of the interaction; this is not
--                                   guaranteed to be unique to the table -
--                                   rather, it is unique within an individual
--                                   interaction type. Combine with type to get
--                                   a unique identifier in this table.
-- @column type                      Type of this interaction, which together
--                                   with scoped_id uniquely identifies this
--                                   interaction. Also corresponds to a SQL
--                                   table name containing more details specific
--                                   to this type of interaction.
-- @column name                      Interaction name - e.g. 'PageLoad', 'Tap',
--                                   etc. Interactions will have unique metrics
--                                   stored in other tables.
-- @column ts                        Timestamp of the CUI event.
-- @column dur                       Duration of the CUI event.
CREATE PERFETTO TABLE chrome_interactions AS
SELECT
  navigation_id AS scoped_id,
  'chrome_page_loads' AS type,
  'PageLoad' AS name,
  navigation_start_ts AS ts,
  IFNULL(lcp, fcp) AS dur
FROM chrome_page_loads;
