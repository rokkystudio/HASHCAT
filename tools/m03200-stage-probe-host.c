/**
 * Capture bcrypt 3200 OpenCL state for the public hashcat self-test candidate.
 *
 * This executable loads the device OpenCL ICD, builds the standalone
 * m03200-stage-probe.cl source, and reads the P/S-box state after init,
 * one expansion round, all 32 expansion rounds, and the final six words.
 *
 * The source is compiled in the existing app OpenCL include directory.
 * Production hashcat, application modules and kernel sources are read-only.
 */

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STAGE_WORDS (18u + 18u + (4u * 256u))
#define STAGE_BYTES (STAGE_WORDS * sizeof (uint32_t))

#define DECLARE_CL(name) static __typeof__ (&name) name##_fn
#define LOAD_CL(name) \
  do \
  { \
    name##_fn = (__typeof__ (&name)) dlsym (library, #name); \
    if (name##_fn == NULL) \
    { \
      fprintf (stderr, "OpenCL API missing: %s\n", #name); \
      return 3; \
    } \
  } while (0)

DECLARE_CL (clGetPlatformIDs);
DECLARE_CL (clGetDeviceIDs);
DECLARE_CL (clCreateContext);
DECLARE_CL (clCreateCommandQueue);
DECLARE_CL (clCreateProgramWithSource);
DECLARE_CL (clBuildProgram);
DECLARE_CL (clGetProgramBuildInfo);
DECLARE_CL (clCreateKernel);
DECLARE_CL (clCreateBuffer);
DECLARE_CL (clSetKernelArg);
DECLARE_CL (clEnqueueNDRangeKernel);
DECLARE_CL (clEnqueueReadBuffer);
DECLARE_CL (clFinish);
DECLARE_CL (clReleaseMemObject);
DECLARE_CL (clReleaseKernel);
DECLARE_CL (clReleaseProgram);
DECLARE_CL (clReleaseCommandQueue);
DECLARE_CL (clReleaseContext);

/**
 * Load one disk file into a NUL-terminated buffer for the OpenCL compiler.
 * The caller frees the returned allocation.
 */
static char *read_source (const char *path, size_t *length)
{
  FILE *file = fopen (path, "rb");

  if (file == NULL)
  {
    fprintf (stderr, "source fopen %s: %s\n", path, strerror (errno));

    return NULL;
  }

  if (fseek (file, 0, SEEK_END) != 0)
  {
    fclose (file);

    return NULL;
  }

  const long size = ftell (file);

  if (size <= 0 || size > 1024 * 1024 || fseek (file, 0, SEEK_SET) != 0)
  {
    fclose (file);

    return NULL;
  }

  char *buffer = (char *) malloc ((size_t) size + 1);

  if (buffer == NULL)
  {
    fclose (file);

    return NULL;
  }

  const size_t amount = fread (buffer, 1, (size_t) size, file);

  fclose (file);

  if (amount != (size_t) size)
  {
    free (buffer);

    return NULL;
  }

  buffer[amount] = 0;
  *length = amount;

  return buffer;
}

/**
 * Store an exact blocking GPU readback without modifying its byte order.
 */
static int store_bytes (const char *directory, const char *stage, const void *bytes, const size_t length)
{
  char path[768];

  const int used = snprintf (path, sizeof (path), "%s/m03200-%s.bin", directory, stage);

  if (used < 0 || (size_t) used >= sizeof (path))
  {
    fprintf (stderr, "output path too long for %s\n", stage);

    return 1;
  }

  FILE *file = fopen (path, "wb");

  if (file == NULL)
  {
    fprintf (stderr, "output fopen %s: %s\n", path, strerror (errno));

    return 1;
  }

  const size_t written = fwrite (bytes, 1, length, file);

  if (fclose (file) != 0 || written != length)
  {
    fprintf (stderr, "output write incomplete: %s\n", path);

    return 1;
  }

  printf ("STAGE_SAVED %s %zu\n", stage, length);
  fflush (stdout);

  return 0;
}

/**
 * Execute one single-work-item OpenCL stage and read its result bytes.
 * Returns an error if execution, synchronization or readback fails.
 */
static int dispatch (cl_command_queue queue, cl_kernel kernel, cl_mem buffer, const size_t length, void *data)
{
  const size_t global = 1;
  const size_t local = 1;

  cl_int code = clEnqueueNDRangeKernel_fn (queue, kernel, 1, NULL, &global, &local, 0, NULL, NULL);

  if (code != CL_SUCCESS)
  {
    fprintf (stderr, "clEnqueueNDRangeKernel: %d\n", code);

    return 1;
  }

  code = clFinish_fn (queue);

  if (code != CL_SUCCESS)
  {
    fprintf (stderr, "clFinish: %d\n", code);

    return 1;
  }

  code = clEnqueueReadBuffer_fn (queue, buffer, CL_TRUE, 0, length, data, 0, NULL, NULL);

  if (code != CL_SUCCESS)
  {
    fprintf (stderr, "clEnqueueReadBuffer: %d\n", code);

    return 1;
  }

  return 0;
}

/**
 * Select the app-accessible OpenCL GPU, compile the public-vector probe,
 * and archive every stage boundary using its original 32-bit storage layout.
 */
int main (int argc, char **argv)
{
  if (argc != 4 || (strcmp (argv[3], "hashcat") != 0 && strcmp (argv[3], "hashcat2") != 0))
  {
    fprintf (stderr, "usage: bcrypt-stage-probe <source.cl> <output-directory> hashcat|hashcat2\n");

    return 2;
  }

  size_t source_length = 0;
  char *source = read_source (argv[1], &source_length);

  if (source == NULL)
  {
    fprintf (stderr, "Cannot read OpenCL probe source\n");

    return 2;
  }

  void *library = dlopen ("libOpenCL.so", RTLD_NOW);

  if (library == NULL)
  {
    fprintf (stderr, "OpenCL dlopen: %s\n", dlerror ());

    return 2;
  }

  LOAD_CL (clGetPlatformIDs);
  LOAD_CL (clGetDeviceIDs);
  LOAD_CL (clCreateContext);
  LOAD_CL (clCreateCommandQueue);
  LOAD_CL (clCreateProgramWithSource);
  LOAD_CL (clBuildProgram);
  LOAD_CL (clGetProgramBuildInfo);
  LOAD_CL (clCreateKernel);
  LOAD_CL (clCreateBuffer);
  LOAD_CL (clSetKernelArg);
  LOAD_CL (clEnqueueNDRangeKernel);
  LOAD_CL (clEnqueueReadBuffer);
  LOAD_CL (clFinish);
  LOAD_CL (clReleaseMemObject);
  LOAD_CL (clReleaseKernel);
  LOAD_CL (clReleaseProgram);
  LOAD_CL (clReleaseCommandQueue);
  LOAD_CL (clReleaseContext);

  cl_platform_id platform = NULL;
  cl_device_id device = NULL;
  cl_uint count = 0;
  cl_int err = clGetPlatformIDs_fn (1, &platform, &count);

  if (err != CL_SUCCESS || count == 0)
  {
    fprintf (stderr, "clGetPlatformIDs: %d platforms=%u\n", err, count);

    return 4;
  }

  err = clGetDeviceIDs_fn (platform, CL_DEVICE_TYPE_GPU, 1, &device, &count);

  if (err != CL_SUCCESS || count == 0)
  {
    fprintf (stderr, "clGetDeviceIDs(GPU): %d devices=%u\n", err, count);

    return 5;
  }

  cl_context context = clCreateContext_fn (NULL, 1, &device, NULL, NULL, &err);

  if (err != CL_SUCCESS || context == NULL)
  {
    fprintf (stderr, "clCreateContext: %d\n", err);

    return 6;
  }

  cl_command_queue queue = clCreateCommandQueue_fn (context, device, 0, &err);

  if (err != CL_SUCCESS || queue == NULL)
  {
    fprintf (stderr, "clCreateCommandQueue: %d\n", err);

    return 7;
  }

  const char *sources[] = { source };
  const size_t sizes[] = { source_length };

  cl_program program = clCreateProgramWithSource_fn (context, 1, sources, sizes, &err);

  if (err != CL_SUCCESS || program == NULL)
  {
    fprintf (stderr, "clCreateProgramWithSource: %d\n", err);

    return 8;
  }

  const char *options =
    "-cl-std=CL3.0 "
    "-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL "
    "-D KERNEL_STATIC -D VECT_SIZE=1 -D FIXED_LOCAL_SIZE=1 "
    "-D DEVICE_TYPE=4 -D VENDOR_ID=2147483648 -D LOCAL_MEM_TYPE=1 "
    "-D ATTACK_MODE=0 -D ATTACK_EXEC=11 -D ATTACK_KERN=3 "
    "-D DGST_ELEM=4 -D DGST_R0=0 -D DGST_R1=1 -D DGST_R2=2 -D DGST_R3=3 "
    "-D HAS_ADD=0 -D HAS_ADDC=0 -D HAS_SUB=0 -D HAS_SUBC=0 "
    "-D HAS_VADD=0 -D HAS_VADDC=0 -D HAS_VADD_CO=0 "
    "-D HAS_VSUB=0 -D HAS_VSUBB=0 -D HAS_VSUB_CO=0 "
    "-D HAS_VPERM=0 -D HAS_VADD3=0 -D HAS_VBFE=0 -D HAS_BFE=0 "
    "-D HAS_LOP3=0 -D HAS_MOV64=0 -D HAS_PRMT=0 -D HAS_SHFW=0";

  printf ("PROBE_BUILD_BEGIN\n");
  fflush (stdout);

  err = clBuildProgram_fn (program, 1, &device, options, NULL, NULL);

  size_t log_size = 0;

  if (clGetProgramBuildInfo_fn (program, device, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_size) == CL_SUCCESS && log_size > 1 && log_size < 1024 * 1024)
  {
    char *log = (char *) calloc (log_size + 1, 1);

    if (log != NULL)
    {
      if (clGetProgramBuildInfo_fn (program, device, CL_PROGRAM_BUILD_LOG, log_size, log, NULL) == CL_SUCCESS && *log)
      {
        printf ("PROBE_BUILD_LOG\n%s\n", log);
      }

      free (log);
    }
  }

  if (err != CL_SUCCESS)
  {
    fprintf (stderr, "clBuildProgram: %d\n", err);

    return 9;
  }

  printf ("PROBE_BUILD_SUCCESS\n");
  fflush (stdout);

  cl_kernel init = clCreateKernel_fn (program, "bcrypt_stage_init", &err);

  if (err != CL_SUCCESS || init == NULL)
  {
    fprintf (stderr, "clCreateKernel(init): %d\n", err);

    return 10;
  }

  cl_kernel loop = clCreateKernel_fn (program, "bcrypt_stage_loop", &err);

  if (err != CL_SUCCESS || loop == NULL)
  {
    fprintf (stderr, "clCreateKernel(loop): %d\n", err);

    return 11;
  }

  cl_kernel comp = clCreateKernel_fn (program, "bcrypt_stage_comp", &err);

  if (err != CL_SUCCESS || comp == NULL)
  {
    fprintf (stderr, "clCreateKernel(comp): %d\n", err);

    return 12;
  }

  uint32_t candidate_words[18] = { 0 };
  const size_t candidate_length = strlen (argv[3]);

  memcpy (candidate_words, argv[3], candidate_length);

  const uint32_t salt_words[4] =
  {
    0x38313532u, 0x32373238u, 0x35363036u, 0x35373633u
  };

  cl_mem state = clCreateBuffer_fn (context, CL_MEM_READ_WRITE, STAGE_BYTES, NULL, &err);

  if (err != CL_SUCCESS || state == NULL)
  {
    fprintf (stderr, "clCreateBuffer(state): %d\n", err);

    return 13;
  }

  cl_mem candidate = clCreateBuffer_fn (context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof (candidate_words), candidate_words, &err);

  if (err != CL_SUCCESS || candidate == NULL)
  {
    fprintf (stderr, "clCreateBuffer(candidate): %d\n", err);

    return 14;
  }

  cl_mem salt = clCreateBuffer_fn (context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, sizeof (salt_words), (void *) salt_words, &err);

  if (err != CL_SUCCESS || salt == NULL)
  {
    fprintf (stderr, "clCreateBuffer(salt): %d\n", err);

    return 15;
  }

  cl_mem cipher = clCreateBuffer_fn (context, CL_MEM_READ_WRITE, 6 * sizeof (uint32_t), NULL, &err);

  if (err != CL_SUCCESS || cipher == NULL)
  {
    fprintf (stderr, "clCreateBuffer(comp): %d\n", err);

    return 16;
  }

  const cl_uint len = (cl_uint) candidate_length;

  err = clSetKernelArg_fn (init, 0, sizeof (state), &state);
  err |= clSetKernelArg_fn (init, 1, sizeof (candidate), &candidate);
  err |= clSetKernelArg_fn (init, 2, sizeof (len), &len);
  err |= clSetKernelArg_fn (init, 3, sizeof (salt), &salt);

  err |= clSetKernelArg_fn (loop, 0, sizeof (state), &state);
  err |= clSetKernelArg_fn (loop, 1, sizeof (salt), &salt);

  err |= clSetKernelArg_fn (comp, 0, sizeof (state), &state);
  err |= clSetKernelArg_fn (comp, 1, sizeof (cipher), &cipher);

  if (err != CL_SUCCESS)
  {
    fprintf (stderr, "clSetKernelArg: %d\n", err);

    return 17;
  }

  uint32_t words[STAGE_WORDS] = { 0 };

  printf ("PROBE_INIT_BEGIN\n");
  fflush (stdout);

  if (dispatch (queue, init, state, STAGE_BYTES, words) != 0 || store_bytes (argv[2], "init", words, STAGE_BYTES) != 0) return 18;

  const cl_uint one = 1;

  err = clSetKernelArg_fn (loop, 2, sizeof (one), &one);

  if (err != CL_SUCCESS)
  {
    fprintf (stderr, "clSetKernelArg(loop1): %d\n", err);

    return 19;
  }

  printf ("PROBE_LOOP1_BEGIN\n");
  fflush (stdout);

  if (dispatch (queue, loop, state, STAGE_BYTES, words) != 0 || store_bytes (argv[2], "loop1", words, STAGE_BYTES) != 0) return 20;

  const cl_uint remaining = 31;

  err = clSetKernelArg_fn (loop, 2, sizeof (remaining), &remaining);

  if (err != CL_SUCCESS)
  {
    fprintf (stderr, "clSetKernelArg(loop31): %d\n", err);

    return 21;
  }

  printf ("PROBE_LOOP_FINAL_BEGIN\n");
  fflush (stdout);

  if (dispatch (queue, loop, state, STAGE_BYTES, words) != 0 || store_bytes (argv[2], "loop_final", words, STAGE_BYTES) != 0) return 22;

  uint32_t result_words[6] = { 0 };

  printf ("PROBE_COMP_BEGIN\n");
  fflush (stdout);

  if (dispatch (queue, comp, cipher, sizeof (result_words), result_words) != 0 || store_bytes (argv[2], "comp", result_words, sizeof (result_words)) != 0) return 23;

  printf ("PROBE_COMPLETE candidate=%s\n", argv[3]);

  clReleaseMemObject_fn (cipher);
  clReleaseMemObject_fn (salt);
  clReleaseMemObject_fn (candidate);
  clReleaseMemObject_fn (state);
  clReleaseKernel_fn (comp);
  clReleaseKernel_fn (loop);
  clReleaseKernel_fn (init);
  clReleaseProgram_fn (program);
  clReleaseCommandQueue_fn (queue);
  clReleaseContext_fn (context);

  free (source);
  dlclose (library);

  return 0;
}
