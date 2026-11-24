--
-- Copyright 2026 The Android Open Source Project
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

-- Debug names given to Vulkan objects through vkSetDebugUtilsObjectNameEXT /
-- vkDebugMarkerSetObjectNameEXT (VulkanApiEvent.VkDebugUtilsObjectName).
--
-- One row is appended per naming event, so the same (vk_device, object_type,
-- object) handle can appear several times: a later row renames the object and
-- a row with a NULL name records that the previous name was cleared or the
-- object was destroyed, after which the handle may be reused. The name in
-- effect at a timestamp is the newest row with ts <= that timestamp.
CREATE PERFETTO VIEW gpu_vulkan_debug_names(
  -- The id of the row.
  id ID,
  -- Timestamp of the naming event.
  ts TIMESTAMP,
  -- Process which named the object, or NULL when the event carries no pid.
  upid JOINID(process.id),
  -- VkDevice handle owning the object, or NULL when the event carries no
  -- device.
  vk_device LONG,
  -- VkObjectType enum value of the named object, e.g. 24 for
  -- VK_OBJECT_TYPE_FRAMEBUFFER.
  object_type LONG,
  -- Vulkan object handle, interpreted per object_type.
  object LONG,
  -- Debug name, or NULL when the name was cleared (empty or omitted
  -- object_name).
  name STRING
)
AS
SELECT id, ts, upid, vk_device, object_type, object, name
FROM __intrinsic_vulkan_debug_names;
