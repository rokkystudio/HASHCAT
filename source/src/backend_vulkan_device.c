/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#if defined (WITH_VULKAN_DISCOVERY)

#include "common.h"
#include "types.h"
#include "memory.h"
#include "event.h"
#include "mpsp.h"

#include "backend_vulkan.h"
#include "backend_vulkan_device.h"
#include "backend_vulkan_bindings.h"
#include "backend_vulkan_check_spirv.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HC_VULKAN_FENCE_TIMEOUT_NS (5ULL * 1000ULL * 1000ULL * 1000ULL)

/**
 * Diagnostic buffer layout used while no hashconfig exists (hashcat -I).
 *
 * Once a hashlist is loaded the exact sizes come from the values hashcat has
 * already computed for every backend in backend_session_begin(); only the
 * -I verification path has to synthesize a small, fixed layout. The element
 * count is chosen so that the check program exceeds maxComputeWorkGroupCount
 * with local_size 1 (131072 groups > 65535 on Adreno) while the derived
 * pws_comp buffer stays around 32 MiB.
 */

#define HC_VULKAN_DIAG_KERNEL_POWER  131072ULL
#define HC_VULKAN_DIAG_BITMAP_BYTES  (1024ULL * 1024ULL)
#define HC_VULKAN_DIAG_RESULT_BYTES  (HC_VULKAN_DIAG_KERNEL_POWER * 2ULL * sizeof (u32))
#define HC_VULKAN_DIAG_TMP_BYTES     16ULL
#define HC_VULKAN_DIAG_HOOK_BYTES    16ULL
#define HC_VULKAN_DIAG_PLAIN_ENTRIES 1024ULL

/**
 * Translate a Vulkan vendor id into the short vendor name printed by -I.
 *
 * Only ids from the Vulkan vendor registry are mapped. Unknown vendors keep a
 * neutral name instead of being guessed from the free-form device name.
 */

static const char *backend_vulkan_vendor_name (const u32 vendor_id)
{
  switch (vendor_id)
  {
    case 0x1002:  return "AMD";
    case 0x1010:  return "Imagination Technologies";
    case 0x10de:  return "NVIDIA";
    case 0x13b5:  return "ARM";
    case 0x5143:  return "Qualcomm";
    case 0x8086:  return "Intel";
    case 0x10005: return "Mesa";
  }

  return "Unknown";
}

/**
 * Return the buffer slot name used in diagnostics.
 */

static const char *backend_vulkan_buffer_name (const u32 buffer_idx)
{
  static const char *const names[HC_VK_BUFFER_COUNT] =
  {
    "pws_buf", "pws_amp_buf", "pws_comp_buf", "pws_idx",
    "rules", "rules_c", "combs", "combs_c",
    "bfs", "bfs_c", "tm_c",
    "bitmap_s1_a", "bitmap_s1_b", "bitmap_s1_c", "bitmap_s1_d",
    "bitmap_s2_a", "bitmap_s2_b", "bitmap_s2_c", "bitmap_s2_d",
    "plain_bufs", "digests_buf", "digests_shown",
    "salt_bufs", "esalt_bufs", "tmps", "hooks", "result",
    "extra0_buf", "extra1_buf", "extra2_buf", "extra3_buf",
    "root_css_buf", "markov_css_buf",
    "st_digests_buf", "st_salts_buf", "st_esalts_buf",
    "kernel_param"
  };

  if (buffer_idx >= HC_VK_BUFFER_COUNT) return "unknown";

  return names[buffer_idx];
}

/**
 * The immutable program registry lives in backend_vulkan_programs.c. This
 * file only consumes it through backend_vulkan_program_count/at/by_name so
 * device code never depends on registry order or storage.
 */

/**
 * Build the hc_vulkan_device metadata for one catalogued physical device.
 *
 * No Vulkan logical device is created here. This function only copies the
 * immutable properties needed by -I and --backend-info-json into the owned
 * structure, so no reader has to look at opencl_device_* slots any more.
 */

static hc_vulkan_device_t *backend_vulkan_device_metadata_create (const vulkan_core10_device_catalog_t *catalog, const u32 device_index)
{
  hc_vulkan_device_t *vulkan = (hc_vulkan_device_t *) hccalloc (1, sizeof (hc_vulkan_device_t));

  if (vulkan == NULL) return NULL;

  vulkan->index = device_index;

  vulkan->physical = catalog->physical[device_index];

  vkGetPhysicalDeviceProperties (vulkan->physical, &vulkan->properties);

  vkGetPhysicalDeviceMemoryProperties (vulkan->physical, &vulkan->memory_properties);

  snprintf (vulkan->name, sizeof (vulkan->name), "%s", vulkan->properties.deviceName);

  vulkan->vendor_id   = vulkan->properties.vendorID;
  vulkan->device_type = (u32) vulkan->properties.deviceType;
  vulkan->api_version = vulkan->properties.apiVersion;

  snprintf (vulkan->vendor, sizeof (vulkan->vendor), "%s", backend_vulkan_vendor_name (vulkan->properties.vendorID));

  snprintf (vulkan->api_version_string, sizeof (vulkan->api_version_string), "Vulkan %u.%u",
            VK_API_VERSION_MAJOR (vulkan->properties.apiVersion),
            VK_API_VERSION_MINOR (vulkan->properties.apiVersion));

  vulkan->device = VK_NULL_HANDLE;

  return vulkan;
}

/**
 * Find one Compute-capable queue family.
 */

