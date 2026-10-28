/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#define NEW_SIMD_CODE

#ifdef M22000_ADRENO_NOINLINE
#define M22000_NOINLINE __attribute__ ((noinline))
#else
#define M22000_NOINLINE
#endif

#ifdef KERNEL_STATIC
#include M2S(INCLUDE_PATH/inc_vendor.h)
#include M2S(INCLUDE_PATH/inc_types.h)
#include M2S(INCLUDE_PATH/inc_platform.cl)
#include M2S(INCLUDE_PATH/inc_common.cl)
#include M2S(INCLUDE_PATH/inc_simd.cl)
#include M2S(INCLUDE_PATH/inc_hash_md5.cl)
#include M2S(INCLUDE_PATH/inc_hash_sha1.cl)
#include M2S(INCLUDE_PATH/inc_hash_sha256.cl)
#include M2S(INCLUDE_PATH/inc_cipher_aes.cl)
#else
#include "inc_vendor.h"
#include "inc_types.h"
#include "inc_platform.h"
#include "inc_common.h"
#include "inc_simd.h"
#include "inc_hash_md5.h"
#include "inc_hash_sha1.h"
#include "inc_hash_sha256.h"
#include "inc_cipher_aes.h"
#endif

#define COMPARE_S M2S(INCLUDE_PATH/inc_comp_single.cl)
#define COMPARE_M M2S(INCLUDE_PATH/inc_comp_multi.cl)

typedef struct wpa_pbkdf2_tmp
{
  u32 ipad[5];
  u32 opad[5];

  u32 dgst[10];
  u32 out[10];

} wpa_pbkdf2_tmp_t;

typedef struct wpa
{
  u32  essid_buf[16];
  u32  essid_len;

  u32  mac_ap[2];
  u32  mac_sta[2];

  u32  type;            // 1 = PMKID, 2 = EAPOL

  // PMKID specific

  u32  pmkid[4];
  u32  pmkid_data[16];

  // EAPOL specific

  u32  keymic[4];
  u32  anonce[8];

  u32  keyver;

  u32  eapol[64 + 16];
  u32  eapol_len;

  u32  pke[32];

  int  message_pair_chgd;
  u32  message_pair;

  int  nonce_error_corrections_chgd;
  int  nonce_error_corrections;

  int  nonce_compare;
  int  detected_le;
  int  detected_be;

} wpa_t;

DECLSPEC void make_kn (PRIVATE_AS u32 *k)
{
  u32 kl[4];
  u32 kr[4];

  kl[0] = (k[0] << 1) & 0xfefefefe;
  kl[1] = (k[1] << 1) & 0xfefefefe;
  kl[2] = (k[2] << 1) & 0xfefefefe;
  kl[3] = (k[3] << 1) & 0xfefefefe;

  kr[0] = (k[0] >> 7) & 0x01010101;
  kr[1] = (k[1] >> 7) & 0x01010101;
  kr[2] = (k[2] >> 7) & 0x01010101;
  kr[3] = (k[3] >> 7) & 0x01010101;

  const u32 c = kr[0] & 1;

  kr[0] = kr[0] >> 8 | kr[1] << 24;
  kr[1] = kr[1] >> 8 | kr[2] << 24;
  kr[2] = kr[2] >> 8 | kr[3] << 24;
  kr[3] = kr[3] >> 8;

  k[0] = kl[0] | kr[0];
  k[1] = kl[1] | kr[1];
  k[2] = kl[2] | kr[2];
  k[3] = kl[3] | kr[3];

  k[3] ^= c * 0x87000000;
}

