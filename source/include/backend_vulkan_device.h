/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#ifndef HC_BACKEND_VULKAN_DEVICE_H
#define HC_BACKEND_VULKAN_DEVICE_H

#include "types.h"

/**
 * Opaque per-device Vulkan state.
 *
 * Only the Vulkan device layer owns this structure. types.h keeps a bare
 * pointer to it so that no Vulkan header or layout leaks into the generic
 * device table. The structure is populated in a later milestone; in M1 every
 * registered device carries NULL here.
 */

struct hc_vulkan_device;

/**
 * Register the physical devices of the already opened Vulkan catalogue as
 * ordinary device table entries.
 *
 * The entries are appended after every schedulable backend so that existing
 * -d identifiers stay stable. They are visible in -I and --backend-info-json
 * and can be selected with -d, but they are not schedulable until Vulkan
 * hashing is implemented.
 */

void backend_vulkan_devices_init (hashcat_ctx_t *hashcat_ctx);

/**
 * Release the state allocated by backend_vulkan_devices_init.
 *
 * The catalogue itself is owned by backend_vulkan_catalog_close and is left
 * untouched. Calling this function without registered devices is harmless.
 */

void backend_vulkan_devices_term (hashcat_ctx_t *hashcat_ctx);

/**
 * Report the 1-based device id when the user explicitly selected a Vulkan
 * device through -d / --backend-device, otherwise zero.
 */

u32 backend_vulkan_device_selected (hashcat_ctx_t *hashcat_ctx);

#endif // HC_BACKEND_VULKAN_DEVICE_H
