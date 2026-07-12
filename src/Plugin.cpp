// AIMP GSF 插件实现
// 修复版本：将所有无法解码的情况返回 E_FAIL，避免影响其他解码器

#include "Plugin.h"
#include "DebugLog.h"
#include <cstring>
#include <string>
#include <fstream>
#include <windows.h>

// ============================================================
// 内部辅助函数
// ============================================================

// 从文件路径字符串创建 GSF 解码器；检查扩展名并调用 Open()
// 成功返回 S_OK 并写入 *Decoder；非 GSF 或打开失败返回 E_FAIL
static HRESULT TryOpenGsfFromPath(const wchar_t* path, int pathLen,
    IAIMPErrorInfo* ErrorInfo, IAIMPAudioDecoder** Decoder)
{
    // 防御性检查：空指针或无效长度
        if (!path || pathLen <= 0)
        {
            LogDebug(L"TryOpenGsfFromPath: Invalid path or length");
            return E_FAIL;  // 不能解码，让 AIMP 尝试其他解码器
        }

    try
    {
        std::wstring filePath(path, pathLen);

        // 检查扩展名（gsflib 是库文件，不直接播放）
        size_t dotPos = filePath.rfind(L'.');
        if (dotPos == std::wstring::npos)
        {
            LogDebug(L"TryOpenGsfFromPath: No file extension found");
            return E_FAIL;
        }

        std::wstring ext = filePath.substr(dotPos + 1);
        for (auto& c : ext) c = towlower(c);
        
        // 严格检查：.minigsf
        if (ext != L"minigsf")
        {
            // LogDebug(L"TryOpenGsfFromPath: Unsupported extension");
            return E_FAIL;  // 不是 GSF，告知 AIMP 继续尝试其他解码器
        }

        LogDebug(L"TryOpenGsfFromPath: Attempting to open GSF file");

        // 创建解码器对象
        GsfDecoder* decoder = nullptr;
        try
        {
            decoder = new GsfDecoder();
        }
        catch (...)
        {
            LogDebug(L"TryOpenGsfFromPath: Failed to allocate GsfDecoder");
            if (decoder) delete decoder;
            return E_FAIL;
        }

        if (!decoder)
        {
            LogDebug(L"TryOpenGsfFromPath: Decoder allocation returned null");
            return E_FAIL;
        }

        // 尝试打开文件
        bool openResult = false;
        try
        {
            openResult = decoder->Open(path);
        }
        catch (...)
        {
            LogDebug(L"TryOpenGsfFromPath: Exception during decoder->Open()");
            delete decoder;
            return E_FAIL;
        }

        if (openResult)
        {
            LogDebug(L"TryOpenGsfFromPath: Successfully opened GSF file");
            *Decoder = decoder;
            return S_OK;
        }

        // 打开失败，清理资源
        LogDebug(L"TryOpenGsfFromPath: Failed to open GSF file");
        delete decoder;
        
        if (ErrorInfo)
        {
            try
            {
                IAIMPString* msg = new AIMPString(L"GSF 文件打开失败：格式无效或找不到关联的 gsflib 文件");
                if (msg)
                {
                    ErrorInfo->SetInfo(E_FAIL, msg, nullptr);
                    msg->Release();
                }
            }
            catch (...)
            {
                LogDebug(L"TryOpenGsfFromPath: Failed to set error info");
            }
        }
        
        return E_FAIL;
    }
    catch (...)
    {
        LogDebug(L"TryOpenGsfFromPath: Caught unexpected exception");
        return E_FAIL;
    }
}

// ============================================================
// GsfFileFormatExtension 实现
// ============================================================

