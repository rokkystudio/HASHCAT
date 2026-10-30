/**
 * Verify the unmodified hashcat bcrypt mode 3200 comparison kernel on CPU.
 *
 * This executable includes the original m03200-pure.cl and inc_comp_multi.cl
 * code, together with hashcat's bitmap, binary digest lookup, and mark_hash
 * functions. The candidate's 4240-byte P/S-box state is read from the native
 * stock-kernel CPU regression, not from a live device or unknown hash.
 *
 * A single expected 128-bit digest is passed by the synthetic test driver.
 * The bitmap tables contain exactly the corresponding digest's four words.
 * No OpenCL ICD, GPU process or password search is executed.
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

#define KERNEL_STATIC

#define INCLUDE_PATH source/OpenCL
#define XM2S(x) #x
#define M2S(x) XM2S(x)

#include "inc_common.h"

#undef KERN_ATTR_TMPS
#define KERN_ATTR_TMPS(t)                       \
  const pw_t *pws,                              \
  const salt_t *salt_bufs,                      \
  t *tmps,                                     \
  const digest_t *digests_buf,                  \
  u32 *hashes_shown,                           \
  plain_t *plains_buf,                         \
  u32 *d_return_buf,                           \
  const u32 *bitmaps_buf_s1_a,                  \
  const u32 *bitmaps_buf_s1_b,                  \
  const u32 *bitmaps_buf_s1_c,                  \
  const u32 *bitmaps_buf_s1_d,                  \
  const u32 *bitmaps_buf_s2_a,                  \
  const u32 *bitmaps_buf_s2_b,                  \
  const u32 *bitmaps_buf_s2_c,                  \
  const u32 *bitmaps_buf_s2_d

#undef GID_CNT
#define GID_CNT 1

#undef SALT_POS_HOST
#define SALT_POS_HOST 0

#undef DIGESTS_CNT
#define DIGESTS_CNT 1

#undef DIGESTS_OFFSET_HOST
#define DIGESTS_OFFSET_HOST 0

#undef BITMAP_MASK
#define BITMAP_MASK 65535u

#undef BITMAP_SHIFT1
#define BITMAP_SHIFT1 5u

#undef BITMAP_SHIFT2
#define BITMAP_SHIFT2 13u

static u32 cpu_loop_count = 1;

#undef LOOP_CNT
#define LOOP_CNT cpu_loop_count

#include "m03200-pure.cl"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Return the global work-item index for the one-candidate CPU comparison.
 */
size_t get_global_id (u32 dimension)
{
  if (dimension != 0) abort ();

  return 0;
}

/**
 * Return the local work-item index for the one-candidate CPU comparison.
 */
size_t get_local_id (u32 dimension)
{
  if (dimension != 0) abort ();

  return 0;
}

/**
 * Return the workgroup size for the one-candidate CPU comparison.
 */
size_t get_local_size (u32 dimension)
{
  if (dimension != 0) abort ();

  return 1;
}

/**
 * Atomically decrement the host-side result counter for hashcat's mark_hash.
 */
u32 hc_atomic_dec (volatile u32 *value)
{
  return __atomic_fetch_sub (value, 1, __ATOMIC_RELAXED);
}

/**
 * Atomically increment the host-side result counter for hashcat's mark_hash.
 */
u32 hc_atomic_inc (volatile u32 *value)
{
  return __atomic_fetch_add (value, 1, __ATOMIC_RELAXED);
}

/**
 * Atomically OR the host-side bit mask for hashcat's common helpers.
 */
u32 hc_atomic_or (volatile u32 *value, volatile const u32 mask)
{
  return __atomic_fetch_or (value, mask, __ATOMIC_RELAXED);
}

/**
 * Read exactly one native bcrypt_tmp_t value from the CPU stage file.
 */
static int read_stage (const char *path, bcrypt_tmp_t *tmps)
{
  FILE *file = fopen (path, "rb");

  if (file == NULL)
  {
    fprintf (stderr, "Cannot read stock bcrypt state: %s: %s\n", path, strerror (errno));

    return 1;
  }

  const size_t bytes = fread (tmps, 1, sizeof (*tmps), file);
  const int end = fgetc (file);
  const int error = ferror (file);
  const int close_status = fclose (file);

  if (bytes != sizeof (*tmps) || end != EOF || error != 0 || close_status != 0)
  {
    fprintf (stderr, "Stock bcrypt state must contain exactly 4240 bytes: %s\n", path);

    return 1;
  }

  return 0;
}

