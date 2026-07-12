// GSF 文件格式解析实现

#include "GsfFormat.h"
#include <algorithm>
#include <cctype>
#include <sstream>

// 使用 Windows 内置的 zlib（通过 Cabinet API 或直接链接 zlib）
// 这里使用 miniz（内嵌的 zlib 兼容库）
#include "miniz.h"

bool GSFParser::ReadFile(const wchar_t* path, std::vector<uint8_t>& data)
{
    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart > 64 * 1024 * 1024)
    {
        CloseHandle(hFile);
        return false;
    }

    data.resize(static_cast<size_t>(fileSize.QuadPart));
    DWORD bytesRead = 0;
    // 使用 ::ReadFile 显式调用 Win32 API，避免被类静态方法 GSFParser::ReadFile 遮蔽
    BOOL ok = ::ReadFile(hFile, data.data(), static_cast<DWORD>(data.size()), &bytesRead, nullptr);
    CloseHandle(hFile);

    return ok && bytesRead == data.size();
}

bool GSFParser::DecompressZlib(const uint8_t* compressed, size_t compressedSize,
                                std::vector<uint8_t>& decompressed)
{
    // 预分配解压缓冲区（GBA ROM 最大 32MB）
    decompressed.resize(32 * 1024 * 1024);
    mz_ulong destLen = static_cast<mz_ulong>(decompressed.size());

    int result = mz_uncompress(decompressed.data(), &destLen,
                               compressed, static_cast<mz_ulong>(compressedSize));
    if (result != MZ_OK)
    {
        decompressed.clear();
        return false;
    }

    decompressed.resize(destLen);
    return true;
}

void GSFParser::ParseTags(const uint8_t* tagData, size_t tagSize, PSFTags& tags)
{
    // PSF 标签格式：
    // [TAG]\n
    // key=value\n
    // key=value\n
    // ...

    std::string text(reinterpret_cast<const char*>(tagData), tagSize);

    // 查找 [TAG] 标记
    size_t tagStart = text.find("[TAG]");
    if (tagStart == std::string::npos)
        return;

    tagStart += 5; // 跳过 "[TAG]"
    if (tagStart < text.size() && text[tagStart] == '\n')
        tagStart++;
    else if (tagStart + 1 < text.size() && text[tagStart] == '\r' && text[tagStart + 1] == '\n')
        tagStart += 2;

    // 逐行解析
    std::istringstream ss(text.substr(tagStart));
    std::string line;
    while (std::getline(ss, line))
    {
        // 去除行尾 \r
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // 转换 key 为小写
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

        if (key == "title")
            tags.title = value;
        else if (key == "artist" || key == "game artist")
            tags.artist = value;
        else if (key == "game")
            tags.game = value;
        else if (key == "year")
            tags.year = value;
        else if (key == "genre")
            tags.genre = value;
        else if (key == "comment")
            tags.comment = value;
        else if (key == "copyright")
            tags.copyright = value;
        else if (key == "psfby" || key == "tagger")
            tags.psfby = value;
        else if (key == "_lib")
            tags._lib = value;
        else if (key == "length")
        {
            // 格式：mm:ss.xxx 或 ss.xxx
            tags.length = ParseTime(value);
        }
        else if (key == "fade")
        {
            tags.fade = ParseTime(value);
        }
        else
        {
            tags.extra[key] = value;
        }
    }
}

double GSFParser::ParseTime(const std::string& timeStr)
{
    // 支持格式：
    // ss.xxx
    // mm:ss.xxx
    // hh:mm:ss.xxx
    double result = 0.0;
    std::string s = timeStr;

    // 查找冒号
    size_t colon1 = s.find(':');
    if (colon1 != std::string::npos)
    {
        size_t colon2 = s.find(':', colon1 + 1);
        if (colon2 != std::string::npos)
        {
            // hh:mm:ss.xxx
            double h = std::stod(s.substr(0, colon1));
            double m = std::stod(s.substr(colon1 + 1, colon2 - colon1 - 1));
            double sec = std::stod(s.substr(colon2 + 1));
            result = h * 3600.0 + m * 60.0 + sec;
        }
        else
        {
            // mm:ss.xxx
            double m = std::stod(s.substr(0, colon1));
            double sec = std::stod(s.substr(colon1 + 1));
            result = m * 60.0 + sec;
        }
    }
    else
    {
        // ss.xxx
        try { result = std::stod(s); } catch (...) { result = 0.0; }
    }

    return result;
}