DECLSPEC M22000_NOINLINE void hmac_sha1_run_V (PRIVATE_AS u32x *w0, PRIVATE_AS u32x *w1, PRIVATE_AS u32x *w2, PRIVATE_AS u32x *w3, PRIVATE_AS const u32x *ipad, PRIVATE_AS const u32x *opad, PRIVATE_AS u32x *digest)
{
  digest[0] = ipad[0];
  digest[1] = ipad[1];
  digest[2] = ipad[2];
  digest[3] = ipad[3];
  digest[4] = ipad[4];

  sha1_transform_vector (w0, w1, w2, w3, digest);

  w0[0] = digest[0];
  w0[1] = digest[1];
  w0[2] = digest[2];
  w0[3] = digest[3];
  w1[0] = digest[4];
  w1[1] = 0x80000000;
  w1[2] = 0;
  w1[3] = 0;
  w2[0] = 0;
  w2[1] = 0;
  w2[2] = 0;
  w2[3] = 0;
  w3[0] = 0;
  w3[1] = 0;
  w3[2] = 0;
  w3[3] = (64 + 20) * 8;

  digest[0] = opad[0];
  digest[1] = opad[1];
  digest[2] = opad[2];
  digest[3] = opad[3];
  digest[4] = opad[4];

  sha1_transform_vector (w0, w1, w2, w3, digest);
}

#ifdef M22000_ADRENO_NOINLINE
DECLSPEC M22000_NOINLINE void m22000_init_block (PRIVATE_AS const sha1_hmac_ctx_t *base, const u32 block, PRIVATE_AS u32 *out)
{
  u32 w0[4] = { block, 0, 0, 0 };
  u32 w1[4] = { 0, 0, 0, 0 };
  u32 w2[4] = { 0, 0, 0, 0 };
  u32 w3[4] = { 0, 0, 0, 0 };

  sha1_hmac_ctx_t ctx = *base;

  sha1_hmac_update_64 (&ctx, w0, w1, w2, w3, 4);
  sha1_hmac_final (&ctx);

  out[0] = ctx.opad.h[0];
  out[1] = ctx.opad.h[1];
  out[2] = ctx.opad.h[2];
  out[3] = ctx.opad.h[3];
  out[4] = ctx.opad.h[4];
}

