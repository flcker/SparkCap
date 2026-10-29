// SparkCap 内部：屏幕域 DXGI Desktop Duplication 后端（src/screen/ 私有头）。
// 域独立会话——不继承 camera 的 ICaptureSession，facade 的 screen_* 直接调用；
// 复用 common 设施（FrameQueue/CallbackGuard/日志路由）与 camera 会话的行为模式。
// 裁剪构建（SPARKCAP_ENABLE_SCREEN=OFF）由 backend_dxgi_stub.cpp 提供同形桩。

#pragma once

#include "common/capture_common.h"
#include "common/frame_queue.h"

#include "sparkcap_types.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace sparkcap {

// F12：枚举桌面输出（两段式拆分在 facade，此处返回全量列表，按 adapter/output 序）。
int screenListDisplays(std::vector<sparkcap_display_info_t>& out);

// 屏幕采集会话：一台已打开显示器的采集控制（DXGI Desktop Duplication）。
// 语义契约（技术选型 §10.5）：静止桌面 getFrame 返回 TIMEOUT 为正常语义；
// 分辨率/格式不可协商（start 无偏好参数）；帧不含鼠标光标；整帧交付。
class SparkCapScreenSession {
public:
    SparkCapScreenSession(int displayId,
                          Microsoft::WRL::ComPtr<IDXGIOutput1> output,
                          Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter);
    ~SparkCapScreenSession();

    SparkCapScreenSession(const SparkCapScreenSession&) = delete;
    SparkCapScreenSession& operator=(const SparkCapScreenSession&) = delete;

    // F14：启动（无偏好参数：分辨率/格式由显示器决定）
    int start();

    // F5（须在 start 前；Spec §4.5）
    int setCallback(sparkcap_device_t* device, sparkcap_frame_cb cb, void* userdata);

    // Spec §4.5（须在 start 前）
    int setErrorCallback(sparkcap_device_t* device, sparkcap_error_cb cb, void* userdata);

    // F14（静止桌面 TIMEOUT 为正常语义）
    int getFrame(sparkcap_frame_t* outFrame, int timeoutMs);

    // F6
    int stop();

private:
    // 队列槽：连续 RGB32 缓冲（库分配、库释放，Spec §4.3）
    struct ScreenQueueSlot {
        std::vector<char> data;
        sparkcap_frame_t frame = {};  // data 指向本缓冲
    };

    int  createChainLocked();  // start/重建共用：D3D 链 → duplication → staging
    void releaseChainLocked();
    int  rebuildChain();       // 重枚举定位 output 后重建（拓扑/模式可能已变化）
    bool popFrameLocked(sparkcap_frame_t* outFrame);
    void invokeCallbackLocked();
    void captureThread();
    void copyFrameToQueue(Microsoft::WRL::ComPtr<IDXGIResource> desktop);
    bool recoverFromLoss();   // ACCESS_LOST：错误回调 + 重建（1s 退避，stop 可中断）
    static int64_t now100ns();  // QPC → 100ns（与 camera 域时间戳同源同单位）

    // ---- DXGI/D3D 采集链（open 定位 output，start 建链，stop/重建释放）----
    int m_displayId;
    Microsoft::WRL::ComPtr<IDXGIAdapter1>          m_adapter;
    Microsoft::WRL::ComPtr<IDXGIOutput1>           m_output;
    Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        m_staging;

    HANDLE m_frameEvent = nullptr;  // auto-reset：新帧/stop 唤醒 getFrame
    HANDLE m_wakeEvent = nullptr;   // auto-reset：stop 中断采集线程退避等待
    std::thread m_thread;
    std::mutex m_stopMutex;  // start/stop 全程互斥（并发调用串行化，join 安全）

    std::mutex m_mutex;
    sparkcap_frame_t m_frameDesc = {};  // 格式骨架（start/重建时定，槽帧复用）
    FrameQueue<ScreenQueueSlot> m_queue{8};  // 环形队列（满丢最旧，N2 同构）
    ScreenQueueSlot m_consumed;               // 当前交付应用的帧
    std::atomic<bool> m_running{false};
    bool m_streamFailed = false;  // 输出永久消失（拔除）：getFrame 返回 STREAM_LOST

    sparkcap_device_t* m_callbackDevice = nullptr;  // 弱引用（生命周期 ⊂ device）
    sparkcap_frame_cb  m_frameCallback = nullptr;
    void*              m_callbackUserdata = nullptr;

    sparkcap_device_t* m_errorDevice = nullptr;  // 错误回调（Spec §4.5）
    sparkcap_error_cb  m_errorCallback = nullptr;
    void*              m_errorUserdata = nullptr;
};

// F13：按 displayId 创建屏幕会话（内部重枚举定位 output，不存在返回 NOT_FOUND）。
int screenOpenSession(int displayId, std::unique_ptr<SparkCapScreenSession>* out);

} // namespace sparkcap
