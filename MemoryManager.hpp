#include <memory_resource>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <new>

namespace Lab {

class MemoryManager {
public:
    struct alignas(std::max_align_t) Header {
        size_t size;
    };

    static MemoryManager& Instance() {
        static MemoryManager instance;
        return instance;
    }

    enum class Result {
        Success = 0,
        AlreadyInitialized,
        InvalidBuffer,
        BufferSizeTooSmall
    };

    // 初期化：バッファ内に管理オブジェクトを直接構築
    Result Initialize(void* buffer, size_t size) {
        std::lock_guard<std::mutex> lock(m_syncMutex);
        if (m_initialized) return Result::AlreadyInitialized;

        if (!buffer || size < (sizeof(std::pmr::monotonic_buffer_resource) + 
                             sizeof(std::pmr::synchronized_pool_resource) + 1024)) {
            return Result::BufferSizeTooSmall;
        }

        uint8_t* rawBuf = static_cast<uint8_t*>(buffer);
        
        // 配置newでリソースを構築
        m_mono_res = new (&rawBuf[0]) std::pmr::monotonic_buffer_resource(
            &rawBuf[sizeof(std::pmr::monotonic_buffer_resource) + sizeof(std::pmr::synchronized_pool_resource)],
            size - (sizeof(std::pmr::monotonic_buffer_resource) + sizeof(std::pmr::synchronized_pool_resource)),
            std::pmr::null_memory_resource()
        );

        m_pool_res = new (&rawBuf[sizeof(std::pmr::monotonic_buffer_resource)]) 
            std::pmr::synchronized_pool_resource(m_mono_res);

        m_initialized = true;
        return Result::Success;
    }

    // タイムアウト付き確保
    void* Allocate(size_t size, int timeout_ms = 0) {
        if (!m_initialized) return nullptr;

        size_t totalSize = size + sizeof(Header);
        std::unique_lock<std::mutex> lock(m_syncMutex);

        auto attempt = [&]() -> void* {
            try {
                void* raw = m_pool_res->allocate(totalSize, alignof(Header));
                if (raw) {
                    Header* h = static_cast<Header*>(raw);
                    h->size = totalSize;
                    return static_cast<char*>(raw) + sizeof(Header);
                }
            } catch (const std::bad_alloc&) {}
            return nullptr;
        };

        void* ptr = attempt();
        if (ptr || timeout_ms == 0) return ptr;

        // 指定時間、または条件が満たされるまで待機
        if (timeout_ms < 0) {
            m_cv.wait(lock, [&] { return (ptr = attempt()) != nullptr; });
        } else {
            m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
                return (ptr = attempt()) != nullptr;
            });
        }
        return ptr;
    }

    void Deallocate(void* ptr) {
        if (!ptr || !m_initialized) return;

        {
            std::lock_guard<std::mutex> lock(m_syncMutex);
            Header* h = reinterpret_cast<Header*>(static_cast<char*>(ptr) - sizeof(Header));
            m_pool_res->deallocate(h, h->size, alignof(Header));
        }
        m_cv.notify_all(); // 待機中のスレッドへ通知
    }

private:
    MemoryManager() = default;
    
    bool m_initialized = false;
    std::mutex m_syncMutex;
    std::condition_variable m_cv;
    std::pmr::monotonic_buffer_resource* m_mono_res = nullptr;
    std::pmr::synchronized_pool_resource* m_pool_res = nullptr;
};

} // namespace Lab