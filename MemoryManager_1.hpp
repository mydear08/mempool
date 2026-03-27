#pragma once
#include <memory_resource>
#include <mutex>
#include <condition_variable>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <optional>
#include <algorithm>

namespace Lab {

class MemoryManager {
public:
    static MemoryManager& Instance() {
        static MemoryManager instance;
        return instance;
    }

    MemoryManager() : m_capacity(0), m_used(0) {}

    /**
     * @brief 聖域の構築
     * @param capacity 確保したい合計サイズ。これを一括チャンクとしてOSから取得します。
     */
    void Initialize(size_t capacity) {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        m_capacity = capacity;
        m_used = 0;

        // 閣下の戦略：指定サイズを一気にチャンクとして要求するオプション
        std::pmr::pool_options opts;
        opts.max_blocks_per_chunk = m_capacity; 
        opts.largest_required_pool_block = 0;   // プールの最適化アルゴリズムに委ねます

        // 既存のプールを破棄し、新しい「100MB級チャンク設定」で再生成
        // これにより、使い終わった古いメモリはOSへ返却され、新しい聖域が築かれます
        m_pool.emplace(opts, std::pmr::new_delete_resource());
    }

    void* Allocate(size_t size, int timeout_ms = -1) {
        if (!m_pool) return nullptr;

        // アライメントの定義（i.MX 8 等の 64bit 環境では通常 16バイト）
        constexpr size_t alignment = alignof(std::max_align_t);
        
        // 【修正点】ヘッダとデータの合計を、アライメント境界へ切り上げ
        // 次に Allocate されるブロックの開始位置がズレないための絶対防衛線です
        const size_t total = (size + sizeof(Header) + alignment - 1) & ~(alignment - 1);

        std::unique_lock<std::mutex> lock(m_mutex);

        auto can_alloc = [&] { return m_used + total <= m_capacity; };

        if (!can_alloc()) {
            if (timeout_ms == 0) return nullptr;
            if (timeout_ms < 0) {
                m_cv.wait(lock, can_alloc);
            } else {
                if (!m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), can_alloc)) {
                    return nullptr;
                }
            }
        }

        void* raw = nullptr;
        try {
            // 一括確保されたチャンクの中から、アラインされたサイズで切り出し
            raw = m_pool->allocate(total, alignment);
        } catch (...) {
            return nullptr;
        }

        if (!raw) return nullptr;

        Header* h = static_cast<Header*>(raw);
        h->size = total;
        h->magic = MAGIC_NUMBER;
        m_used += total;

        // ユーザーにはヘッダの直後のアドレスを渡します
        return static_cast<char*>(raw) + sizeof(Header);
    }

    void Deallocate(void* ptr) {
        if (!ptr || !m_pool) return;

        Header* h = reinterpret_cast<Header*>(static_cast<char*>(ptr) - sizeof(Header));

        std::lock_guard<std::mutex> lock(m_mutex);

        if (h->magic != MAGIC_NUMBER) return;

        h->magic = 0; // 解放済みマーク
        const size_t size = h->size;

        // プールの「懐（フリーリスト）」へ返却。
        // これで 100MB の内側で、サイズ不定でも効率よく再利用されます
        m_pool->deallocate(h, size, alignof(std::max_align_t));
        m_used -= size;

        m_cv.notify_all();
    }

    size_t GetUsed() const { return m_used; }
    size_t GetCapacity() const { return m_capacity; }

private:
    // ヘッダ自体もアライメント境界に配置されるよう強制します
    struct alignas(std::max_align_t) Header {
        size_t size;
        uint32_t magic;
    };
    static constexpr uint32_t MAGIC_NUMBER = 0xDEADC0DE;

    std::optional<std::pmr::unsynchronized_pool_resource> m_pool;
    
    size_t m_capacity;
    size_t m_used;
    std::mutex m_mutex;
    std::condition_variable m_cv;

    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;
};

} // namespace Lab