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
 * Host-side program registry.
 *
 * M2 registers exactly one immutable program: the data-path check shader.
 * Per-mode programs arrive in M3 and must keep the v1 binding table or bump
 * VK_BINDINGS_VERSION.
 */

static const vk_program_t backend_vulkan_programs[] =
{
  {
    .name                = "m2-check-sum",
    .spirv               = backend_vulkan_check_spirv,
    .spirv_len           = BACKEND_VULKAN_CHECK_SPIRV_BYTES,
    .local_size          = { 1u, 1u, 1u },
    .bindings            = vk_hashcat_bindings,
    .binding_count       = VK_BINDING_COUNT,
    .push_constant_bytes = 16u,
    .flags               = 0u,
    .bindings_version    = VK_BINDINGS_VERSION
  }
};

#define BACKEND_VULKAN_PROGRAM_COUNT (sizeof (backend_vulkan_programs) / sizeof (backend_vulkan_programs[0]))

const vk_program_t *backend_vulkan_program_by_name (const char *name)
{
  if (name == NULL) return NULL;

  for (u32 program_idx = 0; program_idx < BACKEND_VULKAN_PROGRAM_COUNT; program_idx++)
  {
    if (strcmp (backend_vulkan_programs[program_idx].name, name) == 0)
    {
      return &backend_vulkan_programs[program_idx];
    }
  }

  return NULL;
}

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

  sizes[HC_VK_BUFFER_PLAIN]         = sizeof (plain_t);
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

    if (buffer->host_buffer != VK_NULL_HANDLE)
    {
      vkDestroyBuffer (vulkan->device, buffer->host_buffer, NULL);
    }

    if (buffer->host_memory != VK_NULL_HANDLE)
    {
      vkFreeMemory (vulkan->device, buffer->host_memory, NULL);
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

  for (u32 program_idx = 0; program_idx < BACKEND_VULKAN_PROGRAM_COUNT; program_idx++)
  {
    if (backend_vulkan_device_program_prepare (hashcat_ctx, device_param, &backend_vulkan_programs[program_idx]) != 0)
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

  hc_device_param_t *device_param = &backend_ctx->devices_param[backend_device_idx];

  device_param->device_id = backend_device_idx;

  device_param->device_name = hcstrdup (vulkan->name);

  device_param->device_processors         = 0;
  device_param->device_maxclock_frequency = 0;
  device_param->device_global_mem         = 0;
  device_param->device_available_mem      = 0;

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

  backend_ctx->vulkan_devices_active = 0;
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
