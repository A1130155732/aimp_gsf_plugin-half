// GSF (Game Boy Sound Format) 文件格式解析
// GSF 是 PSF (Portable Sound Format) 的子格式，专用于 GBA 游戏音乐
//
// PSF/GSF 文件结构：
//   [0x00] 4 bytes  - 魔数 "PSF\x22" (0x50 0x53 0x46 0x22)
//   [0x04] 4 bytes  - 保留区长度 (reserved area length)
//   [0x08] 4 bytes  - 压缩程序区长度 (compressed program length)
//   [0x0C] 4 bytes  - 压缩程序区 CRC32
//   [0x10] N bytes  - 保留区 (reserved area)
//   [0x10+N] M bytes - zlib 压缩的程序区 (GBA ROM 数据)
//   [0x10+N+M] ...  - PSF 标签区 (UTF-8 文本，以 "[TAG]" 开头)
//
// GSF 版本号 (PSF 版本字节) = 0x22 = 34
// miniGSF 是 GSF 的子文件，引用 gsflib 中的 ROM 数据

#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <cstdint>

// PSF 魔数
#define PSF_SIGNATURE       "PSF"
#define GSF_VERSION         0x22    // GSF 版本号
#define MINIGSF_VERSION     0x22    // miniGSF 与 GSF 版本号相同

// PSF 文件头结构
#pragma pack(push, 1)
struct PSFHeader
{
    uint8_t  signature[3];      // "PSF"
    uint8_t  version;           // 0x22 for GSF
    uint32_t reserved_size;     // 保留区大小（字节）
    uint32_t compressed_size;   // 压缩程序区大小（字节）
    uint32_t compressed_crc32;  // 压缩程序区 CRC32
};
static_assert(sizeof(PSFHeader) == 16, "PSFHeader size must be 16 bytes (3+1+4+4+4)");
#pragma pack(pop)

// GSF 程序区头（解压后的 ROM 数据前缀）
// 解压后的数据直接是 GBA ROM 内容，加载到 GBA 内存地址空间
#pragma pack(push, 1)
struct GSFProgramHeader
{
    uint32_t entry_point;   // GBA 入口点地址（通常 0x08000000）
    uint32_t offset;        // ROM 数据在 GBA 地址空间的偏移
    uint32_t size;          // ROM 数据大小
};
#pragma pack(pop)

// PSF 标签结构
struct PSFTags
{
    std::string title;
    std::string artist;
    std::string game;
    std::string year;
    std::string genre;
    std::string comment;
    std::string copyright;
    std::string psfby;
    double      length;         // 播放时长（秒），-1 表示未指定
    double      fade;           // 淡出时长（秒）
    std::string _lib;           // 引用的库文件（gsflib）
    std::map<std::string, std::string> extra;

    PSFTags() : length(-1.0), fade(5.0) {}
};

// GSF 文件解析结果
struct GSFFile
{
    bool        valid;              // 是否有效
    bool        is_minigsf;         // 是否是 miniGSF（引用 gsflib）
    std::vector<uint8_t> rom_data;  // 解压后的 ROM 数据
    uint32_t    rom_offset;         // ROM 数据在 GBA 地址空间的偏移
    uint32_t    entry_point;        // GBA 程序入口点（从 GSFProgramHeader 解析）
    PSFTags     tags;               // 标签信息
    std::wstring lib_path;          // gsflib 文件路径（如果是 miniGSF）

    GSFFile() : valid(false), is_minigsf(false), rom_offset(0) {}
};

// CRC32 计算
class CRC32
{
public:
    static uint32_t Calculate(const uint8_t* data, size_t length)
    {
        static uint32_t table[256] = {0};
        static bool initialized = false;

        if (!initialized)
        {
            for (uint32_t i = 0; i < 256; i++)
            {
                uint32_t c = i;
                for (int j = 0; j < 8; j++)
                    c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            initialized = true;
        }

        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < length; i++)
            crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFF;
    }
};

// GSF 文件解析器
class GSFParser
{
public:
    // 解析 GSF/miniGSF 文件
    // filePath: 文件路径（宽字符）
    // baseDir:  文件所在目录（用于查找 gsflib）
    static bool Parse(const wchar_t* filePath, const wchar_t* baseDir, GSFFile& result);

    // 解析 PSF 标签区
    static void ParseTags(const uint8_t* tagData, size_t tagSize, PSFTags& tags);

    // 合并 gsflib 和 miniGSF 的 ROM 数据
    static bool MergeLibrary(GSFFile& mainFile, const GSFFile& libFile);

    // 解析时间字符串（mm:ss.xxx 或 ss.xxx）
    static double ParseTime(const std::string& timeStr);

private:
    // 解压 zlib 数据
    static bool DecompressZlib(const uint8_t* compressed, size_t compressedSize,
                                std::vector<uint8_t>& decompressed);

    // 读取文件内容
    static bool ReadFile(const wchar_t* path, std::vector<uint8_t>& data);

    // 解析单个 PSF/GSF 文件（不处理 lib 引用）
    static bool ParseSingle(const std::vector<uint8_t>& fileData, GSFFile& result);
};