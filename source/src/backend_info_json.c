/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#include "common.h"
#include "types.h"

#include "backend_info_json.h"

#if defined (WITH_VULKAN_DISCOVERY)
#include "backend_vulkan_device.h"
#endif

/**
 * Print one JSON string literal, including the surrounding quotes.
 *
 * Device names are free-form text, so control characters and the two JSON
 * metacharacters must be escaped. The output stays ASCII-safe.
 */
static void json_print_string (const char *value)
{
  fputc ('"', stdout);

  if (value == NULL)
  {
    value = "";
  }

  for (const unsigned char *ptr = (const unsigned char *) value; *ptr != 0; ptr++)
  {
    const unsigned char character = *ptr;

    switch (character)
    {
      case '"':  fputs ("\\\"", stdout); break;
      case '\\': fputs ("\\\\", stdout); break;
      case '\b': fputs ("\\b",  stdout); break;
      case '\f': fputs ("\\f",  stdout); break;
      case '\n': fputs ("\\n",  stdout); break;
      case '\r': fputs ("\\r",  stdout); break;
      case '\t': fputs ("\\t",  stdout); break;

      default:
        if (character < 0x20)
        {
          printf ("\\u%04x", character);
        }
        else
        {
          fputc (character, stdout);
        }
    }
  }

  fputc ('"', stdout);
}

/**
 * Map a registered compute device to one of the four contract type names.
 *
 * OpenCL exposes an explicit device type; CUDA, HIP and Metal only register
 * GPU devices. Vulkan devices store their physical-device class in the reused
 * OpenCL type slot. hashcat prints "Accelerator" for the remaining OpenCL
 * classes, so the contract value for those is "ACCEL". No backend emits FPGA
 * today.
 */
static const char *json_device_type (const hc_device_param_t *device_param)
{
  if (device_param->is_vulkan == true)
  {
    if ((device_param->vulkan != NULL) && (device_param->vulkan->device_type == VK_PHYSICAL_DEVICE_TYPE_CPU)) return "CPU";

    return "GPU";
  }

  if (device_param->is_opencl == true)
  {
    if (device_param->opencl_device_type & CL_DEVICE_TYPE_CPU)         return "CPU";
    if (device_param->opencl_device_type & CL_DEVICE_TYPE_GPU)         return "GPU";
    if (device_param->opencl_device_type & CL_DEVICE_TYPE_ACCELERATOR) return "ACCEL";

    return "ACCEL";
  }

  return "GPU";
}

/**
 * Return the vendor id hashcat reported for the device.
 *
 * OpenCL and Vulkan expose a numeric vendor id (Vulkan stores it in the reused
 * OpenCL slot). It is unsigned because generic ids such as Qualcomm's
 * 0x80000000 do not fit into a signed value.
 */
static u32 json_device_vendor_id (const hc_device_param_t *device_param)
{
  if (device_param->is_vulkan == true)
  {
    return (device_param->vulkan != NULL) ? device_param->vulkan->vendor_id : 0;
  }

  if (device_param->is_opencl == true)
  {
    return (u32) device_param->opencl_device_vendor_id;
  }

  return 0;
}

void backend_info_json (hashcat_ctx_t *hashcat_ctx, const char *version_tag)
{
  const backend_ctx_t *backend_ctx = hashcat_ctx->backend_ctx;

  /**
   * The backends array always contains the same five entries in this fixed
   * order, whether the interface is compiled in, opened or owns devices.
   */

  const char *const backend_names[5] = { "cuda", "hip", "metal", "opencl", "vulkan" };

  bool interface_open[5];

  interface_open[0] = (backend_ctx->cuda != NULL);
  interface_open[1] = (backend_ctx->hip  != NULL);
  interface_open[2] = (backend_ctx->mtl  != NULL);
  interface_open[3] = (backend_ctx->ocl  != NULL);
  interface_open[4] = false;

  int devices_cnt[5];

  devices_cnt[0] = backend_ctx->cuda_devices_cnt;
  devices_cnt[1] = backend_ctx->hip_devices_cnt;
  devices_cnt[2] = backend_ctx->metal_devices_cnt;
  devices_cnt[3] = backend_ctx->opencl_devices_cnt;
  devices_cnt[4] = 0;

  #if defined (WITH_VULKAN_DISCOVERY)
  interface_open[4] = (backend_ctx->vulkan_catalog != NULL);

  devices_cnt[4] = backend_ctx->vulkan_devices_active;
  #endif

  const int *device_map[5];

  device_map[0] = backend_ctx->backend_device_from_cuda;
  device_map[1] = backend_ctx->backend_device_from_hip;
  device_map[2] = backend_ctx->backend_device_from_metal;
  device_map[3] = backend_ctx->backend_device_from_opencl;
  device_map[4] = backend_ctx->backend_device_from_vulkan;

  printf ("{\n");
  printf ("  \"schema\": 1,\n");
  printf ("  \"hashcat_version\": ");
  json_print_string (version_tag);
  printf (",\n");
  printf ("  \"backends\": [\n");

  for (int backend_idx = 0; backend_idx < 5; backend_idx++)
  {
    bool detected = (interface_open[backend_idx] == true) && (devices_cnt[backend_idx] > 0);

    /**
     * Vulkan detection comes from the opened catalogue. The registered device
     * array can be smaller than the physical-device count only when the device
     * table is exhausted, and detection must still be reported in that case.
     */

    if (backend_idx == 4)
    {
      detected = (interface_open[4] == true) && (backend_ctx->vulkan_devices_cnt > 0);
    }

    /**
     * Vulkan devices are discovered and verified but never scheduled in this
     * version, so schedulable stays false even when detected is true.
     */

    const bool schedulable = (backend_idx < 4) ? detected : false;

    printf ("    {\n");
    printf ("      \"id\": ");
    json_print_string (backend_names[backend_idx]);
    printf (",\n");
    printf ("      \"detected\": %s,\n", (detected    == true) ? "true" : "false");
    printf ("      \"schedulable\": %s,\n", (schedulable == true) ? "true" : "false");

    if (devices_cnt[backend_idx] == 0)
    {
      printf ("      \"devices\": []\n");
    }
    else
    {
      printf ("      \"devices\": [\n");

      for (int device_idx = 0; device_idx < devices_cnt[backend_idx]; device_idx++)
      {
        const int backend_device_idx = device_map[backend_idx][device_idx];

        const hc_device_param_t *device_param = backend_ctx->devices_param + backend_device_idx;

        printf ("        { \"id\": %u, \"name\": ", (u32) (device_param->device_id + 1));
        json_print_string (device_param->device_name);
        printf (", \"type\": ");
        json_print_string (json_device_type (device_param));
        printf (", \"vendor_id\": %u, \"backend\": ", json_device_vendor_id (device_param));
        json_print_string (backend_names[backend_idx]);
        printf (" }%s\n", ((device_idx + 1) < devices_cnt[backend_idx]) ? "," : "");
      }

      printf ("      ]\n");
    }

    printf ("    }%s\n", ((backend_idx + 1) < 5) ? "," : "");
  }

  printf ("  ],\n");
  printf ("  \"modes\": {}\n");
  printf ("}\n");

  fflush (stdout);
}
