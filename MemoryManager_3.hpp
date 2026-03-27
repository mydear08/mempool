#pragma once
#include <mutex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cassert>

namespace Lab {

class MemoryManager {
public:
    static MemoryManager& Instance() {
        static MemoryManager instance;
        return instance;
    }

    // ★ 外部バッファを渡す
    void Initialize(void* buffer, size_t capacity) {
        std::lock_guard<std::mutex> lock(m_mutex);

        m_begin = static_cast<std::byte*>(buffer);
        m_capacity = capacity;

        // 先頭にフリーブロックを作る
        m_freeList = reinterpret_cast<Block*>(m_begin);
        m_freeList->size = capacity - sizeof(Block);
        m_freeList->next = nullptr;

        m_used = 0;
    }

    void* Allocate(size_t size, int timeout_ms = -1) {
        constexpr size_t alignment = alignof(std::max_align_t);

        size = (size + alignment - 1) & ~(alignment - 1);

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
            if (curr->size >= size + sizeof(Block)) {
                // 分割
                auto* next = reinterpret_cast<Block*>(
                    reinterpret_cast<std::byte*>(curr)
                    + sizeof(Block) + size
                );

                next->size = curr->size - size - sizeof(Block);
                next->next = curr->next;

                if (prev) prev->next = next;
                else m_freeList = next;

                curr->size = size;
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

        block->next = m_freeList;
        m_freeList = block;

        m_used -= block->size;

        coalesce();
        m_cv.notify_all();
    }

private:
    struct Block {
        size_t size;
        Block* next;
    };

    Block* findBlock(size_t size) {
        Block* curr = m_freeList;
        while (curr) {
            if (curr->size >= size + sizeof(Block))
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
                if (a != b) {
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