/**
 * Parse a public test vector's first four digest words as 32 hexadecimal
 * digits in the same numeric word order used by hashcat mode 3200.
 */
static int parse_digest (const char *hex, digest_t *digest)
{
  if (strlen (hex) != 32)
  {
    fprintf (stderr, "Expected 32 lowercase hexadecimal digits for four words\n");

    return 1;
  }

  for (u32 i = 0; i < 4; i++)
  {
    char word[9];

    memcpy (word, hex + (i * 8), 8);
    word[8] = 0;

    for (u32 j = 0; j < 8; j++)
    {
      if (!((word[j] >= '0' && word[j] <= '9') || (word[j] >= 'a' && word[j] <= 'f')))
      {
        fprintf (stderr, "Invalid numeric bcrypt digest word %u\n", (unsigned int) i);

        return 1;
      }
    }

    digest->digest_buf[i] = (u32) strtoul (word, NULL, 16);
  }

  return 0;
}

/**
 * Construct the exact one-digest bitmap entries used by the real
 * inc_comp_multi.cl prefilter at bitmap shifts five and thirteen.
 */
static void set_expected_bits (u32 *bitmaps[8], const digest_t *digest)
{
  for (u32 i = 0; i < 4; i++)
  {
    const u32 word = digest->digest_buf[i];
    const u32 bit = 1u << (word & 31u);

    bitmaps[i + 0][(word >> BITMAP_SHIFT1) & BITMAP_MASK] |= bit;
    bitmaps[i + 4][(word >> BITMAP_SHIFT2) & BITMAP_MASK] |= bit;
  }
}

/**
 * Execute the stock m03200_comp kernel on one known CPU-computed state,
 * then print its match count, duplicate protection and marked candidate.
 */
int main (int argc, char **argv)
{
  if (argc != 3)
  {
    fprintf (stderr, "usage: bcrypt-stock-comp-cpu <loop-final.bin> <digest-first-4-words-hex>\n");

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

  bcrypt_tmp_t tmps;

  if (read_stage (argv[1], &tmps) != 0) return 4;

  digest_t digest;

  if (parse_digest (argv[2], &digest) != 0) return 5;

  u32 *bitmaps[8] = { 0 };

  for (u32 i = 0; i < 8; i++)
  {
    bitmaps[i] = (u32 *) calloc (BITMAP_MASK + 1u, sizeof (u32));

    if (bitmaps[i] == NULL)
    {
      fprintf (stderr, "Cannot allocate exact hashcat bitmap table\n");

      for (u32 j = 0; j < i; j++)
      {
        free (bitmaps[j]);
      }

      return 6;
    }
  }

  set_expected_bits (bitmaps, &digest);

  pw_t candidate;

  memset (&candidate, 0, sizeof (candidate));

  salt_t salt;

  memset (&salt, 0, sizeof (salt));

  u32 hashes_shown[1] = { 0 };
  plain_t plains_buf[1] = { 0 };
  u32 d_return_buf[1] = { 0 };

  m03200_comp
  (
    &candidate, &salt, &tmps,
    &digest, hashes_shown, plains_buf, d_return_buf,
    bitmaps[0], bitmaps[1], bitmaps[2], bitmaps[3],
    bitmaps[4], bitmaps[5], bitmaps[6], bitmaps[7]
  );

  printf ("MATCH_COUNT %u\n", (unsigned int) d_return_buf[0]);
  printf ("HASHES_SHOWN %u\n", (unsigned int) hashes_shown[0]);
  printf ("PLAIN_GID %llu\n", (unsigned long long) plains_buf[0].gidvid);
  printf ("PLAIN_SALT_POS %u\n", (unsigned int) plains_buf[0].salt_pos);
  printf ("PLAIN_DIGEST_POS %u\n", (unsigned int) plains_buf[0].digest_pos);
  printf ("PLAIN_HASH_POS %u\n", (unsigned int) plains_buf[0].hash_pos);

  for (u32 i = 0; i < 8; i++)
  {
    free (bitmaps[i]);
  }

  if (d_return_buf[0] > 1 || hashes_shown[0] > 1 || d_return_buf[0] != hashes_shown[0])
  {
    fprintf (stderr, "Stock bcrypt comparison result counters are inconsistent\n");

    return 7;
  }

  return 0;
}
