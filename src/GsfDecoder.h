// AIMP GSF 解码器
// 实现 IAIMPAudioDecoder 接口，将 GSF 文件解码为 PCM 音频流

#pragma once

// 必须在任何 Windows 头文件之前定义，防止 windows.h 的 min/max 宏
// 污染 std::min / std::max（MSVC C2589/C2059）
#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include "../sdk/AIMP/apiDecoders.h"
#include "GsfFormat.h"
#include "GbaEmulator.h"
#include <memory>
#include <string>

// 输出音频格式
#define GSF_OUTPUT_SAMPLERATE   44100
#define GSF_OUTPUT_CHANNELS     2
#define GSF_OUTPUT_BITS         16

// 默认播放时长（秒），当标签未指定时使用
#define GSF_DEFAULT_DURATION    180.0
// 默认淡出时长（秒）
#define GSF_DEFAULT_FADE        5.0

// 简单引用计数基类
class RefCounted
{
public:
    RefCounted() : m_refCount(1) {}
    virtual ~RefCounted() {}

    ULONG AddRef() { return ++m_refCount; }
    ULONG Release()
    {
        ULONG count = --m_refCount;
        if (count == 0) delete this;
        return count;
    }

private:
    volatile LONG m_refCount;
};

// AIMP String 实现（用于返回字符串给 AIMP）
class AIMPString : public RefCounted, public IAIMPString
{
public:
    AIMPString() {}
    explicit AIMPString(const wchar_t* str) : m_data(str ? str : L"") {}
    explicit AIMPString(const std::wstring& str) : m_data(str) {}

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override
    {
        if (!ppvObject) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IAIMPString)
        {
            *ppvObject = static_cast<IAIMPString*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return RefCounted::AddRef(); }
    STDMETHOD_(ULONG, Release)() override { return RefCounted::Release(); }

    // IAIMPString — 仅实现核心读写，其余接口返回 E_NOTIMPL 存根
    STDMETHOD_(TChar*, GetData)() override { return const_cast<TChar*>(m_data.c_str()); }
    STDMETHOD_(int, GetLength)() override { return (int)m_data.size(); }
    STDMETHOD_(int, GetHashCode)() override { return 0; }
    STDMETHOD(GetChar)(int Index, TChar* Char) override
    {
        if (!Char || Index < 0 || Index >= (int)m_data.size()) return E_INVALIDARG;
        *Char = m_data[Index];
        return S_OK;
    }
    STDMETHOD(SetChar)(int Index, TChar Char) override
    {
        if (Index < 0 || Index >= (int)m_data.size()) return E_INVALIDARG;
        m_data[Index] = Char;
        return S_OK;
    }
    STDMETHOD(SetData)(TChar* Chars, int Length) override
    {
        if (Chars && Length > 0)
            m_data.assign(Chars, Length);
        else if (Chars)
            m_data = Chars;
        else
            m_data.clear();
        return S_OK;
    }
    // 以下方法本插件内部不调用，提供空实现以满足纯虚函数要求
    STDMETHOD(Add)(IAIMPString* /*S*/) override { return E_NOTIMPL; }
    STDMETHOD(Add2)(TChar* /*Chars*/, int /*CharCount*/) override { return E_NOTIMPL; }
    STDMETHOD(ChangeCase)(int /*Mode*/) override { return E_NOTIMPL; }
    STDMETHOD(Clone)(IAIMPString** S) override
    {
        if (!S) return E_POINTER;
        *S = new AIMPString(m_data);
        return S_OK;
    }
    STDMETHOD(Compare)(IAIMPString* /*S*/, int* /*CompareResult*/, BOOL /*IgnoreCase*/) override { return E_NOTIMPL; }
    STDMETHOD(Compare2)(TChar* /*Chars*/, int /*CharCount*/, int* /*CompareResult*/, BOOL /*IgnoreCase*/) override { return E_NOTIMPL; }
    STDMETHOD(Delete)(int /*Index*/, int /*Count*/) override { return E_NOTIMPL; }
    STDMETHOD(Find)(IAIMPString* /*S*/, int* /*Index*/, int /*Flags*/, int /*StartFromIndex*/) override { return E_NOTIMPL; }
    STDMETHOD(Find2)(TChar* /*Chars*/, int /*CharCount*/, int* /*Index*/, int /*Flags*/, int /*StartFromIndex*/) override { return E_NOTIMPL; }
    STDMETHOD(Insert)(int /*Index*/, IAIMPString* /*S*/) override { return E_NOTIMPL; }
    STDMETHOD(Insert2)(int /*Index*/, TChar* /*Chars*/, int /*CharCount*/) override { return E_NOTIMPL; }
    STDMETHOD(Replace)(IAIMPString* /*OldPattern*/, IAIMPString* /*NewPattern*/, int /*Flags*/) override { return E_NOTIMPL; }
    STDMETHOD(Replace2)(TChar* /*OldPatternChars*/, int /*OldPatternCharCount*/,
        TChar* /*NewPatternChars*/, int /*NewPatternCharCount*/, int /*Flags*/) override { return E_NOTIMPL; }
    STDMETHOD(SubString)(int /*Index*/, int /*Count*/, IAIMPString** /*S*/) override { return E_NOTIMPL; }

private:
    std::wstring m_data;
};

// GSF 音频解码器
class GsfDecoder : public RefCounted, public IAIMPAudioDecoder
{
public:
    GsfDecoder();
    virtual ~GsfDecoder();

    // 打开 GSF 文件
    bool Open(const wchar_t* filePath);

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override;
    STDMETHOD_(ULONG, AddRef)() override { return RefCounted::AddRef(); }
    STDMETHOD_(ULONG, Release)() override { return RefCounted::Release(); }

    // IAIMPAudioDecoder (官方 SDK v5.40)
    STDMETHOD_(BOOL, GetFileInfo)(IAIMPFileInfo *FileInfo) override;
    STDMETHOD_(BOOL, GetStreamInfo)(int *SampleRate, int *Channels, int *SampleFormat) override;
    STDMETHOD_(BOOL, IsSeekable)() override;
    STDMETHOD_(BOOL, IsRealTimeStream)() override;
    STDMETHOD_(INT64, GetAvailableData)() override;
    STDMETHOD_(INT64, GetSize)() override;
    STDMETHOD_(INT64, GetPosition)() override;
    STDMETHOD_(BOOL, SetPosition)(const INT64 Value) override;
    STDMETHOD_(int, Read)(void *Buffer, int Count) override;

private:
    GSFFile             m_gsfFile;          // 解析后的 GSF 文件
    GbaEmulator         m_emulator;         // GBA 模拟器
    double              m_duration;         // 总时长（秒）
    double              m_fadeLength;       // 淡出时长（秒）
    double              m_position;         // 当前播放位置（秒）
    INT64               m_totalSamples;     // 总样本数
    INT64               m_currentSample;    // 当前样本位置
    bool                m_opened;           // 是否已打开
    std::wstring        m_filePath;         // 文件路径

    // 内部缓冲区（用于重采样和淡出处理）
    std::vector<int16_t> m_sampleBuffer;
    int                  m_bufferPos;
    int                  m_bufferSize;

    // 填充内部缓冲区
    void FillBuffer();

    // 应用淡出效果
    void ApplyFade(int16_t* samples, int count);

    // 将文件路径转换为目录路径
    static std::wstring GetDirectory(const std::wstring& filePath);

    // UTF-8 转宽字符
    static std::wstring Utf8ToWide(const std::string& utf8);
};