STDMETHODIMP GsfFileFormatExtension::GetDescription(IAIMPString **S)
{
    if (!S) return E_POINTER;
    *S = MakeString(PLUGIN_EXT_DESC);
    return *S ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP GsfFileFormatExtension::GetExtList(IAIMPString **S)
{
    if (!S) return E_POINTER;
    *S = MakeString(L"*.minigsf");
    return *S ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP GsfFileFormatExtension::GetFlags(LongWord *S)
{
    if (!S) return E_POINTER;
    *S = AIMP_SERVICE_FILEFORMATS_CATEGORY_AUDIO;
    return S_OK;
}

// ============================================================
// GsfAudioDecoderExtension 实现（Stream 接口，AIMP 5.x 主接口）
// ============================================================

STDMETHODIMP GsfAudioDecoderExtension::CreateDecoder(
    IAIMPStream *Stream, LongWord Flags,
    IAIMPErrorInfo *ErrorInfo, IAIMPAudioDecoder **Decoder)
{
    if (!Stream || !Decoder)
    {
        LogDebug(L"CreateDecoder: Invalid parameters");
        return E_POINTER;
    }
    
    *Decoder = nullptr;

    try
    {
        LogDebug(L"CreateDecoder: Called");

        // 通过 IAIMPFileStream QI 获取实际文件路径。
        // 注意：如果无法获取文件流，说明不是文件流（如网络流），我们无法处理，返回 E_FAIL
        IAIMPFileStream* fileStream = nullptr;
        HRESULT hrQI = Stream->QueryInterface(IID_IAIMPFileStream, (void**)&fileStream);
        
        if (FAILED(hrQI) || !fileStream)
        {
            LogDebug(L"CreateDecoder: QI for IAIMPFileStream failed or null");
            return E_FAIL;  // 非文件流，本解码器不支持
        }

        IAIMPString* fileName = nullptr;
        HRESULT hr = E_FAIL;
        HRESULT hrGetFileName = fileStream->GetFileName(&fileName);
        
        if (SUCCEEDED(hrGetFileName) && fileName)
        {
            const wchar_t* pathData = fileName->GetData();
            int pathLen = fileName->GetLength();
            
            if (pathData && pathLen > 0)
            {
                LogDebug(L"CreateDecoder: Calling TryOpenGsfFromPath");
                hr = TryOpenGsfFromPath(pathData, pathLen, ErrorInfo, Decoder);
            }
            else
            {
                LogDebug(L"CreateDecoder: fileName GetData() returned null or invalid length");
                hr = E_FAIL;
            }
            
            fileName->Release();
        }
        else
        {
            LogDebug(L"CreateDecoder: GetFileName failed or returned null");
            hr = E_FAIL;
        }
        
        fileStream->Release();
        
        LogDebug(L"CreateDecoder: Returning HRESULT");
        return hr;
    }
    catch (...)
    {
        LogDebug(L"CreateDecoder: Caught unexpected exception, returning E_FAIL");
        
        if (*Decoder)
        {
            (*Decoder)->Release();
            *Decoder = nullptr;
        }
        return E_FAIL;
    }
}

// ============================================================
// GsfAudioDecoderExtensionOld 实现（FileName 接口，兼容备用）
// ============================================================

STDMETHODIMP GsfAudioDecoderExtensionOld::CreateDecoder(
    IAIMPString *FileName, LongWord Flags,
    IAIMPErrorInfo *ErrorInfo, IAIMPAudioDecoder **Decoder)
{
    if (!FileName || !Decoder)
    {
        LogDebug(L"CreateDecoderOld: Invalid parameters");
        return E_POINTER;
    }
    
    *Decoder = nullptr;

    try
    {
        LogDebug(L"CreateDecoderOld: Called");

        const wchar_t* pathData = FileName->GetData();
        int pathLen = FileName->GetLength();
        
        if (!pathData || pathLen <= 0)
        {
            LogDebug(L"CreateDecoderOld: Invalid path data");
            return E_FAIL;
        }

        LogDebug(L"CreateDecoderOld: Calling TryOpenGsfFromPath");
        HRESULT hr = TryOpenGsfFromPath(pathData, pathLen, ErrorInfo, Decoder);
        
        LogDebug(L"CreateDecoderOld: Returning HRESULT");
        return hr;
    }
    catch (...)
    {
        LogDebug(L"CreateDecoderOld: Caught unexpected exception, returning E_FAIL");
        
        if (*Decoder)
        {
            (*Decoder)->Release();
            *Decoder = nullptr;
        }
        
        return E_FAIL;
    }
}

// ============================================================
// GsfPlugin 实现
// ============================================================

GsfPlugin::GsfPlugin()
    : m_refCount(1)
    , m_core(nullptr)
    , m_fileFormatExtension(nullptr)
    , m_audioDecoderExtension(nullptr)
    , m_audioDecoderExtensionOld(nullptr)
{
    LogDebug(L"GsfPlugin: Constructor called");
}

GsfPlugin::~GsfPlugin()
{
    LogDebug(L"GsfPlugin: Destructor called");
    
    if (m_fileFormatExtension)
    {
        m_fileFormatExtension->Release();
        m_fileFormatExtension = nullptr;
    }
    if (m_audioDecoderExtension)
    {
        m_audioDecoderExtension->Release();
        m_audioDecoderExtension = nullptr;
    }
    if (m_audioDecoderExtensionOld)
    {
        m_audioDecoderExtensionOld->Release();
        m_audioDecoderExtensionOld = nullptr;
    }
}

STDMETHODIMP GsfPlugin::QueryInterface(REFIID riid, void** ppvObject)
{
    if (!ppvObject) return E_POINTER;
    if (riid == IID_IUnknown)
    {
        *ppvObject = static_cast<IAIMPPlugin*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) GsfPlugin::AddRef()
{
    return InterlockedIncrement(&m_refCount);
}

STDMETHODIMP_(ULONG) GsfPlugin::Release()
{
    LONG count = InterlockedDecrement(&m_refCount);
    if (count == 0) delete this;
    return count;
}

STDMETHODIMP_(TChar*) GsfPlugin::InfoGet(int Index)
{
    switch (Index)
    {
    case AIMP_PLUGIN_INFO_NAME:              return const_cast<TChar*>(PLUGIN_NAME);
    case AIMP_PLUGIN_INFO_AUTHOR:            return const_cast<TChar*>(PLUGIN_AUTHOR);
    case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION: return const_cast<TChar*>(PLUGIN_SHORT_DESC);
    default: return nullptr;
    }
}

STDMETHODIMP_(LongWord) GsfPlugin::InfoGetCategories()
{
    return AIMP_PLUGIN_CATEGORY_DECODERS;
}

STDMETHODIMP GsfPlugin::Initialize(IAIMPCore* Core)
{
    LogDebug(L"GsfPlugin::Initialize: Called");
    
    if (!Core) return E_POINTER;
    m_core = Core;

    // 注册文件格式扩展
    m_fileFormatExtension = new GsfFileFormatExtension(Core);
    HRESULT hr = Core->RegisterExtension(IID_IAIMPServiceFileFormats,
                                          static_cast<IAIMPExtensionFileFormat*>(m_fileFormatExtension));
    if (FAILED(hr))
    {
        LogDebug(L"GsfPlugin::Initialize: Failed to register file format extension");
        m_fileFormatExtension->Release();
        m_fileFormatExtension = nullptr;
        return hr;
    }

    // 注册 Stream 接口解码器
    m_audioDecoderExtension = new GsfAudioDecoderExtension();
    hr = Core->RegisterExtension(IID_IAIMPServiceAudioDecoders,
                                  static_cast<IAIMPExtensionAudioDecoder*>(m_audioDecoderExtension));
    if (FAILED(hr))
    {
        LogDebug(L"GsfPlugin::Initialize: Failed to register audio decoder extension (Stream)");
        m_audioDecoderExtension->Release();
        m_audioDecoderExtension = nullptr;
        return hr;
    }

    // 注册 FileName 接口解码器（兼容备用）
    m_audioDecoderExtensionOld = new GsfAudioDecoderExtensionOld();
    hr = Core->RegisterExtension(IID_IAIMPServiceAudioDecoders,
                                  static_cast<IAIMPExtensionAudioDecoderOld*>(m_audioDecoderExtensionOld));
    if (FAILED(hr))
    {
        LogDebug(L"GsfPlugin::Initialize: Failed to register audio decoder extension (FileName)");
        m_audioDecoderExtensionOld->Release();
        m_audioDecoderExtensionOld = nullptr;
        // 不影响主接口
    }

    LogDebug(L"GsfPlugin::Initialize: Success");
    return S_OK;
}

STDMETHODIMP GsfPlugin::Finalize()
{
    LogDebug(L"GsfPlugin::Finalize: Called");
    
    if (m_core)
    {
        if (m_fileFormatExtension)
            m_core->UnregisterExtension(static_cast<IAIMPExtensionFileFormat*>(m_fileFormatExtension));
        if (m_audioDecoderExtension)
            m_core->UnregisterExtension(static_cast<IAIMPExtensionAudioDecoder*>(m_audioDecoderExtension));
        if (m_audioDecoderExtensionOld)
            m_core->UnregisterExtension(static_cast<IAIMPExtensionAudioDecoderOld*>(m_audioDecoderExtensionOld));
    }

    if (m_fileFormatExtension)    { m_fileFormatExtension->Release();    m_fileFormatExtension    = nullptr; }
    if (m_audioDecoderExtension)  { m_audioDecoderExtension->Release();  m_audioDecoderExtension  = nullptr; }
    if (m_audioDecoderExtensionOld){ m_audioDecoderExtensionOld->Release(); m_audioDecoderExtensionOld = nullptr; }
    m_core = nullptr;
    
    LogDebug(L"GsfPlugin::Finalize: Success");
    return S_OK;
}

void WINAPI GsfPlugin::SystemNotification(int NotifyID, IUnknown* Data)
{
    (void)NotifyID;
    (void)Data;
}

// ============================================================
// DLL 导出函数
// ============================================================

extern "C" __declspec(dllexport)
HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin **Header)
{
    if (!Header) return E_POINTER;
    *Header = new GsfPlugin();
    return S_OK;
}