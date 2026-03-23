#pragma once
#include <memory_resource>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <new>

namespace Lab {

class MemoryManager {
public:
    // 管理オブジェクトのアライメントを保証する構造体
    struct alignas(std::max_align_t) ControlBlock {
        std::pmr::monotonic_buffer_resource mono_res;
        std::pmr::unsynchronized_pool_resource pool_res;
        // 依存関係があるため、デフォルト構築後に再構築するわ
        ControlBlock() : mono_res(), pool_res(&mono_res) {} 
    };

    struct alignas(std::max_align_t) Header {
        size_t size;
    };

    enum class Result { Success = 0, AlreadyInitialized, InvalidBuffer, AlignmentError, BufferSizeTooSmall };

    static MemoryManager& Instance() {
        static MemoryManager instance;
        return instance;
    }

    Result Initialize(void* buffer, size_t size) {
        std::lock_guard<std::mutex> lock(m_syncMutex);
        if (m_initialized) return Result::AlreadyInitialized;

        // アライメントチェック
        if (!buffer || reinterpret_cast<uintptr_t>(buffer) % alignof(std::max_align_t) != 0) {
            return Result::AlignmentError;
        }

        const size_t controlSize = sizeof(ControlBlock);
        if (size <= controlSize + 1024) return Result::BufferSizeTooSmall;

        uint8_t* base = static_cast<uint8_t*>(buffer);
        void* dataArea = base + controlSize;
        size_t dataSize = size - controlSize;

        // 二段階構築による確実な生存期間管理
        m_cb = new (base) ControlBlock();
        new (&m_cb->mono_res) std::pmr::monotonic_buffer_resource(dataArea, dataSize, std::pmr::null_memory_resource());
        new (&m_cb->pool_res) std::pmr::unsynchronized_pool_resource(&m_cb->mono_res);

        m_initialized = true;
        return Result::Success;
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(m_syncMutex);
        if (m_initialized && m_cb) {
            // LIFO順でのデストラクタ呼び出し
            m_cb->pool_res.~unsynchronized_pool_resource();
            m_cb->mono_res.~monotonic_buffer_resource();
            m_cb->~ControlBlock();
            m_cb = nullptr;
            m_initialized = false;
        }
    }

    void* Allocate(size_t size, int timeout_ms) {
        if (!m_initialized) return nullptr;
        size_t totalSize = size + sizeof(Header);
        std::unique_lock<std::mutex> lock(m_syncMutex);

        auto attempt = [&]() -> void* {
            try {
                void* raw = m_cb->pool_res.allocate(totalSize, alignof(Header));
                if (raw) {
                    static_cast<Header*>(raw)->size = totalSize;
                    return static_cast<char*>(raw) + sizeof(Header);
                }
            } catch (const std::bad_alloc&) {}
            return nullptr;
        };

        void* ptr = attempt();
        if (ptr || timeout_ms == 0) return ptr;

        auto cond = [&] { return (ptr = attempt()) != nullptr; };
        if (timeout_ms < 0) m_cv.wait(lock, cond);
        else m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), cond);

        return ptr;
    }

    void Deallocate(void* ptr) {
        if (!ptr || !m_initialized) return;
        {
            std::lock_guard<std::mutex> lock(m_syncMutex);
            Header* h = reinterpret_cast<Header*>(static_cast<char*>(ptr) - sizeof(Header));
            m_cb->pool_res.deallocate(h, h->size, alignof(Header));
        }
        m_cv.notify_all();
    }

private:
    MemoryManager() = default;
    ~MemoryManager() { Shutdown(); }

    ControlBlock* m_cb = nullptr;
    bool m_initialized = false;
    std::mutex m_syncMutex;
    std::condition_variable m_cv;
};

} // namespace Lab