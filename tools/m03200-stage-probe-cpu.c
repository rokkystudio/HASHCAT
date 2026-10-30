/**
 * Execute the unchanged standalone bcrypt stage OpenCL kernels through
 * hashcat's native CPU emulation macros for the two public test candidates.
 *
 * The generated files contain the exact in-memory bcrypt_stage_t structure
 * and the six final Blowfish ciphertext words. No GPU or OpenCL ICD is used.
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

#define KERNEL_STATIC
#define FIXED_LOCAL_SIZE 1
#define FIXED_THREAD_COUNT(n)

#include "m03200-stage-probe.cl"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Emulate a single work-item OpenCL launch on the native CPU.
 */
size_t get_global_id (u32 dimension)
{
  if (dimension != 0) abort ();

  return 0;
}

/**
 * Return the local ID of the single CPU work-item.
 */
size_t get_local_id (u32 dimension)
{
  if (dimension != 0) abort ();

  return 0;
}

/**
 * Return the local size of the single-work-item CPU launch.
 */
size_t get_local_size (u32 dimension)
{
  if (dimension != 0) abort ();

  return 1;
}

/**
 * Implement the atomic decrement operation for native hashcat helpers.
 */
u32 hc_atomic_dec (volatile u32 *value)
{
  return __atomic_fetch_sub (value, 1, __ATOMIC_RELAXED);
}

/**
 * Implement the atomic increment operation for native hashcat helpers.
 */
u32 hc_atomic_inc (volatile u32 *value)
{
  return __atomic_fetch_add (value, 1, __ATOMIC_RELAXED);
}

/**
 * Implement the atomic OR operation for native hashcat helpers.
 */
u32 hc_atomic_or (volatile u32 *value, volatile const u32 mask)
{
  return __atomic_fetch_or (value, mask, __ATOMIC_RELAXED);
}

/**
 * Write one exact CPU-stage memory image into the supplied temporary folder.
 */
static int write_stage (const char *directory, const char *name, const void *buffer, size_t length)
{
  char path[768];

  const int size = snprintf (path, sizeof (path), "%s/m03200-%s.bin", directory, name);

  if (size < 0 || (size_t) size >= sizeof (path))
  {
    fprintf (stderr, "Stage output path too long: %s\n", name);

    return 1;
  }

  FILE *file = fopen (path, "wb");

  if (file == NULL)
  {
    fprintf (stderr, "Cannot create CPU stage: %s\n", path);

    return 1;
  }

  const size_t written = fwrite (buffer, 1, length, file);
  const int status = fclose (file);

  if (written != length || status != 0)
  {
    fprintf (stderr, "Incomplete CPU stage output: %s\n", path);

    return 1;
  }

  printf ("CPU_STAGE_SAVED %s %zu\n", name, length);

  return 0;
}

/**
 * Execute init, one loop iteration, the remaining 31 rounds, and final
 * ciphertext calculation using the complete standalone probe source.
 */
int main (int argc, char **argv)
{
  if (argc != 3 || (strcmp (argv[1], "hashcat") != 0 && strcmp (argv[1], "hashcat2") != 0))
  {
    fprintf (stderr, "usage: bcrypt-stage-probe-cpu hashcat|hashcat2 <output-directory>\n");

    return 2;
  }

  if (sizeof (bcrypt_stage_t) != 4240
   || offsetof (bcrypt_stage_t, E)  !=    0
   || offsetof (bcrypt_stage_t, P)  !=   72
   || offsetof (bcrypt_stage_t, S0) !=  144
   || offsetof (bcrypt_stage_t, S1) != 1168
   || offsetof (bcrypt_stage_t, S2) != 2192
   || offsetof (bcrypt_stage_t, S3) != 3216)
  {
    fprintf (stderr, "Unexpected bcrypt_stage_t ABI layout\n");

    return 3;
  }

  const char *password = argv[1];
  const u32 pw_len = (u32) strlen (password);
  u32 candidate_words[18] = { 0 };

  memcpy (candidate_words, password, pw_len);

  const u32 salt_words[4] =
  {
    0x38313532u, 0x32373238u, 0x35363036u, 0x35373633u
  };

  bcrypt_stage_t stage;

  memset (&stage, 0, sizeof (stage));

  bcrypt_stage_init (&stage, candidate_words, pw_len, salt_words);

  if (write_stage (argv[2], "init", &stage, sizeof (stage)) != 0) return 4;

  bcrypt_stage_loop (&stage, salt_words, 1);

  if (write_stage (argv[2], "loop1", &stage, sizeof (stage)) != 0) return 5;

  bcrypt_stage_loop (&stage, salt_words, 31);

  if (write_stage (argv[2], "loop_final", &stage, sizeof (stage)) != 0) return 6;

  u32 result[6] = { 0 };

  bcrypt_stage_comp (&stage, result);

  if (write_stage (argv[2], "comp", result, sizeof (result)) != 0) return 7;

  puts ("CPU_EMULATED_OPENCL_PROBE_COMPLETE");

  return 0;
}
