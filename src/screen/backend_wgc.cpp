// Copyright (c) 2026 flcker <molanyousi@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// 屏幕域 WGC 后端 —— 窗口捕获主后端（F17，Win10 1903+）。
// WinRT COM 直调（零 C++/WinRT 包依赖）：
//   D3D11 device → CreateDirect3D11DeviceFromDXGIDevice → IDirect3DDevice
//   → RoGetActivationFactory(GraphicsCaptureItem) → IGraphicsCaptureItemInterop
//     ::CreateForWindow（<1903 无此 interop → E_NOINTERFACE → 会话层回落 GDI）
//   → Direct3D11CaptureFramePool::CreateFreeThreaded（双缓冲，B8G8R8A8，
//     尺寸取 item Size；free-threaded：FrameArrived 在线程池线程触发）
//   → CreateCaptureSession → StartCapture
// 等待模型：add_FrameArrived（WRL handler 仅 SetEvent 置 Win32 auto-reset 事件——
//   本 SDK 无 CreateFrameArrivedEvent ABI，以事件桥等价实现"事件句柄而非 WinRT
//   回调"）+ 采集线程 WaitForMultipleObjects(frameEvent, wakeEvent)，可中断。
// 取帧：TryGetNextFrame → Surface → IDirect3DDxgiInterfaceAccess → ID3D11Texture2D
//   → CopyResource staging → Map 按 RowPitch 逐行拷出 BGRA。
// resize：frame ContentSize 与池尺寸不同 → Recreate 池 + staging（单线程访问
//   安全；过渡帧丢弃，下一帧即新尺寸，宽高如实上报）。

#include "screen_session.h"  // 已含 capture_common.h（sparkcapLog 诊断路由）

#include <d3d11.h>
#include <dxgi.h>
#include <objbase.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.foundation.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <cstring>

namespace sparkcap {
namespace {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;

namespace wgc = ABI::Windows::Graphics::Capture;
namespace wgd = ABI::Windows::Graphics::DirectX;
namespace wgd3d = ABI::Windows::Graphics::DirectX::Direct3D11;
namespace wfnd = ABI::Windows::Foundation;

// FrameArrived → Win32 事件（free-threaded 池在线程池线程回调；SetEvent 纳秒级，
// 不做任何帧处理——取帧统一在采集线程 TryGetNextFrame）。
// FtmBase（自由线程封送器）：free-threaded 池要求事件处理器敏捷，
// 不混入则 add_FrameArrived 返回 RO_E_MUST_BE_AGILE（0x8000001C）。
class FrameArrivedHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::FtmBase,
          wfnd::ITypedEventHandler<wgc::Direct3D11CaptureFramePool*, IInspectable*>> {
public:
    explicit FrameArrivedHandler(HANDLE ev)
        : m_event(ev)
    {
    }

    IFACEMETHOD(Invoke)(wgc::IDirect3D11CaptureFramePool*, IInspectable*) override
    {
        SetEvent(m_event);
        return S_OK;
    }

private:
    HANDLE m_event;
};

// 窗口捕获后端（会话层经 IScreenBackend 驱动；采集线程独占访问）
class ScreenBackendWgc final : public IScreenBackend {
public:
    ScreenBackendWgc(uint64_t hwnd, const Host& host, uint32_t options)
        : m_hwnd(hwnd)
        , m_host(host)
        , m_options(options)
    {
    }

    int start() override;
    ScreenCapResult captureNext(DWORD waitMs) override;
    const void* frameData() const override { return m_buffer.data(); }
    int frameWidth() const override { return m_width; }
    int frameHeight() const override { return m_height; }
    void stop() override;

private:
    bool createStaging(int width, int height);
    void releaseAll();

    uint64_t m_hwnd;
    Host m_host;
    uint32_t m_options;  // sparkcap_screen_option_t（best-effort，Spec §4.7）

    ComPtr<wgd3d::IDirect3DDevice>     m_iD3dDevice;   // WinRT 侧设备（frame pool 用）
    ComPtr<wgc::IGraphicsCaptureItem>  m_item;
    ComPtr<wgc::IDirect3D11CaptureFramePool> m_pool;
    ComPtr<wgc::IGraphicsCaptureSession>     m_session;
    ComPtr<ID3D11Device>           m_device;    // 同一 D3D 设备的原生侧
    ComPtr<ID3D11DeviceContext>    m_context;
    ComPtr<ID3D11Texture2D>        m_staging;
    ComPtr<FrameArrivedHandler>    m_handler;
    EventRegistrationToken         m_frameArrivedToken = {};
    HANDLE                         m_frameEvent = nullptr;

