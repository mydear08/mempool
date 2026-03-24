#pragma once
#include <memory_resource>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <new>
#include <cstdint>
#include <array>

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
            void* current = static_cast<uint8_t*>(m_ptr) + m_offset;
            size_t space = m_size - m_offset;

            void* aligned = std::align(alignment, bytes, current, space);
            if (!aligned) throw std::bad_alloc();

            m_offset = (reinterpret_cast<uint8_t*>(aligned) - static_cast<uint8_t*>(m_ptr)) + bytes;
            return aligned;
        }

        // Monotonic resource: deallocate is a no-op.
        // Memory is reclaimed only on Shutdown().
        void do_deallocate(void*, size_t, size_t) override {}

        bool do_is_equal(const memory_resource& other) const noexcept override {
            return &other == this;
        }
    };

    static constexpr size_t LARGE_THRESHOLD = 1024 * 1024; // 1MB
    static constexpr size_t MAX_CACHE_BLOCKS = 8;

    struct CachedBlock {
        void* ptr;
        size_t size; // total_size
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
              cache_count(0)
        {}
    };

    struct alignas(std::max_align_t) Header {
        size_t total_size;   // allocator内部サイズ
        size_t user_size;    // ユーザー要求サイズ
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
        static_assert((alignment & (alignment - 1)) == 0, "alignment must be power of two");

        const size_t headerSize = sizeof(Header);

        // オーバーフロー対策
        if (size > SIZE_MAX - headerSize - alignment)
            return nullptr;

        size_t totalSize = size + headerSize + alignment;

        std::unique_lock<std::mutex> lock(m_syncMutex);

        auto attempt = [&]() -> void* {
            // 巨大オブジェクトはキャッシュ優先
            if (size >= LARGE_THRESHOLD) {
                for (size_t i = 0; i < m_cb->cache_count; ++i) {
                    if (m_cb->large_cache[i].size >= totalSize) {
                        CachedBlock found = std::move(m_cb->large_cache[i]);

                        m_cb->large_cache[i] = m_cb->large_cache[m_cb->cache_count - 1];
                        m_cb->large_cache[m_cb->cache_count - 1] = {nullptr, 0};
                        m_cb->cache_count--;

                        uintptr_t uAddr =
                            (reinterpret_cast<uintptr_t>(found.ptr) + headerSize + alignment - 1)
                            & ~(alignment - 1);

                        Header* h = reinterpret_cast<Header*>(uAddr - headerSize);
                        h->total_size = found.size;
                        h->user_size = size;
                        h->original_ptr = found.ptr;

                        return reinterpret_cast<void*>(uAddr);
                    }
                }
            }

            try {
                void* raw = m_cb->pool_res.allocate(totalSize, alignment);

                uintptr_t uAddr =
                    (reinterpret_cast<uintptr_t>(raw) + headerSize + alignment - 1)
                    & ~(alignment - 1);

                Header* h = reinterpret_cast<Header*>(uAddr - headerSize);
                h->total_size = totalSize;
                h->user_size = size;
                h->original_ptr = raw;

                return reinterpret_cast<void*>(uAddr);
            } catch (...) {}

            return nullptr;
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

            Header* h = reinterpret_cast<Header*>(
                static_cast<uint8_t*>(ptr) - sizeof(Header));

            if (h->user_size >= LARGE_THRESHOLD) {
                if (m_cb->cache_count < MAX_CACHE_BLOCKS) {
                    m_cb->large_cache[m_cb->cache_count++] =
                        {h->original_ptr, h->total_size};
                }
            } else {
                m_cb->pool_res.deallocate(
                    h->original_ptr,
                    h->total_size,
                    alignof(std::max_align_t));
            }
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