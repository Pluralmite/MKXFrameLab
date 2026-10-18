// MKXFrameLab - keeps keyboard presses away from the game (DirectInput) while the mod's own menu is open.
// The game reads the keyboard through DirectInput 8; every keyboard device shares dinput8's implementation,
// so hooking GetDeviceState / GetDeviceData of a throwaway device of ours covers the game's devices too.
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <atomic>
#include <mutex>
#include <map>
#include <cstring>
#include "MinHook.h"
#include "log.h"

namespace dib {

static std::atomic<bool> g_block{false};
void SetBlock(bool on) { g_block = on; }

using GetStateFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
static GetStateFn o_state[2];
static GetDataFn o_data[2];
static void* g_hooked[4];

static bool IsKeyboard(void* dev, int wide) {
    static std::mutex mtx;
    static std::map<void*, bool> known;
    std::lock_guard<std::mutex> l(mtx);
    auto it = known.find(dev);
    if (it != known.end()) return it->second;
    bool kb = false;
    DIDEVCAPS caps{}; caps.dwSize = sizeof(caps);   // same layout for the A and W interfaces
    if (SUCCEEDED(((IDirectInputDevice8W*)dev)->GetCapabilities(&caps))) kb = GET_DIDEVICE_TYPE(caps.dwDevType) == DI8DEVTYPE_KEYBOARD;
    (void)wide;
    known[dev] = kb;
    return kb;
}

template <int W>
static HRESULT STDMETHODCALLTYPE h_state(void* dev, DWORD cb, LPVOID data) {
    HRESULT hr = o_state[W](dev, cb, data);
    if (SUCCEEDED(hr) && g_block && data && cb == 256) memset(data, 0, 256);   // keyboard state: all keys up
    return hr;
}
template <int W>
static HRESULT STDMETHODCALLTYPE h_data(void* dev, DWORD cbObj, LPDIDEVICEOBJECTDATA data, LPDWORD inOut, DWORD flags) {
    HRESULT hr = o_data[W](dev, cbObj, data, inOut, flags);
    if (SUCCEEDED(hr) && g_block && data && inOut && *inOut && cbObj >= sizeof(DIDEVICEOBJECTDATA) && IsKeyboard(dev, W)) {
        // drop key-downs, keep key-ups (so nothing stays stuck down in the game)
        DWORD kept = 0;
        for (DWORD i = 0; i < *inOut; ++i) {
            auto* e = (LPDIDEVICEOBJECTDATA)((uint8_t*)data + (size_t)i * cbObj);
            if (e->dwData & 0x80) continue;
            if (kept != i) memcpy((uint8_t*)data + (size_t)kept * cbObj, e, cbObj);
            ++kept;
        }
        *inOut = kept;
    }
    return hr;
}

void Init() {
    HMODULE m = LoadLibraryW(L"dinput8.dll");
    using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto create = m ? (CreateFn)GetProcAddress(m, "DirectInput8Create") : nullptr;
    if (!create) { Log("dinput: DirectInput8Create not found"); return; }
    int hooks = 0;
    for (int w = 0; w < 2; ++w) {
        void* di = nullptr;
        if (FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, w ? IID_IDirectInput8W : IID_IDirectInput8A, &di, nullptr)) || !di) continue;
        void* dev = nullptr;
        HRESULT hr = w ? ((IDirectInput8W*)di)->CreateDevice(GUID_SysKeyboard, (LPDIRECTINPUTDEVICE8W*)&dev, nullptr)
                       : ((IDirectInput8A*)di)->CreateDevice(GUID_SysKeyboard, (LPDIRECTINPUTDEVICE8A*)&dev, nullptr);
        if (SUCCEEDED(hr) && dev) {
            void** vt = *(void***)dev;
            void* fs = vt[9];
            void* fd = vt[10];
            bool dupS = false, dupD = false;
            for (void* h : g_hooked) { if (h == fs) dupS = true; if (h == fd) dupD = true; }
            if (!dupS && MH_CreateHook(fs, w ? (LPVOID)h_state<1> : (LPVOID)h_state<0>, (LPVOID*)&o_state[w]) == MH_OK) { g_hooked[w * 2] = fs; ++hooks; }
            else if (dupS) o_state[w] = o_state[0];
            if (!dupD && MH_CreateHook(fd, w ? (LPVOID)h_data<1> : (LPVOID)h_data<0>, (LPVOID*)&o_data[w]) == MH_OK) { g_hooked[w * 2 + 1] = fd; ++hooks; }
            else if (dupD) o_data[w] = o_data[0];
            ((IUnknown*)dev)->Release();
        }
        ((IUnknown*)di)->Release();
    }
    MH_EnableHook(MH_ALL_HOOKS);
    Log("dinput: keyboard block hooks %d", hooks);
}

}  // namespace dib