    int m_width = 0;
    int m_height = 0;
    std::vector<char> m_buffer;  // 连续 BGRA（RGB32，库分配）
};

void ScreenBackendWgc::releaseAll()
{
    if (m_pool != nullptr && m_handler != nullptr)
        m_pool->remove_FrameArrived(m_frameArrivedToken);
    m_handler.Reset();
    if (m_frameEvent != nullptr) {
        CloseHandle(m_frameEvent);
        m_frameEvent = nullptr;
    }
    m_session.Reset();
    m_pool.Reset();
    m_item.Reset();
    m_iD3dDevice.Reset();
    m_staging.Reset();
    m_context.Reset();
    m_device.Reset();
    m_width = 0;
    m_height = 0;
    m_buffer.clear();
    m_buffer.shrink_to_fit();
}

bool ScreenBackendWgc::createStaging(int width, int height)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(m_device->CreateTexture2D(&td, nullptr,
                                               m_staging.ReleaseAndGetAddressOf()));
}

int ScreenBackendWgc::start()
{
    // COM/WinRT 就绪（借用应用线程已初始化的单元；RPC_E_CHANGED_MODE 容忍）。
    // 不配对 CoUninitialize：uninit 须与 init 同线程，而 stop 可能在别的线程；
    // MTA 为进程级单元，线程仅加入（facade 对 backend 生命周期线程同此模式）。
    const HRESULT cohr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(cohr) && cohr != RPC_E_CHANGED_MODE) {
        sparkcapLog(SPARKCAP_LOG_WARN, "screen", "wgc: CoInitializeEx hr=0x%08lX",
                    static_cast<unsigned long>(cohr));
        releaseAll();
        return SPARKCAP_ERR_START_FAILED;
    }

