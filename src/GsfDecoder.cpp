// GSF 解码器实现

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "GsfDecoder.h"
#include "DebugLog.h"
#include <algorithm>
#include <cmath>
#include <cstring>

GsfDecoder::GsfDecoder()
    : m_duration(GSF_DEFAULT_DURATION)
    , m_fadeLength(GSF_DEFAULT_FADE)
    , m_position(0.0)
    , m_totalSamples(0)
    , m_currentSample(0)
    , m_opened(false)
    , m_bufferPos(0)
    , m_bufferSize(0)
{
    // 预分配内部缓冲区（约 0.1 秒的音频）
    m_sampleBuffer.resize(GSF_OUTPUT_SAMPLERATE / 10 * GSF_OUTPUT_CHANNELS);
}

GsfDecoder::~GsfDecoder()
{
}

std::wstring GsfDecoder::GetDirectory(const std::wstring& filePath)
{
    size_t pos = filePath.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return L".";
    return filePath.substr(0, pos);
}

std::wstring GsfDecoder::Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty()) return L"";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring result(wlen - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &result[0], wlen);
    return result;
}

bool GsfDecoder::Open(const wchar_t* filePath)
{
    if (!filePath) return false;

    m_filePath = filePath;
    std::wstring dir = GetDirectory(m_filePath);

    // 解析 GSF 文件
    if (!GSFParser::Parse(filePath, dir.c_str(), m_gsfFile))
    {
        LogDebug(L"GsfDecoder::Open: Failed to parse GSF file");
        return false;
    }

    if (m_gsfFile.rom_data.empty())
    {
        LogDebug(L"GsfDecoder::Open: ROM data is empty");
        return false;
    }

    LogDebug(L"GsfDecoder::Open: ROM Size=%d, Offset=0x%08X, Entry=0x%08X", 
             (int)m_gsfFile.rom_data.size(), m_gsfFile.rom_offset, m_gsfFile.entry_point);

    // 从标签读取时长
    if (m_gsfFile.tags.length > 0)
        m_duration = m_gsfFile.tags.length;
    else
        m_duration = GSF_DEFAULT_DURATION;

    if (m_gsfFile.tags.fade >= 0)
        m_fadeLength = m_gsfFile.tags.fade;
    else
        m_fadeLength = GSF_DEFAULT_FADE;

    // 总时长 = 播放时长 + 淡出时长
    double totalDuration = m_duration + m_fadeLength;
    m_totalSamples = (INT64)(totalDuration * GSF_OUTPUT_SAMPLERATE);
    m_currentSample = 0;

    // 初始化 GBA 模拟器
    m_emulator.Init(GSF_OUTPUT_SAMPLERATE);
    if (!m_emulator.LoadRom(m_gsfFile.rom_data.data(),
                            m_gsfFile.rom_data.size(),
                            m_gsfFile.rom_offset,
                            m_gsfFile.entry_point))
    {
        LogDebug(L"GsfDecoder::Open: Failed to load ROM into emulator");
        return false;
    }

    m_emulator.Reset();
    LogDebug(L"GsfDecoder::Open: Emulator initialized and reset");
    m_bufferPos = 0;
    m_bufferSize = 0;
    m_opened = true;
    return true;
}

