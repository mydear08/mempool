#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <condition_variable>
#include <chrono>
#include <iostream>
#include <mutex>
#include <new>
#include <sys/resource.h>

namespace lab {

class MemoryManager {
 public:
  // -------------------------------
  // シングルトン取得
  // -------------------------------
  static MemoryManager& Instance() {
    static MemoryManager instance;
    return instance;
  }

  // コピー・ムーブ禁止
  MemoryManager(const MemoryManager&) = delete;
  MemoryManager& operator=(const MemoryManager&) = delete;
  MemoryManager(MemoryManager&&) = delete;
  MemoryManager& operator=(MemoryManager&&) = delete;

  // -------------------------------
  // プロセスメモリ上限（任意）
  // -------------------------------
  bool SetProcessLimit(size_t bytes) {
    struct rlimit rl;
    rl.rlim_cur = bytes;
    rl.rlim_max = bytes;

    if (setrlimit(RLIMIT_AS, &rl) != 0) {
      perror("setrlimit failed");
      return false;
    }
    return true;
  }

  // -------------------------------
  // 初期化（論理上限）
  // -------------------------------
  void Initialize(size_t max_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    limit_ = max_bytes;
    used_.store(0, std::memory_order_relaxed);
  }

  // -------------------------------
  // Allocate
  // -------------------------------
  void* Allocate(size_t size,
                 int timeout_ms = -1,
                 size_t alignment = alignof(std::max_align_t)) {
    if (size == 0) size = 1;

    const size_t header_size = AlignUp(sizeof(Header), alignment);
    const size_t total = header_size + size;

    std::unique_lock<std::mutex> lock(mutex_);

    auto can_allocate = [&]() {
      return used_.load(std::memory_order_relaxed) + total <= limit_;
    };

    if (!can_allocate()) {
      if (timeout_ms == 0) return nullptr;

      if (timeout_ms < 0) {
        cv_.wait(lock, can_allocate);
      } else {
        if (!cv_.wait_for(lock,
                          std::chrono::milliseconds(timeout_ms),
                          can_allocate)) {
          return nullptr;
        }
      }
    }

    void* raw = AllocateRaw(total, alignment);
    if (!raw) {
      HandleOOM(total);
      return nullptr;
    }

    auto* header = reinterpret_cast<Header*>(raw);
    header->size = total;

    used_.fetch_add(total, std::memory_order_relaxed);

    return reinterpret_cast<std::byte*>(raw) + header_size;
  }

  // -------------------------------
  // Deallocate
  // -------------------------------
  void Deallocate(void* ptr) {
    if (!ptr) return;

    const size_t alignment = alignof(std::max_align_t);
    const size_t header_size = AlignUp(sizeof(Header), alignment);

    auto* raw = reinterpret_cast<std::byte*>(ptr) - header_size;
    auto* header = reinterpret_cast<Header*>(raw);

    const size_t total = header->size;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      used_.fetch_sub(total, std::memory_order_relaxed);
    }

    std::free(raw);
    cv_.notify_one();
  }

  size_t GetUsed() const {
    return used_.load(std::memory_order_relaxed);
  }

  size_t GetLimit() const {
    return limit_;
  }

 private:
  MemoryManager() = default;

  struct Header {
    size_t size;
  };

  static size_t AlignUp(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
  }

  static void* AllocateRaw(size_t size, size_t alignment) {
    if (alignment <= alignof(std::max_align_t)) {
      return std::malloc(size);
    }

#if (__cplusplus >= 201703L)
    size_t aligned_size = AlignUp(size, alignment);
    return std::aligned_alloc(alignment, aligned_size);
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
      return nullptr;
    }
    return ptr;
#endif
  }

  static void HandleOOM(size_t size) {
    std::lock_guard<std::mutex> lock(GetLogMutex());
    std::cerr << "[OOM] Allocation failed: " << size << " bytes\n";
  }

  static std::mutex& GetLogMutex() {
    static std::mutex m;
    return m;
  }

 private:
  std::atomic<size_t> used_{0};
  size_t limit_ = 0;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
};

}  // namespace lab
