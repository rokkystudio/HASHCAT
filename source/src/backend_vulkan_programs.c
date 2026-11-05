/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

/**
 * Immutable host-side program registry for the Vulkan compute backend.
 *
 * A program is a compiled SPIR-V module plus the exact topology, descriptor
 * table and push-constant size it was built for. The registry is the single
 * place that decides which hashcat workloads the backend is allowed to run:
 * M3 registers the M2 data-path check and the mode 0 / -a 0 straight kernel.
 */

#if defined (WITH_VULKAN_DISCOVERY)

#include "common.h"
#include "types.h"

#include "backend_vulkan_device.h"
#include "backend_vulkan_bindings.h"
#include "backend_vulkan_check_spirv.h"
#include "backend_vulkan_md5_straight_spirv.h"

#include <string.h>

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
  },
  {
    .name                = "m0-straight",
    .spirv               = backend_vulkan_md5_straight_spirv,
    .spirv_len           = BACKEND_VULKAN_MD5_STRAIGHT_SPIRV_BYTES,
    .local_size          = { 64u, 1u, 1u },
    .bindings            = vk_hashcat_bindings,
    .binding_count       = VK_BINDING_COUNT,
    .push_constant_bytes = VK_MD5_STRAIGHT_PUSH_BYTES,
    .flags               = VK_PROGRAM_FLAG_MODE0_MD5
                         | VK_PROGRAM_FLAG_STRAIGHT
                         | VK_PROGRAM_FLAG_NO_RULES
                         | VK_PROGRAM_FLAG_NO_BRAIN
                         | VK_PROGRAM_FLAG_ONE_BLOCK,
    .bindings_version    = VK_BINDINGS_VERSION
  }
};

#define BACKEND_VULKAN_PROGRAM_COUNT (sizeof (backend_vulkan_programs) / sizeof (backend_vulkan_programs[0]))

u32 backend_vulkan_program_count (void)
{
  return (u32) BACKEND_VULKAN_PROGRAM_COUNT;
}

const struct vk_program *backend_vulkan_program_at (const u32 index)
{
  if (index >= BACKEND_VULKAN_PROGRAM_COUNT) return NULL;

  return &backend_vulkan_programs[index];
}

const struct vk_program *backend_vulkan_program_by_name (const char *name)
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

const struct vk_program *backend_vulkan_program_md5_straight (void)
{
  return backend_vulkan_program_by_name ("m0-straight");
}

#endif // WITH_VULKAN_DISCOVERY
