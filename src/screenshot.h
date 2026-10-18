// MKXFrameLab - back-buffer screenshots (HDR-safe)
#pragma once
#include <string>
struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace shot {
void Request();
void RequestToGameDir();      // same, saved to <game>\\FrameLabShots (diagnostics)                 // take one on the next frames (menus hidden for it)
bool HidingMenus();             // true while a capture is pending: don't draw the menus
void OnPresent(IDXGISwapChain* sc, ID3D11Device* dev, ID3D11DeviceContext* ctx);   // after the overlay is drawn
std::string Status();           // short message for a few seconds after a capture ("" otherwise)
}
