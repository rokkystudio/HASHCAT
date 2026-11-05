/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#ifndef HC_BACKEND_VULKAN_BINDINGS_H
#define HC_BACKEND_VULKAN_BINDINGS_H

#include "types.h"
#include "backend_vulkan_device.h"

#include <stddef.h>
#include <stdint.h>

/**
 * Descriptor and push-constant ABI version 1 for hashcat Vulkan programs.
 *
 * The layout is a fixed contract between the host and every SPIR-V module:
 *
 *   set 0, b0   kernel_param via push constants when its size fits 128 bytes
 *   set 0, b1   pws_buf
 *   set 0, b2   digests_buf
 *   set 0, b3   salt_bufs
 *   set 0, b4   esalt_bufs
 *   set 0, b5   tmps
 *   set 0, b6   hooks
 *   set 0, b7   result
 *   set 0, b8   bitmap_s1_a
 *   set 0, b9   bitmap_s1_b
 *   set 0, b10  bitmap_s1_c
 *   set 0, b11  bitmap_s1_d
 *   set 0, b12  bitmap_s2_a
 *   set 0, b13  bitmap_s2_b
 *   set 0, b14  plains_buf          (added in version 2)
 *
 * b0 is not a descriptor: the kernel_param block travels in a Compute push
 * constant range. A program whose push-constant block does not fit is
 * rejected instead of silently falling back to a different layout.
 *
 * Version 2 adds plains_buf so a mode program can emit plain_t records with
 * the same mark_hash() semantics as OpenCL instead of overloading result.
 * The entries b0..b13 keep their version-1 meaning.
 *
 * Dispatch splitting convention: the last four bytes of the push-constant
 * block hold the base workgroup of the current vkCmdDispatch chunk. The host
 * overwrites that word before every chunk, because a split dispatch restarts
 * gl_GlobalInvocationID at zero. Programs that can exceed
 * maxComputeWorkGroupCount[0] therefore must declare at least four bytes of
 * push constants and add the base to their invocation index.
 *
 * Changing any entry, count or kind requires a new VK_BINDINGS_VERSION.
 */

#define VK_BINDINGS_VERSION 2u

#define VK_BINDING_COUNT 15u

#define VK_BINDINGS_PUSH_CONSTANT_MAX 128u

/**
 * Program flags describe the mode boundaries a program was built for. The
 * scheduler checks them before it accepts an attack; a program that does not
 * name a supported boundary is never scheduled.
 */

#define VK_PROGRAM_FLAG_MODE0_MD5   (1u << 0)
#define VK_PROGRAM_FLAG_STRAIGHT    (1u << 1)
#define VK_PROGRAM_FLAG_NO_RULES    (1u << 2)
#define VK_PROGRAM_FLAG_NO_BRAIN    (1u << 3)
#define VK_PROGRAM_FLAG_ONE_BLOCK   (1u << 4)

#define VK_MD5_STRAIGHT_MAX_LEN 55u

/**
 * Push-constant block of the mode 0 straight program.
 *
 * The first member is hashcat's kernel_param_t, byte-for-byte. The trailing
 * word is the base workgroup of the current split vkCmdDispatch chunk; it is
 * the required last-four-bytes convention documented above. The GLSL module
 * declares the same bytes as uint pairs, so it does not depend on the
 * optional shaderInt64 device feature.
 */

typedef struct vk_md5_straight_push
{
  kernel_param_t kernel_param;
  u32            base_workgroup;
} vk_md5_straight_push_t;

#define VK_MD5_STRAIGHT_PUSH_BYTES ((u32) (offsetof (vk_md5_straight_push_t, base_workgroup) + sizeof (u32)))

_Static_assert (sizeof (kernel_param_t) == 72u,
                "kernel_param_t must keep the 72-byte v1 push-constant layout");

_Static_assert (sizeof (pw_t) == 260u,
                "the mode 0 program assumes the 65-word pw_t stride");

/**
 * The mode 0 program writes 8-byte aligned 40-byte plain_t records. The host
 * structure contains a pointer, so its size depends on the target: 40 bytes on
 * 64-bit ABIs and 36 bytes on 32-bit ones. A build-time equality assert would
 * reject every 32-bit target, so the size is validated at device registration
 * instead and the Vulkan device is simply not offered when it does not match.
 */

