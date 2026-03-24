#include "MemoryManager.hpp" // あなたのクラス名に合わせてね
#include <iostream>
#include <thread>
#include <vector>
#include <iomanip>

// 100MBの静的バッファ
static uint8_t system_heap[100 * 1024 * 1024] __attribute__((aligned(16)));

void WorkerThread(int id) {
    for (int i = 0; i < 5; ++i) {
        // 30MBを確保
        size_t allocSize = 1024 * 1024 * 30;
        void* ptr = Lab::MemoryManager::Instance().Allocate(allocSize, 500); // 500ms待機

        if (ptr) {
            {
                // 出力が混ざらないようにロック（テスト用）
                static std::mutex s_logMutex;
                std::lock_guard<std::mutex> logLock(s_logMutex);
                std::cout << "[Thread " << id << "] Iteration " << i + 1 
                          << " Address: " << ptr << std::endl;
            }

            // 何か処理をしているフリ（少し待つ）
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            // 解放
            Lab::MemoryManager::Instance().Deallocate(ptr);
        } else {
            std::cerr << "[Thread " << id << "] Allocation FAILED!" << std::endl;
        }
    }
}

int main() {
    // 1. 初期化
    auto result = Lab::MemoryManager::Instance().Initialize(system_heap, sizeof(system_heap));
    if (result != Lab::MemoryManager::Result::Success) {
        std::cerr << "Initialize Failed!" << std::endl;
        return -1;
    }

    std::cout << "Starting Multi-threaded Test..." << std::endl;

    // 2. スレッドを2つ生成
    std::thread t1(WorkerThread, 1);
    std::thread t2(WorkerThread, 2);

    t1.join();
    t2.join();

    std::cout << "Test Completed." << std::endl;
    return 0;
}