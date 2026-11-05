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
 * Fill one device table entry from a catalogued physical device.
 *
 * The entry is marked is_vulkan and skipped. Vulkan hashing is not enabled in
 * this milestone, so every scheduling loop must ignore the entry while the
 * information output still lists it and -d can name it.
 *
 * The generic device table has no backend-neutral type, vendor id or version
 * fields. The OpenCL metadata slots are reused here: they are only read when
 * is_opencl is true, and the Vulkan output paths read them through is_vulkan.
 * M2 moves this data into hc_vulkan_device.
 */

static void backend_vulkan_device_register (hashcat_ctx_t *hashcat_ctx, const u32 device_index, const int backend_device_idx)
{
  backend_ctx_t *backend_ctx = hashcat_ctx->backend_ctx;

  const vulkan_core10_device_catalog_t *catalog = backend_ctx->vulkan_catalog;

  VkPhysicalDeviceProperties properties = { 0 };

  vkGetPhysicalDeviceProperties (catalog->physical[device_index], &properties);

  hc_device_param_t *device_param = &backend_ctx->devices_param[backend_device_idx];

  device_param->device_id = backend_device_idx;

  device_param->device_name = hcstrdup (properties.deviceName);

  device_param->opencl_device_type      = (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) ? CL_DEVICE_TYPE_CPU : CL_DEVICE_TYPE_GPU;
  device_param->opencl_device_vendor_id = properties.vendorID;
  device_param->opencl_device_vendor    = hcstrdup (backend_vulkan_vendor_name (properties.vendorID));

  char version[64];

  snprintf (version, sizeof (version), "Vulkan %u.%u",
            VK_API_VERSION_MAJOR (properties.apiVersion),
            VK_API_VERSION_MINOR (properties.apiVersion));

  device_param->opencl_device_version = hcstrdup (version);

  device_param->device_processors         = 0;
  device_param->device_maxclock_frequency = 0;
  device_param->device_global_mem         = 0;
  device_param->device_available_mem      = 0;

  device_param->is_vulkan = true;
  device_param->vulkan    = NULL;

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

    backend_ctx->vulkan_devices_active++;

    const hc_device_param_t *device_param = &backend_ctx->devices_param[backend_device_idx];

    event_log_info (hashcat_ctx, "Vulkan Device #%02d: %s, %s, %s",
                    device_param->device_id + 1,
                    device_param->device_name,
                    device_param->opencl_device_vendor,
                    device_param->opencl_device_version);
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

    hcfree (device_param->opencl_device_vendor);
    hcfree (device_param->opencl_device_version);

    device_param->opencl_device_vendor  = NULL;
    device_param->opencl_device_version = NULL;

    device_param->vulkan = NULL;
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
