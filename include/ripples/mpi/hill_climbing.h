//===------------------------------------------------------------*- C++ -*-===//
//
//             Ripples: A C++ Library for Influence Maximization
//                  Marco Minutoli <marco.minutoli@pnnl.gov>
//                   Pacific Northwest National Laboratory
//
//===----------------------------------------------------------------------===//
//
// Copyright (c) 2019, Battelle Memorial Institute
//
// Battelle Memorial Institute (hereinafter Battelle) hereby grants permission
// to any person or entity lawfully obtaining a copy of this software and
// associated documentation files (hereinafter “the Software”) to redistribute
// and use the Software in source and binary forms, with or without
// modification.  Such person or entity may use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and may permit
// others to do so, subject to the following conditions:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimers.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Other than as used herein, neither the name Battelle Memorial Institute or
//    Battelle may be used in any form whatsoever without the express written
//    consent of Battelle.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL BATTELLE OR CONTRIBUTORS BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//===----------------------------------------------------------------------===//

#ifndef RIPPLES_MPI_HILL_CLIMBING_H
#define RIPPLES_MPI_HILL_CLIMBING_H

#include "ripples/hill_climbing.h"
#include "spdlog/spdlog.h"

#include <chrono>
#include "mpi.h"

namespace ripples {
namespace mpi {
//! Engine scheduling dynamically sampling tasks for the Hill Climbing.
//!
//! \tparam GraphTy The type of the input graph.
template <typename GraphTy, typename ItrTy, typename VItrTy>
class HCWorker {
 public:
  //! Construct the Sampling worker.
  //! \param G The input Graph.
  HCWorker(const GraphTy &G) : G_(G) {}
  //! Destructor.
  virtual ~HCWorker() = default;

  virtual void build_frontier(std::atomic<size_t> &mpmc_head, ItrTy B,
                              ItrTy E) = 0;
  virtual void build_counters(std::atomic<size_t> &mpmc_head, VItrTy B,
                              VItrTy E, size_t sample_id, size_t base,
                              ItrTy eMask) = 0;

 protected:
  const GraphTy &G_;
};

template <typename GraphTy, typename ItrTy, typename VItrTy>
class HCCPUCountingWorker : public HCWorker<GraphTy, ItrTy, VItrTy> {
  using vertex_type = typename GraphTy::vertex_type;
  using HCWorker<GraphTy, ItrTy, VItrTy>::G_;

 public:
  HCCPUCountingWorker(const GraphTy &G, std::vector<long> &count,
                      std::vector<std::vector<bool>> &frontier_cache,
                      std::set<vertex_type> &S)
      : HCWorker<GraphTy, ItrTy, VItrTy>(G),
        count_(count),
        frontier_cache_(frontier_cache),
        S_(S) {}

  void build_frontier(std::atomic<size_t> &mpmc_head, ItrTy B, ItrTy E) {
    size_t offset = 0;
    while ((offset = mpmc_head.fetch_add(batch_size_)) < std::distance(B, E)) {
      auto first = B;
      std::advance(first, offset);
      auto last = first;
      std::advance(last, batch_size_);

      if (last > E) last = E;
      batch_frontier(first, last, offset);
    }
  }

  void build_counters(std::atomic<size_t> &mpmc_head, VItrTy B, VItrTy E,
                      size_t sample_id, size_t base, ItrTy eMask) {
    size_t offset = 0;
    while ((offset = mpmc_head.fetch_add(batch_size_)) < (E - B)) {
      auto first = B + offset;
      auto last = first + batch_size_;

      if (last > E) last = E;
      batch_counters(first, last, sample_id, base, eMask);
    }
  }

