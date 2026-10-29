/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  /* Device buffers of seekdb_cuvs_search, kept between calls and grown on demand; caps are
   * in elements. mu serializes searches on this index because they share the buffers. */
  pthread_mutex_t mu;
  float*    qd; long q_cap;
  unsigned* nd; long n_cap;
  float*    dd; long d_cap;
} sk_cuvs_index;

static sk_cuvs_index* sk_index_alloc(void)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (h && pthread_mutex_init(&h->mu, NULL) != 0) { free(h); h = NULL; }
  return h;
}

static void sk_index_release(sk_cuvs_index* h)
{
  pthread_mutex_destroy(&h->mu);
  free(h);
}

/* Makes *buf hold at least want elements of elem bytes, reallocating only when it is short. */
static int sk_reserve(cuvsResources_t res, void** buf, long* cap, long want, size_t elem)
{
  if (want <= *cap) return 0;
  if (*buf) cuvsRMMFree(res, *buf, elem * *cap);
  *buf = 0; *cap = 0;
  if (cuvsRMMAlloc(res, buf, elem * want) != CUVS_SUCCESS) { *buf = 0; return 1; }
  *cap = want;
  return 0;
}

static void sk_drop_search_buffers(sk_cuvs_index* h)
{
  if (h->dd) cuvsRMMFree(h->res, h->dd, sizeof(float) * h->d_cap);
  if (h->nd) cuvsRMMFree(h->res, h->nd, sizeof(unsigned) * h->n_cap);
  if (h->qd) cuvsRMMFree(h->res, h->qd, sizeof(float) * h->q_cap);
  h->qd = 0; h->nd = 0; h->dd = 0;
  h->q_cap = h->n_cap = h->d_cap = 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = sk_index_alloc();
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { sk_index_release(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 128;
  /* NN-descent's lists are as wide as the final graph: within the iteration cap, 128-wide lists
   * get closer to converged than 256-wide ones, so the graph is cheaper and no worse. */
  h->ip->intermediate_graph_degree = 128;
  /* NN-descent iteration cap; 20 is cuVS's default. Its early stop needs fewer than
   * 0.0001 * n * dim / 100 updates among every 100th row within one iteration. */
  h->ip->nn_descent_niter = 20;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); sk_index_release(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  int rc = 0;
  cuvsCagraSearchParams_t sp = 0;
  pthread_mutex_lock(&h->mu);
  if (sk_reserve(h->res, (void**)&h->qd, &h->q_cap, nq*h->dim, sizeof(float))   != 0) { rc=10; goto done; }
  if (sk_reserve(h->res, (void**)&h->nd, &h->n_cap, nq*topk, sizeof(unsigned)) != 0) { rc=11; goto done; }
  if (sk_reserve(h->res, (void**)&h->dd, &h->d_cap, nq*topk, sizeof(float))    != 0) { rc=12; goto done; }
  if (cudaMemcpy(h->qd, query, sizeof(float)*nq*h->dim, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=h->qd; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=h->nd; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=h->dd; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) { rc=14; goto done; }
    {
      /* More effort than V5's itopk max(512, 4k) and width 4 wins back the recall that capping
       * NN-descent costs the graph, with room to spare for data harder than the proxies; multi-CTA
       * it costs little (RX 7900 XTX: 0.60-0.63 ms per search at 2048, 0.53-0.58 at 1024). */
      size_t required_itopk = (size_t)topk * 16;
      if (required_itopk < 2048) required_itopk = 2048;
      if (sp->itopk_size < required_itopk) sp->itopk_size = required_itopk;
      sp->search_width = 8;
      /* hipVS's search params start on the single-CTA kernel (algo 0), which rejects itopk above
       * 512 and, for these single-row queries on gfx1100, is about 5x slower than multi-CTA at
       * the same recall. */
      sp->algo = MULTI_CTA;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, h->nd, sizeof(unsigned)*nq*topk, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist) cudaMemcpy(out_dist, h->dd, sizeof(float)*nq*topk, cudaMemcpyDefault);
  }
done:
  if (sp) cuvsCagraSearchParamsDestroy(sp);
  /* After a failure start over with fresh buffers, as every call did before they were kept. */
  if (rc) sk_drop_search_buffers(h);
  pthread_mutex_unlock(&h->mu);
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  sk_drop_search_buffers(h);
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  sk_index_release(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = sk_index_alloc();
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  sk_index_release(index);
  return NULL;
}

long seekdb_cuvs_graph_degree(void* handle)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  int64_t degree = 0;
  if (!index || cuvsCagraIndexGetGraphDegree(index->idx, &degree) != CUVS_SUCCESS) return 0;
  return (long)degree;
}

int seekdb_cuvs_copy_graph(void* handle, unsigned int* out_graph)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !out_graph) return 1;
  DLManagedTensor graph;
  memset(&graph, 0, sizeof(graph));
  if (cuvsCagraIndexGetGraph(index->idx, &graph) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] get graph failed: %s\n", cuvsGetLastErrorText());
    return 2;
  }
  int rc = 0;
  const DLTensor* t = &graph.dl_tensor;
  if (t->ndim != 2 || t->dtype.code != kDLUInt || t->dtype.bits != 32 || t->strides ||
      t->shape[0] != index->n || t->shape[1] <= 0) {
    rc = 3;
  } else if (cuvsStreamSync(index->res) != CUVS_SUCCESS) {
    rc = 5;
  } else if (cudaMemcpy(out_graph, (const char*)t->data + t->byte_offset,
                        sizeof(unsigned int) * t->shape[0] * t->shape[1],
                        cudaMemcpyDefault) != cudaSuccess) {
    rc = 4;
  }
  if (graph.deleter) graph.deleter(&graph);
  if (rc) fprintf(stderr, "[seekdb_cuvs] copy graph rc=%d\n", rc);
  return rc;
}