KERNEL_FQ KERNEL_FA void m22000_init (KERN_ATTR_TMPS_ESALT (wpa_pbkdf2_tmp_t, wpa_t))
{
  const u64 gid = get_global_id (0);

  if (gid >= GID_CNT) return;

  sha1_hmac_ctx_t base;

  sha1_hmac_init_global_swap (&base, pws[gid].i, pws[gid].pw_len);

  tmps[gid].ipad[0] = base.ipad.h[0];
  tmps[gid].ipad[1] = base.ipad.h[1];
  tmps[gid].ipad[2] = base.ipad.h[2];
  tmps[gid].ipad[3] = base.ipad.h[3];
  tmps[gid].ipad[4] = base.ipad.h[4];

  tmps[gid].opad[0] = base.opad.h[0];
  tmps[gid].opad[1] = base.opad.h[1];
  tmps[gid].opad[2] = base.opad.h[2];
  tmps[gid].opad[3] = base.opad.h[3];
  tmps[gid].opad[4] = base.opad.h[4];

  sha1_hmac_update_global_swap (&base, esalt_bufs[DIGESTS_OFFSET_HOST].essid_buf, esalt_bufs[DIGESTS_OFFSET_HOST].essid_len);

  u32 out1[5];
  u32 out2[5];

  m22000_init_block (&base, 1, out1);
  m22000_init_block (&base, 2, out2);

  for (u32 i = 0; i < 5; i++)
  {
    tmps[gid].dgst[i + 0] = out1[i];
    tmps[gid].out [i + 0] = out1[i];
    tmps[gid].dgst[i + 5] = out2[i];
    tmps[gid].out [i + 5] = out2[i];
  }
}
#else
KERNEL_FQ KERNEL_FA void m22000_init (KERN_ATTR_TMPS_ESALT (wpa_pbkdf2_tmp_t, wpa_t))
{
  /**
   * base
   */

  const u64 gid = get_global_id (0);

  if (gid >= GID_CNT) return;

  sha1_hmac_ctx_t sha1_hmac_ctx0;

  sha1_hmac_init_global_swap (&sha1_hmac_ctx0, pws[gid].i, pws[gid].pw_len);

  tmps[gid].ipad[0] = sha1_hmac_ctx0.ipad.h[0];
  tmps[gid].ipad[1] = sha1_hmac_ctx0.ipad.h[1];
  tmps[gid].ipad[2] = sha1_hmac_ctx0.ipad.h[2];
  tmps[gid].ipad[3] = sha1_hmac_ctx0.ipad.h[3];
  tmps[gid].ipad[4] = sha1_hmac_ctx0.ipad.h[4];

  tmps[gid].opad[0] = sha1_hmac_ctx0.opad.h[0];
  tmps[gid].opad[1] = sha1_hmac_ctx0.opad.h[1];
  tmps[gid].opad[2] = sha1_hmac_ctx0.opad.h[2];
  tmps[gid].opad[3] = sha1_hmac_ctx0.opad.h[3];
  tmps[gid].opad[4] = sha1_hmac_ctx0.opad.h[4];

  sha1_hmac_update_global_swap (&sha1_hmac_ctx0, esalt_bufs[DIGESTS_OFFSET_HOST].essid_buf, esalt_bufs[DIGESTS_OFFSET_HOST].essid_len);

  u32 w0[4];
  u32 w1[4];
  u32 w2[4];
  u32 w3[4];

  // w0[0] = 1

  sha1_hmac_ctx_t sha1_hmac_ctx1 = sha1_hmac_ctx0;

  w0[0] = 1;
  w0[1] = 0;
  w0[2] = 0;
  w0[3] = 0;
  w1[0] = 0;
  w1[1] = 0;
  w1[2] = 0;
  w1[3] = 0;
  w2[0] = 0;
  w2[1] = 0;
  w2[2] = 0;
  w2[3] = 0;
  w3[0] = 0;
  w3[1] = 0;
  w3[2] = 0;
  w3[3] = 0;

  sha1_hmac_update_64 (&sha1_hmac_ctx1, w0, w1, w2, w3, 4);

  sha1_hmac_final (&sha1_hmac_ctx1);

  tmps[gid].dgst[0] = sha1_hmac_ctx1.opad.h[0];
  tmps[gid].dgst[1] = sha1_hmac_ctx1.opad.h[1];
  tmps[gid].dgst[2] = sha1_hmac_ctx1.opad.h[2];
  tmps[gid].dgst[3] = sha1_hmac_ctx1.opad.h[3];
  tmps[gid].dgst[4] = sha1_hmac_ctx1.opad.h[4];

  tmps[gid].out[0] = sha1_hmac_ctx1.opad.h[0];
  tmps[gid].out[1] = sha1_hmac_ctx1.opad.h[1];
  tmps[gid].out[2] = sha1_hmac_ctx1.opad.h[2];
  tmps[gid].out[3] = sha1_hmac_ctx1.opad.h[3];
  tmps[gid].out[4] = sha1_hmac_ctx1.opad.h[4];

  // w0[0] = 2

  sha1_hmac_ctx_t sha1_hmac_ctx2 = sha1_hmac_ctx0;

  w0[0] = 2;
  w0[1] = 0;
  w0[2] = 0;
  w0[3] = 0;
  w1[0] = 0;
  w1[1] = 0;
  w1[2] = 0;
  w1[3] = 0;
  w2[0] = 0;
  w2[1] = 0;
  w2[2] = 0;
  w2[3] = 0;
  w3[0] = 0;
  w3[1] = 0;
  w3[2] = 0;
  w3[3] = 0;

  sha1_hmac_update_64 (&sha1_hmac_ctx2, w0, w1, w2, w3, 4);

  sha1_hmac_final (&sha1_hmac_ctx2);

  tmps[gid].dgst[5] = sha1_hmac_ctx2.opad.h[0];
  tmps[gid].dgst[6] = sha1_hmac_ctx2.opad.h[1];
  tmps[gid].dgst[7] = sha1_hmac_ctx2.opad.h[2];
  tmps[gid].dgst[8] = sha1_hmac_ctx2.opad.h[3];
  tmps[gid].dgst[9] = sha1_hmac_ctx2.opad.h[4];

  tmps[gid].out[5] = sha1_hmac_ctx2.opad.h[0];
  tmps[gid].out[6] = sha1_hmac_ctx2.opad.h[1];
  tmps[gid].out[7] = sha1_hmac_ctx2.opad.h[2];
  tmps[gid].out[8] = sha1_hmac_ctx2.opad.h[3];
  tmps[gid].out[9] = sha1_hmac_ctx2.opad.h[4];
}
#endif