 private:
  void batch_frontier(ItrTy B, ItrTy E, size_t offset) {
    for (auto itr = B; itr < E; ++itr, ++offset) {
      frontier_cache_[offset].resize(G_.num_nodes(), false);
      BFS(G_, *itr, S_.begin(), S_.end(), frontier_cache_[offset]);
    }
  }
  void batch_counters(VItrTy B, VItrTy E, size_t sample_id, size_t base,
                      ItrTy eMask) {
    for (vertex_type v = B; v < E; ++v) {
      if (S_.find(v) != S_.end()) continue;
      long count = base;
      if (!frontier_cache_[sample_id][v])
        count = BFS(G_, *eMask, v, frontier_cache_[sample_id]);
      count_[v % count_.size()] += count;
    }
  }

  static constexpr size_t batch_size_ = 8;
  std::vector<long> &count_;
  std::vector<std::vector<bool>> &frontier_cache_;
  std::set<vertex_type> &S_;
};

template <typename GraphTy, typename ItrTy, typename VItrTy>
class HCGPUCountingWorker : public HCWorker<GraphTy, ItrTy, VItrTy> {
#ifdef RIPPLES_ENABLE_CUDA
  using vertex_type = typename GraphTy::vertex_type;
  using d_vertex_type = typename cuda_device_graph<GraphTy>::vertex_t;
  using bfs_solver_t = nvgraph::Bfs<int>;
  using HCWorker<GraphTy, ItrTy, VItrTy>::G_;

 public:
  struct config_t {
    config_t(size_t num_workers)
        : block_size_(bfs_solver_t::traverse_block_size()),
          max_blocks_(num_workers ? cuda_max_blocks() / num_workers : 0) {
      auto console = spdlog::get("console");
      console->trace(
          "> [GPUWalkWorkerIC::config_t] "
          "max_blocks_={}\tblock_size_={}",
          max_blocks_, block_size_);
    }

    size_t num_gpu_threads() const { return max_blocks_ * block_size_; }

    const size_t max_blocks_;
    const size_t block_size_;
  };

  HCGPUCountingWorker(const config_t &conf, const GraphTy &G,
                      cuda_ctx<GraphTy> *ctx, std::vector<long> &count,
                      std::vector<std::vector<bool>> &frontier_cache,
                      std::set<vertex_type> &S)
      : HCWorker<GraphTy, ItrTy, VItrTy>(G),
        conf_(conf),
        ctx_(ctx),
        count_(count),
        frontier_cache_(frontier_cache),
        S_(S),
        edge_filter_(new d_vertex_type[G_.num_edges()]) {
    cuda_set_device(ctx_->gpu_id);
    cuda_stream_create(&cuda_stream_);

    // allocate host/device memory
    cuda_malloc((void **)&d_edge_filter_,
                G_.num_edges() * sizeof(d_vertex_type));

    // create the solver
    solver_ = new bfs_solver_t(this->G_.num_nodes(), this->G_.num_edges(),
                               cuda_graph_index(ctx_), cuda_graph_edges(ctx_),
                               cuda_graph_weights(ctx_), true,
                               TRAVERSAL_DEFAULT_ALPHA, TRAVERSAL_DEFAULT_BETA,
                               conf_.max_blocks_, cuda_stream_);
    solver_->configure(nullptr, nullptr, d_edge_filter_);
    visited_ = std::unique_ptr<int[]>(new int[solver_->bmap_size()]);
    cuda_sync(cuda_stream_);
  }

  ~HCGPUCountingWorker() {
    cuda_set_device(ctx_->gpu_id);

    delete solver_;
    cuda_stream_destroy(cuda_stream_);

    // free host/device memory
    cuda_free(d_edge_filter_);
  }

  void build_frontier(std::atomic<size_t> &mpmc_head, ItrTy B, ItrTy E) {
    size_t offset = 0;
    while ((offset = mpmc_head.fetch_add(batch_size_)) < std::distance(B, E)) {
      auto first = B;
      std::advance(first, offset);
      auto last = first;
      std::advance(last, batch_size_);

      if (last > E) last = E;
      batch_frontier(first, last, offset);
    }
  }

