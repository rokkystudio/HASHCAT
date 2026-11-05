/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#if defined (WITH_VULKAN_DISCOVERY)

#include "common.h"
#include "types.h"
#include "memory.h"
#include "event.h"

#include "backend_vulkan.h"
#include "backend_vulkan_device.h"

#include <stdio.h>
#include <string.h>

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

/**
 * Release one Vulkan payload. In M2 this also owns the logical device, its
 * buffers and pipelines; the teardown is written so that a partially
 * initialized payload is always safe to free.
 */

static void backend_vulkan_device_destroy (hc_vulkan_device_t *vulkan)
{
  if (vulkan == NULL) return;

  if (vulkan->device != VK_NULL_HANDLE)
  {
    vkDeviceWaitIdle (vulkan->device);

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

      if (buffer->host_mapped != NULL && buffer->host_memory != VK_NULL_HANDLE)
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
    }

    if (vulkan->descriptor_pool != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorPool (vulkan->device, vulkan->descriptor_pool, NULL);
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
  }

  hcfree (vulkan);
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

    backend_vulkan_device_destroy (device_param->vulkan);

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