KERNEL_FQ KERNEL_FA void m22000_loop (KERN_ATTR_TMPS_ESALT (wpa_pbkdf2_tmp_t, wpa_t))
{
  const u64 gid = get_global_id (0);

  if ((gid * VECT_SIZE) >= GID_CNT) return;

  u32x ipad[5];
  u32x opad[5];

  ipad[0] = packv (tmps, ipad, gid, 0);
  ipad[1] = packv (tmps, ipad, gid, 1);
  ipad[2] = packv (tmps, ipad, gid, 2);
  ipad[3] = packv (tmps, ipad, gid, 3);
  ipad[4] = packv (tmps, ipad, gid, 4);

  opad[0] = packv (tmps, opad, gid, 0);
  opad[1] = packv (tmps, opad, gid, 1);
  opad[2] = packv (tmps, opad, gid, 2);
  opad[3] = packv (tmps, opad, gid, 3);
  opad[4] = packv (tmps, opad, gid, 4);

  u32x dgst[5];
  u32x out[5];

  // w0[0] = 1

  dgst[0] = packv (tmps, dgst, gid, 0);
  dgst[1] = packv (tmps, dgst, gid, 1);
  dgst[2] = packv (tmps, dgst, gid, 2);
  dgst[3] = packv (tmps, dgst, gid, 3);
  dgst[4] = packv (tmps, dgst, gid, 4);

  out[0] = packv (tmps, out, gid, 0);
  out[1] = packv (tmps, out, gid, 1);
  out[2] = packv (tmps, out, gid, 2);
  out[3] = packv (tmps, out, gid, 3);
  out[4] = packv (tmps, out, gid, 4);

  for (u32 j = 0; j < LOOP_CNT; j++)
  {
    u32x w0[4];
    u32x w1[4];
    u32x w2[4];
    u32x w3[4];

    w0[0] = dgst[0];
    w0[1] = dgst[1];
    w0[2] = dgst[2];
    w0[3] = dgst[3];
    w1[0] = dgst[4];
    w1[1] = 0x80000000;
    w1[2] = 0;
    w1[3] = 0;
    w2[0] = 0;
    w2[1] = 0;
    w2[2] = 0;
    w2[3] = 0;
    w3[0] = 0;
    w3[1] = 0;
    w3[2] = 0;
    w3[3] = (64 + 20) * 8;

    hmac_sha1_run_V (w0, w1, w2, w3, ipad, opad, dgst);

    out[0] ^= dgst[0];
    out[1] ^= dgst[1];
    out[2] ^= dgst[2];
    out[3] ^= dgst[3];
    out[4] ^= dgst[4];
  }

  unpackv (tmps, dgst, gid, 0, dgst[0]);
  unpackv (tmps, dgst, gid, 1, dgst[1]);
  unpackv (tmps, dgst, gid, 2, dgst[2]);
  unpackv (tmps, dgst, gid, 3, dgst[3]);
  unpackv (tmps, dgst, gid, 4, dgst[4]);

  unpackv (tmps, out, gid, 0, out[0]);
  unpackv (tmps, out, gid, 1, out[1]);
  unpackv (tmps, out, gid, 2, out[2]);
  unpackv (tmps, out, gid, 3, out[3]);
  unpackv (tmps, out, gid, 4, out[4]);

  // w0[0] = 2

  dgst[0] = packv (tmps, dgst, gid, 5);
  dgst[1] = packv (tmps, dgst, gid, 6);
  dgst[2] = packv (tmps, dgst, gid, 7);
  dgst[3] = packv (tmps, dgst, gid, 8);
  dgst[4] = packv (tmps, dgst, gid, 9);

  out[0] = packv (tmps, out, gid, 5);
  out[1] = packv (tmps, out, gid, 6);
  out[2] = packv (tmps, out, gid, 7);
  out[3] = packv (tmps, out, gid, 8);
  out[4] = packv (tmps, out, gid, 9);

  for (u32 j = 0; j < LOOP_CNT; j++)
  {
    u32x w0[4];
    u32x w1[4];
    u32x w2[4];
    u32x w3[4];

    w0[0] = dgst[0];
    w0[1] = dgst[1];
    w0[2] = dgst[2];
    w0[3] = dgst[3];
    w1[0] = dgst[4];
    w1[1] = 0x80000000;
    w1[2] = 0;
    w1[3] = 0;
    w2[0] = 0;
    w2[1] = 0;
    w2[2] = 0;
    w2[3] = 0;
    w3[0] = 0;
    w3[1] = 0;
    w3[2] = 0;
    w3[3] = (64 + 20) * 8;

    hmac_sha1_run_V (w0, w1, w2, w3, ipad, opad, dgst);

    out[0] ^= dgst[0];
    out[1] ^= dgst[1];
    out[2] ^= dgst[2];
    out[3] ^= dgst[3];
    out[4] ^= dgst[4];
  }

  unpackv (tmps, dgst, gid, 5, dgst[0]);
  unpackv (tmps, dgst, gid, 6, dgst[1]);
  unpackv (tmps, dgst, gid, 7, dgst[2]);
  unpackv (tmps, dgst, gid, 8, dgst[3]);
  unpackv (tmps, dgst, gid, 9, dgst[4]);

  unpackv (tmps, out, gid, 5, out[0]);
  unpackv (tmps, out, gid, 6, out[1]);
  unpackv (tmps, out, gid, 7, out[2]);
  unpackv (tmps, out, gid, 8, out[3]);
  unpackv (tmps, out, gid, 9, out[4]);
}

