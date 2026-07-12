// miniz.h - 单文件 zlib 兼容压缩/解压库
// 版本: 3.0.2 (精简版，仅包含解压功能)
// 原始项目: https://github.com/richgel999/miniz
// 许可证: MIT
//
// 此文件提供 mz_uncompress() 函数，用于解压 GSF 文件中的 zlib 压缩数据

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long mz_ulong;
typedef unsigned char mz_uint8;
typedef unsigned short mz_uint16;
typedef unsigned int mz_uint32;
typedef unsigned int mz_uint;
typedef long long mz_int64;
typedef unsigned long long mz_uint64;

#define MZ_OK                   0
#define MZ_STREAM_END           1
#define MZ_NEED_DICT            2
#define MZ_ERRNO               (-1)
#define MZ_STREAM_ERROR        (-2)
#define MZ_DATA_ERROR          (-3)
#define MZ_MEM_ERROR           (-4)
#define MZ_BUF_ERROR           (-5)
#define MZ_VERSION_ERROR       (-6)
#define MZ_PARAM_ERROR         (-10000)

#define MZ_DEFAULT_COMPRESSION  (-1)
#define MZ_NO_COMPRESSION        0
#define MZ_BEST_SPEED            1
#define MZ_BEST_COMPRESSION      9
#define MZ_UBER_COMPRESSION     10
#define MZ_DEFAULT_LEVEL         6
#define MZ_DEFAULT_STRATEGY      0
#define MZ_FILTERED              1
#define MZ_HUFFMAN_ONLY          2
#define MZ_RLE                   3
#define MZ_FIXED                 4

// 解压函数声明
int mz_uncompress(unsigned char *pDest, mz_ulong *pDest_len,
                  const unsigned char *pSource, mz_ulong source_len);

int mz_uncompress2(unsigned char *pDest, mz_ulong *pDest_len,
                   const unsigned char *pSource, mz_ulong *pSource_len);

#ifdef __cplusplus
}
#endif