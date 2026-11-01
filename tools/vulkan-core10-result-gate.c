/**
 * Apply explicit Vulkan core 1.0 results to submission eligibility.
 *
 * This independent policy contains no Vulkan handles or dispatch code.
 * Its caller records genuine driver status values and consults the
 * permission gate before attempting work on a logical device.
 * Negative VkResult values permanently disable new submissions for
 * that device; Vulkan timeouts and pending fence status are nonfatal
 * only for the corresponding named query operation.
 */

#include "vulkan-core10-result-gate.h"

#include <inttypes.h>
#include <stddef.h>

/**
 * Return the declared diagnostic name for one Vulkan result site.
 *
 * Unknown sites return "invalid" and cannot accept a VkResult as
 * successful. The policy does not infer a site from a numeric code.
 */
static const char *vulkan_core10_result_gate_site_name (
  vulkan_core10_result_site_t site)
{
  switch (site)
  {
    case VULKAN_CORE10_RESULT_SITE_FENCE_STATUS: return "fence_status";
    case VULKAN_CORE10_RESULT_SITE_FENCE_WAIT:   return "fence_wait";
    case VULKAN_CORE10_RESULT_SITE_TRANSFER:     return "transfer";
  }

  return "invalid";
}

/**
 * Begin one non-reusable Vulkan logical-device result lifecycle.
 *
 * Requires a zeroed object and valid diagnostic stream. Records the
 * ACTIVE state only, without creating or querying any Vulkan object.
 */
VkResult vulkan_core10_result_gate_open (
  vulkan_core10_result_gate_t *gate,
  FILE *diagnostics)
{
  if (gate == NULL || diagnostics == NULL
   || gate->state != VULKAN_CORE10_RESULT_GATE_ZERO
   || gate->allowed != 0u || gate->blocked != 0u
   || gate->accepted_statuses != 0u
   || gate->successful_results != 0u
   || gate->first_error != VK_SUCCESS
   || gate->diagnostics != NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  gate->state = VULKAN_CORE10_RESULT_GATE_ACTIVE;
  gate->diagnostics = diagnostics;

  fprintf (
    diagnostics,
    "RESULT_GATE_OPEN state=active first_error=0"
    " allowed=0 blocked=0\n"
  );

  return VK_SUCCESS;
}

/**
 * Apply a VkResult from an exact declared Vulkan query or transfer.
 *
 * Known positive completion states keep the device active only for
 * their matching site. An unexpected result is terminal, storing the
 * exact negative Vulkan error or VK_ERROR_INITIALIZATION_FAILED for
 * an invalid positive code. The first failure remains sticky until
 * the gate is closed; later successes cannot reactivate the device.
 */
VkResult vulkan_core10_result_gate_observe (
  vulkan_core10_result_gate_t *gate,
  vulkan_core10_result_site_t site,
  VkResult observed)
{
  if (gate == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  if (gate->state == VULKAN_CORE10_RESULT_GATE_FAILED)
  {
    return gate->first_error;
  }

  if (gate->state != VULKAN_CORE10_RESULT_GATE_ACTIVE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const char *name = vulkan_core10_result_gate_site_name (site);

  if (site != VULKAN_CORE10_RESULT_SITE_FENCE_STATUS
   && site != VULKAN_CORE10_RESULT_SITE_FENCE_WAIT
   && site != VULKAN_CORE10_RESULT_SITE_TRANSFER)
  {
    gate->first_error = VK_ERROR_INITIALIZATION_FAILED;
  }
  else if (observed == VK_SUCCESS)
  {
    gate->successful_results++;

    fprintf (
      gate->diagnostics,
      "RESULT_GATE_OBSERVE site=%s observed=0"
      " state=active successes=%" PRIu32 "\n",
      name, gate->successful_results
    );

    return VK_SUCCESS;
  }
  else if ((site == VULKAN_CORE10_RESULT_SITE_FENCE_STATUS
         && observed == VK_NOT_READY)
        || (site == VULKAN_CORE10_RESULT_SITE_FENCE_WAIT
         && observed == VK_TIMEOUT))
  {
    gate->accepted_statuses++;

    fprintf (
      gate->diagnostics,
      "RESULT_GATE_OBSERVE site=%s observed=%d"
      " state=active nonfatal=%" PRIu32 "\n",
      name, observed, gate->accepted_statuses
    );

    return observed;
  }
  else if (observed < VK_SUCCESS)
  {
    gate->first_error = observed;
  }
  else
  {
    gate->first_error = VK_ERROR_INITIALIZATION_FAILED;
  }

  gate->state = VULKAN_CORE10_RESULT_GATE_FAILED;

  fprintf (
    gate->diagnostics,
    "RESULT_GATE_TERMINAL site=%s observed=%d"
    " first_error=%d state=failed"
    " allowed=%" PRIu32
    " blocked=%" PRIu32 "\n",
    name, observed, gate->first_error,
    gate->allowed, gate->blocked
  );

  return gate->first_error;
}

/**
 * Gate every potential queue submission for the current logical device.
 *
 * ACTIVE grants a decision and increments the allowed counter. FAILED
 * rejects the attempt with its original Vulkan result and increments
 * blocked. Other states return an initialization error; no function
 * in this policy touches Vulkan queues, command buffers or host output.
 */
VkResult vulkan_core10_result_gate_before_submit (
  vulkan_core10_result_gate_t *gate)
{
  if (gate == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  if (gate->state == VULKAN_CORE10_RESULT_GATE_ACTIVE)
  {
    gate->allowed++;

    fprintf (
      gate->diagnostics,
      "RESULT_GATE_ALLOW count=%" PRIu32
      " state=active\n",
      gate->allowed
    );

    return VK_SUCCESS;
  }

  if (gate->state == VULKAN_CORE10_RESULT_GATE_FAILED)
  {
    gate->blocked++;

    fprintf (
      gate->diagnostics,
      "RESULT_GATE_BLOCK count=%" PRIu32
      " error=%d state=failed\n",
      gate->blocked, gate->first_error
    );

    return gate->first_error;
  }

  return VK_ERROR_INITIALIZATION_FAILED;
}

/**
 * Close an active or terminal policy after Vulkan resources are released.
 *
 * Reports the retained first error and decision counts, then marks
 * the policy CLOSED permanently. The policy does not destroy Vulkan
 * resources or permit reopening the same object.
 */
VkResult vulkan_core10_result_gate_close (
  vulkan_core10_result_gate_t *gate)
{
  if (gate == NULL
   || (gate->state != VULKAN_CORE10_RESULT_GATE_ACTIVE
    && gate->state != VULKAN_CORE10_RESULT_GATE_FAILED))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkResult first_error = gate->first_error;

  fprintf (
    gate->diagnostics,
    "RESULT_GATE_CLOSE previous=%s first_error=%d"
    " allowed=%" PRIu32
    " blocked=%" PRIu32
    " nonfatal=%" PRIu32
    " successes=%" PRIu32
    " state=closed\n",
    gate->state == VULKAN_CORE10_RESULT_GATE_FAILED
      ? "failed" : "active",
    first_error,
    gate->allowed, gate->blocked,
    gate->accepted_statuses, gate->successful_results
  );

  gate->state = VULKAN_CORE10_RESULT_GATE_CLOSED;

  return VK_SUCCESS;
}