static VkResult backend_vulkan_queue_family_find (const hc_vulkan_device_t *vulkan, u32 *queue_family)
{
  u32 queue_family_count = 0;

  vkGetPhysicalDeviceQueueFamilyProperties (vulkan->physical, &queue_family_count, NULL);

  if (queue_family_count == 0) return VK_ERROR_FEATURE_NOT_PRESENT;

  VkQueueFamilyProperties *families = (VkQueueFamilyProperties *) hccalloc (queue_family_count, sizeof (VkQueueFamilyProperties));

  if (families == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;

  vkGetPhysicalDeviceQueueFamilyProperties (vulkan->physical, &queue_family_count, families);

  VkResult result = VK_ERROR_FEATURE_NOT_PRESENT;

  for (u32 family_idx = 0; family_idx < queue_family_count; family_idx++)
  {
    if (families[family_idx].queueCount == 0) continue;
    if ((families[family_idx].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0) continue;

    *queue_family = family_idx;

    result = VK_SUCCESS;

    break;
  }

  hcfree (families);

  return result;
}

/**
 * Select a compatible memory type by property priority.
 *
 * The first pass requires every bit in property_flags and prefers
 * preferred_flags. The second pass drops the preference. Returns
 * VK_ERROR_FEATURE_NOT_PRESENT when no compatible type satisfies the exact
 * requirement.
 */

static VkResult backend_vulkan_memory_type_find (const VkPhysicalDeviceMemoryProperties *memory_properties, const u32 type_bits, const VkMemoryPropertyFlags property_flags, const VkMemoryPropertyFlags preferred_flags, u32 *memory_type_index)
{
  for (int prefer = 1; prefer >= 0; prefer--)
  {
    for (u32 type_idx = 0; type_idx < memory_properties->memoryTypeCount; type_idx++)
    {
      if ((type_bits & (1u << type_idx)) == 0) continue;

      const VkMemoryPropertyFlags flags = memory_properties->memoryTypes[type_idx].propertyFlags;

      if ((flags & property_flags) != property_flags) continue;

      if ((prefer == 1) && (preferred_flags != 0) && ((flags & preferred_flags) != preferred_flags)) continue;

      *memory_type_index = type_idx;

      return VK_SUCCESS;
    }
  }

  return VK_ERROR_FEATURE_NOT_PRESENT;
}

/**
 * Allocate and map one VkDeviceMemory for a buffer.
 */

static VkResult backend_vulkan_memory_allocate (hc_vulkan_device_t *vulkan, const VkMemoryRequirements *requirements, const VkMemoryPropertyFlags property_flags, const VkMemoryPropertyFlags preferred_flags, VkDeviceMemory *memory, u32 *memory_type_index, VkBool32 *coherent)
{
  const VkResult find_result = backend_vulkan_memory_type_find (&vulkan->memory_properties, requirements->memoryTypeBits, property_flags, preferred_flags, memory_type_index);

  if (find_result != VK_SUCCESS) return find_result;

  const VkMemoryAllocateInfo allocate_info =
  {
    .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize  = requirements->size,
    .memoryTypeIndex = *memory_type_index
  };

  const VkResult allocate_result = vkAllocateMemory (vulkan->device, &allocate_info, NULL, memory);

  if (allocate_result != VK_SUCCESS) return allocate_result;

  *coherent = (vulkan->memory_properties.memoryTypes[*memory_type_index].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? VK_TRUE : VK_FALSE;

  return VK_SUCCESS;
}

/**
 * Create one storage buffer and its host access path.
 *
 * Policy: prefer a host-visible coherent memory type so the CPU can read and
 * write the buffer directly; fall back to a host-visible non-coherent type
 * with explicit flush/invalidate; use a device-local allocation plus a
 * host-visible staging buffer only when the driver exposes no host-visible
 * memory type for the buffer. Mobile drivers normally expose at least a
 * coherent host-visible type, so the staging path is the exception.
 */

static VkResult backend_vulkan_buffer_create (hc_vulkan_device_t *vulkan, hc_vulkan_buffer_t *buffer, const u32 buffer_idx, const VkDeviceSize requested_size)
{
  snprintf (buffer->name, sizeof (buffer->name), "%s", backend_vulkan_buffer_name (buffer_idx));

  buffer->requested_size = requested_size;
  buffer->buffer         = VK_NULL_HANDLE;
  buffer->memory         = VK_NULL_HANDLE;
  buffer->host_buffer    = VK_NULL_HANDLE;
  buffer->host_memory    = VK_NULL_HANDLE;
  buffer->host_mapped    = NULL;

  if (requested_size == 0) return VK_ERROR_INITIALIZATION_FAILED;

  if (requested_size > vulkan->properties.limits.maxStorageBufferRange)
  {
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }

  const VkBufferCreateInfo buffer_info =
  {
    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size        = requested_size,
    .usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE
  };

  VkResult result = vkCreateBuffer (vulkan->device, &buffer_info, NULL, &buffer->buffer);

  if (result != VK_SUCCESS) return result;

  VkMemoryRequirements requirements = { 0 };

  vkGetBufferMemoryRequirements (vulkan->device, buffer->buffer, &requirements);

  u32 memory_type_index = 0;

  const VkBool32 force_staging = (getenv ("HASHCAT_VULKAN_FORCE_STAGING") != NULL) ? VK_TRUE : VK_FALSE;

  if (force_staging == VK_FALSE)
  {
    result = backend_vulkan_memory_allocate (vulkan, &requirements,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                             &buffer->memory, &memory_type_index, &buffer->coherent);
  }
  else
  {
    result = VK_ERROR_FEATURE_NOT_PRESENT;
  }

  if (result == VK_SUCCESS)
  {
    buffer->host_visible = VK_TRUE;
    buffer->staging      = VK_FALSE;
    buffer->size         = requirements.size;

    result = vkBindBufferMemory (vulkan->device, buffer->buffer, buffer->memory, 0);

    if (result != VK_SUCCESS) return result;

    result = vkMapMemory (vulkan->device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->host_mapped);

    if (result != VK_SUCCESS) return result;

    buffer->host_buffer = buffer->buffer;
    buffer->host_memory = buffer->memory;

    return VK_SUCCESS;
  }

  /**
   * No host-visible memory type: keep the storage buffer device-local and
   * pair it with a host-visible staging allocation for transfers.
   */

  result = backend_vulkan_memory_allocate (vulkan, &requirements,
                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                           0,
                                           &buffer->memory, &memory_type_index, &buffer->coherent);

  if (result != VK_SUCCESS) return result;

  buffer->host_visible = VK_FALSE;
  buffer->staging      = VK_TRUE;
  buffer->size         = requirements.size;

  result = vkBindBufferMemory (vulkan->device, buffer->buffer, buffer->memory, 0);

  if (result != VK_SUCCESS) return result;

  const VkBufferCreateInfo host_info =
  {
    .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size        = requested_size,
    .usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE
  };

  result = vkCreateBuffer (vulkan->device, &host_info, NULL, &buffer->host_buffer);

  if (result != VK_SUCCESS) return result;

  VkMemoryRequirements host_requirements = { 0 };

  vkGetBufferMemoryRequirements (vulkan->device, buffer->host_buffer, &host_requirements);

  VkBool32 host_coherent = VK_FALSE;

  result = backend_vulkan_memory_allocate (vulkan, &host_requirements,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                           &buffer->host_memory, &memory_type_index, &host_coherent);

  if (result != VK_SUCCESS) return result;

  buffer->coherent = host_coherent;

  result = vkBindBufferMemory (vulkan->device, buffer->host_buffer, buffer->host_memory, 0);

  if (result != VK_SUCCESS) return result;

  result = vkMapMemory (vulkan->device, buffer->host_memory, 0, VK_WHOLE_SIZE, 0, &buffer->host_mapped);

  if (result != VK_SUCCESS) return result;

  return VK_SUCCESS;
}

/**
 * Create the version-1 descriptor layout, pool, set and pipeline layout.
 *
 * The layout contains one storage buffer for every binding 1..13 of the v1
 * table. b0 is the push-constant kernel_param block and therefore has no
 * descriptor entry.
 */

static VkResult backend_vulkan_descriptors_create (hc_vulkan_device_t *vulkan)
{
  if (vulkan->properties.limits.maxPushConstantsSize < VK_BINDINGS_PUSH_CONSTANT_MAX)
  {
    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  VkDescriptorSetLayoutBinding layout_bindings[VK_BINDING_COUNT - 1u];

  for (u32 table_idx = 1; table_idx < VK_BINDING_COUNT; table_idx++)
  {
    const vk_binding_t *binding = &vk_hashcat_bindings[table_idx];

    layout_bindings[table_idx - 1u].binding         = binding->binding;
    layout_bindings[table_idx - 1u].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    layout_bindings[table_idx - 1u].descriptorCount = 1;
    layout_bindings[table_idx - 1u].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    layout_bindings[table_idx - 1u].pImmutableSamplers = NULL;
  }

  const VkDescriptorSetLayoutCreateInfo layout_info =
  {
    .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = VK_BINDING_COUNT - 1u,
    .pBindings    = layout_bindings
  };

  VkResult result = vkCreateDescriptorSetLayout (vulkan->device, &layout_info, NULL, &vulkan->descriptor_set_layout);

  if (result != VK_SUCCESS) return result;

  const VkPushConstantRange push_range =
  {
    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    .offset     = 0,
    .size       = VK_BINDINGS_PUSH_CONSTANT_MAX
  };

  const VkPipelineLayoutCreateInfo pipeline_layout_info =
  {
    .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount         = 1,
    .pSetLayouts            = &vulkan->descriptor_set_layout,
    .pushConstantRangeCount = 1,
    .pPushConstantRanges    = &push_range
  };

  result = vkCreatePipelineLayout (vulkan->device, &pipeline_layout_info, NULL, &vulkan->pipeline_layout);

  if (result != VK_SUCCESS) return result;

  const VkDescriptorPoolSize pool_size =
  {
    .type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    .descriptorCount = VK_BINDING_COUNT - 1u
  };

  const VkDescriptorPoolCreateInfo pool_info =
  {
    .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets       = 1,
    .poolSizeCount = 1,
    .pPoolSizes    = &pool_size
  };

  result = vkCreateDescriptorPool (vulkan->device, &pool_info, NULL, &vulkan->descriptor_pool);

  if (result != VK_SUCCESS) return result;

  const VkDescriptorSetAllocateInfo set_info =
  {
    .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool     = vulkan->descriptor_pool,
    .descriptorSetCount = 1,
    .pSetLayouts        = &vulkan->descriptor_set_layout
  };

  result = vkAllocateDescriptorSets (vulkan->device, &set_info, &vulkan->descriptor_set);

  if (result != VK_SUCCESS) return result;

  vulkan->descriptor_binding_count = VK_BINDING_COUNT - 1u;

  return VK_SUCCESS;
}

/**
 * Point every v1 descriptor at its fixed buffer slot.
 */

static VkResult backend_vulkan_descriptors_update (hc_vulkan_device_t *vulkan)
{
  VkDescriptorBufferInfo buffer_infos[VK_BINDING_COUNT - 1u];
  VkWriteDescriptorSet   writes[VK_BINDING_COUNT - 1u];

  memset (buffer_infos, 0, sizeof (buffer_infos));
  memset (writes,       0, sizeof (writes));

  for (u32 table_idx = 1; table_idx < VK_BINDING_COUNT; table_idx++)
  {
    const vk_binding_t *binding = &vk_hashcat_bindings[table_idx];

    if (binding->buffer_index >= HC_VK_BUFFER_COUNT) return VK_ERROR_INITIALIZATION_FAILED;

    const hc_vulkan_buffer_t *buffer = &vulkan->buffers[binding->buffer_index];

    buffer_infos[table_idx - 1u].buffer = buffer->buffer;
    buffer_infos[table_idx - 1u].offset = 0;
    buffer_infos[table_idx - 1u].range  = buffer->size;

    writes[table_idx - 1u].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[table_idx - 1u].dstSet          = vulkan->descriptor_set;
    writes[table_idx - 1u].dstBinding      = binding->binding;
    writes[table_idx - 1u].dstArrayElement = 0;
    writes[table_idx - 1u].descriptorCount = 1;
    writes[table_idx - 1u].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[table_idx - 1u].pBufferInfo     = &buffer_infos[table_idx - 1u];
  }

  vkUpdateDescriptorSets (vulkan->device, VK_BINDING_COUNT - 1u, writes, 0, NULL);

  return VK_SUCCESS;
}

/**
 * Validate a registry program against the device and create its pipeline.
 *
 * A version or table mismatch is rejected before any Vulkan object is
 * created, so a stale program can never feed a different ABI.
 */

int backend_vulkan_device_program_prepare (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, const vk_program_t *program)
{
  if ((device_param == NULL) || (device_param->vulkan == NULL) || (program == NULL)) return -1;

  hc_vulkan_device_t *vulkan = device_param->vulkan;

  if (vulkan->memory_ready == false) return -1;

  if (program->name == NULL) return -1;

  if (program->bindings_version != VK_BINDINGS_VERSION)
  {
    event_log_error (hashcat_ctx, "Vulkan program %s uses bindings version %u, expected %u.", program->name, program->bindings_version, VK_BINDINGS_VERSION);

    return -1;
  }

  if ((program->bindings == NULL) || (program->binding_count != VK_BINDING_COUNT))
  {
    event_log_error (hashcat_ctx, "Vulkan program %s does not carry the complete v1 binding table.", program->name);

    return -1;
  }

  if (memcmp (program->bindings, vk_hashcat_bindings, sizeof (vk_hashcat_bindings)) != 0)
  {
    event_log_error (hashcat_ctx, "Vulkan program %s binding table differs from v1.", program->name);

    return -1;
  }

  if ((program->spirv == NULL) || (program->spirv_len < 5u * sizeof (uint32_t)) || ((program->spirv_len % sizeof (uint32_t)) != 0))
  {
    event_log_error (hashcat_ctx, "Vulkan program %s has an invalid SPIR-V length.", program->name);

    return -1;
  }

  if (program->spirv[0] != 0x07230203u)
  {
    event_log_error (hashcat_ctx, "Vulkan program %s does not start with the SPIR-V magic word.", program->name);

    return -1;
  }

  const u32 local_x = program->local_size[0];
  const u32 local_y = program->local_size[1];
  const u32 local_z = program->local_size[2];

  if ((local_x == 0) || (local_y == 0) || (local_z == 0))
  {
    event_log_error (hashcat_ctx, "Vulkan program %s declares a zero local size.", program->name);

    return -1;
  }

  const u64 invocations = (u64) local_x * (u64) local_y * (u64) local_z;

  if (invocations > vulkan->properties.limits.maxComputeWorkGroupInvocations)
  {
    event_log_error (hashcat_ctx, "Vulkan program %s needs %llu invocations per workgroup, device limit is %u.",
                     program->name,
                     (unsigned long long) invocations,
                     vulkan->properties.limits.maxComputeWorkGroupInvocations);

    return -1;
  }

  if ((local_x > vulkan->properties.limits.maxComputeWorkGroupSize[0]) ||
      (local_y > vulkan->properties.limits.maxComputeWorkGroupSize[1]) ||
      (local_z > vulkan->properties.limits.maxComputeWorkGroupSize[2]))
  {
    event_log_error (hashcat_ctx, "Vulkan program %s local size exceeds the device workgroup limits.", program->name);

    return -1;
  }

  if (program->push_constant_bytes > VK_BINDINGS_PUSH_CONSTANT_MAX)
  {
    event_log_error (hashcat_ctx, "Vulkan program %s needs %u push-constant bytes, limit is %u.",
                     program->name,
                     program->push_constant_bytes,
                     VK_BINDINGS_PUSH_CONSTANT_MAX);

    return -1;
  }

  for (u32 pipeline_idx = 0; pipeline_idx < vulkan->pipeline_count; pipeline_idx++)
  {
    if (vulkan->pipelines[pipeline_idx].program == program) return 0;
  }

  if (vulkan->pipeline_count >= HC_VULKAN_MAX_PIPELINES) return -1;

  const VkShaderModuleCreateInfo module_info =
  {
    .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = program->spirv_len,
    .pCode    = program->spirv
  };

  VkShaderModule shader_module = VK_NULL_HANDLE;

  VkResult result = vkCreateShaderModule (vulkan->device, &module_info, NULL, &shader_module);

  if (result != VK_SUCCESS)
  {
    event_log_error (hashcat_ctx, "Vulkan program %s shader module creation failed with VkResult %d.", program->name, result);

    return -1;
  }

  const VkComputePipelineCreateInfo pipeline_info =
  {
    .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .stage  =
    {
      .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shader_module,
      .pName  = "main"
    },
    .layout = vulkan->pipeline_layout
  };

  VkPipeline pipeline = VK_NULL_HANDLE;

  result = vkCreateComputePipelines (vulkan->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline);

  if (result != VK_SUCCESS)
  {
    vkDestroyShaderModule (vulkan->device, shader_module, NULL);

    event_log_error (hashcat_ctx, "Vulkan program %s pipeline creation failed with VkResult %d.", program->name, result);

    return -1;
  }

  hc_vulkan_pipeline_t *entry = &vulkan->pipelines[vulkan->pipeline_count];

  entry->program       = program;
  entry->shader_module = shader_module;
  entry->pipeline      = pipeline;

  vulkan->pipeline_count++;

  event_log_info (hashcat_ctx, "Vulkan program %s ready: %zu bytes SPIR-V, local=%ux%ux%u, push_constants=%u.",
                  program->name,
                  program->spirv_len,
                  local_x, local_y, local_z,
                  program->push_constant_bytes);

  return 0;
}

/**
 * Open the logical device, Compute queue and the reusable command/fence pair.
 */

static VkResult backend_vulkan_device_open (hc_vulkan_device_t *vulkan)
{
  if (vulkan->device_ready == true) return VK_SUCCESS;

  u32 queue_family = 0;

  VkResult result = backend_vulkan_queue_family_find (vulkan, &queue_family);

  if (result != VK_SUCCESS) return result;

  const float queue_priority = 1.0f;

  const VkDeviceQueueCreateInfo queue_info =
  {
    .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = queue_family,
    .queueCount       = 1,
    .pQueuePriorities = &queue_priority
  };

  const VkDeviceCreateInfo device_info =
  {
    .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount    = 1,
    .pQueueCreateInfos       = &queue_info,
    .enabledLayerCount       = 0,
    .enabledExtensionCount   = 0,
    .pEnabledFeatures        = NULL
  };

  result = vkCreateDevice (vulkan->physical, &device_info, NULL, &vulkan->device);

  if (result != VK_SUCCESS) return result;

  vulkan->queue_family = queue_family;

  vkGetDeviceQueue (vulkan->device, queue_family, 0, &vulkan->queue);

  if (vulkan->queue == VK_NULL_HANDLE) return VK_ERROR_INITIALIZATION_FAILED;

  const VkCommandPoolCreateInfo pool_info =
  {
    .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    .queueFamilyIndex = queue_family
  };

  result = vkCreateCommandPool (vulkan->device, &pool_info, NULL, &vulkan->command_pool);

  if (result != VK_SUCCESS) return result;

  const VkCommandBufferAllocateInfo command_info =
  {
    .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool        = vulkan->command_pool,
    .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1
  };

  result = vkAllocateCommandBuffers (vulkan->device, &command_info, &vulkan->command_buffer);

  if (result != VK_SUCCESS) return result;

  const VkFenceCreateInfo fence_info =
  {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  result = vkCreateFence (vulkan->device, &fence_info, NULL, &vulkan->fence);

  if (result != VK_SUCCESS) return result;

  result = backend_vulkan_descriptors_create (vulkan);

  if (result != VK_SUCCESS) return result;

  vulkan->device_ready = true;

  return VK_SUCCESS;
}

/**
 * Wait for mapped host writes to become visible to the device.
 */

static VkResult backend_vulkan_flush (hc_vulkan_device_t *vulkan, hc_vulkan_buffer_t *buffer)
{
  if ((buffer->host_visible == VK_FALSE) || (buffer->coherent == VK_TRUE)) return VK_SUCCESS;

  const VkMappedMemoryRange range =
  {
    .sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
    .memory = buffer->host_memory,
    .offset = 0,
    .size   = VK_WHOLE_SIZE
  };

  return vkFlushMappedMemoryRanges (vulkan->device, 1, &range);
}

/**
 * Make device writes visible to a mapped host read.
 */

static VkResult backend_vulkan_invalidate (hc_vulkan_device_t *vulkan, hc_vulkan_buffer_t *buffer)
{
  if ((buffer->host_visible == VK_FALSE) || (buffer->coherent == VK_TRUE)) return VK_SUCCESS;

  const VkMappedMemoryRange range =
  {
    .sType  = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
    .memory = buffer->host_memory,
    .offset = 0,
    .size   = VK_WHOLE_SIZE
  };

  return vkInvalidateMappedMemoryRanges (vulkan->device, 1, &range);
}

/**
 * Reset, record and submit the reusable command buffer, then wait for its fence.
 */

static VkResult backend_vulkan_command_submit_and_wait (hc_vulkan_device_t *vulkan)
{
  VkResult result = vkEndCommandBuffer (vulkan->command_buffer);

  if (result != VK_SUCCESS) return result;

  result = vkResetFences (vulkan->device, 1, &vulkan->fence);

  if (result != VK_SUCCESS) return result;

  const VkSubmitInfo submit_info =
  {
    .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount   = 1,
    .pCommandBuffers      = &vulkan->command_buffer
  };

  result = vkQueueSubmit (vulkan->queue, 1, &submit_info, vulkan->fence);

  if (result != VK_SUCCESS) return result;

  return vkWaitForFences (vulkan->device, 1, &vulkan->fence, VK_TRUE, HC_VULKAN_FENCE_TIMEOUT_NS);
}

/**
 * Start recording the reusable one-time command buffer.
 */

static VkResult backend_vulkan_command_begin (hc_vulkan_device_t *vulkan)
{
  VkResult result = vkResetCommandBuffer (vulkan->command_buffer, 0);

  if (result != VK_SUCCESS) return result;

  const VkCommandBufferBeginInfo begin_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
  };

  return vkBeginCommandBuffer (vulkan->command_buffer, &begin_info);
}

/**
 * Record a transfer-to-shader dependency for one buffer.
 */

static void backend_vulkan_barrier_transfer_to_shader (hc_vulkan_device_t *vulkan, VkBuffer buffer)
{
  const VkBufferMemoryBarrier barrier =
  {
    .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
    .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .buffer              = buffer,
    .offset              = 0,
    .size                = VK_WHOLE_SIZE
  };

  vkCmdPipelineBarrier (vulkan->command_buffer,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0, 0, NULL, 1, &barrier, 0, NULL);
}

/**
 * Record a transfer-write-to-host-read dependency for one buffer.
 */

static void backend_vulkan_barrier_transfer_to_host (hc_vulkan_device_t *vulkan, VkBuffer buffer)
{
  const VkBufferMemoryBarrier barrier =
  {
    .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
    .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask       = VK_ACCESS_HOST_READ_BIT,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .buffer              = buffer,
    .offset              = 0,
    .size                = VK_WHOLE_SIZE
  };

  vkCmdPipelineBarrier (vulkan->command_buffer,
                        VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT,
                        0, 0, NULL, 1, &barrier, 0, NULL);
}

/**
 * Copy one complete buffer through the transfer path and wait for it.
 */

static VkResult backend_vulkan_buffer_copy (hc_vulkan_device_t *vulkan, VkBuffer source, VkBuffer destination, const VkDeviceSize bytes, const VkBool32 source_is_staging, const VkBool32 destination_is_staging)
{
  VkResult result = backend_vulkan_command_begin (vulkan);

  if (result != VK_SUCCESS) return result;

  const VkBufferCopy copy_region =
  {
    .srcOffset = 0,
    .dstOffset = 0,
    .size      = bytes
  };

  vkCmdCopyBuffer (vulkan->command_buffer, source, destination, 1, &copy_region);

  if (destination_is_staging == VK_TRUE)
  {
    backend_vulkan_barrier_transfer_to_host (vulkan, destination);
  }
  else
  {
    backend_vulkan_barrier_transfer_to_shader (vulkan, destination);
  }

  (void) source_is_staging;

  return backend_vulkan_command_submit_and_wait (vulkan);
}

/**
 * Record a Compute write-to-read/write dependency between dispatch chunks.
 */

static void backend_vulkan_barrier_compute_to_compute (hc_vulkan_device_t *vulkan)
{
  const VkMemoryBarrier barrier =
  {
    .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
  };

  vkCmdPipelineBarrier (vulkan->command_buffer,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0, 1, &barrier, 0, NULL, 0, NULL);
}

/**
 * Record a Compute write-to-host-read dependency for the result buffer.
 */

static void backend_vulkan_barrier_compute_to_host (hc_vulkan_device_t *vulkan, VkBuffer buffer)
{
  const VkBufferMemoryBarrier barrier =
  {
    .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
    .srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask       = VK_ACCESS_HOST_READ_BIT,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .buffer              = buffer,
    .offset              = 0,
    .size                = VK_WHOLE_SIZE
  };

  vkCmdPipelineBarrier (vulkan->command_buffer,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT,
                        0, 0, NULL, 1, &barrier, 0, NULL);
}

/**
 * Dispatch one registry program over group_count workgroups.
 *
 * The group count is split into chunks no larger than
 * maxComputeWorkGroupCount[0]; Adreno reports 65535, so a 131072-group check
 * needs three vkCmdDispatch calls in one command buffer. A full Compute
 * write-to-read barrier separates chunks. The reusable fence is reset before
 * the submission and waited on with a fixed timeout.
 */

int backend_vulkan_device_dispatch (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, const vk_program_t *program, const u64 group_count, const void *push_constants, const u32 push_constant_bytes)
{
  if ((device_param == NULL) || (device_param->vulkan == NULL)) return -1;
  if ((program == NULL) || (group_count == 0)) return -1;

  hc_vulkan_device_t *vulkan = device_param->vulkan;

  if (vulkan->memory_ready == false) return -1;

  if (push_constant_bytes > program->push_constant_bytes) return -1;

  if (push_constant_bytes > VK_BINDINGS_PUSH_CONSTANT_MAX) return -1;

  if (backend_vulkan_device_program_prepare (hashcat_ctx, device_param, program) != 0) return -1;

  const hc_vulkan_pipeline_t *pipeline = NULL;

  for (u32 pipeline_idx = 0; pipeline_idx < vulkan->pipeline_count; pipeline_idx++)
  {
    if (vulkan->pipelines[pipeline_idx].program == program)
    {
      pipeline = &vulkan->pipelines[pipeline_idx];

      break;
    }
  }

  if (pipeline == NULL) return -1;

  const u32 max_groups = vulkan->properties.limits.maxComputeWorkGroupCount[0];

  if (max_groups == 0) return -1;

  /**
   * A split dispatch restarts gl_GlobalInvocationID at zero. The last four
   * bytes of the push-constant block carry the base workgroup of the current
   * chunk, so a program that can be split must declare at least one u32.
   */

  if ((group_count > max_groups) && (push_constant_bytes < sizeof (u32))) return -1;

  if (push_constant_bytes > 0)
  {
    if (push_constants == NULL) return -1;

    memcpy (vulkan->push_constants, push_constants, push_constant_bytes);

    vulkan->push_constant_bytes = push_constant_bytes;
  }

  VkResult result = backend_vulkan_command_begin (vulkan);

  if (result != VK_SUCCESS) return -1;

  vkCmdBindPipeline (vulkan->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);

  vkCmdBindDescriptorSets (vulkan->command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, vulkan->pipeline_layout, 0, 1, &vulkan->descriptor_set, 0, NULL);

  u64 remaining = group_count;

  u32 split_count = 0;

  while (remaining > 0)
  {
    const u64 chunk_base = group_count - remaining;

    const u32 chunk_groups = (remaining > max_groups) ? max_groups : (u32) remaining;

    if (push_constant_bytes >= sizeof (u32))
    {
      const u32 base_group = (u32) chunk_base;

      memcpy (vulkan->push_constants + push_constant_bytes - sizeof (u32), &base_group, sizeof (u32));

      vkCmdPushConstants (vulkan->command_buffer, vulkan->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_constant_bytes, vulkan->push_constants);
    }

    vkCmdDispatch (vulkan->command_buffer, chunk_groups, 1, 1);

    split_count++;

    remaining -= chunk_groups;

    if (remaining > 0)
    {
      backend_vulkan_barrier_compute_to_compute (vulkan);
    }
  }

  backend_vulkan_barrier_compute_to_host (vulkan, vulkan->buffers[HC_VK_BUFFER_RESULT].buffer);

  result = backend_vulkan_command_submit_and_wait (vulkan);

  if (result != VK_SUCCESS)
  {
    event_log_warning (hashcat_ctx, "Vulkan dispatch of %s failed: groups=%llu splits=%u VkResult=%d.",
                       program->name,
                       (unsigned long long) group_count,
                       split_count,
                       result);

    if (result == VK_TIMEOUT)
    {
      vkDeviceWaitIdle (vulkan->device);
    }

    return -1;
  }

  vulkan->dispatch_groups_total  += group_count;
  vulkan->dispatch_splits        += split_count;

  if (group_count > vulkan->dispatch_groups_largest)
  {
    vulkan->dispatch_groups_largest = group_count;
  }

  event_log_info (hashcat_ctx, "Vulkan dispatch %s complete: groups=%llu splits=%u local=%ux%ux%u.",
                  program->name,
                  (unsigned long long) group_count,
                  split_count,
                  program->local_size[0],
                  program->local_size[1],
                  program->local_size[2]);

  return 0;
}

/**
 * Read back the complete result buffer after a dispatch.
 */

int backend_vulkan_device_readback_result (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, void *destination, const VkDeviceSize bytes)
{
  return backend_vulkan_device_download (hashcat_ctx, device_param, HC_VK_BUFFER_RESULT, destination, bytes);
}

/**
 * Push-constant block shared with tools/vulkan-hashcat-check.comp.
 */

typedef struct vk_check_parameters
{
  u32 total_elements;
  u32 stride;
  u32 seed;
  u32 base_group;
} vk_check_parameters_t;

/**
 * Fill pws_buf with a deterministic pattern and verify the complete GPU path.
 *
 * The check program processes every 32-bit word of pws_buf exactly once,
 * writes one (sum, count) pair per workgroup and is dispatched with
 * group_count = ceil(kernel_power / local_size_x). With the 131072-element
 * diagnostic layout and local_size_x = 1 this is 131072 groups, above the
 * 65535 Adreno limit, so the dispatch-splitting path is part of the test.
 *
 * The CPU reference uses the same wrapping arithmetic and compares every
 * group, not only the total. An unwritten group stays at 0xffffffff because
 * the result buffer is pre-filled with 0xff.
 */

int backend_vulkan_device_run_check (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param)
{
  if ((device_param == NULL) || (device_param->vulkan == NULL)) return -1;

  hc_vulkan_device_t *vulkan = device_param->vulkan;

  if (vulkan->memory_ready == false) return -1;

  const vk_program_t *program = backend_vulkan_program_by_name ("m2-check-sum");

  if (program == NULL) return -1;

  hc_vulkan_buffer_t *pws = &vulkan->buffers[HC_VK_BUFFER_PWS];

  const VkDeviceSize pattern_bytes = pws->requested_size;

  if ((pattern_bytes == 0) || ((pattern_bytes % sizeof (u32)) != 0)) return -1;

  const u64 total_elements = pattern_bytes / sizeof (u32);

  if (total_elements > 0xffffffffULL) return -1;

  const u64 local_size = (u64) program->local_size[0] * (u64) program->local_size[1] * (u64) program->local_size[2];

  if (local_size == 0) return -1;

  u64 kernel_power = device_param->kernel_power;

  if (kernel_power == 0) kernel_power = pattern_bytes / sizeof (pw_t);

  if (kernel_power == 0) return -1;

  const u64 group_count = (kernel_power + local_size - 1) / local_size;

  const VkDeviceSize result_bytes = (VkDeviceSize) group_count * 2ULL * sizeof (u32);

  if (result_bytes > vulkan->buffers[HC_VK_BUFFER_RESULT].requested_size) return -1;

  u8 *pattern = (u8 *) hcmalloc ((size_t) pattern_bytes);

  if (pattern == NULL) return -1;

  for (VkDeviceSize idx = 0; idx < pattern_bytes; idx++)
  {
    pattern[idx] = (u8) ((idx * 37ULL + 11ULL) & 0xffULL);
  }

  int rc = -1;

  u32 *result_words = (u32 *) hcmalloc ((size_t) result_bytes);

  u32 *reference_sum = (u32 *) hccalloc ((size_t) group_count, sizeof (u32));

  u32 *reference_cnt = (u32 *) hccalloc ((size_t) group_count, sizeof (u32));

  if ((result_words == NULL) || (reference_sum == NULL) || (reference_cnt == NULL)) goto cleanup;

  if (backend_vulkan_device_upload (hashcat_ctx, device_param, HC_VK_BUFFER_PWS, pattern, pattern_bytes) != 0)
  {
    event_log_warning (hashcat_ctx, "Vulkan check could not upload the pws_buf pattern.");

    goto cleanup;
  }

  if (backend_vulkan_device_memset (hashcat_ctx, device_param, HC_VK_BUFFER_RESULT, 0xff, result_bytes) != 0)
  {
    event_log_warning (hashcat_ctx, "Vulkan check could not initialize the result buffer.");

    goto cleanup;
  }

  const vk_check_parameters_t parameters =
  {
    .total_elements = (u32) total_elements,
    .stride         = (u32) group_count,
    .seed           = 0x9e3779b9u,
    .base_group     = 0u
  };

  if (backend_vulkan_device_dispatch (hashcat_ctx, device_param, program, group_count, &parameters, sizeof (parameters)) != 0)
  {
    event_log_warning (hashcat_ctx, "Vulkan check dispatch failed.");

    goto cleanup;
  }

  if (backend_vulkan_device_readback_result (hashcat_ctx, device_param, result_words, result_bytes) != 0)
  {
    event_log_warning (hashcat_ctx, "Vulkan check result readback failed.");

    goto cleanup;
  }

  /**
   * CPU reference: element index i is handled by invocation (i mod stride),
   * which is its workgroup because local_size is 1.
   */

  const u32 *pattern_words = (const u32 *) pattern;

  u64 total_count = 0;

  for (u64 element = 0; element < total_elements; element++)
  {
    const u32 value = pattern_words[element];

    const u32 contribution = (value * 1664525u + 1013904223u) ^ ((u32) element * 2654435769u) ^ parameters.seed;

    const u64 group = element % group_count;

    reference_sum[group] += contribution;
    reference_cnt[group] += 1u;
  }

  u64 mismatches = 0;

  for (u64 group = 0; group < group_count; group++)
  {
    const u32 actual_sum = result_words[group * 2 + 0];
    const u32 actual_cnt = result_words[group * 2 + 1];

    if ((actual_sum != reference_sum[group]) || (actual_cnt != reference_cnt[group]))
    {
      mismatches++;
    }

    total_count += actual_cnt;
  }

  if (mismatches != 0)
  {
    event_log_warning (hashcat_ctx, "Vulkan check mismatch: %llu of %llu groups differ from the CPU reference.",
                       (unsigned long long) mismatches,
                       (unsigned long long) group_count);

    goto cleanup;
  }

  if (total_count != total_elements)
  {
    event_log_warning (hashcat_ctx, "Vulkan check coverage mismatch: %llu of %llu elements were reported.",
                       (unsigned long long) total_count,
                       (unsigned long long) total_elements);

    goto cleanup;
  }

  u32 total_sum = 0;

  for (u64 group = 0; group < group_count; group++)
  {
    total_sum += reference_sum[group];
  }

  event_log_info (hashcat_ctx, "Vulkan check passed: groups=%llu elements=%llu sum=%08x count=%llu dispatch_splits=%u",
                  (unsigned long long) group_count,
                  (unsigned long long) total_elements,
                  total_sum,
                  (unsigned long long) total_count,
                  vulkan->dispatch_splits);

  rc = 0;

cleanup:
  hcfree (pattern);
  hcfree (result_words);
  hcfree (reference_sum);
  hcfree (reference_cnt);

  return rc;
}

/**
 * Upload bytes into one buffer slot.
 */

int backend_vulkan_device_upload (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, const u32 buffer_idx, const void *source, const VkDeviceSize bytes)
{
  (void) hashcat_ctx;

  if ((device_param == NULL) || (device_param->vulkan == NULL)) return -1;
  if (source == NULL) return -1;
  if (buffer_idx >= HC_VK_BUFFER_COUNT) return -1;

  hc_vulkan_device_t *vulkan = device_param->vulkan;

  if (vulkan->memory_ready == false) return -1;

  hc_vulkan_buffer_t *buffer = &vulkan->buffers[buffer_idx];

  if (bytes > buffer->requested_size) return -1;

  memcpy (buffer->host_mapped, source, (size_t) bytes);

  VkResult result = backend_vulkan_flush (vulkan, buffer);

  if (result != VK_SUCCESS) return -1;

  if (buffer->staging == VK_TRUE)
  {
    result = backend_vulkan_buffer_copy (vulkan, buffer->host_buffer, buffer->buffer, bytes, VK_TRUE, VK_FALSE);

    if (result != VK_SUCCESS) return -1;
  }

  return 0;
}

/**
 * Download bytes from one buffer slot after the producing submission finished.
 */

int backend_vulkan_device_download (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, const u32 buffer_idx, void *destination, const VkDeviceSize bytes)
{
  (void) hashcat_ctx;

  if ((device_param == NULL) || (device_param->vulkan == NULL)) return -1;
  if (destination == NULL) return -1;
  if (buffer_idx >= HC_VK_BUFFER_COUNT) return -1;

  hc_vulkan_device_t *vulkan = device_param->vulkan;

  if (vulkan->memory_ready == false) return -1;

  hc_vulkan_buffer_t *buffer = &vulkan->buffers[buffer_idx];

  if (bytes > buffer->requested_size) return -1;

  if (buffer->staging == VK_TRUE)
  {
    const VkResult result = backend_vulkan_buffer_copy (vulkan, buffer->buffer, buffer->host_buffer, bytes, VK_FALSE, VK_TRUE);

    if (result != VK_SUCCESS) return -1;
  }

  const VkResult result = backend_vulkan_invalidate (vulkan, buffer);

  if (result != VK_SUCCESS) return -1;

  memcpy (destination, buffer->host_mapped, (size_t) bytes);

  return 0;
}

/**
 * Fill bytes of one buffer slot with a constant value.
 */

int backend_vulkan_device_memset (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, const u32 buffer_idx, const int value, const VkDeviceSize bytes)
{
  if ((device_param == NULL) || (device_param->vulkan == NULL)) return -1;
  if (buffer_idx >= HC_VK_BUFFER_COUNT) return -1;

  hc_vulkan_device_t *vulkan = device_param->vulkan;

  if (vulkan->memory_ready == false) return -1;

  hc_vulkan_buffer_t *buffer = &vulkan->buffers[buffer_idx];

  if (bytes > buffer->requested_size) return -1;

  memset (buffer->host_mapped, value, (size_t) bytes);

  const VkResult result = backend_vulkan_flush (vulkan, buffer);

  if (result != VK_SUCCESS) return -1;

  if (buffer->staging == VK_TRUE)
  {
    const VkResult copy_result = backend_vulkan_buffer_copy (vulkan, buffer->host_buffer, buffer->buffer, bytes, VK_TRUE, VK_FALSE);

    if (copy_result != VK_SUCCESS) return -1;
  }

  (void) hashcat_ctx;

  return 0;
}

/**
 * Copy one buffer slot into another through the host-visible path.
 *
 * Vulkan 1.0 could record this as a single vkCmdCopyBuffer, but the slots may
 * combine direct and staged memory and M2 has no caller that needs the
 * device-to-device fast path. The host round trip keeps the staging rules in
 * one place and is documented as an M3 optimisation point.
 */

int backend_vulkan_device_copy (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, const u32 source_idx, const u32 destination_idx, const VkDeviceSize bytes)
{
  if (source_idx >= HC_VK_BUFFER_COUNT) return -1;
  if (destination_idx >= HC_VK_BUFFER_COUNT) return -1;

  hc_vulkan_device_t *vulkan = device_param->vulkan;

  if (vulkan == NULL) return -1;

  if (bytes > vulkan->buffers[source_idx].requested_size) return -1;
  if (bytes > vulkan->buffers[destination_idx].requested_size) return -1;

  void *temporary = hcmalloc ((size_t) bytes);

  if (temporary == NULL) return -1;

  if (backend_vulkan_device_download (hashcat_ctx, device_param, source_idx, temporary, bytes) != 0)
  {
    hcfree (temporary);

    return -1;
  }

  if (backend_vulkan_device_upload (hashcat_ctx, device_param, destination_idx, temporary, bytes) != 0)
  {
    hcfree (temporary);

    return -1;
  }

  hcfree (temporary);

  return 0;
}

/**
 * Fill the buffer size table.
 *
 * When hashcat has already computed the per-device sizes for an active
 * hashconfig, those exact values are used; this keeps the Vulkan layout a
 * copy of the OpenCL one. During -I no hashconfig exists, so a fixed
 * diagnostic layout is synthesized. Both paths cover the same 37 slots.
 */

static int backend_vulkan_buffer_sizes (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, VkDeviceSize *sizes)
{
  const hashconfig_t *hashconfig = hashcat_ctx->hashconfig;

  if ((hashconfig != NULL) && (device_param->size_pws != 0))
  {
    const u64 extra_per_buffer = 4096 + (device_param->extra_buffer_size / 4);

    sizes[HC_VK_BUFFER_PWS]           = device_param->size_pws;
    sizes[HC_VK_BUFFER_PWS_AMP]       = device_param->size_pws_amp;
    sizes[HC_VK_BUFFER_PWS_COMP]      = device_param->size_pws_comp;
    sizes[HC_VK_BUFFER_PWS_IDX]       = device_param->size_pws_idx;
    sizes[HC_VK_BUFFER_RULES]         = device_param->size_rules;
    sizes[HC_VK_BUFFER_RULES_C]       = device_param->size_rules_c;
    sizes[HC_VK_BUFFER_COMBS]         = device_param->size_combs;
    sizes[HC_VK_BUFFER_COMBS_C]       = device_param->size_combs;
    sizes[HC_VK_BUFFER_BFS]           = device_param->size_bfs;
    sizes[HC_VK_BUFFER_BFS_C]         = device_param->size_bfs;
    sizes[HC_VK_BUFFER_TM_C]          = device_param->size_tm;

    const u64 bitmap_bytes = (hashcat_ctx->bitmap_ctx != NULL) ? hashcat_ctx->bitmap_ctx->bitmap_size : HC_VULKAN_DIAG_BITMAP_BYTES;

    for (u32 bitmap_idx = HC_VK_BUFFER_BITMAP_S1_A; bitmap_idx <= HC_VK_BUFFER_BITMAP_S2_D; bitmap_idx++)
    {
      sizes[bitmap_idx] = bitmap_bytes;
    }

    sizes[HC_VK_BUFFER_PLAIN]         = device_param->size_plains;
    sizes[HC_VK_BUFFER_DIGESTS]       = device_param->size_digests;
    sizes[HC_VK_BUFFER_DIGESTS_SHOWN] = device_param->size_shown;
    sizes[HC_VK_BUFFER_SALTS]         = device_param->size_salts;
    sizes[HC_VK_BUFFER_ESALTS]        = device_param->size_esalts;
    sizes[HC_VK_BUFFER_TMPS]          = device_param->size_tmps;
    sizes[HC_VK_BUFFER_HOOKS]         = device_param->size_hooks;
    sizes[HC_VK_BUFFER_RESULT]        = device_param->size_results;

    sizes[HC_VK_BUFFER_EXTRA0]        = extra_per_buffer;
    sizes[HC_VK_BUFFER_EXTRA1]        = extra_per_buffer;
    sizes[HC_VK_BUFFER_EXTRA2]        = extra_per_buffer;
    sizes[HC_VK_BUFFER_EXTRA3]        = extra_per_buffer;

    sizes[HC_VK_BUFFER_ROOT_CSS]      = device_param->size_root_css;
    sizes[HC_VK_BUFFER_MARKOV_CSS]    = device_param->size_markov_css;
    sizes[HC_VK_BUFFER_ST_DIGESTS]    = device_param->size_st_digests;
    sizes[HC_VK_BUFFER_ST_SALTS]      = device_param->size_st_salts;
    sizes[HC_VK_BUFFER_ST_ESALTS]     = device_param->size_st_esalts;
    sizes[HC_VK_BUFFER_KERNEL_PARAM]  = device_param->size_kernel_params;

    /**
     * Optional slots (esalts, short-term buffers) are zero when the selected
     * mode does not use them. A zero-size VkBuffer is invalid, so the fixed
     * v1 descriptor set keeps them as one word each.
     */

    for (u32 buffer_idx = 0; buffer_idx < HC_VK_BUFFER_COUNT; buffer_idx++)
    {
      if (sizes[buffer_idx] == 0) sizes[buffer_idx] = sizeof (u32);
    }

    return 0;
  }

  const u64 kernel_power = HC_VULKAN_DIAG_KERNEL_POWER;

  sizes[HC_VK_BUFFER_PWS]           = kernel_power * sizeof (pw_t);
  sizes[HC_VK_BUFFER_PWS_AMP]       = kernel_power * sizeof (pw_t);
  sizes[HC_VK_BUFFER_PWS_COMP]      = kernel_power * (sizeof (u32) * 64);
  sizes[HC_VK_BUFFER_PWS_IDX]       = (kernel_power + 1) * sizeof (pw_idx_t);
  sizes[HC_VK_BUFFER_RULES]         = KERNEL_RULES * sizeof (kernel_rule_t);
  sizes[HC_VK_BUFFER_RULES_C]       = KERNEL_RULES * sizeof (kernel_rule_t);
  sizes[HC_VK_BUFFER_COMBS]         = KERNEL_COMBS * sizeof (pw_t);
  sizes[HC_VK_BUFFER_COMBS_C]       = KERNEL_COMBS * sizeof (pw_t);
  sizes[HC_VK_BUFFER_BFS]           = KERNEL_BFS * sizeof (bf_t);
  sizes[HC_VK_BUFFER_BFS_C]         = KERNEL_BFS * sizeof (bf_t);
  sizes[HC_VK_BUFFER_TM_C]          = 32 * sizeof (bs_word_t);

  for (u32 bitmap_idx = HC_VK_BUFFER_BITMAP_S1_A; bitmap_idx <= HC_VK_BUFFER_BITMAP_S2_D; bitmap_idx++)
  {
    sizes[bitmap_idx] = HC_VULKAN_DIAG_BITMAP_BYTES;
  }

  sizes[HC_VK_BUFFER_PLAIN]         = HC_VULKAN_DIAG_PLAIN_ENTRIES * sizeof (plain_t);
  sizes[HC_VK_BUFFER_DIGESTS]       = 256;
  sizes[HC_VK_BUFFER_DIGESTS_SHOWN] = sizeof (u32);
  sizes[HC_VK_BUFFER_SALTS]         = sizeof (salt_t);
  sizes[HC_VK_BUFFER_ESALTS]        = 256;
  sizes[HC_VK_BUFFER_TMPS]          = kernel_power * HC_VULKAN_DIAG_TMP_BYTES;
  sizes[HC_VK_BUFFER_HOOKS]         = kernel_power * HC_VULKAN_DIAG_HOOK_BYTES;
  sizes[HC_VK_BUFFER_RESULT]        = HC_VULKAN_DIAG_RESULT_BYTES;

  sizes[HC_VK_BUFFER_EXTRA0]        = 4096;
  sizes[HC_VK_BUFFER_EXTRA1]        = 4096;
  sizes[HC_VK_BUFFER_EXTRA2]        = 4096;
  sizes[HC_VK_BUFFER_EXTRA3]        = 4096;

  sizes[HC_VK_BUFFER_ROOT_CSS]      = SP_PW_MAX * sizeof (cs_t);
  sizes[HC_VK_BUFFER_MARKOV_CSS]    = SP_PW_MAX * CHARSIZ * sizeof (cs_t);
  sizes[HC_VK_BUFFER_ST_DIGESTS]    = 256;
  sizes[HC_VK_BUFFER_ST_SALTS]      = sizeof (salt_t);
  sizes[HC_VK_BUFFER_ST_ESALTS]     = 256;
  sizes[HC_VK_BUFFER_KERNEL_PARAM]  = sizeof (kernel_param_t);

  return 0;
}

/**
 * Allocate the complete hashcat buffer set on one opened device.
 */

static int backend_vulkan_buffers_create (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param)
{
  hc_vulkan_device_t *vulkan = device_param->vulkan;

  VkDeviceSize sizes[HC_VK_BUFFER_COUNT] = { 0 };

  if (backend_vulkan_buffer_sizes (hashcat_ctx, device_param, sizes) != 0) return -1;

  for (u32 buffer_idx = 0; buffer_idx < HC_VK_BUFFER_COUNT; buffer_idx++)
  {
    if (sizes[buffer_idx] == 0)
    {
      event_log_error (hashcat_ctx, "Vulkan device #%u: buffer %s has a zero size.", device_param->device_id + 1, backend_vulkan_buffer_name (buffer_idx));

      return -1;
    }

    const VkResult create_result = backend_vulkan_buffer_create (vulkan, &vulkan->buffers[buffer_idx], buffer_idx, sizes[buffer_idx]);

    vulkan->buffer_count = buffer_idx + 1;

    if (create_result != VK_SUCCESS)
    {
      event_log_error (hashcat_ctx, "Vulkan device #%u: buffer %s (%" PRIu64 " bytes) allocation failed with VkResult %d.",
                       device_param->device_id + 1,
                       backend_vulkan_buffer_name (buffer_idx),
                       (u64) sizes[buffer_idx],
                       create_result);

      if (create_result == VK_ERROR_OUT_OF_DEVICE_MEMORY && sizes[buffer_idx] > vulkan->properties.limits.maxStorageBufferRange)
      {
        event_log_error (hashcat_ctx, "Vulkan device #%u: buffer exceeds maxStorageBufferRange (%" PRIu64 " bytes).",
                         device_param->device_id + 1,
                         (u64) vulkan->properties.limits.maxStorageBufferRange);
      }

      return -1;
    }

    event_log_info (hashcat_ctx, "Vulkan device #%u: buffer %-16s %8" PRIu64 " KiB (%s, coherent=%u)",
                    device_param->device_id + 1,
                    vulkan->buffers[buffer_idx].name,
                    (u64) (sizes[buffer_idx] / 1024),
                    vulkan->buffers[buffer_idx].staging ? "staging" : "host-visible",
                    (u32) vulkan->buffers[buffer_idx].coherent);
  }

  return 0;
}

/**
 * Release the logical device and every runtime object but keep the metadata.
 */

static void backend_vulkan_device_close (hc_vulkan_device_t *vulkan)
{
  if (vulkan == NULL) return;

  if (vulkan->device == VK_NULL_HANDLE)
  {
    vulkan->device_ready = false;
    vulkan->memory_ready = false;
    vulkan->buffer_count = 0;
    vulkan->pipeline_count = 0;

    return;
  }

  vkDeviceWaitIdle (vulkan->device);

  if (vulkan->descriptor_pool != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorPool (vulkan->device, vulkan->descriptor_pool, NULL);

    vulkan->descriptor_pool = VK_NULL_HANDLE;
    vulkan->descriptor_set  = VK_NULL_HANDLE;
  }

  for (u32 pipeline_idx = 0; pipeline_idx < vulkan->pipeline_count; pipeline_idx++)
  {
    if (vulkan->pipelines[pipeline_idx].pipeline != VK_NULL_HANDLE)
    {
      vkDestroyPipeline (vulkan->device, vulkan->pipelines[pipeline_idx].pipeline, NULL);
    }

    if (vulkan->pipelines[pipeline_idx].shader_module != VK_NULL_HANDLE)
    {
      vkDestroyShaderModule (vulkan->device, vulkan->pipelines[pipeline_idx].shader_module, NULL);
    }
  }

  for (u32 buffer_idx = 0; buffer_idx < vulkan->buffer_count; buffer_idx++)
  {
    hc_vulkan_buffer_t *buffer = &vulkan->buffers[buffer_idx];

    if (buffer->host_mapped != NULL)
    {
      vkUnmapMemory (vulkan->device, buffer->host_memory);
    }

    /**
     * Direct mode aliases host_buffer/host_memory to buffer/memory. Destroying
     * both would release the same Vulkan object twice; only the staging mode
     * owns a second pair.
     */

    if (buffer->staging == VK_TRUE)
    {
      if (buffer->host_buffer != VK_NULL_HANDLE)
      {
        vkDestroyBuffer (vulkan->device, buffer->host_buffer, NULL);
      }

      if (buffer->host_memory != VK_NULL_HANDLE)
      {
        vkFreeMemory (vulkan->device, buffer->host_memory, NULL);
      }
    }

    if (buffer->buffer != VK_NULL_HANDLE)
    {
      vkDestroyBuffer (vulkan->device, buffer->buffer, NULL);
    }

    if (buffer->memory != VK_NULL_HANDLE)
    {
      vkFreeMemory (vulkan->device, buffer->memory, NULL);
    }

    memset (buffer, 0, sizeof (*buffer));
  }

  if (vulkan->descriptor_set_layout != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorSetLayout (vulkan->device, vulkan->descriptor_set_layout, NULL);
  }

  if (vulkan->pipeline_layout != VK_NULL_HANDLE)
  {
    vkDestroyPipelineLayout (vulkan->device, vulkan->pipeline_layout, NULL);
  }

  if (vulkan->command_pool != VK_NULL_HANDLE)
  {
    vkDestroyCommandPool (vulkan->device, vulkan->command_pool, NULL);
  }

  if (vulkan->fence != VK_NULL_HANDLE)
  {
    vkDestroyFence (vulkan->device, vulkan->fence, NULL);
  }

  vkDestroyDevice (vulkan->device, NULL);

  vulkan->device                 = VK_NULL_HANDLE;
  vulkan->queue                  = VK_NULL_HANDLE;
  vulkan->command_pool           = VK_NULL_HANDLE;
  vulkan->command_buffer         = VK_NULL_HANDLE;
  vulkan->fence                  = VK_NULL_HANDLE;
  vulkan->descriptor_pool        = VK_NULL_HANDLE;
  vulkan->descriptor_set         = VK_NULL_HANDLE;
  vulkan->descriptor_set_layout  = VK_NULL_HANDLE;
  vulkan->pipeline_layout        = VK_NULL_HANDLE;
  vulkan->device_ready           = false;
  vulkan->memory_ready           = false;
  vulkan->buffer_count           = 0;
  vulkan->pipeline_count         = 0;
}

/**
 * Report process RSS in KiB from /proc/self/status (Linux/Android).
 */

u64 backend_vulkan_process_rss_kib (void)
{
  FILE *status = fopen ("/proc/self/status", "r");

  if (status == NULL) return 0;

  char line[256];

  u64 rss_kib = 0;

  while (fgets (line, sizeof (line), status) != NULL)
  {
    if (strncmp (line, "VmRSS:", 6) != 0) continue;

    if (sscanf (line + 6, "%" SCNu64, &rss_kib) != 1) rss_kib = 0;

    break;
  }

  fclose (status);

  return rss_kib;
}

/**
 * Open the logical device and allocate every hashcat buffer on it.
 *
 * Requires an is_vulkan device_param produced by backend_vulkan_devices_init.
 * Repeated calls are idempotent while the runtime is already open. On any
 * failure the partially created runtime is closed before returning, so the
 * caller never sees a half-initialized payload.
 */

int backend_vulkan_devices_memory_init (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param)
{
  if ((device_param == NULL) || (device_param->is_vulkan == false) || (device_param->vulkan == NULL)) return -1;

  hc_vulkan_device_t *vulkan = device_param->vulkan;

  if (vulkan->memory_ready == true) return 0;

  VkResult result = backend_vulkan_device_open (vulkan);

  if (result != VK_SUCCESS)
  {
    event_log_error (hashcat_ctx, "Vulkan device #%u: logical device creation failed with VkResult %d.", device_param->device_id + 1, result);

    backend_vulkan_device_close (vulkan);

    return -1;
  }

  if (backend_vulkan_buffers_create (hashcat_ctx, device_param) != 0)
  {
    backend_vulkan_device_close (vulkan);

    return -1;
  }

  const VkResult descriptor_result = backend_vulkan_descriptors_update (vulkan);

  if (descriptor_result != VK_SUCCESS)
  {
    event_log_error (hashcat_ctx, "Vulkan device #%u: descriptor update failed with VkResult %d.", device_param->device_id + 1, descriptor_result);

    backend_vulkan_device_close (vulkan);

    return -1;
  }

  vulkan->memory_ready = true;

  for (u32 program_idx = 0; program_idx < backend_vulkan_program_count (); program_idx++)
  {
    const vk_program_t *program = backend_vulkan_program_at (program_idx);

    if (backend_vulkan_device_program_prepare (hashcat_ctx, device_param, program) != 0)
    {
      backend_vulkan_device_close (vulkan);

      return -1;
    }
  }

  return 0;
}

/**
 * Upload a deterministic pattern into pws_buf and read it back.
 *
 * This validates the CPU-to-GPU-to-CPU path of the memory layer before any
 * shader exists. It deliberately stays smaller than the slot and uses the
 * normal upload/download entry points, not a private staging copy.
 */

int backend_vulkan_device_memory_selftest (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param)
{
  (void) hashcat_ctx;

  if ((device_param == NULL) || (device_param->vulkan == NULL)) return -1;

  hc_vulkan_device_t *vulkan = device_param->vulkan;

  if (vulkan->memory_ready == false) return -1;

  hc_vulkan_buffer_t *pws = &vulkan->buffers[HC_VK_BUFFER_PWS];

  u32 bytes = 4096;

  if (pws->requested_size < bytes) bytes = (u32) pws->requested_size;

  u8 *pattern = (u8 *) hcmalloc (bytes);
  u8 *readback = (u8 *) hcmalloc (bytes);

  if ((pattern == NULL) || (readback == NULL))
  {
    hcfree (pattern);
    hcfree (readback);

    return -1;
  }

  for (u32 idx = 0; idx < bytes; idx++)
  {
    pattern[idx] = (u8) ((idx * 37u + 11u) & 0xffu);
  }

  memset (readback, 0, bytes);

  if (backend_vulkan_device_upload (hashcat_ctx, device_param, HC_VK_BUFFER_PWS, pattern, bytes) != 0)
  {
    hcfree (pattern);
    hcfree (readback);

    return -1;
  }

  if (backend_vulkan_device_download (hashcat_ctx, device_param, HC_VK_BUFFER_PWS, readback, bytes) != 0)
  {
    hcfree (pattern);
    hcfree (readback);

    return -1;
  }

  const int diff = memcmp (pattern, readback, bytes);

  hcfree (pattern);
  hcfree (readback);

  return (diff == 0) ? 0 : -1;
}

/**
 * Release the Vulkan runtime of one device while keeping its metadata.
 */

int backend_vulkan_devices_memory_term (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param)
{
  (void) hashcat_ctx;

  if ((device_param == NULL) || (device_param->vulkan == NULL)) return 0;

  backend_vulkan_device_close (device_param->vulkan);

  return 0;
}

/**
 * Fixed CPU reference digests for the mode 0 self-check.
 *
 * The values are the standard MD5 digests of the fixed public strings below,
 * stored as hashcat's little-endian digest_buf words:
 *   "hashcat"          -> 8743b52063cd84097a65d1633f5c74f5
 *   'a' * 55           -> ef1772b6dff9a122358552954ad0df65
 */

static const u32 backend_vulkan_md5_kat_digest_hashcat[4] = { 0x20b54387u, 0x0984cd63u, 0x63d1657au, 0xf5745c3fu };
static const u32 backend_vulkan_md5_kat_digest_a55[4]     = { 0xb67217efu, 0x22a1f9dfu, 0x95528535u, 0x65dfd04au };

/**
 * Run a fixed-vector MD5 straight check through the real program.
 *
 * Three candidates are uploaded through the normal buffer path: "hashcat"
 * (matches digest 0), "hashcat!" (matches nothing) and 55 x 'a' (matches
 * digest 1). The program must report exactly two hits and their plain_t
 * records must carry the right gidvid/digest_pos/hash_pos. This is the CPU-
 * referenced KAT for the single stage mode 0 uses.
 */

int backend_vulkan_device_md5_straight_check (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param)
{
  if ((device_param == NULL) || (device_param->vulkan == NULL)) return -1;

  hc_vulkan_device_t *vulkan = device_param->vulkan;

  if (vulkan->memory_ready == false) return -1;

  const vk_program_t *program = backend_vulkan_program_md5_straight ();

  if (program == NULL)
  {
    event_log_error (hashcat_ctx, "Vulkan mode 0 straight program is not registered.");

    return -1;
  }

  pw_t *pws = (pw_t *) hccalloc (3, sizeof (pw_t));

  if (pws == NULL) return -1;

  memcpy (pws[0].i, "hashcat", 7);
  pws[0].pw_len = 7;

  memcpy (pws[1].i, "hashcat!", 8);
  pws[1].pw_len = 8;

  memset (pws[2].i, 'a', 55);
  pws[2].pw_len = 55;

  u32 digests[8];

  memcpy (&digests[0], backend_vulkan_md5_kat_digest_hashcat, sizeof (backend_vulkan_md5_kat_digest_hashcat));
  memcpy (&digests[4], backend_vulkan_md5_kat_digest_a55,     sizeof (backend_vulkan_md5_kat_digest_a55));

  int rc = -1;

  if (backend_vulkan_device_upload (hashcat_ctx, device_param, HC_VK_BUFFER_PWS, pws, 3 * sizeof (pw_t)) != 0) goto done;
  if (backend_vulkan_device_upload (hashcat_ctx, device_param, HC_VK_BUFFER_DIGESTS, digests, sizeof (digests)) != 0) goto done;
  if (backend_vulkan_device_memset (hashcat_ctx, device_param, HC_VK_BUFFER_RESULT, 0, sizeof (u32)) != 0) goto done;

  {
    vk_md5_straight_push_t push;

    memset (&push, 0, sizeof (push));

    push.kernel_param.salt_pos_host       = 0;
    push.kernel_param.digests_cnt         = 2;
    push.kernel_param.digests_offset_host = 0;
    push.kernel_param.il_cnt              = 1;
    push.kernel_param.pws_pos             = 0;
    push.kernel_param.gid_max             = 3;

    const u64 group_count = (3 + program->local_size[0] - 1) / program->local_size[0];

    if (backend_vulkan_device_dispatch (hashcat_ctx, device_param, program, group_count, &push, VK_MD5_STRAIGHT_PUSH_BYTES) != 0) goto done;
  }

  {
    u32 count = 0;

    if (backend_vulkan_device_download (hashcat_ctx, device_param, HC_VK_BUFFER_RESULT, &count, sizeof (count)) != 0) goto done;

    if (count != 2)
    {
      event_log_error (hashcat_ctx, "Vulkan MD5 straight self-check: expected 2 hits, got %u.", count);

      goto done;
    }

    plain_t plains[2];

    if (backend_vulkan_device_download (hashcat_ctx, device_param, HC_VK_BUFFER_PLAIN, plains, 2 * sizeof (plain_t)) != 0) goto done;

    u32 seen_hashcat = 0;
    u32 seen_a55     = 0;

    for (u32 idx = 0; idx < 2; idx++)
    {
      if ((plains[idx].salt_pos != 0) || (plains[idx].il_pos != 0)) continue;

      if ((plains[idx].gidvid == 0) && (plains[idx].digest_pos == 0) && (plains[idx].hash_pos == 0)) seen_hashcat = 1;
      if ((plains[idx].gidvid == 2) && (plains[idx].digest_pos == 1) && (plains[idx].hash_pos == 1)) seen_a55     = 1;
    }

    if ((seen_hashcat == 0) || (seen_a55 == 0))
    {
      event_log_error (hashcat_ctx, "Vulkan MD5 straight self-check: plain_t records do not match the CPU reference.");

      goto done;
    }

    event_log_info (hashcat_ctx, "Vulkan MD5 straight self-check passed: vectors=3 hits=2 (1-block 0..55 bytes).");
  }

  rc = 0;

done:
  hcfree (pws);

  return rc;
}

/**
 * Expand the host candidate arrays and dispatch one mode 0 straight batch.
 *
 * hashcat keeps fast-attack candidates compressed in pws_comp/pws_idx on the
 * host and normally expands them with the OpenCL decompress kernel. Vulkan
 * expands the same arrays on the host and uploads pw_t records directly, so
 * the GPU only executes the real MD5/compare stage. A candidate longer than
 * one MD5 block is refused with a message instead of being skipped.
 */

int backend_vulkan_device_run_md5_straight (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, const u64 pws_pos, const u64 pws_cnt)
{
  if ((device_param == NULL) || (device_param->vulkan == NULL)) return -1;

  if (pws_cnt == 0) return 0;

  hc_vulkan_device_t *vulkan = device_param->vulkan;

  if (vulkan->memory_ready == false) return -1;

  if ((pws_cnt * sizeof (pw_t)) > device_param->size_pws)
  {
    event_log_error (hashcat_ctx, "Vulkan device #%u: %llu candidates do not fit the %llu-byte pws buffer.",
                     device_param->device_id + 1,
                     (unsigned long long) pws_cnt,
                     (unsigned long long) device_param->size_pws);

    return -1;
  }

  const vk_program_t *program = backend_vulkan_program_md5_straight ();

  if (program == NULL) return -1;

  pw_t *pws = (pw_t *) hccalloc ((size_t) pws_cnt, sizeof (pw_t));

  if (pws == NULL) return -1;

  int rc = -1;

  for (u64 idx = 0; idx < pws_cnt; idx++)
  {
    const pw_idx_t *pw_idx = &device_param->pws_idx[idx];

    if (pw_idx->len > VK_MD5_STRAIGHT_MAX_LEN)
    {
      event_log_error (hashcat_ctx, "Vulkan mode 0 straight supports candidates of 1..%u bytes; candidate %llu is %u bytes.",
                       VK_MD5_STRAIGHT_MAX_LEN,
                       (unsigned long long) idx,
                       pw_idx->len);

      goto done;
    }

    u32 words = (pw_idx->len + 3u) / 4u;

    if (words > 64u) words = 64u;

    for (u32 word = 0; word < words; word++)
    {
      pws[idx].i[word] = device_param->pws_comp[pw_idx->off + word];
    }

    pws[idx].pw_len = pw_idx->len;
  }

  if (backend_vulkan_device_upload (hashcat_ctx, device_param, HC_VK_BUFFER_PWS, pws, (VkDeviceSize) (pws_cnt * sizeof (pw_t))) != 0)
  {
    event_log_error (hashcat_ctx, "Vulkan mode 0: pws upload failed.");

    goto done;
  }

  if (backend_vulkan_device_memset (hashcat_ctx, device_param, HC_VK_BUFFER_RESULT, 0, sizeof (u32)) != 0)
  {
    event_log_error (hashcat_ctx, "Vulkan mode 0: result pre-fill failed.");

    goto done;
  }

  {
    vk_md5_straight_push_t push;

    memset (&push, 0, sizeof (push));

    push.kernel_param = device_param->kernel_param;
    push.kernel_param.il_cnt  = 1;
    push.kernel_param.pws_pos = pws_pos;
    push.kernel_param.gid_max = pws_cnt;

    const u64 local  = program->local_size[0];
    const u64 groups = (pws_cnt + local - 1) / local;

    if (backend_vulkan_device_dispatch (hashcat_ctx, device_param, program, groups, &push, VK_MD5_STRAIGHT_PUSH_BYTES) != 0)
    {
      event_log_error (hashcat_ctx, "Vulkan mode 0: dispatch failed.");

      goto done;
    }
  }

  rc = 0;

done:
  hcfree (pws);

  return rc;
}

/**
 * Fill one device table entry from a catalogued physical device.
 *
 * The entry is marked is_vulkan and skipped. Vulkan hashing is not enabled in
 * this milestone, so every scheduling loop must ignore the entry while the
 * information output still lists it and -d can name it.
 *
 * The generic device_name remains a separate allocation because the generic
 * destroy loop frees it independently of the Vulkan payload. No Vulkan data
 * is stored in the OpenCL metadata slots any more.
 */

static void backend_vulkan_device_register (hashcat_ctx_t *hashcat_ctx, const u32 device_index, const int backend_device_idx)
{
  backend_ctx_t *backend_ctx = hashcat_ctx->backend_ctx;

  const vulkan_core10_device_catalog_t *catalog = backend_ctx->vulkan_catalog;

  hc_vulkan_device_t *vulkan = backend_vulkan_device_metadata_create (catalog, device_index);

  if (vulkan == NULL)
  {
    event_log_warning (hashcat_ctx, "Vulkan device %u metadata allocation failed; the device is not registered.", device_index);

    return;
  }

  /**
   * The mode 0 push-constant and plain_t layout is defined for 64-bit hosts.
   * A 32-bit build keeps working for every other backend, but must not offer a
   * Vulkan device whose result records would not match the shader layout.
   */

  if (sizeof (plain_t) != VK_PLAIN_T_HOST_SIZE)
  {
    event_log_warning (hashcat_ctx, "Vulkan device %u: host plain_t is %u bytes, the mode 0 program expects %u; the device is not registered.",
      device_index,
      (unsigned int) sizeof (plain_t),
      (unsigned int) VK_PLAIN_T_HOST_SIZE);

    return;
  }

  hc_device_param_t *device_param = &backend_ctx->devices_param[backend_device_idx];

  device_param->device_id = backend_device_idx;


  device_param->device_name = hcstrdup (vulkan->name);

  /**
   * Memory accounting for the generic size/tuning code. The heap sum is the
   * device-local total; available memory starts at half of it because the
   * Android driver does not expose a reliable free-memory query here. The
   * single-allocation ceiling is capped at 512 MiB so the accel shrink loop
   * keeps one Vulkan batch well below the mobile heap limits.
   */

  VkDeviceSize heap_bytes = 0;

  for (u32 heap_idx = 0; heap_idx < vulkan->memory_properties.memoryHeapCount; heap_idx++)
  {
    if (vulkan->memory_properties.memoryHeaps[heap_idx].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
    {
      heap_bytes += vulkan->memory_properties.memoryHeaps[heap_idx].size;
    }
  }

  if (heap_bytes == 0) heap_bytes = 512ULL * 1024ULL * 1024ULL;

  VkDeviceSize max_alloc = vulkan->properties.limits.maxStorageBufferRange;

  if (max_alloc > (512ULL * 1024ULL * 1024ULL)) max_alloc = 512ULL * 1024ULL * 1024ULL;

  device_param->device_processors         = 16;
  device_param->device_maxclock_frequency = 1000;
  device_param->device_global_mem         = (u64) heap_bytes;
  device_param->device_available_mem      = (u64) (heap_bytes / 2);
  device_param->device_maxmem_alloc       = (u64) max_alloc;
  device_param->device_maxworkgroup_size  = vulkan->properties.limits.maxComputeWorkGroupInvocations;

  if (vulkan->device_type == VK_PHYSICAL_DEVICE_TYPE_CPU)
  {
    device_param->opencl_device_type = CL_DEVICE_TYPE_CPU;
  }
  else
  {
    device_param->opencl_device_type = CL_DEVICE_TYPE_GPU;
  }

  device_param->is_vulkan = true;
  device_param->vulkan    = vulkan;

  device_param->skipped = true;

  backend_ctx->backend_device_from_vulkan[device_index] = backend_device_idx;
}

void backend_vulkan_devices_init (hashcat_ctx_t *hashcat_ctx)
{
  backend_ctx_t *backend_ctx = hashcat_ctx->backend_ctx;

  if (backend_ctx->vulkan_devices_active > 0) return;

  const vulkan_core10_device_catalog_t *catalog = backend_ctx->vulkan_catalog;

  if (catalog == NULL) return;
  if (catalog->opened != VK_TRUE) return;
  if (catalog->physical == NULL) return;

  const int backend_devices_base = backend_ctx->backend_devices_cnt;

  for (u32 device_index = 0; device_index < catalog->device_count; device_index++)
  {
    const int backend_device_idx = backend_devices_base + (int) device_index;

    if (backend_device_idx >= DEVICES_MAX)
    {
      event_log_warning (hashcat_ctx, "Only %d of %u Vulkan devices fit into the backend device table.", backend_ctx->vulkan_devices_active, catalog->device_count);

      break;
    }

    backend_vulkan_device_register (hashcat_ctx, device_index, backend_device_idx);

    const hc_device_param_t *device_param = &backend_ctx->devices_param[backend_device_idx];

    if (device_param->vulkan == NULL) continue;

    backend_ctx->vulkan_devices_active++;

    event_log_info (hashcat_ctx, "Vulkan Device #%02d: %s, %s, %s",
                    device_param->device_id + 1,
                    device_param->vulkan->name,
                    device_param->vulkan->vendor,
                    device_param->vulkan->api_version_string);
  }
}

void backend_vulkan_devices_term (hashcat_ctx_t *hashcat_ctx)
{
  backend_ctx_t *backend_ctx = hashcat_ctx->backend_ctx;

  for (int vulkan_device_idx = 0; vulkan_device_idx < backend_ctx->vulkan_devices_active; vulkan_device_idx++)
  {
    const int backend_device_idx = backend_ctx->backend_device_from_vulkan[vulkan_device_idx];

    if (backend_device_idx < 0) continue;
    if (backend_device_idx >= DEVICES_MAX) continue;

    hc_device_param_t *device_param = &backend_ctx->devices_param[backend_device_idx];

    if (device_param->is_vulkan == false) continue;

    if (device_param->vulkan != NULL)
    {
      backend_vulkan_device_close (device_param->vulkan);

      hcfree (device_param->vulkan);
    }

    device_param->vulkan    = NULL;
    device_param->is_vulkan = false;
  }

  backend_ctx->vulkan_devices_active      = 0;
  backend_ctx->vulkan_devices_schedulable = 0;
}

/**
 * Validate one workload against the boundaries of the registered program.
 *
 * The M3 program implements exactly mode 0, attack -a 0, one MD5 block, no
 * rules, no brain, no -O, no --increment, no slow candidates and no
 * benchmark. Everything else is refused here, before any GPU work, with a
 * message that names the unsupported option.
 */

int backend_vulkan_validate_workload (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param)
{
  const hashconfig_t   *hashconfig   = hashcat_ctx->hashconfig;
  const user_options_t *user_options = hashcat_ctx->user_options;

  (void) device_param;

  if (hashconfig->hash_mode != 0)
  {
    event_log_error (hashcat_ctx, "Vulkan backend: hash mode %u is not implemented; only -m 0 (MD5) is available.", hashconfig->hash_mode);

    return -1;
  }

  if (user_options->attack_mode != ATTACK_MODE_STRAIGHT)
  {
    event_log_error (hashcat_ctx, "Vulkan backend: attack mode -a %u is not implemented; only -a 0 (straight wordlist) is available.", user_options->attack_mode);

    return -1;
  }

  if ((user_options->rp_files_cnt > 0) || (user_options->rp_gen > 0))
  {
    event_log_error (hashcat_ctx, "Vulkan backend: rules (-r / --generate-rules) are not implemented.");

    return -1;
  }

  /**
   * The Android OpenCL path may force the Adreno optimized kernel on its own
   * (fork patch P3). That changes hashconfig->opti_type before a Vulkan run
   * even when the user did not pass -O, so the refusal keys on the user
   * option, not on the derived hashconfig bit. The activation hook clears the
   * derived bit for a Vulkan session.
   */

  if (user_options->optimized_kernel == true)
  {
    event_log_error (hashcat_ctx, "Vulkan backend: -O / --optimized-kernel is not implemented.");

    return -1;
  }

  if (user_options->increment != INCREMENT_NONE)
  {
    event_log_error (hashcat_ctx, "Vulkan backend: --increment is not implemented.");

    return -1;
  }

  if (user_options->slow_candidates == true)
  {
    event_log_error (hashcat_ctx, "Vulkan backend: -S / --slow-candidates is not implemented.");

    return -1;
  }

  if (strcmp (user_options->encoding_from, user_options->encoding_to) != 0)
  {
    event_log_error (hashcat_ctx, "Vulkan backend: --encoding-from / --encoding-to are not implemented.");

    return -1;
  }

  if (user_options->benchmark == true)
  {
    event_log_error (hashcat_ctx, "Vulkan backend: benchmark mode (-b) is not implemented.");

    return -1;
  }

  if ((hashcat_ctx->brain_ctx != NULL) && (hashcat_ctx->brain_ctx->enabled == true))
  {
    event_log_error (hashcat_ctx, "Vulkan backend: brain is not implemented.");

    return -1;
  }

  if (hashconfig->attack_exec != ATTACK_EXEC_INSIDE_KERNEL)
  {
    event_log_error (hashcat_ctx, "Vulkan backend: this hash mode does not use an inside-kernel attack.");

    return -1;
  }

  return 0;
}

/**
 * Open the runtime with the real hashconfig sizes and upload immutable data.
 */

int backend_vulkan_devices_session_begin (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param)
{
  const hashes_t *hashes = hashcat_ctx->hashes;

  if (backend_vulkan_devices_memory_init (hashcat_ctx, device_param) != 0) return -1;

  if ((device_param->size_digests > 0) && (hashes->digests_buf != NULL))
  {
    if (backend_vulkan_device_upload (hashcat_ctx, device_param, HC_VK_BUFFER_DIGESTS, hashes->digests_buf, (VkDeviceSize) device_param->size_digests) != 0) return -1;
  }

  if ((device_param->size_salts > 0) && (hashes->salts_buf != NULL))
  {
    if (backend_vulkan_device_upload (hashcat_ctx, device_param, HC_VK_BUFFER_SALTS, hashes->salts_buf, (VkDeviceSize) device_param->size_salts) != 0) return -1;
  }

  if ((device_param->size_esalts > 0) && (hashes->esalts_buf != NULL))
  {
    if (backend_vulkan_device_upload (hashcat_ctx, device_param, HC_VK_BUFFER_ESALTS, hashes->esalts_buf, (VkDeviceSize) device_param->size_esalts) != 0) return -1;
  }

  return 0;
}

/**
 * Self-check one device and decide whether it may receive work.
 */

void backend_vulkan_devices_activate (hashcat_ctx_t *hashcat_ctx, const int other_active, const int need_info)
{
  backend_ctx_t        *backend_ctx  = hashcat_ctx->backend_ctx;
  const user_options_t *user_options = hashcat_ctx->user_options;

  const bool explicit_selection = (user_options->backend_devices != NULL);

  for (u32 vulkan_device_idx = 0; vulkan_device_idx < backend_ctx->vulkan_devices_active; vulkan_device_idx++)
  {
    const int backend_device_idx = backend_ctx->backend_device_from_vulkan[vulkan_device_idx];

    if (backend_device_idx < 0) continue;
    if (backend_device_idx >= DEVICES_MAX) continue;

    hc_device_param_t *device_param = &backend_ctx->devices_param[backend_device_idx];

    if ((device_param->is_vulkan == false) || (device_param->vulkan == NULL)) continue;

    const bool selected  = explicit_selection && (backend_ctx->backend_devices_filter[device_param->device_id] == 0);
    const bool only_device = (other_active == 0);
    const bool should_run  = selected || only_device;

    /**
     * A Vulkan device that will not run work only needs opening when the
     * caller asks for backend information, because schedulable must be true
     * in --backend-info-json even when OpenCL is the active backend.
     */

    if ((should_run == false) && (need_info == 0)) continue;

    int check_rc = -1;

    if (backend_vulkan_devices_memory_init (hashcat_ctx, device_param) == 0)
    {
      check_rc = backend_vulkan_device_md5_straight_check (hashcat_ctx, device_param);

      backend_vulkan_devices_memory_term (hashcat_ctx, device_param);
    }

    if (check_rc != 0)
    {
      device_param->vulkan->schedulable = false;

      event_log_error (hashcat_ctx, "Vulkan device #%u: self-check failed; the device is not schedulable.", device_param->device_id + 1);

      continue;
    }

    device_param->vulkan->schedulable = true;

    backend_ctx->vulkan_devices_schedulable++;

    if (should_run == true)
    {
      device_param->skipped = false;

      backend_ctx->backend_devices_active++;

      /**
       * The password-length limit of the Vulkan program is published by
       * hashconfig_init() after the module limits: device activation runs
       * before that call, so hashconfig->pw_max is still zero here and any
       * assignment made at this point would be overwritten.
       */
    }

    event_log_info (hashcat_ctx, "Vulkan device #%u: self-check passed (mode 0 straight); schedulable=1.", device_param->device_id + 1);
  }
}

u32 backend_vulkan_device_selected (hashcat_ctx_t *hashcat_ctx)
{
  const backend_ctx_t  *backend_ctx  = hashcat_ctx->backend_ctx;
  const user_options_t *user_options = hashcat_ctx->user_options;

  if (user_options->backend_devices == NULL) return 0;

  for (int vulkan_device_idx = 0; vulkan_device_idx < backend_ctx->vulkan_devices_active; vulkan_device_idx++)
  {
    const int backend_device_idx = backend_ctx->backend_device_from_vulkan[vulkan_device_idx];

    if (backend_device_idx < 0) continue;
    if (backend_device_idx >= DEVICES_MAX) continue;

    const hc_device_param_t *device_param = &backend_ctx->devices_param[backend_device_idx];

    if (backend_ctx->backend_devices_filter[device_param->device_id] == 0)
    {
      return (u32) (device_param->device_id + 1);
    }
  }

  return 0;
}

#endif // WITH_VULKAN_DISCOVERY