#define VK_PLAIN_T_HOST_SIZE 40u

_Static_assert (offsetof (vk_md5_straight_push_t, base_workgroup) == 72u,
                "base_workgroup must follow kernel_param_t immediately");

typedef enum vk_binding_kind
{
  VK_BINDING_KIND_PUSH_CONSTANT  = 0,
  VK_BINDING_KIND_STORAGE_BUFFER = 1
} vk_binding_kind_t;

typedef struct vk_binding
{
  uint32_t set;
  uint32_t binding;
  uint32_t kind;
  uint32_t buffer_index;
  uint32_t flags;
} vk_binding_t;

/**
 * One immutable SPIR-V program in the host registry.
 *
 * spirv_len is the module length in bytes. local_size is the declared
 * workgroup topology and must match the SPIR-V local size. bindings points to
 * the complete v1 table and binding_count must equal VK_BINDING_COUNT.
 * push_constant_bytes is the exact kernel_param block size used by the
 * program. flags is reserved for M3 stage semantics.
 */

typedef struct vk_program
{
  const char           *name;
  const uint32_t       *spirv;
  size_t                spirv_len;
  uint32_t              local_size[3];
  const vk_binding_t   *bindings;
  uint32_t              binding_count;
  uint32_t              push_constant_bytes;
  uint32_t              flags;
  uint32_t              bindings_version;
} vk_program_t;

static const vk_binding_t vk_hashcat_bindings[VK_BINDING_COUNT] =
{
  { 0u,  0u, VK_BINDING_KIND_PUSH_CONSTANT,  HC_VK_BUFFER_KERNEL_PARAM,  0u },
  { 0u,  1u, VK_BINDING_KIND_STORAGE_BUFFER, HC_VK_BUFFER_PWS,           0u },
  { 0u,  2u, VK_BINDING_KIND_STORAGE_BUFFER, HC_VK_BUFFER_DIGESTS,       0u },
  { 0u,  3u, VK_BINDING_KIND_STORAGE_BUFFER, HC_VK_BUFFER_SALTS,         0u },
  { 0u,  4u, VK_BINDING_KIND_STORAGE_BUFFER, HC_VK_BUFFER_ESALTS,        0u },
  { 0u,  5u, VK_BINDING_KIND_STORAGE_BUFFER, HC_VK_BUFFER_TMPS,          0u },
  { 0u,  6u, VK_BINDING_KIND_STORAGE_BUFFER, HC_VK_BUFFER_HOOKS,         0u },
  { 0u,  7u, VK_BINDING_KIND_STORAGE_BUFFER, HC_VK_BUFFER_RESULT,        0u },
  { 0u,  8u, VK_BINDING_KIND_STORAGE_BUFFER, HC_VK_BUFFER_BITMAP_S1_A,   0u },
  { 0u,  9u, VK_BINDING_KIND_STORAGE_BUFFER, HC_VK_BUFFER_BITMAP_S1_B,   0u },
  { 0u, 10u, VK_BINDING_KIND_STORAGE_BUFFER, HC_VK_BUFFER_BITMAP_S1_C,   0u },
  { 0u, 11u, VK_BINDING_KIND_STORAGE_BUFFER, HC_VK_BUFFER_BITMAP_S1_D,   0u },
  { 0u, 12u, VK_BINDING_KIND_STORAGE_BUFFER, HC_VK_BUFFER_BITMAP_S2_A,   0u },
  { 0u, 13u, VK_BINDING_KIND_STORAGE_BUFFER, HC_VK_BUFFER_BITMAP_S2_B,   0u },
  { 0u, 14u, VK_BINDING_KIND_STORAGE_BUFFER, HC_VK_BUFFER_PLAIN,         0u }
};

_Static_assert (sizeof (vk_hashcat_bindings) / sizeof (vk_hashcat_bindings[0]) == VK_BINDING_COUNT,
                "binding table size must match VK_BINDING_COUNT");

_Static_assert (VK_BINDINGS_VERSION == 2u,
                "descriptor layout changes require a new VK_BINDINGS_VERSION");

_Static_assert (sizeof (kernel_param_t) <= VK_BINDINGS_PUSH_CONSTANT_MAX,
                "kernel_param_t must fit into the 128-byte push-constant range");

#endif // HC_BACKEND_VULKAN_BINDINGS_H