    // D3D11 设备（硬件优先，失败回退 WARP——虚拟机/远程会话）
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr,
                                   0, D3D11_SDK_VERSION, m_device.ReleaseAndGetAddressOf(),
                                   nullptr, m_context.ReleaseAndGetAddressOf());
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, m_device.ReleaseAndGetAddressOf(),
                               nullptr, m_context.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        sparkcapLog(SPARKCAP_LOG_WARN, "screen", "wgc: D3D11CreateDevice hr=0x%08lX",
                    static_cast<unsigned long>(hr));
        releaseAll();
        return SPARKCAP_ERR_START_FAILED;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_device.As(&dxgiDevice);
    ComPtr<IInspectable> inspectable;
    if (SUCCEEDED(hr))
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(),
                                                  inspectable.GetAddressOf());
    if (SUCCEEDED(hr) && inspectable != nullptr)
        hr = inspectable.As(&m_iD3dDevice);
    if (FAILED(hr) || m_iD3dDevice == nullptr) {
        sparkcapLog(SPARKCAP_LOG_WARN, "screen",
                    "wgc: CreateDirect3D11DeviceFromDXGIDevice hr=0x%08lX",
                    static_cast<unsigned long>(hr));
        releaseAll();
        return SPARKCAP_ERR_START_FAILED;
    }

    // capture item：<1903 无 IGraphicsCaptureItemInterop（E_NOINTERFACE）、
    // 策略禁用或窗口类型不支持 → START_FAILED（会话层回落 GDI PrintWindow）
    HStringReference itemCls(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem);
    ComPtr<IGraphicsCaptureItemInterop> interop;
    hr = RoGetActivationFactory(itemCls.Get(), IID_PPV_ARGS(interop.GetAddressOf()));
    if (SUCCEEDED(hr))
        hr = interop->CreateForWindow(reinterpret_cast<HWND>(m_hwnd),
                                      IID_PPV_ARGS(m_item.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || m_item == nullptr) {
        sparkcapLog(SPARKCAP_LOG_WARN, "screen", "wgc: CreateForWindow hr=0x%08lX",
                    static_cast<unsigned long>(hr));
        releaseAll();
        return SPARKCAP_ERR_START_FAILED;
    }

    ABI::Windows::Graphics::SizeInt32 size = {};
    hr = m_item->get_Size(&size);
    if (FAILED(hr) || size.Width <= 0 || size.Height <= 0) {
        sparkcapLog(SPARKCAP_LOG_WARN, "screen", "wgc: item size hr=0x%08lX (%dx%d)",
                    static_cast<unsigned long>(hr), size.Width, size.Height);
        releaseAll();
        return SPARKCAP_ERR_START_FAILED;  // 最小化/无效窗口等
    }

    // frame pool（free-threaded + 双缓冲 B8G8R8A8）
    HStringReference poolCls(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool);
    ComPtr<wgc::IDirect3D11CaptureFramePoolStatics2> poolStatics;
    hr = RoGetActivationFactory(poolCls.Get(), IID_PPV_ARGS(poolStatics.GetAddressOf()));
    if (SUCCEEDED(hr))
        hr = poolStatics->CreateFreeThreaded(
            m_iD3dDevice.Get(),
            wgd::DirectXPixelFormat_B8G8R8A8UIntNormalized,
            2, size, m_pool.ReleaseAndGetAddressOf());
    if (FAILED(hr) || m_pool == nullptr) {
        sparkcapLog(SPARKCAP_LOG_WARN, "screen", "wgc: CreateFreeThreaded hr=0x%08lX",
                    static_cast<unsigned long>(hr));
        releaseAll();
        return SPARKCAP_ERR_START_FAILED;
    }

    // 会话启动
    hr = m_pool->CreateCaptureSession(m_item.Get(), m_session.ReleaseAndGetAddressOf());
    if (FAILED(hr) || m_session == nullptr) {
        sparkcapLog(SPARKCAP_LOG_WARN, "screen", "wgc: CreateCaptureSession hr=0x%08lX",
                    static_cast<unsigned long>(hr));
        releaseAll();
        return SPARKCAP_ERR_START_FAILED;
    }

    // 选项（Spec §4.7 行为矩阵，best-effort）——须在 StartCapture 前设置：
    // CURSOR 未设（默认）→ IsCursorCaptureEnabled(FALSE)：WGC 默认把光标画进帧，
    // 而 DXGI/GDI 路径天然不含光标——不设置即违反"帧不含光标"契约；
    // 设 → TRUE（显式）。接口不存在（Win10）→ 跳过：帧含光标（文档如实标注）。
    ComPtr<wgc::IGraphicsCaptureSession2> session2;
    if (SUCCEEDED(m_session.As(&session2))) {
        hr = session2->put_IsCursorCaptureEnabled(
            (m_options & SPARKCAP_SCREEN_CURSOR) ? TRUE : FALSE);
        if (FAILED(hr))
            sparkcapLog(SPARKCAP_LOG_WARN, "screen",
                        "wgc: put_IsCursorCaptureEnabled hr=0x%08lX (cursor stays on)",
                        static_cast<unsigned long>(hr));
    } else {
        sparkcapLog(SPARKCAP_LOG_WARN, "screen",
                    "wgc: IGraphicsCaptureSession2 unavailable (Win10?): cursor stays on");
    }

    // BORDER 未设（默认）→ IsBorderRequired(FALSE)（Win11 官方黄框关闭路径）；
    // 失败静默保持默认（旧系统接口不存在/权限不足）；设 → 不调（系统默认黄框）。
    // 该属性在 IGraphicsCaptureSession3（SDK 契约版本 0xc0000+）。
    if ((m_options & SPARKCAP_SCREEN_BORDER) == 0) {
        ComPtr<wgc::IGraphicsCaptureSession3> session3;
        if (SUCCEEDED(m_session.As(&session3)))
            session3->put_IsBorderRequired(FALSE);  // best-effort：失败不报错
    }

    hr = m_session->StartCapture();
    if (FAILED(hr)) {
        sparkcapLog(SPARKCAP_LOG_WARN, "screen", "wgc: StartCapture hr=0x%08lX",
                    static_cast<unsigned long>(hr));
        releaseAll();
        return SPARKCAP_ERR_START_FAILED;
    }

    // FrameArrived → Win32 事件（auto-reset）
    m_frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_frameEvent == nullptr) {
        releaseAll();
        return SPARKCAP_ERR_START_FAILED;
    }
    m_handler = Microsoft::WRL::Make<FrameArrivedHandler>(m_frameEvent);
    if (m_handler == nullptr) {
        releaseAll();
        return SPARKCAP_ERR_NO_MEMORY;
    }
    hr = m_pool->add_FrameArrived(m_handler.Get(), &m_frameArrivedToken);
    if (FAILED(hr)) {
        sparkcapLog(SPARKCAP_LOG_WARN, "screen", "wgc: add_FrameArrived hr=0x%08lX",
                    static_cast<unsigned long>(hr));
        releaseAll();
        return SPARKCAP_ERR_START_FAILED;
    }

    if (!createStaging(size.Width, size.Height)) {
        sparkcapLog(SPARKCAP_LOG_WARN, "screen", "wgc: CreateTexture2D(staging) failed");
        releaseAll();
        return SPARKCAP_ERR_START_FAILED;
    }

    m_width = size.Width;
    m_height = size.Height;
    return SPARKCAP_ERR_OK;
}

