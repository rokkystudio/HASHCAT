/**
 * Inspect actual Vulkan 1.0 memory heaps and allocate four synthetic buffers.
 *
 * The standalone Android ARM64 diagnostic creates two storage/transfer GPU
 * buffers and two host-visible staging transfer buffers. The actual Vulkan
 * memory requirements are planned jointly against driver-declared heaps and
 * maxMemoryAllocationCount. Each resulting VkDeviceMemory is then allocated
 * and bound to its buffer; host-visible staging memory is mapped and tested.
 *
 * No pipeline, shader, command buffer or GPU workload is submitted. No
 * hashcat runtime, cryptographic input or password material is accessed.
 */

#include "vulkan-auto-config.h"
#include "vulkan-memory-heap-policy.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_COUNT 4u
#define TEST_BUFFER_BYTES (2048u * sizeof (uint32_t))

typedef struct vulkan_heap_probe_buffer
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  void *mapped;
} vulkan_heap_probe_buffer_t;

#define VK_TRY(call) \
  do \
  { \
    result = (call); \
    if (result != VK_SUCCESS) \
    { \
      fprintf (stderr, "VULKAN_HEAP_CALL_FAILED %s result=%d\n", \
        #call, result); \
      goto cleanup; \
    } \
  } while (0)

/**
 * Validate one real physical GPU's Vulkan memory properties and four buffers.
 *
 * All four memory allocations are smaller than a public 8-KiB buffer plus
 * driver-required padding. The plan uses actual memoryTypeBits, heap indices,
 * heap capacities, and maxMemoryAllocationCount; no index is hard-coded.
 * After successful allocation the two host staging buffers are mapped,
 * filled with a deterministic public pattern, flushed if noncoherent,
 * and unmapped without submitting any GPU work.
 */
static int inspect_device (const vulkan_auto_device_t *selected)
{
  int status = 1;
  VkResult result = VK_SUCCESS;
  VkDevice device = VK_NULL_HANDLE;
  vulkan_heap_probe_buffer_t buffers[BUFFER_COUNT] = { 0 };
  vulkan_heap_allocation_request_t requests[BUFFER_COUNT] = { 0 };
  vulkan_heap_allocation_plan_t plan = { 0 };
  uint32_t allocated_count = 0u;
  uint32_t mapped_count = 0u;

  static const char *names[BUFFER_COUNT] =
  {
    "gpu-input",
    "gpu-output",
    "staging-upload",
    "staging-download"
  };
  static const VkBufferUsageFlags usages[BUFFER_COUNT] =
  {
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    VK_BUFFER_USAGE_TRANSFER_DST_BIT
  };

  const VkPhysicalDeviceMemoryProperties *memory =
    &selected->memory_properties;

  fprintf (
    stdout,
    "DEVICE_HEAP_PROFILE memory_types=%" PRIu32
    " memory_heaps=%" PRIu32
    " max_allocations=%" PRIu32 "\n",
    memory->memoryTypeCount, memory->memoryHeapCount,
    selected->properties.limits.maxMemoryAllocationCount
  );

  for (uint32_t index = 0u; index < memory->memoryHeapCount; index++)
  {
    fprintf (
      stdout,
      "DEVICE_HEAP index=%" PRIu32
      " size=%" PRIu64 " flags=0x%08" PRIx32 "\n",
      index,
      (uint64_t) memory->memoryHeaps[index].size,
      memory->memoryHeaps[index].flags
    );
  }

  for (uint32_t index = 0u; index < memory->memoryTypeCount; index++)
  {
    fprintf (
      stdout,
      "DEVICE_MEMORY_TYPE index=%" PRIu32
      " heap=%" PRIu32 " flags=0x%08" PRIx32 "\n",
      index, memory->memoryTypes[index].heapIndex,
      memory->memoryTypes[index].propertyFlags
    );
  }

  const float priority = 1.0f;

  const VkDeviceQueueCreateInfo queue_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = selected->compute_queue_family,
    .queueCount = 1u,
    .pQueuePriorities = &priority
  };
  const VkDeviceCreateInfo device_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount = 1u,
    .pQueueCreateInfos = &queue_info
  };

  VK_TRY (vkCreateDevice (
    selected->physical, &device_info, NULL, &device
  ));

  for (uint32_t index = 0u; index < BUFFER_COUNT; index++)
  {
    const VkBufferCreateInfo buffer_info =
    {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = TEST_BUFFER_BYTES,
      .usage = usages[index],
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    VK_TRY (vkCreateBuffer (
      device, &buffer_info, NULL, &buffers[index].buffer
    ));

    requests[index].label = names[index];
    requests[index].role =
      index < 2u ? VULKAN_HEAP_BUFFER_GPU_STORAGE
                 : VULKAN_HEAP_BUFFER_HOST_TRANSFER;

    /*
     * This Vulkan 1.0-only device enables neither protectedMemory nor
     * VK_AMD_device_coherent_memory. Their types must not be allocated.
     */
    requests[index].forbidden_flags =
      VK_MEMORY_PROPERTY_PROTECTED_BIT
      | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;

    vkGetBufferMemoryRequirements (
      device, buffers[index].buffer, &requests[index].requirements
    );

    fprintf (
      stdout,
      "ACTUAL_REQUIREMENTS index=%" PRIu32
      " label=%s size=%" PRIu64 " alignment=%" PRIu64
      " memory_bits=0x%08" PRIx32 "\n",
      index, names[index],
      (uint64_t) requests[index].requirements.size,
      (uint64_t) requests[index].requirements.alignment,
      requests[index].requirements.memoryTypeBits
    );
  }

  result = vulkan_heap_plan_allocations (
    memory, selected->properties.limits.maxMemoryAllocationCount,
    0u, requests, BUFFER_COUNT, &plan, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "VULKAN_HEAP_PLAN_FAILED index=%" PRIu32 " result=%d\n",
      selected->index, result
    );

    goto cleanup;
  }

  for (uint32_t index = 0u; index < BUFFER_COUNT; index++)
  {
    const VkMemoryAllocateInfo allocation_info =
    {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requests[index].requirements.size,
      .memoryTypeIndex = plan.memory_type_indices[index]
    };

    VK_TRY (vkAllocateMemory (
      device, &allocation_info, NULL, &buffers[index].memory
    ));

    allocated_count++;

    VK_TRY (vkBindBufferMemory (
      device, buffers[index].buffer, buffers[index].memory, 0u
    ));

    fprintf (
      stdout,
      "ALLOCATED_BUFFER index=%" PRIu32
      " type=%" PRIu32 " heap=%" PRIu32
      " bytes=%" PRIu64 "\n",
      index, plan.memory_type_indices[index],
      plan.heap_indices[index],
      (uint64_t) requests[index].requirements.size
    );
  }

  for (uint32_t index = 2u; index < BUFFER_COUNT; index++)
  {
    VK_TRY (vkMapMemory (
      device, buffers[index].memory, 0u, VK_WHOLE_SIZE, 0u,
      &buffers[index].mapped
    ));

    memset (buffers[index].mapped,
      (int) (0x43u + index), TEST_BUFFER_BYTES);

    const vulkan_auto_memory_plan_t map_plan =
    {
      .allocation_bytes = requests[index].requirements.size,
      .memory_type_index = plan.memory_type_indices[index],
      .coherent = plan.coherent[index],
      .non_coherent_atom_size =
        selected->properties.limits.nonCoherentAtomSize
    };

    VK_TRY (vulkan_auto_sync_mapped_memory (
      device, buffers[index].memory, &map_plan, VK_FALSE
    ));

    fprintf (
      stdout,
      "MAPPED_STAGING index=%" PRIu32
      " coherent=%u bytes=%" PRIu64 "\n",
      index, plan.coherent[index], (uint64_t) TEST_BUFFER_BYTES
    );

    vkUnmapMemory (device, buffers[index].memory);
    buffers[index].mapped = NULL;
    mapped_count++;
  }

  fprintf (
    stdout,
    "PASS_VULKAN_HEAP_PROBE device_index=%" PRIu32
    " buffers=%u allocations=%" PRIu32
    " mapped_staging=%" PRIu32 " gpu_dispatches=0\n",
    selected->index, BUFFER_COUNT, allocated_count, mapped_count
  );

  status = 0;