  void build_counters(std::atomic<size_t> &mpmc_head, VItrTy B, VItrTy E,
                      size_t sample_id, size_t base, ItrTy eMask) {
    size_t offset = 0;
    while ((offset = mpmc_head.fetch_add(batch_size_)) < (E - B)) {
      auto first = B + offset;
      auto last = first + batch_size_;

      if (last > E) last = E;
      batch_counters(first, last, sample_id, base, eMask);
    }
  }

 private:
  void batch_frontier(ItrTy B, ItrTy E, size_t offset) {
    std::vector<d_vertex_type> seeds(S_.begin(), S_.end());
    for (auto itr = B; itr < E; ++itr, ++offset) {
      frontier_cache_[offset].resize(G_.num_nodes(), false);
      std::transform(itr->begin(), itr->end(), edge_filter_.get(),
                     [](bool v) -> d_vertex_type { return v ? 1 : 0; });

      cuda_h2d(d_edge_filter_, edge_filter_.get(),
               G_.num_edges() * sizeof(d_vertex_type), cuda_stream_);

      d_vertex_type base_count;
      solver_->traverse(seeds.data(), seeds.size(), visited_.get(),
                        &base_count);
      cuda_sync(cuda_stream_);
      for (vertex_type v = 0; v < G_.num_nodes(); ++v) {
        int m = 1 << (v % (8 * sizeof(int)));
        if ((visited_[v / (8 * sizeof(int))] && m) == 1) {
          frontier_cache_[offset][v] = true;
        }
      }
    }
  }
  void batch_counters(VItrTy B, VItrTy E, size_t sample_id, size_t base_count,
                      ItrTy eMask) {
    std::vector<d_vertex_type> seeds(S_.begin(), S_.end());
    std::transform(eMask->begin(), eMask->end(), edge_filter_.get(),
                   [](bool v) -> d_vertex_type { return v ? 1 : 0; });
    cuda_h2d(d_edge_filter_, edge_filter_.get(),
             G_.num_edges() * sizeof(d_vertex_type), cuda_stream_);

    std::memset(visited_.get(), sizeof(int) * solver_->bmap_size(), 0);
    for (vertex_type v = 0; v < G_.num_nodes(); ++v) {
      if (frontier_cache_[sample_id][v]) {
        int m = 1 << (v % (8 * sizeof(int)));
        visited_[v / (8 * sizeof(int))] |= m;
      }
    }

    for (vertex_type v = B; v < E; ++v) {
      if (S_.find(v) != S_.end()) continue;
      long update_count = base_count;
      if (!frontier_cache_[sample_id][v]) {
        d_vertex_type count;
        solver_->traverse(v, base_count, visited_.get(), &count);
        cuda_sync(cuda_stream_);

        update_count = count;
      }
      count_[v % count_.size()] += update_count;
    }
  }

  static constexpr size_t batch_size_ = 2;
  config_t conf_;
  cuda_ctx<GraphTy> *ctx_;
  cudaStream_t cuda_stream_;
  bfs_solver_t *solver_;
  std::unique_ptr<d_vertex_type[]> edge_filter_;
  std::unique_ptr<int[]> visited_;
  d_vertex_type *d_edge_filter_;

  std::vector<long> &count_;
  std::set<vertex_type> &S_;
  std::vector<std::vector<bool>> &frontier_cache_;
#endif
};

template <typename GraphTy, typename ItrTy>
class SeedSelectionEngine {
  using vertex_type = typename GraphTy::vertex_type;
  using worker_type = mpi::HCWorker<GraphTy, ItrTy, vertex_type>;
  using cpu_worker_type = mpi::HCCPUCountingWorker<GraphTy, ItrTy, vertex_type>;
  using gpu_worker_type = mpi::HCGPUCountingWorker<GraphTy, ItrTy, vertex_type>;

