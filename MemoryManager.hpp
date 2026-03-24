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

namespace Lab {

class MemoryManager {
public:
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

            size_t used =
                (static_cast<uint8_t*>(aligned) - static_cast<uint8_t*>(m_ptr)) + bytes;

            m_offset = used;
            return aligned;
        }

        void do_deallocate(void*, size_t, size_t) override {}

        bool do_is_equal(const memory_resource& other) const noexcept override {
            return &other == this;
        }
    };

    static constexpr size_t LARGE_THRESHOLD = 1024 * 1024;
    static constexpr size_t MAX_CACHE_BLOCKS = 8;

    struct CachedBlock {
        void* ptr;
        size_t size;
    };

    struct alignas(std::max_align_t) ControlBlock {
        InternalFixedResource fixed_res;
        std::pmr::unsynchronized_pool_resource pool_res;

        std::array<CachedBlock, MAX_CACHE_BLOCKS> large_cache;
        size_t cache_count = 0;

        ControlBlock(void* dataArea, size_t dataSize)
            : fixed_res(dataArea, dataSize),
              pool_res(&fixed_res),
              large_cache{},
              cache_count(0) {}
    };

    struct alignas(std::max_align_t) Header {
        uint32_t magic;
        size_t total_size;
        size_t user_size;
        void* original_ptr;
    };

    enum class Result {
        Success = 0,
        AlreadyInitialized,
        InvalidBuffer,
        AlignmentError,
        BufferSizeTooSmall
    };

    static MemoryManager& Instance() {
        static MemoryManager instance;
        return instance;
    }

    Result Initialize(void* buffer, size_t size) {
        std::lock_guard<std::mutex> lock(m_syncMutex);

        if (m_initialized) return Result::AlreadyInitialized;
        if (!buffer) return Result::InvalidBuffer;

        if (reinterpret_cast<uintptr_t>(buffer) % alignof(ControlBlock) != 0)
            return Result::AlignmentError;

        const size_t controlSize = sizeof(ControlBlock);
        if (size <= controlSize + 1024)
            return Result::BufferSizeTooSmall;

        uint8_t* base = static_cast<uint8_t*>(buffer);
        m_cb = new (base) ControlBlock(base + controlSize, size - controlSize);

        m_initialized = true;
        return Result::Success;
    }

    void* Allocate(size_t size, int timeout_ms) {
        if (!m_initialized) return nullptr;

        constexpr size_t alignment = alignof(std::max_align_t);
        const size_t headerSize = sizeof(Header);

        if (size > SIZE_MAX - headerSize - alignment)
            return nullptr;

        size_t totalSize = size + headerSize + alignment;

        std::unique_lock<std::mutex> lock(m_syncMutex);

        auto attempt = [&]() -> void* {
            // ===== Large cache =====
            if (size >= LARGE_THRESHOLD) {
                size_t best_index = SIZE_MAX;
                size_t best_size = SIZE_MAX;

                for (size_t i = 0; i < m_cb->cache_count; ++i) {
                    size_t s = m_cb->large_cache[i].size;
                    if (s >= totalSize && s < best_size) {
                        best_size = s;
                        best_index = i;
                    }
                }

                if (best_index != SIZE_MAX) {
                    CachedBlock blk = m_cb->large_cache[best_index];

                    m_cb->large_cache[best_index] =
                        m_cb->large_cache[m_cb->cache_count - 1];
                    m_cb->cache_count--;

                    uint8_t* base = static_cast<uint8_t*>(blk.ptr);

                    Header* h = reinterpret_cast<Header*>(base);

                    void* user_ptr = base + sizeof(Header);
                    size_t space = blk.size - sizeof(Header);

                    void* aligned = std::align(alignment, size, user_ptr, space);
                    if (!aligned) return nullptr;

                    h->magic = 0xDEADBEEF;
                    h->total_size = blk.size;
                    h->user_size = size;
                    h->original_ptr = blk.ptr;

                    return aligned;
                }
            }

            // ===== Pool allocate =====
            try {
                void* raw = m_cb->pool_res.allocate(totalSize, alignment);

                uint8_t* base = static_cast<uint8_t*>(raw);
                Header* h = reinterpret_cast<Header*>(base);

                void* user_ptr = base + sizeof(Header);
                size_t space = totalSize - sizeof(Header);

                void* aligned = std::align(alignment, size, user_ptr, space);
                if (!aligned) throw std::bad_alloc();

                h->magic = 0xDEADBEEF;
                h->total_size = totalSize;
                h->user_size = size;
                h->original_ptr = raw;

                return aligned;

            } catch (const std::bad_alloc&) {
                return nullptr;
            }
        };

        void* ptr = attempt();
        if (ptr || timeout_ms == 0) return ptr;

        auto wait_cond = [&] { return (ptr = attempt()) != nullptr; };

        if (timeout_ms < 0)
            m_cv.wait(lock, wait_cond);
        else
            m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), wait_cond);

        return ptr;
    }

    void Deallocate(void* ptr) {
        if (!ptr || !m_initialized) return;

        {
            std::lock_guard<std::mutex> lock(m_syncMutex);

            uint8_t* p = static_cast<uint8_t*>(ptr);
            Header* h = reinterpret_cast<Header*>(p - sizeof(Header));

            assert(h->magic == 0xDEADBEEF);

            if (h->user_size >= LARGE_THRESHOLD) {
                if (m_cb->cache_count < MAX_CACHE_BLOCKS) {
                    m_cb->large_cache[m_cb->cache_count++] =
                        {h->original_ptr, h->total_size};
                    return;
                }
            }

            m_cb->pool_res.deallocate(
                h->original_ptr,
                h->total_size,
                alignof(std::max_align_t));
        }

        m_cv.notify_all();
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(m_syncMutex);

        if (m_initialized && m_cb) {
            m_cb->~ControlBlock();
            m_cb = nullptr;
            m_initialized = false;
        }
    }

private:
    MemoryManager() = default;

    ControlBlock* m_cb = nullptr;
    bool m_initialized = false;

    std::mutex m_syncMutex;
    std::condition_variable m_cv;
};

} // namespace Lab