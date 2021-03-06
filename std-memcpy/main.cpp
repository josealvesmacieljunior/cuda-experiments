#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <sstream>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cstring>
#include <climits>

#include <unistd.h>
#include <omp.h>
#include <numa.h>

#include "common/common.hpp"
#include "op.hpp"

static void print_row(const Device &dstDev, const Device &srcDev, const size_t count, const size_t par)
{

  assert(srcDev.is_cpu() && dstDev.is_cpu());
  const long pageSize = sysconf(_SC_PAGESIZE);

  // create allocations
  bind_cpu(srcDev);
  double *srcPtr = static_cast<double *>(aligned_alloc(pageSize, count));
  std::memset(srcPtr, 0, count);
  bind_cpu(dstDev);
  double *dstPtr = static_cast<double *>(aligned_alloc(pageSize, count));
  std::memset(dstPtr, 0, count);

#ifdef OP_DST
  bind_cpu(dstDev);
#elif OP_SRC
  bind_cpu(srcDev);
#else
#error "woah"
#endif

  omp_set_num_threads(par);

// bind affinity for openmp threads too
#pragma omp parallel
  {
#ifdef OP_DST
    //printf("\nrd on %s\n", dstDev.name().c_str());
    bind_cpu(dstDev);
#elif OP_SRC
    bind_cpu(srcDev);
#else
#error "woah"
#endif
  }

  std::vector<double> times;
  const size_t numIters = 15;
  const size_t elemsPerCpu = (count / sizeof(*dstPtr)) / par;
  for (size_t i = 0; i < numIters; ++i)
  {

    {
      //#pragma omp barrier
      auto start = std::chrono::high_resolution_clock::now();

#pragma omp parallel for schedule(static)
      for (size_t i = 0; i < par; ++i)
      {
        std::memcpy(&dstPtr[i * elemsPerCpu], &srcPtr[i * elemsPerCpu], elemsPerCpu * sizeof(*dstPtr));
      }

      auto end = std::chrono::high_resolution_clock::now();
      dummy(dstPtr);
      dummy(srcPtr);
      std::chrono::duration<double> txSeconds = end - start;
      times.push_back(txSeconds.count());
    }
  }

  const double minTime = *std::min_element(times.begin(), times.end());
  //const double avgTime = std::accumulate(times.begin(), times.end(), 0.0) / times.size();

  printf(",%.2f", count / 1024.0 / 1024.0 / minTime);
  free(srcPtr);
  free(dstPtr);
}

int main(void)
{
  numa_set_strict(1);
  numa_exit_on_error = 1;
  auto cpus = get_cpus();

  size_t numCpusPerNode = min_cpus_per_node(cpus);
  numCpusPerNode = std::min(size_t(16), numCpusPerNode);

  // Print column headers
  printf("Transfer Size (MB),");
  for (size_t par = 1; par <= numCpusPerNode; par *= 2)
  {
    for (const auto src : cpus)
    {
      for (const auto dst : cpus)
      {
        printf("%s to %s (%lu),", src.name().c_str(), dst.name().c_str(), par);
      }
    }
  }
  printf("\n");

  long long freeMem = cpu_free_memory(cpus);
  freeMem /= 2; // two allocations
  freeMem = std::min(freeMem, 8ll * 1024ll * 1024ll * 1024ll);

  auto counts = Sequence::geometric(2048, freeMem, 2) |
                Sequence::geometric(2048 * 1.5, freeMem, 2);

  for (auto count : counts)
  {
    printf("%f", count / 1024.0 / 1024.0);
    for (size_t par = 1; par <= numCpusPerNode; par *= 2)
    {
      for (const auto src : cpus)
      {
        for (const auto dst : cpus)
        {
          print_row(dst, src, count, par);
        }
      }
    }
    printf("\n");
  }

  return 0;
}
