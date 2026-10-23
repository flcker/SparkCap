// qedit.h 已从新版 Windows SDK 移除，此处内嵌 ISampleGrabber 最小声明（src/ 私有头）。
// 声明与旧版 um/qedit.h 一致；实现位于 qedit.dll（x64/x86 均随系统部署），
// IID 符号在 strmiids.lib。CLSID 值已经本机注册表验证。

#pragma once

#include <dshow.h>  // AM_MEDIA_TYPE、IMediaSample

// {0579154A-2B53-4994-B0D0-E773148EFF85}
MIDL_INTERFACE("0579154A-2B53-4994-B0D0-E773148EFF85")
ISampleGrabberCB : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime, IMediaSample* pSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE* pBuffer,
                                               long BufferLen) = 0;
};

// {6B652FFF-11FE-4FCE-92AD-0266B5D7C78F}
MIDL_INTERFACE("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F")
ISampleGrabber : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* pBufferSize, long* pBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** ppSample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* pCallback,
                                                  LONG WhichMethodToCallback) = 0;
};

// {C1F400A0-3F08-11D3-9F0B-006008039E37}
class DECLSPEC_UUID("C1F400A0-3F08-11D3-9F0B-006008039E37") SampleGrabber;