 public:
  SeedSelectionEngine(const GraphTy &G, size_t cpu_workers, size_t gpu_workers)
      : G_(G),
        local_count_(),
        global_count_(),
        frontier_cache_(),
        S_(),
        logger_(spdlog::stdout_color_st("SeedSelectionEngine")) {
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    vertex_block_size_ = (G.num_nodes() / world_size) + 1;
    global_count_.resize(vertex_block_size_, 0);
    local_count_.resize(vertex_block_size_, 0);

    MPI_Win_create(global_count_.data(), vertex_block_size_ * sizeof(long),
                   sizeof(long), MPI_INFO_NULL, MPI_COMM_WORLD, &win);

    size_t num_threads = cpu_workers + gpu_workers;
    // Construct workers.
    logger_->debug("Number of Threads = {}", num_threads);
    workers_.reserve(num_threads);

    for (size_t i = 0; i < cpu_workers; ++i) {
      logger_->debug("> mapping: omp {}\t->CPU", i);
      auto w = new cpu_worker_type(G_, local_count_, frontier_cache_, S_);
      workers_.push_back(w);
      cpu_workers_.push_back(w);
    }
#if RIPPLES_ENABLE_CUDA
    size_t num_devices = cuda_num_devices();
    for (size_t i = 0; i < gpu_workers; ++i) {
      size_t device_id = i % num_devices;
      logger_->debug("> mapping: omp {}\t->GPU {}/{}", i + cpu_workers,
                     device_id, num_devices);
      logger_->trace("Building Cuda Context");
      cuda_contexts_.push_back(cuda_make_ctx(G, device_id));
      logger_->trace("Cuda Context Built!");
      typename gpu_worker_type::config_t gpu_conf(gpu_workers);
      auto w = new gpu_worker_type(gpu_conf, G_, cuda_contexts_.back(),
                                   local_count_, frontier_cache_, S_);
      workers_.push_back(w);
      gpu_workers_.push_back(w);
    }
#endif
    MPI_Win_fence(0, win);
  }

  ~SeedSelectionEngine() {
    // Free workers.
    for (auto &v : workers_) delete v;
#if RIPPLES_ENABLE_CUDA
    for (auto ctx : cuda_contexts_) {
      cuda_destroy_ctx(ctx);
      delete ctx;
    }
#endif
    MPI_Win_free(&win);
  }

  std::set<vertex_type> exec(ItrTy B, ItrTy E, size_t k) {
    logger_->trace("Start Seed Selection");

    frontier_cache_.resize(std::distance(B, E));

    for (size_t i = 0; i < k; ++i) {
      mpmc_head_.store(0);
#pragma omp parallel
      {
        assert(workers_.size() == omp_get_num_threads());
        size_t rank = omp_get_thread_num();
        workers_[rank]->build_frontier(mpmc_head_, B, E);
      }
      for (int p = 0; p < world_size; ++p) {
        int current_block = (p + rank) % world_size;

        spdlog::get("console")->info("Rank {} - Block {}", rank, current_block);
        vertex_type start = current_block * vertex_block_size_;
        vertex_type end = std::min(start + vertex_block_size_, G_.num_nodes());
        for (auto itr = B; itr < E; ++itr) {
          size_t sample_id = std::distance(B, itr);
          size_t base = std::count(frontier_cache_[sample_id].begin(),
                                   frontier_cache_[sample_id].end(), true);
          mpmc_head_.store(0);
#pragma omp parallel
          {
            size_t rank = omp_get_thread_num();
            workers_[rank]->build_counters(mpmc_head_, start, end, sample_id,
                                           base, itr);
          }
        }

        MPI_Accumulate(local_count_.data(), vertex_block_size_, MPI_LONG,
                       current_block, 0, vertex_block_size_, MPI_LONG, MPI_SUM,
                       win);

#pragma omp parallel for
        for (size_t i = 0; i < local_count_.size(); ++i) {
          local_count_[i] = 0;
        }
      }

      MPI_Win_fence(0, win);

      vertex_type v = std::distance(
          global_count_.begin(),
          std::max_element(global_count_.begin(), global_count_.end()));

      struct {
        long count;
        int index;
      } local, global;
      local.count = global_count_[v];
      local.index = rank * vertex_block_size_ + v;

      spdlog::get("console")->info("R[{}] ({}, {})", rank, local.count,
                                   local.index);

#pragma omp parallel for
      for (size_t i = 0; i < global_count_.size(); ++i) {
        local_count_[i] = 0;
        global_count_[i] = 0;
      }

#pragma omp parallel for
      for (size_t i = 0; i < frontier_cache_.size(); ++i) {
        frontier_cache_[i].resize(0);
      }

      MPI_Allreduce(&local, &global, 1, MPI_LONG_INT, MPI_MAXLOC,
                    MPI_COMM_WORLD);

      S_.insert(global.index);
    }
    logger_->trace("End Seed Selection");
    return S_;
  }