bool GSFParser::ParseSingle(const std::vector<uint8_t>& fileData, GSFFile& result)
{
    if (fileData.size() < sizeof(PSFHeader))
        return false;

    const PSFHeader* header = reinterpret_cast<const PSFHeader*>(fileData.data());

    // 验证魔数
    if (header->signature[0] != 'P' ||
        header->signature[1] != 'S' ||
        header->signature[2] != 'F')
        return false;

    // 验证版本（GSF = 0x22）
    if (header->version != GSF_VERSION)
        return false;

    size_t offset = sizeof(PSFHeader);

    // 解析保留区（GBA GSF 可能在保留区存放初始寄存器状态，但通常为空）
    const uint8_t* reservedData = fileData.data() + offset;
    uint32_t reservedSize = header->reserved_size;
    offset += reservedSize;
    if (offset > fileData.size())
        return false;

    // 读取压缩程序区
    if (header->compressed_size > 0)
    {
        if (offset + header->compressed_size > fileData.size())
            return false;

        const uint8_t* compData = fileData.data() + offset;

        // 验证 CRC32
        uint32_t crc = CRC32::Calculate(compData, header->compressed_size);
        if (crc != header->compressed_crc32)
        {
            // CRC 不匹配，但仍尝试解压（某些工具生成的文件 CRC 可能有误）
            // return false;
        }

        // 解压 ROM 数据
        std::vector<uint8_t> decompressed;
        if (!DecompressZlib(compData, header->compressed_size, decompressed))
            return false;

        // 解析 GSF 程序头（解压后数据的前 12 字节）
        if (decompressed.size() >= sizeof(GSFProgramHeader))
        {
            const GSFProgramHeader* progHeader =
                reinterpret_cast<const GSFProgramHeader*>(decompressed.data());

            result.entry_point = progHeader->entry_point;
            result.rom_offset = progHeader->offset;
            uint32_t romSize = progHeader->size;

            // ROM 数据从程序头之后开始
            size_t dataStart = sizeof(GSFProgramHeader);
            if (dataStart + romSize <= decompressed.size())
            {
                result.rom_data.assign(
                    decompressed.begin() + dataStart,
                    decompressed.begin() + dataStart + romSize);
            }
            else
            {
                // 使用全部剩余数据
                result.rom_data.assign(
                    decompressed.begin() + dataStart,
                    decompressed.end());
            }
        }
        else
        {
            // 没有程序头，直接使用解压数据
            result.rom_data = decompressed;
            result.entry_point = 0x08000000;
            result.rom_offset = 0x08000000; // GBA ROM 默认地址
        }

        offset += header->compressed_size;
    }

    // 解析标签区
    if (offset < fileData.size())
    {
        ParseTags(fileData.data() + offset, fileData.size() - offset, result.tags);
    }

    result.valid = true;
    return true;
}

bool GSFParser::Parse(const wchar_t* filePath, const wchar_t* baseDir, GSFFile& result)
{
    std::vector<uint8_t> fileData;
    if (!ReadFile(filePath, fileData))
        return false;

    if (!ParseSingle(fileData, result))
        return false;

    // 检查是否引用了 gsflib
    if (!result.tags._lib.empty())
    {
        result.is_minigsf = true;

        // 构建 gsflib 路径
        std::wstring libPath = baseDir;
        if (!libPath.empty() && libPath.back() != L'\\' && libPath.back() != L'/')
            libPath += L'\\';

        // 将 UTF-8 库文件名转换为宽字符
        int wlen = MultiByteToWideChar(CP_UTF8, 0,
            result.tags._lib.c_str(), -1, nullptr, 0);
        if (wlen > 0)
        {
            std::wstring libName(wlen, L'\0');
            MultiByteToWideChar(CP_UTF8, 0,
                result.tags._lib.c_str(), -1, &libName[0], wlen);
            libName.resize(wlen - 1); // 去除 null 终止符
            libPath += libName;
        }

        result.lib_path = libPath;

        // 加载并解析 gsflib
        GSFFile libFile;
        std::vector<uint8_t> libData;
        if (ReadFile(libPath.c_str(), libData) && ParseSingle(libData, libFile))
        {
            // 合并 ROM 数据
            MergeLibrary(result, libFile);
        }
    }

    return result.valid;
}

bool GSFParser::MergeLibrary(GSFFile& mainFile, const GSFFile& libFile)
{
    // 将 gsflib 的 ROM 数据合并到 miniGSF 中
    // gsflib 提供基础 ROM，miniGSF 提供覆盖数据

    if (libFile.rom_data.empty())
        return false;

    // 以 gsflib 的 ROM 为基础
    std::vector<uint8_t> mergedRom = libFile.rom_data;

    // 将 miniGSF 的数据覆盖到对应偏移
    if (!mainFile.rom_data.empty())
    {
        // 计算相对偏移（miniGSF offset 相对于 gsflib offset）
        uint32_t relOffset = 0;
        if (mainFile.rom_offset >= libFile.rom_offset)
            relOffset = mainFile.rom_offset - libFile.rom_offset;

        size_t writeEnd = relOffset + mainFile.rom_data.size();
        if (writeEnd > mergedRom.size())
            mergedRom.resize(writeEnd, 0);

        std::copy(mainFile.rom_data.begin(), mainFile.rom_data.end(),
                  mergedRom.begin() + relOffset);
    }

    mainFile.rom_data = std::move(mergedRom);
    mainFile.rom_offset = libFile.rom_offset;
    // gsflib 是主程序体，其 entry_point 始终优先；miniGSF 只提供数据补丁
    if (libFile.entry_point != 0)
        mainFile.entry_point = libFile.entry_point;
    return true;
}