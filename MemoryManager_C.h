#ifndef MEMORY_MANAGER_C_H
#define MEMORY_MANAGER_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief メモリマネージャの初期化
 * @param buffer 静的に確保されたバッファへのポインタ
 * @param size バッファの全サイズ（バイト単位）
 * @return 1: 成功, 0: 失敗（既に初期化済み、またはバッファ不足）
 */
int LabPool_Init(void* buffer, size_t size);

/**
 * @brief タイムアウト付きのメモリ確保
 * @param size 確保したいサイズ（バイト単位）
 * @param timeout_ms 待機時間（ミリ秒）。0なら即時、-1なら無限に待機。
 * @return 確保された領域へのポインタ。失敗時は NULL。
 */
void* LabPool_MallocTimeout(size_t size, int timeout_ms);

/**
 * @brief 通常のメモリ確保（タイムアウトなし/即時）
 */
static inline void* LabPool_Malloc(size_t size) {
    return LabPool_MallocTimeout(size, 0);
}

/**
 * @brief メモリの解放
 * @param ptr 解放するポインタ
 */
void LabPool_Free(void* ptr);

#ifdef __cplusplus
}
#endif

#endif // MEMORY_MANAGER_C_H