STDMETHODIMP GsfDecoder::QueryInterface(REFIID riid, void** ppvObject)
{
    if (!ppvObject) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IAIMPAudioDecoder)
    {
        *ppvObject = static_cast<IAIMPAudioDecoder*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

// IAIMPAudioDecoder::GetFileInfo - 返回 BOOL
STDMETHODIMP_(BOOL) GsfDecoder::GetFileInfo(IAIMPFileInfo *FileInfo)
{
    if (!FileInfo || !m_opened) return FALSE;

    // 设置时长（毫秒）
    double totalDuration = m_duration + m_fadeLength;
    FileInfo->SetValueAsFloat(AIMP_FILEINFO_PROPID_DURATION, totalDuration);

    // 设置比特率（kbps）
    int bitrate = GSF_OUTPUT_SAMPLERATE * GSF_OUTPUT_CHANNELS * GSF_OUTPUT_BITS / 1000;
    FileInfo->SetValueAsInt32(AIMP_FILEINFO_PROPID_BITRATE, bitrate);

    // 设置采样率
    FileInfo->SetValueAsInt32(AIMP_FILEINFO_PROPID_SAMPLERATE, GSF_OUTPUT_SAMPLERATE);

    // 设置声道数
    FileInfo->SetValueAsInt32(AIMP_FILEINFO_PROPID_CHANNELS, GSF_OUTPUT_CHANNELS);

    // 设置标签信息
    auto setStr = [&](int propId, const std::string& utf8str)
    {
        if (!utf8str.empty())
        {
            std::wstring wide = Utf8ToWide(utf8str);
            AIMPString* str = new AIMPString(wide);
            FileInfo->SetValueAsObject(propId, str);
            str->Release();
        }
    };

    // 标题：优先使用 title 标签，否则使用文件名
    if (!m_gsfFile.tags.title.empty())
        setStr(AIMP_FILEINFO_PROPID_TITLE, m_gsfFile.tags.title);

    // 艺术家
    if (!m_gsfFile.tags.artist.empty())
        setStr(AIMP_FILEINFO_PROPID_ARTIST, m_gsfFile.tags.artist);

    // 专辑（使用游戏名）
    if (!m_gsfFile.tags.game.empty())
        setStr(AIMP_FILEINFO_PROPID_ALBUM, m_gsfFile.tags.game);

    // 年份
    if (!m_gsfFile.tags.year.empty())
        // SDK 无 AIMP_FILEINFO_PROPID_YEAR，年份存入 AIMP_FILEINFO_PROPID_DATE
        setStr(AIMP_FILEINFO_PROPID_DATE, m_gsfFile.tags.year);

    // 流派
    if (!m_gsfFile.tags.genre.empty())
        setStr(AIMP_FILEINFO_PROPID_GENRE, m_gsfFile.tags.genre);

    // 注释
    if (!m_gsfFile.tags.comment.empty())
        setStr(AIMP_FILEINFO_PROPID_COMMENT, m_gsfFile.tags.comment);

    return TRUE;
}

// IAIMPAudioDecoder::GetStreamInfo - 返回采样率、声道数、采样格式
STDMETHODIMP_(BOOL) GsfDecoder::GetStreamInfo(int *SampleRate, int *Channels, int *SampleFormat)
{
    if (!m_opened) return FALSE;
    if (SampleRate)   *SampleRate   = GSF_OUTPUT_SAMPLERATE;
    if (Channels)     *Channels     = GSF_OUTPUT_CHANNELS;
    if (SampleFormat) *SampleFormat = AIMP_DECODER_SAMPLEFORMAT_16BIT;
    return TRUE;
}

// IAIMPAudioDecoder::IsSeekable - GSF 支持 seek（通过重新模拟）
STDMETHODIMP_(BOOL) GsfDecoder::IsSeekable()
{
    return m_opened ? TRUE : FALSE;
}

// IAIMPAudioDecoder::IsRealTimeStream - GSF 不是实时流
STDMETHODIMP_(BOOL) GsfDecoder::IsRealTimeStream()
{
    return FALSE;
}

// IAIMPAudioDecoder::GetAvailableData - 返回剩余可读取的样本数（字节）
STDMETHODIMP_(INT64) GsfDecoder::GetAvailableData()
{
    if (!m_opened) return 0;
    INT64 remaining = m_totalSamples - m_currentSample;
    if (remaining < 0) remaining = 0;
    // 返回字节数：样本数 * 声道数 * 每样本字节数
    return remaining * GSF_OUTPUT_CHANNELS * (GSF_OUTPUT_BITS / 8);
}

// IAIMPAudioDecoder::GetSize - 返回总数据大小（字节）
STDMETHODIMP_(INT64) GsfDecoder::GetSize()
{
    if (!m_opened) return 0;
    return m_totalSamples * GSF_OUTPUT_CHANNELS * (GSF_OUTPUT_BITS / 8);
}

// IAIMPAudioDecoder::GetPosition - 返回当前播放位置（字节）
STDMETHODIMP_(INT64) GsfDecoder::GetPosition()
{
    if (!m_opened) return 0;
    return m_currentSample * GSF_OUTPUT_CHANNELS * (GSF_OUTPUT_BITS / 8);
}

// IAIMPAudioDecoder::SetPosition - 设置播放位置（字节偏移）
STDMETHODIMP_(BOOL) GsfDecoder::SetPosition(const INT64 Value)
{
    if (!m_opened) return FALSE;

    // Value 是字节偏移，转换为样本位置
    INT64 bytesPerFrame = GSF_OUTPUT_CHANNELS * (GSF_OUTPUT_BITS / 8);
    if (bytesPerFrame == 0) return FALSE;
    
    INT64 targetSample = Value / bytesPerFrame;
    if (targetSample < 0) targetSample = 0;
    if (targetSample > m_totalSamples) targetSample = m_totalSamples;

    // 如果向后 seek 或当前位置已超过目标，重置模拟器
    if (targetSample < m_currentSample)
    {
        m_emulator.Reset();
        m_currentSample = 0;
        m_bufferPos = 0;
        m_bufferSize = 0;
    }

    // 向前快进到目标位置（丢弃音频数据）
    if (targetSample > m_currentSample)
    {
        INT64 samplesToSkip = targetSample - m_currentSample;
        std::vector<int16_t> skipBuf(4096 * GSF_OUTPUT_CHANNELS);
        while (samplesToSkip > 0)
        {
            int toSkip = (int)std::min(samplesToSkip, (INT64)4096);
            int got = m_emulator.RunForSamples(skipBuf.data(), toSkip);
            if (got <= 0) break;
            m_currentSample += got;
            samplesToSkip -= got;
        }
    }

    m_position = (double)m_currentSample / GSF_OUTPUT_SAMPLERATE;
    m_bufferPos = 0;
    m_bufferSize = 0;
    return TRUE;
}

void GsfDecoder::FillBuffer()
{
    int maxSamples = (int)(m_sampleBuffer.size() / GSF_OUTPUT_CHANNELS);
    m_bufferSize = m_emulator.RunForSamples(m_sampleBuffer.data(), maxSamples);
    
    static int fillCount = 0;
    if (fillCount < 5) {
        LogDebug(L"GsfDecoder::FillBuffer: Generated %d samples", m_bufferSize);
        fillCount++;
    }
    
    m_bufferPos = 0;
}

void GsfDecoder::ApplyFade(int16_t* samples, int count)
{
    // 计算淡出区间
    INT64 fadeStartSample = (INT64)(m_duration * GSF_OUTPUT_SAMPLERATE);
    INT64 fadeEndSample   = m_totalSamples;

    for (int i = 0; i < count; i++)
    {
        INT64 samplePos = m_currentSample - (m_bufferSize - m_bufferPos) + i;
        if (samplePos >= fadeStartSample && fadeEndSample > fadeStartSample)
        {
            double fadeProgress = (double)(samplePos - fadeStartSample) /
                                  (double)(fadeEndSample - fadeStartSample);
            fadeProgress = std::max(0.0, std::min(1.0, fadeProgress));
            double gain = 1.0 - fadeProgress;

            // 应用到立体声样本
            int sampleIdx = i * GSF_OUTPUT_CHANNELS;
            for (int ch = 0; ch < GSF_OUTPUT_CHANNELS; ch++)
            {
                samples[sampleIdx + ch] = (int16_t)(samples[sampleIdx + ch] * gain);
            }
        }
    }
}

// IAIMPAudioDecoder::Read - 读取音频数据（字节）
STDMETHODIMP_(int) GsfDecoder::Read(void *Buffer, int Count)
{
    if (!m_opened || !Buffer || Count <= 0)
        return 0;

    // Count 是字节数
    int bytesPerSample = GSF_OUTPUT_BITS / 8;
    int bytesPerFrame  = bytesPerSample * GSF_OUTPUT_CHANNELS;
    int framesRequested = Count / bytesPerFrame;

    if (framesRequested <= 0) return 0;

    // 检查是否已到达末尾
    if (m_currentSample >= m_totalSamples)
        return 0;

    int16_t* outBuf = reinterpret_cast<int16_t*>(Buffer);
    int framesWritten = 0;

    while (framesWritten < framesRequested && m_currentSample < m_totalSamples)
    {
        // 如果内部缓冲区为空，填充
        if (m_bufferPos >= m_bufferSize)
        {
            FillBuffer();
            if (m_bufferSize <= 0) break;
        }

        // 从内部缓冲区复制数据
        int available = m_bufferSize - m_bufferPos;
        int toWrite = (available < (framesRequested - framesWritten)) ? available : (framesRequested - framesWritten);
        if ((INT64)toWrite > (m_totalSamples - m_currentSample))
            toWrite = (int)(m_totalSamples - m_currentSample);

        if (toWrite <= 0) break;

        int16_t* src = m_sampleBuffer.data() + m_bufferPos * GSF_OUTPUT_CHANNELS;
        int16_t* dst = outBuf + framesWritten * GSF_OUTPUT_CHANNELS;

        // 复制并应用淡出
        memcpy(dst, src, toWrite * bytesPerFrame);
        ApplyFade(dst, toWrite);

        m_bufferPos     += toWrite;
        m_currentSample += toWrite;
        framesWritten   += toWrite;
    }

    return framesWritten * bytesPerFrame;
}