void ScreenBackendWgc::stop()
{
    releaseAll();
}

ScreenCapResult ScreenBackendWgc::captureNext(DWORD waitMs)
{
    if (m_pool == nullptr)
        return ScreenCapResult::Lost;

    HANDLE handles[2] = { m_frameEvent, m_host.wakeEvent };
    const DWORD count = (m_host.wakeEvent != nullptr) ? 2 : 1;
    const DWORD wr = WaitForMultipleObjects(count, handles, FALSE, waitMs);
    if (wr != WAIT_OBJECT_0)
        return ScreenCapResult::Idle;  // 超时或 stop 唤醒（会话循环顶复查）

    ComPtr<wgc::IDirect3D11CaptureFrame> frame;
    if (FAILED(m_pool->TryGetNextFrame(frame.GetAddressOf())) || frame == nullptr)
        return ScreenCapResult::Idle;

    // resize（F19 帧尺寸跟随）：ContentSize 与池尺寸不同 → Recreate 池 + staging，
    // 过渡帧丢弃（下一帧即新尺寸，宽高如实上报）
    ABI::Windows::Graphics::SizeInt32 content = {};
    if (FAILED(frame->get_ContentSize(&content)))
        return ScreenCapResult::Idle;
    if (content.Width != m_width || content.Height != m_height) {
        if (content.Width > 0 && content.Height > 0 &&
            SUCCEEDED(m_pool->Recreate(m_iD3dDevice.Get(),
                                       wgd::DirectXPixelFormat_B8G8R8A8UIntNormalized,
                                       2, content)) &&
            createStaging(content.Width, content.Height)) {
            m_width = content.Width;
            m_height = content.Height;
        }
        return ScreenCapResult::Idle;
    }

    // Surface → ID3D11Texture2D → staging → Map 逐行拷出
    ComPtr<wgd3d::IDirect3DSurface> surface;
    if (FAILED(frame->get_Surface(surface.GetAddressOf())) || surface == nullptr)
        return ScreenCapResult::Idle;
    ComPtr<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
    if (FAILED(surface.As(&access)))
        return ScreenCapResult::Idle;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(tex.GetAddressOf()))))
        return ScreenCapResult::Idle;

    m_context->CopyResource(m_staging.Get(), tex.Get());
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &map)))
        return ScreenCapResult::Idle;
    m_buffer.assign(static_cast<size_t>(m_width) * 4 * static_cast<size_t>(m_height), 0);
    const char* src = static_cast<const char*>(map.pData);
    char* dst = m_buffer.data();
    const int wbytes = m_width * 4;
    for (int row = 0; row < m_height; ++row)
        memcpy(dst + static_cast<size_t>(wbytes) * row,
               src + static_cast<size_t>(map.RowPitch) * row,
               static_cast<size_t>(wbytes));
    m_context->Unmap(m_staging.Get(), 0);
    return ScreenCapResult::Frame;
}

} // namespace

std::unique_ptr<IScreenBackend> makeWgcBackend(const ScreenTarget& target,
                                               const IScreenBackend::Host& host,
                                               uint32_t options)
{
    if (target.kind != ScreenTarget::Kind::Window)
        return nullptr;
    try {
        return std::make_unique<ScreenBackendWgc>(target.hwnd, host, options);
    } catch (const std::bad_alloc&) {
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

} // namespace sparkcap
