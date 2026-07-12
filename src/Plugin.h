// AIMP GSF 插件主入口
// 实现 IAIMPPlugin、IAIMPExtensionFileFormat、IAIMPExtensionAudioDecoder 接口

#pragma once

#include "../sdk/AIMP/apiPlugin.h"
#include "../sdk/AIMP/apiDecoders.h"
#include "../sdk/AIMP/apiFileManager.h"
#include "GsfDecoder.h"

// 插件版本信息
#define PLUGIN_NAME         L"GSF Decoder (Game Boy Advance)"
#define PLUGIN_AUTHOR       L"Lex Xie & various AI"
#define PLUGIN_SHORT_DESC   L"Plays .minigsf music files, based on GSF decoder"

// 支持的文件扩展名（分号分隔，无点号）
#define PLUGIN_EXT_LIST     L"minigsf"
#define PLUGIN_EXT_DESC     L"Game Boy Advance Sound Files"

// {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
static const GUID CLSID_GsfPlugin =
    {0xA1B2C3D4, 0xE5F6, 0x7890, {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x90}};

// ----------------------------------------------------------------
// IAIMPExtensionFileFormat 实现
// 向 AIMP 注册 .gsf/.minigsf/.gsflib 为音频文件格式
// 若缺少此注册，AIMP 不会将 .gsf 文件交给任何音频解码器处理
// ----------------------------------------------------------------
class GsfFileFormatExtension : public RefCounted, public IAIMPExtensionFileFormat
{
public:
    explicit GsfFileFormatExtension(IAIMPCore* core) : m_core(core) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override
    {
        if (!ppvObject) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IAIMPExtensionFileFormat)
        {
            *ppvObject = static_cast<IAIMPExtensionFileFormat*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)()  override { return RefCounted::AddRef(); }
    STDMETHOD_(ULONG, Release)() override { return RefCounted::Release(); }

    // IAIMPExtensionFileFormat
    STDMETHOD(GetDescription)(IAIMPString **S) override;
    STDMETHOD(GetExtList)(IAIMPString **S) override;
    STDMETHOD(GetFlags)(LongWord *S) override;

private:
    // 使用 Core 创建原生 AIMP 字符串（与 DTS 官方插件 MakeString 等价）
    IAIMPString* MakeString(const wchar_t* text) const
    {
        IAIMPString* s = nullptr;
        if (m_core && SUCCEEDED(m_core->CreateObject(IID_IAIMPString, (void**)&s)) && s)
            s->SetData(const_cast<wchar_t*>(text), static_cast<int>(wcslen(text)));
        return s;
    }

    IAIMPCore* m_core; // 不持有引用，生命周期由 AIMP 管理
};

// ----------------------------------------------------------------
// IAIMPExtensionAudioDecoder 实现（基于 Stream）
// AIMP 5.x 主接口：通过 IAIMPFileStream QI 获取文件路径
// ----------------------------------------------------------------
class GsfAudioDecoderExtension : public RefCounted, public IAIMPExtensionAudioDecoder
{
public:
    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override
    {
        if (!ppvObject) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IAIMPExtensionAudioDecoder)
        {
            *ppvObject = static_cast<IAIMPExtensionAudioDecoder*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)()  override { return RefCounted::AddRef(); }
    STDMETHOD_(ULONG, Release)() override { return RefCounted::Release(); }

    STDMETHOD(CreateDecoder)(IAIMPStream *Stream, LongWord Flags,
        IAIMPErrorInfo *ErrorInfo, IAIMPAudioDecoder **Decoder) override;
};

// ----------------------------------------------------------------
// IAIMPExtensionAudioDecoderOld 实现（基于文件名字符串）
// AIMP 兼容接口：直接接收 IAIMPString* FileName，无需 Stream QI
// 作为 stream 接口的可靠补充（防止 IAIMPFileStream QI 在某些情况下失败）
// ----------------------------------------------------------------
class GsfAudioDecoderExtensionOld : public RefCounted, public IAIMPExtensionAudioDecoderOld
{
public:
    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override
    {
        if (!ppvObject) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IAIMPExtensionAudioDecoderOld)
        {
            *ppvObject = static_cast<IAIMPExtensionAudioDecoderOld*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)()  override { return RefCounted::AddRef(); }
    STDMETHOD_(ULONG, Release)() override { return RefCounted::Release(); }

    STDMETHOD(CreateDecoder)(IAIMPString *FileName, LongWord Flags,
        IAIMPErrorInfo *ErrorInfo, IAIMPAudioDecoder **Decoder) override;
};

// ----------------------------------------------------------------
// IAIMPPlugin 主插件类
// ----------------------------------------------------------------
class GsfPlugin : public IAIMPPlugin
{
public:
    GsfPlugin();
    ~GsfPlugin();

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppvObject) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;

    // IAIMPPlugin
    STDMETHOD_(TChar*, InfoGet)(int Index) override;
    STDMETHOD_(LongWord, InfoGetCategories)() override;
    STDMETHOD(Initialize)(IAIMPCore* Core) override;
    STDMETHOD(Finalize)() override;
    // SDK 定义返回 void，不能用 STDMETHOD（展开为 HRESULT）
    void WINAPI SystemNotification(int NotifyID, IUnknown* Data) override;

private:
    volatile LONG                   m_refCount;
    IAIMPCore*                      m_core;                     // AIMP 核心接口（不持有引用）
    GsfFileFormatExtension*         m_fileFormatExtension;      // 文件格式注册（告知 AIMP .gsf 是音频文件，注册到 IID_IAIMPServiceFileFormats）
    GsfAudioDecoderExtension*       m_audioDecoderExtension;    // 音频解码器扩展（Stream 接口，注册到 IID_IAIMPServiceAudioDecoders）
    GsfAudioDecoderExtensionOld*    m_audioDecoderExtensionOld; // 音频解码器扩展（FileName 接口，兼容备用）
};