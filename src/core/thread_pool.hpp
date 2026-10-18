#pragma once
// thread_pool.hpp — a minimal persistent fork-join pool for the per-creature
// phase of World::update, for targets where OpenMP is unavailable (the
// Emscripten pthreads build). Only compiled under EVO_POOL_THREADS; the native
// OpenMP and serial builds never include it.
//
// run(n, chunk, fn) hands out [0, n) in chunks from an atomic cursor — the same
// shape as OpenMP's schedule(dynamic, chunk) — and the calling thread works
// alongside the pool, returning only when every chunk is done. fn(lo, hi, tid)
// gets a stable thread index in [0, threads()) for per-thread scratch.
//
// NOT deterministic, by the same contract as the OpenMP build: which thread
// steps which creature varies run to run, and each thread draws from its own
// thread_local rng (random.hpp). The single-threaded build stays the
// reproducible reference.
//
// Emscripten note: a thread started from the browser's main thread can only
// come from the pre-spawned worker pool (-sPTHREAD_POOL_SIZE), because spinning
// up a new Worker needs the event loop that the main thread is blocking. So
// maxThreads() must not exceed PTHREAD_POOL_SIZE + 1 (the caller); the build
// sizes the pool from the same min(hardwareConcurrency, EVO_POOL_MAX_THREADS).
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#ifndef EVO_POOL_MAX_THREADS
#define EVO_POOL_MAX_THREADS 8
#endif

namespace evo_par {

class Pool {
public:
    static Pool& instance() {
        static Pool pool;
        return pool;
    }

    // Upper bound on threads(): hardware threads, capped at EVO_POOL_MAX_THREADS.
    static int maxThreads() {
        const unsigned hc = std::thread::hardware_concurrency();
        return std::max(1, std::min<int>(hc ? (int)hc : 1, EVO_POOL_MAX_THREADS));
    }

    // Total threads including the caller, clamped to [1, maxThreads()]. Workers
    // are started on demand and kept; lowering the count only parks the extras.
    // Call between run()s, never during one.
    void setThreads(int n) {
        n = std::clamp(n, 1, maxThreads());
        std::lock_guard<std::mutex> lk(m_);
        while ((int)workers_.size() < n - 1) {
            const int tid = (int)workers_.size() + 1;
            // The starting epoch is fixed HERE, under the lock, not read by the
            // new thread once it gets scheduled: a dispatch() that lands before
            // the thread first runs must still count as unseen work for it, or
            // pending_ never reaches zero.
            const std::uint64_t start = epoch_;
            workers_.emplace_back([this, tid, start] { workerLoop(tid, start); });
        }
        active_ = n;
    }
    int threads() const { return active_; }

    template <class F>
    void run(int n, int chunk, F&& fn) {
        if (n <= 0) return;
        if (chunk < 1) chunk = 1;
        if (active_ <= 1 || n <= chunk) { fn(0, n, 0); return; }
        using Fn = std::remove_reference_t<F>;
        Job job;
        job.n = n;
        job.chunk = chunk;
        job.ctx = const_cast<void*>(static_cast<const void*>(&fn));
        job.call = [](void* ctx, int lo, int hi, int tid) { (*static_cast<Fn*>(ctx))(lo, hi, tid); };
        dispatch(job);
    }

    ~Pool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        wake_.notify_all();
        for (auto& t : workers_) t.join();
    }

private:
#ifdef __EMSCRIPTEN__
    // Start serial; the page sets the count (evo_set_threads) with the same
    // formula that sized -sPTHREAD_POOL_SIZE, so the two cannot disagree.
    Pool() = default;
#else
    Pool() { setThreads(maxThreads()); }
#endif
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    struct Job {
        int n = 0, chunk = 1;
        void* ctx = nullptr;
        void (*call)(void*, int, int, int) = nullptr;
    };

    void dispatch(const Job& job) {
        {
            std::lock_guard<std::mutex> lk(m_);
            job_ = job;
            jobThreads_ = active_;
            pending_ = active_ - 1;
            cursor_.store(0, std::memory_order_relaxed);
            ++epoch_;
        }
        wake_.notify_all();
        work(job, 0);
        std::unique_lock<std::mutex> lk(m_);
        done_.wait(lk, [this] { return pending_ == 0; });
    }

    void work(const Job& job, int tid) {
        for (;;) {
            const int lo = cursor_.fetch_add(job.chunk, std::memory_order_relaxed);
            if (lo >= job.n) return;
            job.call(job.ctx, lo, std::min(lo + job.chunk, job.n), tid);
        }
    }

    void workerLoop(int tid, std::uint64_t seen) {
        for (;;) {
            std::unique_lock<std::mutex> lk(m_);
            wake_.wait(lk, [&] { return stop_ || epoch_ != seen; });
            if (stop_) return;
            seen = epoch_;
            if (tid >= jobThreads_) continue;  // parked for this round
            const Job job = job_;
            lk.unlock();
            work(job, tid);
            lk.lock();
            if (--pending_ == 0) done_.notify_one();
        }
    }

    std::mutex m_;
    std::condition_variable wake_, done_;
    std::vector<std::thread> workers_;
    Job job_;
    std::atomic<int> cursor_{0};
    std::uint64_t epoch_ = 0;
    int pending_ = 0;
    int jobThreads_ = 1;
    int active_ = 1;
    bool stop_ = false;
};

}  // namespace evo_par