 private:
  const GraphTy &G_;
  std::vector<long> local_count_;
  std::vector<long> global_count_;
  std::set<vertex_type> S_;
  // size_t gpu_workers_;
  // size_t cpu_workers_;

  std::shared_ptr<spdlog::logger> logger_;

  std::vector<cpu_worker_type *> cpu_workers_;
#if RIPPLES_ENABLE_CUDA
  std::vector<gpu_worker_type *> gpu_workers_;
  std::vector<cuda_ctx<GraphTy> *> cuda_contexts_;
#endif

  std::vector<std::vector<bool>> frontier_cache_;
  size_t vertex_block_size_;
  std::vector<worker_type *> workers_;
  std::atomic<size_t> mpmc_head_{0};
  int world_size;
  int rank;
  MPI_Win win;
};

template <typename GraphTy, typename GraphMaskItrTy, typename ConfigTy>
auto SeedSelection(GraphTy &G, GraphMaskItrTy B, GraphMaskItrTy E, ConfigTy CFG,
                   HillClimbingExecutionRecord &record) {
  using vertex_type = typename GraphTy::vertex_type;

  mpi::SeedSelectionEngine<GraphTy, GraphMaskItrTy> countingEngine(
      G, CFG.streaming_workers, CFG.streaming_gpu_workers);

  auto start = std::chrono::high_resolution_clock::now();
  auto S = countingEngine.exec(B, E, CFG.k);
  auto end = std::chrono::high_resolution_clock::now();
  record.SeedSelection = end - start;
  return S;
}  // namespace mpi

//! The HillClimbing algorithm for Influence Maximization (MPI Specialization).
//!
//! \tparam GraphTy The type of the input graph.
//! \tparam GeneratorTy The type of the parallel random number generator.
//! \tparam diff_model_tag Type-Tag to select the diffusion model.
//!
//! \param G The input graph.
//! \param k The number of seeds to select.
//! \param num_samples The number of samples to take from G.
//! \param gen The parallel random number generator.
//! \param model_tag The diffusion model tag.
//! \param ex_tag The execution model tag.
//! \returns a set of k vertices of G.
template <typename GraphTy, typename GeneratorTy, typename diff_model_tag,
          typename ConfTy>
auto HillClimbing(GraphTy &G, ConfTy &CFG, GeneratorTy &gen,
                  HillClimbingExecutionRecord &record,
                  diff_model_tag &&model_tag) {
  size_t num_threads = 1;
  int world_size;
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  gen.split(world_size, rank);

  CFG.samples /= world_size;
  auto sampled_graphs =
      SampleFrom(G, CFG, gen, record, std::forward<diff_model_tag>(model_tag));

  spdlog::get("console")->info("Done with Sampling");
  auto S = mpi::SeedSelection(G, sampled_graphs.begin(), sampled_graphs.end(),
                              CFG, record);

  return S;
}
}  // namespace mpi
}  // namespace ripples

#endif