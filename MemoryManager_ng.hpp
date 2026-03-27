#pragma once
#include <memory_resource>
#include <mutex>
#include <condition_variable>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <vector>
#include <memory>
#include <cassert>
#include <algorithm>

namespace Lab {

// ===============================
// 固定バッファ resource
// ===============================
class FixedBufferResource : public std::pmr::memory_resource {
public:
    FixedBufferResource(void* buffer, size_t size)
        : m_begin(static_cast<std::byte*>(buffer)),
          m_size(size),
          m_offset(0) {}

protected:
    void* do_allocate(size_t bytes, size_t alignment) override {
        void* ptr = m_begin + m_offset;
        size_t space = m_size - m_offset;

        void* aligned = std::align(alignment, bytes, ptr, space);
        if (!aligned) {
            throw std::bad_alloc();
        }

        m_offset = (static_cast<std::byte*>(aligned) - m_begin) + bytes;
        return aligned;
    }

    void do_deallocate(void*, size_t, size_t) override {
        // no-op（poolに任せる）
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

private:
    std::byte* m_begin;
    size_t m_size;
    size_t m_offset;
};

// ===============================
// MemoryManager
// ===============================
class MemoryManager {
public:
    static MemoryManager& Instance() {
        static MemoryManager instance;
        return instance;
    }

    void Initialize(size_t capacity) {
        std::lock_guard<std::mutex> lock(m_mutex);

        assert(m_used == 0 && "Initialize while in use");

        m_buffer.resize(capacity);
        m_capacity = capacity;
        m_used = 0;

        m_fixed = std::make_unique<FixedBufferResource>(
            m_buffer.data(), m_buffer.size()
        );

        m_pool = std::make_unique<std::pmr::unsynchronized_pool_resource>(
            m_fixed.get()
        );
    }

    void* Allocate(size_t size, int timeout_ms = -1) {
        if (!m_pool) return nullptr;

        constexpr size_t alignment = alignof(std::max_align_t);

        // ヘッダサイズを安全に丸める
        constexpr size_t header_align = alignof(Header);
        constexpr size_t header_size =
            (sizeof(Header) + header_align - 1) & ~(header_align - 1);

        // 合計サイズも丸める
        const size_t total =
            (size + header_size + alignment - 1) & ~(alignment - 1);

        std::unique_lock<std::mutex> lock(m_mutex);

        auto can_alloc = [&] { return m_used + total <= m_capacity; };

        if (!can_alloc()) {
            if (timeout_ms == 0) return nullptr;

            if (timeout_ms < 0) {
                m_cv.wait(lock, can_alloc);
            } else {
                if (!m_cv.wait_for(lock,
                        std::chrono::milliseconds(timeout_ms), can_alloc)) {
                    return nullptr;
                }
            }
        }

        void* raw = nullptr;
        try {
            raw = m_pool->allocate(total, alignment);
        } catch (const std::bad_alloc&) {
            return nullptr;
        }

        // ヘッダ配置
        auto* h = static_cast<Header*>(raw);
        h->size = total;
        h->magic = MAGIC;

        m_used += total;

        // ユーザーポインタ
        return static_cast<std::byte*>(raw) + header_size;
    }

    void Deallocate(void* ptr) {
        if (!ptr || !m_pool) return;

        constexpr size_t alignment = alignof(std::max_align_t);

        constexpr size_t header_align = alignof(Header);
        constexpr size_t header_size =
            (sizeof(Header) + header_align - 1) & ~(header_align - 1);

        auto* h = reinterpret_cast<Header*>(
            static_cast<std::byte*>(ptr) - header_size
        );

        std::lock_guard<std::mutex> lock(m_mutex);

        if (h->magic != MAGIC) {
            assert(false && "Invalid free");
            return;
        }

        h->magic = 0;

        m_pool->deallocate(h, h->size, alignment);
        m_used -= h->size;

        m_cv.notify_all();
    }

    size_t GetUsed() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_used;
    }

    size_t GetCapacity() const {
        return m_capacity;
    }

private:
    struct alignas(std::max_align_t) Header {
        size_t size;
        uint32_t magic;
        uint32_t padding; // 明示的に埋めてサイズ安定化
    };

    static_assert(sizeof(Header) % alignof(std::max_align_t) == 0,
                  "Header must be aligned");

    static constexpr uint32_t MAGIC = 0xDEADC0DE;

    std::vector<std::byte> m_buffer;
    std::unique_ptr<FixedBufferResource> m_fixed;
    std::unique_ptr<std::pmr::unsynchronized_pool_resource> m_pool;

    size_t m_capacity = 0;
    size_t m_used = 0;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;

    MemoryManager() = default;
    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;
};

} // namespace Lab