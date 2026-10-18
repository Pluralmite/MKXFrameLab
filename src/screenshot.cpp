// MKXFrameLab - screenshots straight from the game's back buffer (with the overlay on it).
// Captured before Windows composes the frame, so HDR on the desktop does not wash them out.
#include "screenshot.h"
#include "log.h"
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <cstdio>

namespace shot {

static std::atomic<int> g_countdown{0};    // frames until capture (menus hidden meanwhile)
static std::mutex g_mtx;
static std::string g_last, g_status;
static ULONGLONG g_statusUntil = 0;

void Request() {
    if (g_countdown.load() == 0) g_countdown = 3;   // let the menus disappear from the frame first
}
bool HidingMenus() { return g_countdown.load() > 0; }
std::string Status() {
    std::lock_guard<std::mutex> l(g_mtx);
    return GetTickCount64() < g_statusUntil ? g_status : std::string();
}

static void SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> l(g_mtx);
    g_status = s;
    g_statusUntil = GetTickCount64() + 4000;
}

static std::atomic<bool> g_toGameDir{false};
void RequestToGameDir() { g_toGameDir = true; Request(); }
static std::wstring OutputDir() {
    std::wstring dir;
    if (g_toGameDir.exchange(false)) {   // diagnostics: next to the game, so the session tooling can read it
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        dir = p;
        dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\FrameLabShots";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
    PWSTR pics = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics)) && pics) {
        dir = std::wstring(pics) + L"\\MKX Frame Lab";
        CoTaskMemFree(pics);
    }
    if (dir.empty()) {
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        dir = p;
        dir = dir.substr(0, dir.find_last_of(L"\\/")) + L"\\Screenshots";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static bool WritePng(const std::wstring& path, const std::vector<uint8_t>& bgr, UINT w, UINT h) {
    bool ok = false;
    HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* f = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) break;
        if (FAILED(f->CreateStream(&st)) || FAILED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) break;
        if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(st, WICBitmapEncoderNoCache))) break;
        if (FAILED(enc->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr)) || FAILED(fr->SetSize(w, h))) break;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        if (FAILED(fr->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat24bppBGR) break;
        if (FAILED(fr->WritePixels(h, w * 3, (UINT)bgr.size(), (BYTE*)bgr.data()))) break;
        if (FAILED(fr->Commit()) || FAILED(enc->Commit())) break;
        ok = true;
    } while (false);
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (f) f->Release();
    if (SUCCEEDED(ci)) CoUninitialize();
    return ok;
}

void OnPresent(IDXGISwapChain* sc, ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    int c = g_countdown.load();
    if (c <= 0) return;
    if (c > 1) { g_countdown = c - 1; return; }
    g_countdown = 0;

    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) { SetStatus("Screenshot failed (no back buffer)"); return; }
    D3D11_TEXTURE2D_DESC d{};
    back->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    ID3D11Texture2D* stg = nullptr;
    if (d.SampleDesc.Count != 1 || FAILED(dev->CreateTexture2D(&sd, nullptr, &stg)) || !stg) {
        back->Release();
        SetStatus("Screenshot failed (format)");
        return;
    }
    ctx->CopyResource(stg, back);
    back->Release();
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m))) { stg->Release(); SetStatus("Screenshot failed (map)"); return; }
    UINT w = d.Width, h = d.Height;
    std::vector<uint8_t> bgr((size_t)w * h * 3);
    bool okFmt = true;
    for (UINT y = 0; y < h && okFmt; ++y) {
        const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
        uint8_t* o = &bgr[(size_t)y * w * 3];
        for (UINT x = 0; x < w; ++x) {
            switch (d.Format) {
            case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                o[x * 3] = row[x * 4 + 2]; o[x * 3 + 1] = row[x * 4 + 1]; o[x * 3 + 2] = row[x * 4];
                break;
            case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
                o[x * 3] = row[x * 4]; o[x * 3 + 1] = row[x * 4 + 1]; o[x * 3 + 2] = row[x * 4 + 2];
                break;
            case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
                uint32_t v = ((const uint32_t*)row)[x];
                o[x * 3 + 2] = (uint8_t)((v & 0x3FF) >> 2);
                o[x * 3 + 1] = (uint8_t)(((v >> 10) & 0x3FF) >> 2);
                o[x * 3] = (uint8_t)(((v >> 20) & 0x3FF) >> 2);
                break;
            }
            default: okFmt = false; break;
            }
        }
    }
    ctx->Unmap(stg, 0);
    stg->Release();
    if (!okFmt) { SetStatus("Screenshot failed (unsupported format " + std::to_string((int)d.Format) + ")"); return; }

    std::thread([bgr = std::move(bgr), w, h]() {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t name[64];
        swprintf(name, 64, L"\\MKXFrameLab_%04d-%02d-%02d_%02d-%02d-%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        std::wstring path = OutputDir() + name;
        bool ok = WritePng(path, bgr, w, h);
        char u8[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, u8, sizeof(u8), nullptr, nullptr);
        Log("screenshot %s: %s (%ux%u)", ok ? "saved" : "FAILED", u8, w, h);
        SetStatus(ok ? std::string("Screenshot saved to Pictures\\MKX Frame Lab") : std::string("Screenshot failed to save"));
        if (ok) { std::lock_guard<std::mutex> l(g_mtx); g_last = u8; }
    }).detach();
}

}  // namespace shot