KERNEL_FQ KERNEL_FA void m22000_comp (KERN_ATTR_TMPS_ESALT (wpa_pbkdf2_tmp_t, wpa_t))
{
  // not in use here, special case...
}

KERNEL_FQ KERNEL_FA void m22000_aux1 (KERN_ATTR_TMPS_ESALT (wpa_pbkdf2_tmp_t, wpa_t))
{
  const u64 gid = get_global_id (0);
  if (gid >= GID_CNT) return;
}


KERNEL_FQ KERNEL_FA void m22000_aux2 (KERN_ATTR_TMPS_ESALT (wpa_pbkdf2_tmp_t, wpa_t))
{
  const u64 gid = get_global_id (0);
  if (gid >= GID_CNT) return;
}


KERNEL_FQ KERNEL_FA void m22000_aux3 (KERN_ATTR_TMPS_ESALT (wpa_pbkdf2_tmp_t, wpa_t))
{
  const u64 gid = get_global_id (0);
  if (gid >= GID_CNT) return;
}


KERNEL_FQ KERNEL_FA void m22000_aux4 (KERN_ATTR_TMPS_ESALT (wpa_pbkdf2_tmp_t, wpa_t))
{
  const u64 gid = get_global_id (0);
  if (gid >= GID_CNT) return;
}
