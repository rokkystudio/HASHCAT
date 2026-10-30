#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
typedef int32_t ci; typedef uint32_t cu; typedef uint64_t cs;
typedef void* cp; typedef void* cd; typedef void* cc; typedef void* cr;
int main(int argc, char **argv) {
  void *lib=dlopen("libOpenCL.so", RTLD_NOW);
  if(!lib){ fprintf(stderr,"dlopen: %s\n",dlerror()); return 2; }
  ci (*platforms)(cu,cp*,cu*)=dlsym(lib,"clGetPlatformIDs");
  ci (*devices)(cp,uint64_t,cu,cd*,cu*)=dlsym(lib,"clGetDeviceIDs");
  cc (*context)(const intptr_t*,cu,const cd*,void*,void*,ci*)=dlsym(lib,"clCreateContext");
  cr (*program)(cc,cu,const char**,const size_t*,ci*)=dlsym(lib,"clCreateProgramWithSource");
  ci (*build)(cr,cu,const cd*,const char*,void*,void*)=dlsym(lib,"clBuildProgram");
  ci (*info)(cr,cd,cu,size_t,void*,size_t*)=dlsym(lib,"clGetProgramBuildInfo");
  ci (*release_program)(cr)=dlsym(lib,"clReleaseProgram");
  ci (*release_context)(cc)=dlsym(lib,"clReleaseContext");
  if(!platforms||!devices||!context||!program||!build||!info){ puts("missing OpenCL API"); return 3; }
  cp plat=0; cu count=0; ci err=platforms(1,&plat,&count);
  printf("platforms err=%d count=%u\n",err,count);
  if(err||!count)return 4;
  cd dev=0;err=devices(plat,4,1,&dev,&count);
  printf("devices err=%d count=%u\n",err,count);
  if(err||!count)return 5;
  cc ctx=context(NULL,1,&dev,NULL,NULL,&err);
  printf("context err=%d\n",err);
  if(!ctx||err)return 6;
  const char *src = "__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);b[i]=a[i]*17u+3u;}";
  const char *which = argc > 1 ? argv[1] : "basic";
  if (!strcmp(which,"loop")) src="__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);uint x=a[i];for(uint k=0;k<4096;k++)x=(x<<5)^(x>>3)^(k+17u);b[i]=x;}";
  if (!strcmp(which,"vector")) src="__kernel void smoke(__global const uint4 *a,__global uint4 *b){uint i=get_global_id(0);uint4 x=a[i];for(uint k=0;k<64;k++)x=rotate(x,(uint4)(3,7,11,19))^(uint4)(k);b[i]=x;}";
  if (!strcmp(which,"sha1like")) src="__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);uint v=a[i];uint w=0x67452301u;for(uint k=0;k<80;k++){uint f=(v&w)|(~v&k);uint t=rotate(v,5u)+f+w+0x5a827999u+k;w=v;v=t;}b[i]=v^w;}";
  if (!strcmp(which,"multi")) src="__kernel void init(__global uint *p){uint i=get_global_id(0);p[i]+=1u;}__kernel void loop(__global uint *p){uint i=get_global_id(0);uint a=p[i];for(uint k=0;k<512;k++)a=rotate(a,5u)^(k+3u);p[i]=a;}__kernel void comp(__global uint *p){uint i=get_global_id(0);p[i]^=0x12345678u;}__kernel void aux1(__global uint *p){uint i=get_global_id(0);p[i]+=7u;}__kernel void aux2(__global uint *p){uint i=get_global_id(0);p[i]+=11u;}__kernel void aux3(__global uint *p){uint i=get_global_id(0);p[i]+=13u;}__kernel void aux4(__global uint *p){uint i=get_global_id(0);p[i]+=17u;}";
  if (!strcmp(which,"helper")) src="uint helper(uint x){for(uint k=0;k<160;k++)x=rotate(x,5u)+((x&0x5a827999u)^(k*7u));return x;}__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);uint x=a[i];for(uint j=0;j<32;j++)x=helper(x);b[i]=x;}";
  if (!strcmp(which,"realsha1") || !strncmp(which,"sha1r",5) || !strcmp(which,"sha1cpu") || !strncmp(which,"sha1s",5)) src="#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.h\"\n#include \"inc_common.h\"\n#include \"inc_hash_sha1.cl\"\n__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);uint w0[4]={a[i],0,0,0};uint w1[4]={0,0,0,0};uint w2[4]={0,0,0,0};uint w3[4]={0,0,0,0};uint dg[5]={0x67452301u,0xefcdab89u,0x98badcfeu,0x10325476u,0xc3d2e1f0u};sha1_transform(w0,w1,w2,w3,dg);b[i]=dg[0];}";
  if (!strcmp(which,"headers")) src="#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.h\"\n#include \"inc_common.h\"\n__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);b[i]=a[i]+3u;}";
  if (!strcmp(which,"sha1unused")) src="#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.h\"\n#include \"inc_common.h\"\n#include \"inc_hash_sha1.cl\"\n__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);b[i]=a[i]+3u;}";
  if (!strcmp(which,"realsha1defs")) src="#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.h\"\n#include \"inc_common.h\"\nDECLSPEC u32 hc_rotl32_S(const u32 a,const int n){return ((a<<n)|(a>>(32-n)));}\nDECLSPEC u32 hc_add3_S(const u32 a,const u32 b,const u32 c){return a+b+c;}\n#include \"inc_hash_sha1.cl\"\n__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);uint w0[4]={a[i],0,0,0};uint w1[4]={0,0,0,0};uint w2[4]={0,0,0,0};uint w3[4]={0,0,0,0};uint dg[5]={0x67452301u,0xefcdab89u,0x98badcfeu,0x10325476u,0xc3d2e1f0u};sha1_transform(w0,w1,w2,w3,dg);b[i]=dg[0];}\n";
  if (!strcmp(which,"realsha1common")) src="#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.h\"\n#include \"inc_common.h\"\n#include \"inc_platform.cl\"\n#include \"inc_common.cl\"\n#include \"inc_hash_sha1.cl\"\n__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);uint w0[4]={a[i],0,0,0};uint w1[4]={0,0,0,0};uint w2[4]={0,0,0,0};uint w3[4]={0,0,0,0};uint dg[5]={0x67452301u,0xefcdab89u,0x98badcfeu,0x10325476u,0xc3d2e1f0u};sha1_transform(w0,w1,w2,w3,dg);b[i]=dg[0];}\n";
  if (!strcmp(which,"realallincludes")) src="#define NEW_SIMD_CODE\n#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.cl\"\n#include \"inc_common.cl\"\n#include \"inc_simd.cl\"\n#include \"inc_hash_md5.cl\"\n#include \"inc_hash_sha1.cl\"\n#include \"inc_hash_sha256.cl\"\n#include \"inc_cipher_aes.cl\"\n__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);u32 w0[4]={a[i],0,0,0};u32 w1[4]={0,0,0,0};u32 w2[4]={0,0,0,0};u32 w3[4]={0,0,0,0};u32 dg[5]={0x67452301u,0xefcdab89u,0x98badcfeu,0x10325476u,0xc3d2e1f0u};sha1_transform(w0,w1,w2,w3,dg);b[i]=dg[0];}\n";
  if (!strcmp(which,"sha512transformunused")) src="#define NEW_SIMD_CODE\n#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.cl\"\n#include \"inc_common.cl\"\n#include \"inc_hash_sha512_transform_only.cl\"\n__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);b[i]=a[i]+3u;}\n";
  if (!strcmp(which,"sha512transformreal")) src="#define NEW_SIMD_CODE\n#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.cl\"\n#include \"inc_common.cl\"\n#include \"inc_hash_sha512_transform_only.cl\"\n__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);u32 w0[4]={a[i],0,0,0};u32 w1[4]={0,0,0,0};u32 w2[4]={0,0,0,0};u32 w3[4]={0,0,0,0};u32 w4[4]={0,0,0,0};u32 w5[4]={0,0,0,0};u32 w6[4]={0,0,0,0};u32 w7[4]={0,0,0,0};u64 dg[8]={SHA512M_A,SHA512M_B,SHA512M_C,SHA512M_D,SHA512M_E,SHA512M_F,SHA512M_G,SHA512M_H};sha512_transform(w0,w1,w2,w3,w4,w5,w6,w7,dg);b[i]=(u32)(dg[0]^(dg[0]>>32));}\n";
  if (!strcmp(which,"sha512scalarunused")) src="#define NEW_SIMD_CODE\n#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.cl\"\n#include \"inc_common.cl\"\n#include \"inc_hash_sha512_scalar.cl\"\n__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);b[i]=a[i]+3u;}\n";
  if (!strcmp(which,"sha512scalarreal")) src="#define NEW_SIMD_CODE\n#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.cl\"\n#include \"inc_common.cl\"\n#include \"inc_hash_sha512_scalar.cl\"\n__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);u32 w0[4]={a[i],0,0,0};u32 w1[4]={0,0,0,0};u32 w2[4]={0,0,0,0};u32 w3[4]={0,0,0,0};u32 w4[4]={0,0,0,0};u32 w5[4]={0,0,0,0};u32 w6[4]={0,0,0,0};u32 w7[4]={0,0,0,0};u64 dg[8]={SHA512M_A,SHA512M_B,SHA512M_C,SHA512M_D,SHA512M_E,SHA512M_F,SHA512M_G,SHA512M_H};sha512_transform(w0,w1,w2,w3,w4,w5,w6,w7,dg);b[i]=(u32)(dg[0]^(dg[0]>>32));}\n";
  if (!strcmp(which,"sha512header")) src="#define NEW_SIMD_CODE\n#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.cl\"\n#include \"inc_common.cl\"\n#include \"inc_hash_sha512.h\"\n__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);b[i]=a[i]+(u32)(SHA512M_A>>32);}\n";
  if (!strcmp(which,"sha512unused")) src="#define NEW_SIMD_CODE\n#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.cl\"\n#include \"inc_common.cl\"\n#include \"inc_hash_sha512.cl\"\n__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);b[i]=a[i]+3u;}\n";
  if (!strcmp(which,"realsha512") || !strcmp(which,"realsha512_noopt")) src="#define NEW_SIMD_CODE\n#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.cl\"\n#include \"inc_common.cl\"\n#include \"inc_hash_sha512.cl\"\n__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);u32 w0[4]={a[i],0,0,0};u32 w1[4]={0,0,0,0};u32 w2[4]={0,0,0,0};u32 w3[4]={0,0,0,0};u32 w4[4]={0,0,0,0};u32 w5[4]={0,0,0,0};u32 w6[4]={0,0,0,0};u32 w7[4]={0,0,0,0};u64 dg[8]={SHA512M_A,SHA512M_B,SHA512M_C,SHA512M_D,SHA512M_E,SHA512M_F,SHA512M_G,SHA512M_H};sha512_transform(w0,w1,w2,w3,w4,w5,w6,w7,dg);b[i]=(u32)(dg[0]^(dg[0]>>32));}\n";

  if (!strcmp(which,"real22000")) src="#include \"m22000-pure.cl\"\n";
  if (!strcmp(which,"probe22000dual")) src="#include \"baseonly/m22000-pure.cl\"\n";
  if (!strcmp(which,"probe22000")) src="#include \"m22000-pure.cl\"\n";
  if (!strcmp(which,"22000cl12")) src="#include \"m22000-pure.cl\"\n";
  if (!strcmp(which,"22000cl30")) src="#include \"m22000-pure.cl\"\n";
  if (!strcmp(which,"22000bf")) src="#include \"m22000-pure.cl\"\n";
  if (!strcmp(which,"22000bf2stage")) src="#include \"m22000-pure.cl\"\n";
  if (!strcmp(which,"22000generic")) src="#include \"m22000-pure.cl\"\n";
  if (!strcmp(which,"22000exec10")) src="#include \"m22000-pure.cl\"\n";
  if (!strcmp(which,"22000cleanbf")) src="#include \"m22000-pure.cl\"\n";
  if (!strcmp(which,"realhmacinit")) src="#define NEW_SIMD_CODE\n#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.cl\"\n#include \"inc_common.cl\"\n#include \"inc_simd.cl\"\n#include \"inc_hash_sha1.cl\"\n__kernel void smoke(__global const uint *a,__global uint *b){uint gid=get_global_id(0);u32 w[16]={0};w[0]=a[gid];sha1_hmac_ctx_t ctx;sha1_hmac_init(&ctx,w,32);b[gid]=ctx.opad.h[0];}\n";
  char variant_src[128]={0};
  if (!strncmp(which,"part-",5)) {
    const char *part=which+5;
    if (strlen(part)>70 || strchr(part,'/') || strchr(part,'\\')) return 19;
    snprintf(variant_src,sizeof(variant_src),"#include \"%s.cl\"\n",part);
    src=variant_src;
  }
  if (!strcmp(which,"realhmacfull")) src="#define NEW_SIMD_CODE\n#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.cl\"\n#include \"inc_common.cl\"\n#include \"inc_simd.cl\"\n#include \"inc_hash_md5.cl\"\n#include \"inc_hash_sha1.cl\"\n#include \"inc_hash_sha256.cl\"\n#include \"inc_cipher_aes.cl\"\n__kernel void smoke(__global const uint *a,__global uint *b){\n uint i=get_global_id(0);\n u32 key[16]={0};key[0]=a[i];\n sha1_hmac_ctx_t ctx;\n sha1_hmac_init(&ctx,key,32);\n sha1_hmac_update_global_swap(&ctx,a+i*16,20);\n sha1_hmac_final(&ctx);\n b[i]=ctx.opad.h[0] ^ ctx.ipad.h[0];\n}";
  printf("variant=%s\n",which);
  if (!strcmp(which,"realsha1_noopt")) src="#include \"inc_vendor.h\"\n#include \"inc_types.h\"\n#include \"inc_platform.h\"\n#include \"inc_common.h\"\n#include \"inc_hash_sha1.cl\"\n__kernel void smoke(__global const uint *a,__global uint *b){uint i=get_global_id(0);uint w0[4]={a[i],0,0,0};uint w1[4]={0,0,0,0};uint w2[4]={0,0,0,0};uint w3[4]={0,0,0,0};uint dg[5]={0x67452301u,0xefcdab89u,0x98badcfeu,0x10325476u,0xc3d2e1f0u};sha1_transform(w0,w1,w2,w3,dg);b[i]=dg[0];}";
  cr pr=program(ctx,1,&src,NULL,&err);
  printf("program err=%d\n",err);
  if(!pr||err)return 7;
  const char *opts=(!strcmp(which,"realsha1")||!strcmp(which,"headers")||!strcmp(which,"sha1unused")||!strcmp(which,"realsha1defs")||!strcmp(which,"realsha1common"))?"-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D VENDOR_ID=1 -D DGST_ELEM=4":"";
  // Compare the advertised platform version with OpenCL C build support.
  if (!strcmp(which,"basic_cl30")) opts="-cl-std=CL3.0";
  if (!strcmp(which,"basic_cl12")) opts="-cl-std=CL1.2";
  char probeopts[1024];
  if (!strncmp(which,"sha1r",5) || !strcmp(which,"sha1cpu") || !strncmp(which,"sha1s",5)) {
    int rounds=(!strncmp(which,"sha1s",5))?1:(!strncmp(which,"sha1r",5))?atoi(which+5):80;
    int stepmode=(!strncmp(which,"sha1s",5))?atoi(which+5):0;
    snprintf(probeopts,sizeof(probeopts),"-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/adreno-sha1-probe -I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D VENDOR_ID=1 -D DGST_ELEM=4 -D SHA1_PROBE_ROUNDS=%d -D SHA1_PROBE_STEP_MODE=%d %s",rounds,stepmode,!strcmp(which,"sha1cpu")?"-D SHA1_PROBE_CPU_SCHEDULE":"");
    opts=probeopts;
  }
  if (!strcmp(which,"realsha1common")) opts="-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D VENDOR_ID=1 -D DGST_ELEM=4 -D DGST_R0=0 -D DGST_R1=1 -D DGST_R2=2 -D DGST_R3=3";
  if (!strcmp(which,"realallincludes") || !strcmp(which,"realhmacinit") || !strcmp(which,"realhmacfull")) opts="-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D VENDOR_ID=1 -D DGST_ELEM=4 -D DGST_R0=0 -D DGST_R1=1 -D DGST_R2=2 -D DGST_R3=3";
  if ((!strcmp(which,"probe22000") || !strcmp(which,"probe22000dual")) && argc > 2) {
    snprintf(probeopts,sizeof(probeopts),"-I %s -I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D INCLUDE_PATH=/data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D XM2S(x)=#x -D M2S(x)=XM2S(x) -D LOCAL_MEM_TYPE=1 -D VENDOR_ID=2147483648 -D CUDA_ARCH=0 -D HAS_ADD=0 -D HAS_ADDC=0 -D HAS_SUB=0 -D HAS_SUBC=0 -D HAS_VADD=0 -D HAS_VADDC=0 -D HAS_VADD_CO=0 -D HAS_VADDC_CO=0 -D HAS_VSUB=0 -D HAS_VSUBB=0 -D HAS_VSUB_CO=0 -D HAS_VSUBB_CO=0 -D HAS_VPERM=0 -D HAS_VADD3=0 -D HAS_VBFE=0 -D HAS_BFE=0 -D HAS_LOP3=0 -D HAS_MOV64=0 -D HAS_PRMT=0 -D HAS_SHFW=0 -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D DGST_R0=0 -D DGST_R1=1 -D DGST_R2=2 -D DGST_R3=3 -D DGST_ELEM=4 -D KERN_TYPE=22000 -D ATTACK_EXEC=10 -D ATTACK_KERN=3 -D ATTACK_MODE=3 -cl-std=CL3.0",argv[2]);
    opts=probeopts;
  }
  if ((!strcmp(which,"sha512transformunused") || !strcmp(which,"sha512transformreal") || !strcmp(which,"sha512scalarunused") || !strcmp(which,"sha512scalarreal") || !strcmp(which,"sha512header") || !strcmp(which,"sha512unused") || !strcmp(which,"realsha512") || !strcmp(which,"realsha512_noopt")) && argc > 2) {
    snprintf(probeopts,sizeof(probeopts),"-I %s -D KERNEL_STATIC -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D VENDOR_ID=1 -D DGST_ELEM=4 -D DGST_R0=0 -D DGST_R1=1 -D DGST_R2=2 -D DGST_R3=3 -cl-std=CL3.0 %s",argv[2],!strcmp(which,"realsha512_noopt")?"-cl-opt-disable":"");
    opts=probeopts;
  }
  if (!strcmp(which,"real22000")) opts="-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D INCLUDE_PATH=/data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D XM2S(x)=#x -D M2S(x)=XM2S(x) -D LOCAL_MEM_TYPE=1 -D VENDOR_ID=1 -D CUDA_ARCH=0 -D HAS_ADD=0 -D HAS_ADDC=0 -D HAS_SUB=0 -D HAS_SUBC=0 -D HAS_VADD=0 -D HAS_VADDC=0 -D HAS_VADD_CO=0 -D HAS_VADDC_CO=0 -D HAS_VSUB=0 -D HAS_VSUBB=0 -D HAS_VSUB_CO=0 -D HAS_VSUBB_CO=0 -D HAS_VPERM=0 -D HAS_VADD3=0 -D HAS_VBFE=0 -D HAS_BFE=0 -D HAS_LOP3=0 -D HAS_MOV64=0 -D HAS_PRMT=0 -D HAS_SHFW=0 -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D DGST_R0=0 -D DGST_R1=1 -D DGST_R2=2 -D DGST_R3=3 -D DGST_ELEM=4 -D KERN_TYPE=22000 -D ATTACK_EXEC=0 -D ATTACK_KERN=0 -D ATTACK_MODE=0";
  if (!strcmp(which,"22000exec10")) opts="-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D INCLUDE_PATH=/data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D XM2S(x)=#x -D M2S(x)=XM2S(x) -D LOCAL_MEM_TYPE=1 -D VENDOR_ID=1 -D CUDA_ARCH=0 -D HAS_ADD=0 -D HAS_ADDC=0 -D HAS_SUB=0 -D HAS_SUBC=0 -D HAS_VADD=0 -D HAS_VADDC=0 -D HAS_VADD_CO=0 -D HAS_VADDC_CO=0 -D HAS_VSUB=0 -D HAS_VSUBB=0 -D HAS_VSUB_CO=0 -D HAS_VSUBB_CO=0 -D HAS_VPERM=0 -D HAS_VADD3=0 -D HAS_VBFE=0 -D HAS_BFE=0 -D HAS_LOP3=0 -D HAS_MOV64=0 -D HAS_PRMT=0 -D HAS_SHFW=0 -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D DGST_R0=0 -D DGST_R1=1 -D DGST_R2=2 -D DGST_R3=3 -D DGST_ELEM=4 -D KERN_TYPE=22000 -D ATTACK_EXEC=10 -D ATTACK_KERN=0 -D ATTACK_MODE=0";
  if (!strcmp(which,"22000generic")) opts="-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D INCLUDE_PATH=/data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D XM2S(x)=#x -D M2S(x)=XM2S(x) -D LOCAL_MEM_TYPE=1 -D VENDOR_ID=2147483648 -D CUDA_ARCH=0 -D HAS_ADD=0 -D HAS_ADDC=0 -D HAS_SUB=0 -D HAS_SUBC=0 -D HAS_VADD=0 -D HAS_VADDC=0 -D HAS_VADD_CO=0 -D HAS_VADDC_CO=0 -D HAS_VSUB=0 -D HAS_VSUBB=0 -D HAS_VSUB_CO=0 -D HAS_VSUBB_CO=0 -D HAS_VPERM=0 -D HAS_VADD3=0 -D HAS_VBFE=0 -D HAS_BFE=0 -D HAS_LOP3=0 -D HAS_MOV64=0 -D HAS_PRMT=0 -D HAS_SHFW=0 -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D DGST_R0=0 -D DGST_R1=1 -D DGST_R2=2 -D DGST_R3=3 -D DGST_ELEM=4 -D KERN_TYPE=22000 -D ATTACK_EXEC=10 -D ATTACK_KERN=0 -D ATTACK_MODE=0";
  if (!strcmp(which,"22000cl30")) opts="-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D INCLUDE_PATH=/data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D XM2S(x)=#x -D M2S(x)=XM2S(x) -D LOCAL_MEM_TYPE=1 -D VENDOR_ID=2147483648 -D CUDA_ARCH=0 -D HAS_ADD=0 -D HAS_ADDC=0 -D HAS_SUB=0 -D HAS_SUBC=0 -D HAS_VADD=0 -D HAS_VADDC=0 -D HAS_VADD_CO=0 -D HAS_VADDC_CO=0 -D HAS_VSUB=0 -D HAS_VSUBB=0 -D HAS_VSUB_CO=0 -D HAS_VSUBB_CO=0 -D HAS_VPERM=0 -D HAS_VADD3=0 -D HAS_VBFE=0 -D HAS_BFE=0 -D HAS_LOP3=0 -D HAS_MOV64=0 -D HAS_PRMT=0 -D HAS_SHFW=0 -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D DGST_R0=0 -D DGST_R1=1 -D DGST_R2=2 -D DGST_R3=3 -D DGST_ELEM=4 -D KERN_TYPE=22000 -D ATTACK_EXEC=10 -D ATTACK_KERN=0 -D ATTACK_MODE=0 -cl-std=CL3.0";
  if (!strcmp(which,"22000cl12")) opts="-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D INCLUDE_PATH=/data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D XM2S(x)=#x -D M2S(x)=XM2S(x) -D LOCAL_MEM_TYPE=1 -D VENDOR_ID=2147483648 -D CUDA_ARCH=0 -D HAS_ADD=0 -D HAS_ADDC=0 -D HAS_SUB=0 -D HAS_SUBC=0 -D HAS_VADD=0 -D HAS_VADDC=0 -D HAS_VADD_CO=0 -D HAS_VADDC_CO=0 -D HAS_VSUB=0 -D HAS_VSUBB=0 -D HAS_VSUB_CO=0 -D HAS_VSUBB_CO=0 -D HAS_VPERM=0 -D HAS_VADD3=0 -D HAS_VBFE=0 -D HAS_BFE=0 -D HAS_LOP3=0 -D HAS_MOV64=0 -D HAS_PRMT=0 -D HAS_SHFW=0 -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D DGST_R0=0 -D DGST_R1=1 -D DGST_R2=2 -D DGST_R3=3 -D DGST_ELEM=4 -D KERN_TYPE=22000 -D ATTACK_EXEC=10 -D ATTACK_KERN=0 -D ATTACK_MODE=0 -cl-std=CL1.2";
  if (!strcmp(which,"22000bf")) opts="-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D INCLUDE_PATH=/data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D XM2S(x)=#x -D M2S(x)=XM2S(x) -D LOCAL_MEM_TYPE=1 -D VENDOR_ID=2147483648 -D CUDA_ARCH=0 -D HAS_ADD=0 -D HAS_ADDC=0 -D HAS_SUB=0 -D HAS_SUBC=0 -D HAS_VADD=0 -D HAS_VADDC=0 -D HAS_VADD_CO=0 -D HAS_VADDC_CO=0 -D HAS_VSUB=0 -D HAS_VSUBB=0 -D HAS_VSUB_CO=0 -D HAS_VSUBB_CO=0 -D HAS_VPERM=0 -D HAS_VADD3=0 -D HAS_VBFE=0 -D HAS_BFE=0 -D HAS_LOP3=0 -D HAS_MOV64=0 -D HAS_PRMT=0 -D HAS_SHFW=0 -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D DGST_R0=0 -D DGST_R1=1 -D DGST_R2=2 -D DGST_R3=3 -D DGST_ELEM=4 -D KERN_TYPE=22000 -D ATTACK_EXEC=10 -D ATTACK_KERN=3 -D ATTACK_MODE=3 -cl-std=CL3.0";
  if (!strcmp(which,"22000bf2stage")) opts="-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D INCLUDE_PATH=/data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D XM2S(x)=#x -D M2S(x)=XM2S(x) -D LOCAL_MEM_TYPE=1 -D VENDOR_ID=2147483648 -D CUDA_ARCH=0 -D HAS_ADD=0 -D HAS_ADDC=0 -D HAS_SUB=0 -D HAS_SUBC=0 -D HAS_VADD=0 -D HAS_VADDC=0 -D HAS_VADD_CO=0 -D HAS_VADDC_CO=0 -D HAS_VSUB=0 -D HAS_VSUBB=0 -D HAS_VSUB_CO=0 -D HAS_VSUBB_CO=0 -D HAS_VPERM=0 -D HAS_VADD3=0 -D HAS_VBFE=0 -D HAS_BFE=0 -D HAS_LOP3=0 -D HAS_MOV64=0 -D HAS_PRMT=0 -D HAS_SHFW=0 -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D DGST_R0=0 -D DGST_R1=1 -D DGST_R2=2 -D DGST_R3=3 -D DGST_ELEM=4 -D KERN_TYPE=22000 -D ATTACK_EXEC=10 -D ATTACK_KERN=3 -D ATTACK_MODE=3 -cl-std=CL3.0";
  if (!strcmp(which,"22000cleanbf")) opts="-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/adreno-sha1-probe -I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D INCLUDE_PATH=/data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D XM2S(x)=#x -D M2S(x)=XM2S(x) -D LOCAL_MEM_TYPE=1 -D VENDOR_ID=2147483648 -D CUDA_ARCH=0 -D HAS_ADD=0 -D HAS_ADDC=0 -D HAS_SUB=0 -D HAS_SUBC=0 -D HAS_VADD=0 -D HAS_VADDC=0 -D HAS_VADD_CO=0 -D HAS_VADDC_CO=0 -D HAS_VSUB=0 -D HAS_VSUBB=0 -D HAS_VSUB_CO=0 -D HAS_VSUBB_CO=0 -D HAS_VPERM=0 -D HAS_VADD3=0 -D HAS_VBFE=0 -D HAS_BFE=0 -D HAS_LOP3=0 -D HAS_MOV64=0 -D HAS_PRMT=0 -D HAS_SHFW=0 -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D DGST_R0=0 -D DGST_R1=1 -D DGST_R2=2 -D DGST_R3=3 -D DGST_ELEM=4 -D KERN_TYPE=22000 -D ATTACK_EXEC=10 -D ATTACK_KERN=3 -D ATTACK_MODE=3 -cl-std=CL3.0";
  if (!strcmp(which,"realhmacinit")) opts="-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D VENDOR_ID=2147483648 -D DGST_ELEM=4 -D DGST_R0=0 -D DGST_R1=1 -D DGST_R2=2 -D DGST_R3=3 -cl-std=CL3.0";
  if (!strncmp(which,"part-",5)) opts="-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/adreno-sha1-probe/variants -I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D INCLUDE_PATH=/data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D XM2S(x)=#x -D M2S(x)=XM2S(x) -D LOCAL_MEM_TYPE=1 -D VENDOR_ID=2147483648 -D CUDA_ARCH=0 -D HAS_ADD=0 -D HAS_ADDC=0 -D HAS_SUB=0 -D HAS_SUBC=0 -D HAS_VADD=0 -D HAS_VADDC=0 -D HAS_VADD_CO=0 -D HAS_VADDC_CO=0 -D HAS_VSUB=0 -D HAS_VSUBB=0 -D HAS_VSUB_CO=0 -D HAS_VSUBB_CO=0 -D HAS_VPERM=0 -D HAS_VADD3=0 -D HAS_VBFE=0 -D HAS_BFE=0 -D HAS_LOP3=0 -D HAS_MOV64=0 -D HAS_PRMT=0 -D HAS_SHFW=0 -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D DGST_R0=0 -D DGST_R1=1 -D DGST_R2=2 -D DGST_R3=3 -D DGST_ELEM=4 -D KERN_TYPE=22000 -D ATTACK_EXEC=10 -D ATTACK_KERN=3 -D ATTACK_MODE=3 -cl-std=CL3.0";
  if ((!strcmp(which,"probe22000") || !strcmp(which,"probe22000dual")) && argc > 3 && !strcmp(argv[3],"noinline")) {
    size_t used=strlen(probeopts);
    if (used + 29 >= sizeof(probeopts)) return 22;
    snprintf(probeopts+used,sizeof(probeopts)-used," -D M22000_ADRENO_NOINLINE");
  }
  if (!strcmp(which,"22000bf2stage")) {
    ci (*compile)(cr,cu,const cd*,const char*,cu,const cr*,const char**,void*,void*)=dlsym(lib,"clCompileProgram");
    cr (*link)(cc,cu,const cd*,const char*,cu,const cr*,void*,void*,ci*)=dlsym(lib,"clLinkProgram");
    if (!compile || !link) { printf("missing compile/link API\n"); err=-100; }
    else {
      err=compile(pr,1,&dev,opts,0,NULL,NULL,NULL,NULL);
      printf("compile err=%d\n",err);
      if (err == 0) {
        ci linkerr=0;
        cr linked=link(ctx,1,&dev,NULL,1,&pr,NULL,NULL,&linkerr);
        printf("link err=%d\n",linkerr);
        if (linked) { if (release_program) release_program(pr); pr=linked; }
        err=linkerr;
      }
    }
  }
  else err=build(pr,1,&dev,opts,NULL,NULL);
  printf("build err=%d\n",err);
  size_t n=0; info(pr,dev,0x1183,0,NULL,&n);
  if(n&&n<1048576){char *log=calloc(1,n+1);if(log){info(pr,dev,0x1183,n,log,NULL);printf("build log: %.2400s\n",log);free(log);}}
  if (!strcmp(which,"probe22000dual") && err == 0) {
    const char *source2="#include \"auxonly/m22000-pure.cl\"\n";
    ci e2=0;
    cr pr2=program(ctx,1,&source2,NULL,&e2);
    printf("second program err=%d\n",e2);
    if (pr2 && e2==0) {
      e2=build(pr2,1,&dev,opts,NULL,NULL);
      printf("second build err=%d\n",e2);
      if(e2==0) {
        void *(*make_kernel)(cr,const char*,ci*)=dlsym(lib,"clCreateKernel");
        ci (*drop_kernel)(void*)=dlsym(lib,"clReleaseKernel");
        const char *names[]={"m22000_init","m22000_loop","m22000_comp","m22000_aux1","m22000_aux2","m22000_aux3","m22000_aux4"};
        if (!make_kernel) e2=-100;
        else for (int i=0;i<7;i++) {
          ci ke=0;void *k=make_kernel(i<3?pr:pr2,names[i],&ke);
          printf("split create %s from %s err=%d\n",names[i],i<3?"base":"aux",ke);
          if(k && drop_kernel)drop_kernel(k);
          if(ke!=0)e2=ke;
        }
      }
      if (release_program)release_program(pr2);
    }
    if(e2!=0)err=e2;
  }
  if (err == 0 && (!strncmp(which,"22000",5) || !strcmp(which,"real22000") || !strcmp(which,"probe22000"))) {
    void *(*make_kernel)(cr,const char*,ci*)=dlsym(lib,"clCreateKernel");
    ci (*drop_kernel)(void*)=dlsym(lib,"clReleaseKernel");
    const char *names[]={"m22000_init","m22000_loop","m22000_comp","m22000_aux1","m22000_aux2","m22000_aux3","m22000_aux4"};
    if (!make_kernel) { printf("no clCreateKernel API\n"); }
    else for (int i=0;i<7;i++) {ci ke=0;void *k=make_kernel(pr,names[i],&ke);printf("create %s err=%d\n",names[i],ke);if(k && drop_kernel)drop_kernel(k);}
  }
  if(release_program)release_program(pr);
  if(release_context)release_context(ctx);
  dlclose(lib);
  return err?8:0;
}