void* seekdb_cuvs_load(const float* base, long n, long dim, const unsigned int* graph, long degree)
{
  if (!base || !graph || n <= 0 || dim <= 0 || degree <= 0) return NULL;
  sk_cuvs_index* index = sk_index_alloc();
  if (!index) return NULL;
  /* Host tensors: the index copies both into device memory it owns. */
  DLManagedTensor dset, gt;
  int64_t ds[2] = {n, dim}, gs[2] = {n, degree};
  memset(&dset, 0, sizeof(dset));
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds;
  gt = dset;
  gt.dl_tensor.data = (void*)graph;
  gt.dl_tensor.dtype.code = kDLUInt;
  gt.dl_tensor.shape = gs;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraIndexFromArgs(index->res, L2Expanded, &gt, &dset, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0, actual_dim = 0, actual_degree = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        cuvsCagraIndexGetGraphDegree(index->idx, &actual_degree) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim || actual_degree != degree) {
      fprintf(stderr, "[seekdb_cuvs] loaded shape mismatch: %ld x %ld deg %ld, expected %ld x %ld deg %ld\n",
              (long)actual_n, (long)actual_dim, (long)actual_degree, n, dim, degree);
      goto fail_idx;
    }
  }
  if (cuvsStreamSync(index->res) != CUVS_SUCCESS) goto fail_idx;
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] load failed: %s\n", cuvsGetLastErrorText());
  sk_index_release(index);
  return NULL;
}

int seekdb_cuvs_warmup(long dim)
{
  /* A tiny build and search with the real parameters on fixed pseudo-random data: it
   * initializes the HIP runtime and loads the CAGRA kernels for this dimension. */
  const long n = 2048;
  if (dim <= 0) return 1;
  float* base = (float*)malloc(sizeof(float) * n * dim);
  if (!base) return 2;
  unsigned int seed = 12345u;
  for (long i = 0; i < n * dim; ++i) {
    seed = seed * 1664525u + 1013904223u;
    base[i] = (float)(seed >> 8) / 16777216.0f;
  }
  int rc = 3;
  void* h = seekdb_cuvs_build(base, n, dim);
  if (h) {
    unsigned int ids[10];
    float dist[10];
    rc = seekdb_cuvs_search(h, base, 1, 10, ids, dist);
    seekdb_cuvs_free(h);
  }
  free(base);
  return rc;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
