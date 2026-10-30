/**
 * Execute stock hashcat bcrypt 3200 init and loop kernels as native CPU code.
 *
 * The source OpenCL/m03200-pure.cl is included verbatim. The native harness
 * supplies the three kernel arguments needed by these kernels, and uses
 * hashcat's actual candidate and salt structures with one work-item.
 *
 * The stock comparison kernel has no CPU readback output; final ciphertext
 * is validated separately by the standalone OpenCL probe CPU regression.
 * No GPU device, real password hash, or OpenCL ICD is used.
 */

#include "common.h"
#include "types.h"
#include "bitops.h"

#define DGST_ELEM 4
#define DGST_R0 0
#define DGST_R1 1
#define DGST_R2 2
#define DGST_R3 3

#include "emu_general.h"

#define FIXED_LOCAL_SIZE 1
#define FIXED_THREAD_COUNT(n)

#include "inc_vendor.h"
#include "inc_types.h"
#include "inc_platform.cl"
#include "inc_common.cl"
#include "inc_cipher_blowfish.cl"

#undef KERN_ATTR_TMPS
#define KERN_ATTR_TMPS(t) const pw_t *pws, const salt_t *salt_bufs, t *tmps

#undef GID_CNT
#define GID_CNT 1

#undef SALT_POS_HOST
#define SALT_POS_HOST 0

static u32 cpu_loop_count = 1;

#undef LOOP_CNT
#define LOOP_CNT cpu_loop_count

#include "m03200-pure.cl"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Return the only global work-item index used by the CPU regression.
 */
size_t get_global_id (u32 dimension)
{
  if (dimension != 0) abort ();

  return 0;
}

/**
 * Return the only local work-item index used by the CPU regression.
 */
size_t get_local_id (u32 dimension)
{
  if (dimension != 0) abort ();

  return 0;
}

/**
 * Return the local workgroup size of the CPU regression.
 */
size_t get_local_size (u32 dimension)
{
  if (dimension != 0) abort ();

  return 1;
}

/**
 * Implement hashcat's atomic decrement for the native helper library.
 */
u32 hc_atomic_dec (volatile u32 *value)
{
  return __atomic_fetch_sub (value, 1, __ATOMIC_RELAXED);
}

/**
 * Implement hashcat's atomic increment for the native helper library.
 */
u32 hc_atomic_inc (volatile u32 *value)
{
  return __atomic_fetch_add (value, 1, __ATOMIC_RELAXED);
}

/**
 * Implement hashcat's atomic OR for the native helper library.
 */
u32 hc_atomic_or (volatile u32 *value, volatile const u32 mask)
{
  return __atomic_fetch_or (value, mask, __ATOMIC_RELAXED);
}

/**
 * Write one complete stock bcrypt temporary buffer to the test output folder.
 */
static int write_stage (const char *directory, const char *name, const void *buffer, size_t length)
{
  char path[768];

  const int size = snprintf (path, sizeof (path), "%s/m03200-%s.bin", directory, name);

  if (size < 0 || (size_t) size >= sizeof (path))
  {
    fprintf (stderr, "Output path too long: %s\n", name);

    return 1;
  }

  FILE *file = fopen (path, "wb");

  if (file == NULL)
  {
    fprintf (stderr, "Cannot create stock CPU stage: %s\n", path);

    return 1;
  }

  const size_t written = fwrite (buffer, 1, length, file);
  const int status = fclose (file);

  if (written != length || status != 0)
  {
    fprintf (stderr, "Incomplete stock CPU stage: %s\n", path);

    return 1;
  }

  printf ("CPU_STOCK_STAGE_SAVED %s %zu\n", name, length);

  return 0;
}

/**
 * Execute stock m03200_init, then one and 31 m03200_loop iterations and
 * record the exact 4240-byte temporary buffer after each stage.
 */
int main (int argc, char **argv)
{
  if (argc != 3 || (strcmp (argv[1], "hashcat") != 0 && strcmp (argv[1], "hashcat2") != 0))
  {
    fprintf (stderr, "usage: bcrypt-stock-cpu hashcat|hashcat2 <output-directory>\n");

    return 2;
  }

  if (sizeof (bcrypt_tmp_t) != 4240
   || offsetof (bcrypt_tmp_t, E)  !=    0
   || offsetof (bcrypt_tmp_t, P)  !=   72
   || offsetof (bcrypt_tmp_t, S0) !=  144
   || offsetof (bcrypt_tmp_t, S1) != 1168
   || offsetof (bcrypt_tmp_t, S2) != 2192
   || offsetof (bcrypt_tmp_t, S3) != 3216)
  {
    fprintf (stderr, "Unexpected stock bcrypt_tmp_t ABI layout\n");

    return 3;
  }

  const char *password = argv[1];
  const u32 password_length = (u32) strlen (password);

  pw_t candidate;

  memset (&candidate, 0, sizeof (candidate));
  memcpy (candidate.i, password, password_length);
  candidate.pw_len = password_length;

  salt_t salt;

  memset (&salt, 0, sizeof (salt));

  salt.salt_buf[0] = 0x38313532u;
  salt.salt_buf[1] = 0x32373238u;
  salt.salt_buf[2] = 0x35363036u;
  salt.salt_buf[3] = 0x35373633u;
  salt.salt_len = 16;
  salt.salt_iter = 32;

  bcrypt_tmp_t tmps;

  memset (&tmps, 0, sizeof (tmps));

  m03200_init (&candidate, &salt, &tmps);

  if (write_stage (argv[2], "init", &tmps, sizeof (tmps)) != 0) return 4;

  cpu_loop_count = 1;

  m03200_loop (&candidate, &salt, &tmps);

  if (write_stage (argv[2], "loop1", &tmps, sizeof (tmps)) != 0) return 5;

  cpu_loop_count = 31;

  m03200_loop (&candidate, &salt, &tmps);

  if (write_stage (argv[2], "loop_final", &tmps, sizeof (tmps)) != 0) return 6;

  puts ("CPU_EMULATED_STOCK_BCRYPT_COMPLETE");

  return 0;
}
