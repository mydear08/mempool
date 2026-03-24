#pragma once
#include <memory_resource>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <new>
#include <cstdint>
#include <array>
#include <cassert>
#include <atomic>

namespace Lab {

class MemoryManager {
public:
    // --- 100MBの境界線を死守する低層リソース ---
    class InternalFixedResource : public std::pmr::memory_resource {
        void* m_ptr;
        size_t m_size;
        size_t m_offset = 0;
    public:
        InternalFixedResource(void* p, size_t s) : m_ptr(p), m_size(s) {}
        void* do_allocate(size_t bytes, size_t alignment) override {
            void* ptr = static_cast<uint8_t*>(m_ptr) + m_offset;
            size_t space = m_size - m_offset;
            void* aligned = std::align(alignment, bytes, ptr, space);
            if (!aligned) throw std::bad_alloc();
            m_offset = (static_cast<uint8_t*>(aligned) - static_cast<uint8_t*>(m_ptr)) + bytes;
            return aligned;
        }
        void do_deallocate(void*, size_t, size_t) override {}
        bool do_is_equal(const memory_resource& other) const noexcept override { return &other == this; }
    };

    static constexpr size_t LARGE_THRESHOLD = 1024 * 1024; // 1MB
    static constexpr size_t MAX_CACHE_BLOCKS = 8;
    static constexpr uint32_t MAGIC_ID = 0xDEADBEEF;

    struct CachedBlock { void* ptr; size_t size; };

    struct alignas(std::max_align_t) ControlBlock {
        InternalFixedResource fixed_res;
        std::pmr::unsynchronized_pool_resource pool_res;
        std::array<CachedBlock, MAX_CACHE_BLOCKS> large_cache;
        size_t cache_count = 0;

        ControlBlock(void* dataArea, size_t dataSize)
            : fixed_res(dataArea, dataSize), pool_res(&fixed_res), large_cache{}, cache_count(0) {}
    };

    struct alignas(std::max_align_t) Header {
        uint32_t magic;
        size_t total_size;
        size_t user_size;
        void* original_ptr;
    };

    enum class Result { Success = 0, AlreadyInitialized, InvalidBuffer, AlignmentError, BufferSizeTooSmall, Timeout };

    static MemoryManager& Instance() { static MemoryManager instance; return instance; }

    Result Initialize(void* buffer, size_t size) {
        std::lock_guard<std::mutex> lock(m_syncMutex);
        if (m_initialized) return Result::AlreadyInitialized;
        if (!buffer || reinterpret_cast<uintptr_t>(buffer) % alignof(ControlBlock) != 0) return Result::AlignmentError;
        if (size <= sizeof(ControlBlock) + 1024) return Result::BufferSizeTooSmall;

        m_cb = new (buffer) ControlBlock(static_cast<uint8_t*>(buffer) + sizeof(ControlBlock), size - sizeof(ControlBlock));
        m_initialized = true;
        m_shutting_down = false;
        m_active_ops.store(0);
        m_live_allocations.store(0);
        return Result::Success;
    }

