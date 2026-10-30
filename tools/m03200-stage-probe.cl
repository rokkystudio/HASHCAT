/**
 * Capture the three bcrypt 3200 Blowfish state boundaries for one synthetic
 * candidate. The arithmetic uses the normal hashcat OpenCL Blowfish helpers.
 * This standalone diagnostic does not call hashcat's cracking runtime.
 */

#define NEW_SIMD_CODE

#include "inc_vendor.h"
#include "inc_types.h"
#include "inc_platform.cl"
#include "inc_common.cl"
#include "inc_cipher_blowfish.cl"

typedef struct bcrypt_stage
{
  u32 E[18];

  u32 P[18];

  u32 S0[256];
  u32 S1[256];
  u32 S2[256];
  u32 S3[256];

} bcrypt_stage_t;

/**
 * Compute expanded password words and the bcrypt salted initial P/S-box state.
 * The output has the same field layout as mode 3200's bcrypt_tmp_t.
 */
KERNEL_FQ KERNEL_FA void bcrypt_stage_init
(
  GLOBAL_AS bcrypt_stage_t *tmps,
  GLOBAL_AS const u32 *candidate,
  const u32 candidate_len,
  GLOBAL_AS const u32 *salt_words
)
{
  if (get_global_id (0) != 0) return;

  u32 W[18];
  u32 E[18] = { 0 };

  for (u32 i = 0; i < 18; i++)
  {
    W[i] = candidate[i];
  }

  expand_key (E, W, candidate_len);

  for (u32 i = 0; i < 18; i++)
  {
    E[i] = hc_swap32_S (E[i]);
    tmps[0].E[i] = E[i];
  }

  u32 salt_buf[4];

  for (u32 i = 0; i < 4; i++)
  {
    salt_buf[i] = salt_words[i];
  }

  LOCAL_VK u32 S0[256];
  LOCAL_VK u32 S1[256];
  LOCAL_VK u32 S2[256];
  LOCAL_VK u32 S3[256];

  u32 P[18];

  blowfish_set_key_salt (E, 18, salt_buf, P, S0, S1, S2, S3);

  for (u32 i = 0; i < 18; i++)
  {
    tmps[0].P[i] = P[i];
  }

  for (u32 i = 0; i < 256; i++)
  {
    tmps[0].S0[i] = GET_KEY32 (S0, i);
    tmps[0].S1[i] = GET_KEY32 (S1, i);
    tmps[0].S2[i] = GET_KEY32 (S2, i);
    tmps[0].S3[i] = GET_KEY32 (S3, i);
  }
}

/**
 * Continue the bcrypt key schedule for exactly 'iterations' rounds using
 * the temporary state supplied by the previous stage.
 */
KERNEL_FQ KERNEL_FA void bcrypt_stage_loop
(
  GLOBAL_AS bcrypt_stage_t *tmps,
  GLOBAL_AS const u32 *salt_words,
  const u32 iterations
)
{
  if (get_global_id (0) != 0) return;

  u32 E[18];
  u32 P[18];

  for (u32 i = 0; i < 18; i++)
  {
    E[i] = tmps[0].E[i];
    P[i] = tmps[0].P[i];
  }

  LOCAL_VK u32 S0[256];
  LOCAL_VK u32 S1[256];
  LOCAL_VK u32 S2[256];
  LOCAL_VK u32 S3[256];

  for (u32 i = 0; i < 256; i++)
  {
    SET_KEY32 (S0, i, tmps[0].S0[i]);
    SET_KEY32 (S1, i, tmps[0].S1[i]);
    SET_KEY32 (S2, i, tmps[0].S2[i]);
    SET_KEY32 (S3, i, tmps[0].S3[i]);
  }

  u32 salt_buf[4];

  for (u32 i = 0; i < 4; i++)
  {
    salt_buf[i] = salt_words[i];
  }

  for (u32 j = 0; j < iterations; j++)
  {
    for (u32 i = 0; i < 18; i++)
    {
      P[i] ^= E[i];
    }

    blowfish_encrypt (P, S0, S1, S2, S3);

    for (u32 i = 0; i < 18; i++)
    {
      P[i] ^= salt_buf[i % 4];
    }

    blowfish_encrypt (P, S0, S1, S2, S3);
  }

  for (u32 i = 0; i < 18; i++)
  {
    tmps[0].P[i] = P[i];
  }

  for (u32 i = 0; i < 256; i++)
  {
    tmps[0].S0[i] = GET_KEY32 (S0, i);
    tmps[0].S1[i] = GET_KEY32 (S1, i);
    tmps[0].S2[i] = GET_KEY32 (S2, i);
    tmps[0].S3[i] = GET_KEY32 (S3, i);
  }
}

/**
 * Encrypt the six bcrypt verification words using the completed P/S-box
 * state. The 23 meaningful bcrypt digest bytes are held in the result buffer.
 */
KERNEL_FQ KERNEL_FA void bcrypt_stage_comp
(
  GLOBAL_AS const bcrypt_stage_t *tmps,
  GLOBAL_AS u32 *result
)
{
  if (get_global_id (0) != 0) return;

  u32 P[18];

  for (u32 i = 0; i < 18; i++)
  {
    P[i] = tmps[0].P[i];
  }

  LOCAL_VK u32 S0[256];
  LOCAL_VK u32 S1[256];
  LOCAL_VK u32 S2[256];
  LOCAL_VK u32 S3[256];

  for (u32 i = 0; i < 256; i++)
  {
    SET_KEY32 (S0, i, tmps[0].S0[i]);
    SET_KEY32 (S1, i, tmps[0].S1[i]);
    SET_KEY32 (S2, i, tmps[0].S2[i]);
    SET_KEY32 (S3, i, tmps[0].S3[i]);
  }

  u32 L0 = BCRYPTM_0;
  u32 R0 = BCRYPTM_1;

  for (u32 i = 0; i < 64; i++)
  {
    BF_ENCRYPT (L0, R0);
  }

  result[0] = L0;
  result[1] = R0;

  L0 = BCRYPTM_2;
  R0 = BCRYPTM_3;

  for (u32 i = 0; i < 64; i++)
  {
    BF_ENCRYPT (L0, R0);
  }

  result[2] = L0;
  result[3] = R0;

  L0 = BCRYPTM_4;
  R0 = BCRYPTM_5;

  for (u32 i = 0; i < 64; i++)
  {
    BF_ENCRYPT (L0, R0);
  }

  result[4] = L0;
  result[5] = R0;
}
