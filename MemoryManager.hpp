#pragma once
#include <memory_resource>
#include <mutex>
#include <condition_variable>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <cassert>

namespace Lab {

class MemoryManager {
public:
    static MemoryManager& Instance() {
        static MemoryManager instance;
        return instance;
    }

    // upstreamは通常のヒープを使用
    MemoryManager() : m_pool(std::pmr::new_delete_resource()) {}

    void Initialize(size_t capacity) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_capacity = capacity;
        m_used = 0;
    }

    void* Allocate(size_t size, int timeout_ms = -1) {
        constexpr size_t alignment = alignof(std::max_align_t);

        // align分の余裕も含める（重要）
        const size_t total = size + sizeof(Header) + alignment;

        std::unique_lock<std::mutex> lock(m_mutex);

        auto can_alloc = [&] {
            return m_used + total <= m_capacity;
        };

        if (!can_alloc()) {
            if (timeout_ms == 0) return nullptr;

            if (timeout_ms < 0) {
                m_cv.wait(lock, can_alloc);
            } else {
                if (!m_cv.wait_for(
                        lock,
                        std::chrono::milliseconds(timeout_ms),
                        can_alloc)) {
                    return nullptr;
                }
            }
        }

        void* raw = nullptr;
        try {
            raw = m_pool.allocate(total, alignment);
        } catch (const std::bad_alloc&) {
            return nullptr;
        }

        if (!raw) return nullptr;

        uint8_t* base = static_cast<uint8_t*>(raw);

        // Headerは先頭に固定
        Header* h = reinterpret_cast<Header*>(base);

        void* user_ptr = base + sizeof(Header);
        size_t space = total - sizeof(Header);

        void* aligned = std::align(alignment, size, user_ptr, space);
        if (!aligned) {
            // ありえないが念のため
            m_pool.deallocate(raw, total, alignment);
            return nullptr;
        }

        h->size = total;
        h->magic = MAGIC_NUMBER;

        m_used += total;

        return aligned;
    }

    void Deallocate(void* ptr) {
        if (!ptr) return;

        uint8_t* p = static_cast<uint8_t*>(ptr);

        // Headerは「直前」ではない可能性があるので探索
        Header* h = nullptr;

        // 最大 alignment 分だけ戻れば必ず見つかる
        for (size_t offset = sizeof(Header);
             offset <= sizeof(Header) + alignof(std::max_align_t);
             ++offset) {

            Header* candidate =
                reinterpret_cast<Header*>(p - offset);

            if (candidate->magic == MAGIC_NUMBER) {
                h = candidate;
                break;
            }
        }

        if (!h) {
            assert(false && "Invalid pointer passed to Deallocate");
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        // 二重解放防止
        if (h->magic != MAGIC_NUMBER) return;
        h->magic = 0;

        const size_t size = h->size;

        m_pool.deallocate(h, size, alignof(std::max_align_t));
        m_used -= size;

        m_cv.notify_all(); // 公平性重視
    }

    size_t GetUsed() const { return m_used; }
    size_t GetCapacity() const { return m_capacity; }

private:
    struct alignas(std::max_align_t) Header {
        size_t size;
        uint32_t magic;
    };

    static constexpr uint32_t MAGIC_NUMBER = 0xDEADC0DE;

    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    std::pmr::unsynchronized_pool_resource m_pool;

    std::mutex m_mutex;
    std::condition_variable m_cv;

    size_t m_capacity = 0;
    size_t m_used = 0;
};

} // namespace Lab