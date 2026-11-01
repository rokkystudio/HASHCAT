#ifndef VULKAN_CORE10_RESULT_GATE_H
#define VULKAN_CORE10_RESULT_GATE_H

/**
 * Control submission eligibility after explicit Vulkan API results.
 *
 * This standalone policy records Vulkan 1.0 VkResult values returned by
 * fence-status queries, fence waits and completed public buffer transfers.
 * VK_NOT_READY is valid only for fence-status polling and VK_TIMEOUT only
 * for fence waits. Every negative VkResult is terminal for the currently
 * owned VkDevice; no retry or replacement device is selected.
 *
 * The policy owns no Vulkan handles and cannot cause GPU device loss.
 * Its caller must consult the submission gate before each operation,
 * release the VkDevice after a terminal result and close the policy.
 * The HASHCAT production backend does not use this policy.
 */

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>

/**
 * Describe the Vulkan operation that produced a result.
 *
 * FENCE_STATUS accepts VK_SUCCESS/VK_NOT_READY, FENCE_WAIT accepts
 * VK_SUCCESS/VK_TIMEOUT, and TRANSFER accepts only VK_SUCCESS.
 * All sites treat VK_ERROR_DEVICE_LOST and other negative VkResult
 * values as fatal for this logical-device lifetime.
 */
typedef enum vulkan_core10_result_site
{
  VULKAN_CORE10_RESULT_SITE_FENCE_STATUS = 1,
  VULKAN_CORE10_RESULT_SITE_FENCE_WAIT = 2,
  VULKAN_CORE10_RESULT_SITE_TRANSFER = 3
} vulkan_core10_result_site_t;

/**
 * Represent one irreversible submission-eligibility lifecycle.
 *
 * ZERO -> ACTIVE -> FAILED -> CLOSED and ZERO -> ACTIVE -> CLOSED
 * are the only state transitions. A FAILED policy retains its exact
 * first negative Vulkan result until closing, including DEVICE_LOST.
 * Terminal states never allow another submission. A zeroed new policy
 * is required for a separately created VkDevice.
 */
typedef enum vulkan_core10_result_gate_state
{
  VULKAN_CORE10_RESULT_GATE_ZERO = 0,
  VULKAN_CORE10_RESULT_GATE_ACTIVE = 1,
  VULKAN_CORE10_RESULT_GATE_FAILED = 2,
  VULKAN_CORE10_RESULT_GATE_CLOSED = 3
} vulkan_core10_result_gate_state_t;

/**
 * Track VkResult observations and submission decisions for one VkDevice.
 *
 * allowed counts successful application-side permission checks; it
 * does not claim a GPU submission occurred. blocked counts rejected
 * checks after a terminal device error. accepted_statuses counts
 * nonfatal VK_NOT_READY and VK_TIMEOUT events from valid query sites.
 * Only the caller owns VkDevice and records real GPU submissions.
 */
typedef struct vulkan_core10_result_gate
{
  vulkan_core10_result_gate_state_t state;
  VkResult first_error;
  uint32_t allowed;
  uint32_t blocked;
  uint32_t accepted_statuses;
  uint32_t successful_results;
  FILE *diagnostics;
} vulkan_core10_result_gate_t;

/**
 * Open a result policy for exactly one newly created Vulkan device.
 *
 * Requires an empty zero-initialized policy and a valid diagnostics
 * stream. Starts in ACTIVE with no observed error or permitted work.
 * An already active, failed or closed policy cannot be reopened.
 * Never creates, destroys or queries a Vulkan handle.
 */
VkResult vulkan_core10_result_gate_open (
  vulkan_core10_result_gate_t *gate,
  FILE *diagnostics);

/**
 * Record a VkResult from the explicitly named Vulkan operation.
 *
 * VK_SUCCESS is accepted at every known site. VK_NOT_READY is
 * accepted only at FENCE_STATUS; VK_TIMEOUT is accepted only at
 * FENCE_WAIT. These status results keep the policy ACTIVE.
 * Every negative VkResult, including VK_ERROR_DEVICE_LOST, moves the
 * policy to FAILED and remains the first_error until close.
 * Unexpected positive VkResult values become a terminal
 * VK_ERROR_INITIALIZATION_FAILED, preserving the observed code in
 * diagnostic output instead of quietly treating it as success.
 *
 * A failed policy returns its original terminal error and does not
 * accept further observations, even if a later caller supplies SUCCESS.
 */
VkResult vulkan_core10_result_gate_observe (
  vulkan_core10_result_gate_t *gate,
  vulkan_core10_result_site_t site,
  VkResult observed);

/**
 * Decide whether the caller may perform a new GPU submission.
 *
 * Returns VK_SUCCESS and increments allowed only while ACTIVE.
 * After the first terminal VkResult, returns that exact error and
 * increments blocked without invoking Vulkan or modifying output
 * buffers. CLOSED or unopened policies return initialization failure.
 * The caller is responsible for enforcing the returned decision
 * before calling any queue/submission API.
 */
VkResult vulkan_core10_result_gate_before_submit (
  vulkan_core10_result_gate_t *gate);

/**
 * Finish one result policy after its logical-device resources close.
 *
 * Marks ACTIVE or FAILED policies CLOSED and reports the exact
 * retained first error, allowed/blocked decisions and nonfatal
 * status observations. CLOSED never transitions back to ACTIVE.
 * Closing an unopened or already CLOSED policy is invalid.
 * This method does not call vkDeviceWaitIdle or vkDestroyDevice.
 */
VkResult vulkan_core10_result_gate_close (
  vulkan_core10_result_gate_t *gate);

#endif
