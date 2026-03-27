#pragma once
#include <mutex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <cassert>
#include <algorithm>

namespace Lab {

class MemoryManager {
public:
    struct BlockInfo {
        void* address;
        size_t size;
        bool free;
    };

    static MemoryManager& Instance() {
        static MemoryManager instance;
        return instance;
    }

    void Initialize(void* buffer, size_t capacity) {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_begin = static_cast<std::byte*>(buffer);
        m_capacity = capacity;

        // 先頭をBlockとして初期化（アライン考慮）
        auto aligned_begin = alignPtr(m_begin, alignof(Block));
        size_t adjust = aligned_begin - m_begin;

        assert(capacity > adjust + sizeof(Block));

        m_freeList = reinterpret_cast<Block*>(aligned_begin);
        m_freeList->size = capacity - adjust - sizeof(Block);
        m_freeList->free = true;
        m_freeList->next = nullptr;

        m_used = 0;
    }

    void* Allocate(size_t size, int timeout_ms = -1) {
        constexpr size_t alignment = alignof(std::max_align_t);
        size = alignUp(size, alignment);

        std::unique_lock<std::mutex> lock(m_mutex);

        auto can_alloc = [&] { return findBlock(size) != nullptr; };

        if (!can_alloc()) {
            if (timeout_ms == 0) return nullptr;
            if (timeout_ms < 0) {
                m_cv.wait(lock, can_alloc);
            } else {
                if (!m_cv.wait_for(lock,
                        std::chrono::milliseconds(timeout_ms), can_alloc))
                    return nullptr;
            }
        }

        Block* prev = nullptr;
        Block* curr = m_freeList;

        while (curr) {
            if (curr->free && curr->size >= size) {

                size_t remaining = curr->size - size;

                if (remaining > sizeof(Block)) {
                    // 分割
                    auto* next = reinterpret_cast<Block*>(
                        reinterpret_cast<std::byte*>(curr)
                        + sizeof(Block) + size
                    );

                    next->size = remaining - sizeof(Block);
                    next->free = true;
                    next->next = curr->next;

                    if (prev) prev->next = next;
                    else m_freeList = next;
                } else {
                    // 分割しない
                    if (prev) prev->next = curr->next;
                    else m_freeList = curr->next;
                }

                curr->size = size;
                curr->free = false;
                m_used += size;

                return reinterpret_cast<std::byte*>(curr) + sizeof(Block);
            }

            prev = curr;
            curr = curr->next;
        }

        return nullptr;
    }

    void Deallocate(void* ptr) {
        if (!ptr) return;

        auto* block = reinterpret_cast<Block*>(
            static_cast<std::byte*>(ptr) - sizeof(Block)
        );

        std::lock_guard<std::mutex> lock(m_mutex);

        block->free = true;
        block->next = m_freeList;
        m_freeList = block;

        m_used -= block->size;

        coalesce();
        m_cv.notify_all();
    }

    // ============================
    // 可視化
    // ============================
    std::vector<BlockInfo> GetMemoryLayout() {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<BlockInfo> result;

        auto* ptr = m_begin;
        auto* end = m_begin + m_capacity;

        while (ptr < end) {
            auto* block = reinterpret_cast<Block*>(ptr);

            result.push_back({
                ptr,
                block->size,
                block->free
            });

            ptr += sizeof(Block) + block->size;
        }

        return result;
    }

    size_t GetLargestFreeBlock() {
        std::lock_guard<std::mutex> lock(m_mutex);

        size_t max_block = 0;
        Block* curr = m_freeList;

        while (curr) {
            if (curr->free)
                max_block = std::max(max_block, curr->size);
            curr = curr->next;
        }

        return max_block;
    }

    double GetFragmentationRatio() {
        std::lock_guard<std::mutex> lock(m_mutex);

        size_t total_free = 0;
        size_t largest = 0;

        Block* curr = m_freeList;
        while (curr) {
            if (curr->free) {
                total_free += curr->size;
                largest = std::max(largest, curr->size);
            }
            curr = curr->next;
        }

        if (total_free == 0) return 0.0;
        return 1.0 - (double)largest / total_free;
    }

    std::string GetHeatmap(size_t width = 80) {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::string result(width, '.');

        auto* ptr = m_begin;
        auto* end = m_begin + m_capacity;

        while (ptr < end) {
            auto* block = reinterpret_cast<Block*>(ptr);

            size_t start = (ptr - m_begin) * width / m_capacity;
            size_t block_end = (ptr - m_begin + sizeof(Block) + block->size) * width / m_capacity;

            char c;
            if (block->free) {
                if (block->size > 1024*1024) c = 'L';   // Large
                else if (block->size > 1024) c = 'M';   // Medium
                else c = 's';                           // small
            } else {
                c = '#';
            }

            for (size_t i = start; i < block_end && i < width; ++i) {
                result[i] = c;
            }

            ptr += sizeof(Block) + block->size;
        }

        return result;
    }

private:
    struct Block {
        size_t size;
        bool free;
        Block* next;
    };

    // ============================
    // Utility
    // ============================
    static size_t alignUp(size_t v, size_t a) {
        return (v + a - 1) & ~(a - 1);
    }

    static std::byte* alignPtr(std::byte* ptr, size_t alignment) {
        uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        p = (p + alignment - 1) & ~(alignment - 1);
        return reinterpret_cast<std::byte*>(p);
    }

    Block* findBlock(size_t size) {
        Block* curr = m_freeList;
        while (curr) {
            if (curr->free && curr->size >= size)
                return curr;
            curr = curr->next;
        }
        return nullptr;
    }

    void coalesce() {
        Block* a = m_freeList;
        while (a) {
            Block* b = m_freeList;
            while (b) {
                if (a != b && a->free && b->free) {
                    auto* a_end = reinterpret_cast<std::byte*>(a)
                        + sizeof(Block) + a->size;

                    if (a_end == reinterpret_cast<std::byte*>(b)) {
                        a->size += sizeof(Block) + b->size;
                        removeBlock(b);
                        b = m_freeList;
                        continue;
                    }
                }
                b = b->next;
            }
            a = a->next;
        }
    }

    void removeBlock(Block* target) {
        Block* prev = nullptr;
        Block* curr = m_freeList;
        while (curr) {
            if (curr == target) {
                if (prev) prev->next = curr->next;
                else m_freeList = curr->next;
                return;
            }
            prev = curr;
            curr = curr->next;
        }
    }

    std::byte* m_begin = nullptr;
    Block* m_freeList = nullptr;

    size_t m_capacity = 0;
    size_t m_used = 0;

    std::mutex m_mutex;
    std::condition_variable m_cv;
};

} // namespace Lab