cleanup:
  if (device != VK_NULL_HANDLE)
  {
    const VkResult idle = vkDeviceWaitIdle (device);

    if (idle != VK_SUCCESS)
    {
      fprintf (
        stderr, "VULKAN_HEAP_WAIT_IDLE_FAILED result=%d\n", idle
      );

      status = 1;
    }

    for (uint32_t index = BUFFER_COUNT; index != 0u; index--)
    {
      vulkan_heap_probe_buffer_t *buffer = &buffers[index - 1u];

      if (buffer->mapped != NULL)
      {
        vkUnmapMemory (device, buffer->memory);
      }

      if (buffer->buffer != VK_NULL_HANDLE)
      {
        vkDestroyBuffer (device, buffer->buffer, NULL);
      }

      if (buffer->memory != VK_NULL_HANDLE)
      {
        vkFreeMemory (device, buffer->memory, NULL);
      }
    }

    vkDestroyDevice (device, NULL);
  }

  return status;
}

/**
 * Inspect the first device with Vulkan 1.0 compute and four small buffers.
 *
 * The test only queries actual memory properties, performs low-volume buffer
 * allocation/binding and maps the host staging buffers. All Vulkan objects
 * are destroyed before exit, and no compute work is submitted.
 */
int main (int argc, char **argv)
{
  if (argc != 1)
  {
    fprintf (stderr, "Usage: %s\n", argv[0]);

    return 2;
  }

  const VkApplicationInfo application =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Portable Vulkan Memory Heap Inspection",
    .applicationVersion = VK_MAKE_VERSION (1, 0, 0),
    .pEngineName = "None",
    .engineVersion = VK_MAKE_VERSION (1, 0, 0),
    .apiVersion = VK_API_VERSION_1_0
  };
  const VkInstanceCreateInfo instance_info =
  {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &application
  };

  VkInstance instance = VK_NULL_HANDLE;
  VkResult result = vkCreateInstance (
    &instance_info, NULL, &instance
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "VULKAN_HEAP_INSTANCE_FAILED result=%d\n", result
    );

    return 1;
  }

  int status = 1;
  uint32_t device_count = 0u;

  result = vkEnumeratePhysicalDevices (
    instance, &device_count, NULL
  );

  if (result != VK_SUCCESS || device_count == 0u)
  {
    fprintf (
      stderr,
      "VULKAN_HEAP_DEVICE_ENUMERATION_FAILED"
      " result=%d count=%" PRIu32 "\n",
      result, device_count
    );

    goto cleanup_instance;
  }

  VkPhysicalDevice *devices =
    (VkPhysicalDevice *) calloc (device_count, sizeof (*devices));

  if (devices == NULL)
  {
    fprintf (stderr, "VULKAN_HEAP_HOST_ALLOCATION_FAILED\n");

    goto cleanup_instance;
  }

  result = vkEnumeratePhysicalDevices (
    instance, &device_count, devices
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "VULKAN_HEAP_ENUMERATION_FAILED result=%d\n", result
    );

    goto cleanup_devices;
  }

  const vulkan_auto_compute_requirements_t workload =
  {
    .local_size = { 32u, 1u, 1u },
    .group_count = { 64u, 1u, 1u },
    .storage_buffer_count = 2u,
    .largest_storage_buffer_bytes = TEST_BUFFER_BYTES,
    .shared_memory_bytes = 0u
  };

  for (uint32_t index = 0u; index < device_count; index++)
  {
    vulkan_auto_device_t selected = { 0 };

    result = vulkan_auto_check_device (
      devices[index], index, &workload, &selected, stdout
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stdout,
        "SKIP_HEAP_DEVICE index=%" PRIu32
        " reason=compute_incompatibility\n", index
      );

      continue;
    }

    if (inspect_device (&selected) == 0)
    {
      status = 0;

      break;
    }

    fprintf (
      stdout,
      "SKIP_HEAP_DEVICE index=%" PRIu32
      " reason=buffer_or_memory_failure\n", index
    );
  }

  if (status != 0)
  {
    fprintf (
      stderr, "NO_COMPATIBLE_VULKAN_HEAP_CONFIGURATION\n"
    );
  }

cleanup_devices:
  free (devices);

cleanup_instance:
  vkDestroyInstance (instance, NULL);

  return status;
}