    void* Allocate(size_t size, int timeout_ms) {
        if (!m_initialized || m_shutting_down) return nullptr;
        m_active_ops.fetch_add(1, std::memory_order_acquire);

        constexpr size_t alignment = alignof(std::max_align_t);
        const size_t totalSize = size + sizeof(Header) + alignment;
        if (size > SIZE_MAX - sizeof(Header) - alignment) { end_op(); return nullptr; }

        std::unique_lock<std::mutex> lock(m_syncMutex);
        auto attempt = [&]() -> void* {
            if (m_shutting_down) return nullptr;
            // Best-fit Large Cache
            if (size >= LARGE_THRESHOLD) {
                size_t best_idx = SIZE_MAX; size_t best_size = SIZE_MAX;
                for (size_t i = 0; i < m_cb->cache_count; ++i) {
                    if (m_cb->large_cache[i].size >= totalSize && m_cb->large_cache[i].size < best_size) {
                        best_size = m_cb->large_cache[i].size; best_idx = i;
                    }
                }
                if (best_idx != SIZE_MAX) {
                    CachedBlock blk = m_cb->large_cache[best_idx];
                    m_cb->large_cache[best_idx] = m_cb->large_cache[--m_cb->cache_count];
                    void* user_ptr = static_cast<uint8_t*>(blk.ptr) + sizeof(Header);
                    size_t space = blk.size - sizeof(Header);
                    void* aligned = std::align(alignment, size, user_ptr, space);
                    if (!aligned) return nullptr;
                    Header* h = reinterpret_cast<Header*>(static_cast<uint8_t*>(aligned) - sizeof(Header));
                    h->magic = MAGIC_ID; h->total_size = blk.size; h->user_size = size; h->original_ptr = blk.ptr;
                    m_live_allocations.fetch_add(1, std::memory_order_relaxed);
                    return aligned;
                }
            }
            // Pool Allocate
            try {
                void* raw = m_cb->pool_res.allocate(totalSize, alignment);
                void* user_ptr = static_cast<uint8_t*>(raw) + sizeof(Header);
                size_t space = totalSize - sizeof(Header);
                void* aligned = std::align(alignment, size, user_ptr, space);
                if (!aligned) throw std::bad_alloc();
                Header* h = reinterpret_cast<Header*>(static_cast<uint8_t*>(aligned) - sizeof(Header));
                h->magic = MAGIC_ID; h->total_size = totalSize; h->user_size = size; h->original_ptr = raw;
                m_live_allocations.fetch_add(1, std::memory_order_relaxed);
                return aligned;
            } catch (...) { return nullptr; }
        };

        void* ptr = attempt();
        if (!ptr && timeout_ms != 0) {
            auto cond = [&] { return (ptr = attempt()) != nullptr || m_shutting_down; };
            if (timeout_ms < 0) m_cv.wait(lock, cond);
            else m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), cond);
        }
        lock.unlock();
        end_op();
        return ptr;
    }

    void Deallocate(void* ptr) {
        if (!ptr || !m_initialized) return;
        m_active_ops.fetch_add(1, std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(m_syncMutex);
            Header* h = reinterpret_cast<Header*>(static_cast<uint8_t*>(ptr) - sizeof(Header));
            assert(h->magic == MAGIC_ID);
            if (h->user_size >= LARGE_THRESHOLD && m_cb->cache_count < MAX_CACHE_BLOCKS) {
                m_cb->large_cache[m_cb->cache_count++] = {h->original_ptr, h->total_size};
            } else {
                m_cb->pool_res.deallocate(h->original_ptr, h->total_size, alignof(std::max_align_t));
            }
            m_live_allocations.fetch_sub(1, std::memory_order_relaxed);
        }
        end_op();
        m_cv.notify_all();
    }

    Result Shutdown(int timeout_ms = 3000) {
        std::unique_lock<std::mutex> lock(m_syncMutex);
        if (!m_initialized) return Result::Success;
        m_shutting_down = true;
        m_cv.notify_all();

        auto cond = [&] { return m_active_ops.load(std::memory_order_acquire) == 0 && 
                                 m_live_allocations.load(std::memory_order_acquire) == 0; };

        bool success = (timeout_ms < 0) ? (m_shutdown_cv.wait(lock, cond), true) : 
                                          m_shutdown_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), cond);

        if (!success) return Result::Timeout; // 立ち退き拒否発生

        m_cb->~ControlBlock();
        m_cb = nullptr;
        m_initialized = false;
        return Result::Success;
    }

private:
    MemoryManager() = default;
    void end_op() {
        if (m_active_ops.fetch_sub(1, std::memory_order_release) == 1 && m_shutting_down) {
            std::lock_guard<std::mutex> lock(m_syncMutex);
            m_shutdown_cv.notify_all();
        }
    }
    ControlBlock* m_cb = nullptr;
    bool m_initialized = false;
    std::mutex m_syncMutex;
    std::condition_variable m_cv, m_shutdown_cv;
    std::atomic<size_t> m_active_ops{0}, m_live_allocations{0};
    bool m_shutting_down = false;
};

} // namespace Lab