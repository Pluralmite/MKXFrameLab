// MKXFrameLab - input buffer probe + optional extended input buffer (XInput)
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ibuf {
struct ProbeRow {
    uint64_t pressTick;
    int framesEarly;      // >0: pressed this many ticks before the first free tick; 0 = on the free tick; <0 = after
    bool cameOut;         // a new attack started within 2 ticks of becoming free
    bool gameBuffered;    // game's own buffered-move slot was set after the press
    bool injected;        // our extended buffer re-pressed it
    uint16_t buttons;
};

void Init();                         // hooks XInputGetState
void OnTick(uint64_t tick, bool p1Valid, uint32_t p1StateFlags, uintptr_t p1Obj, bool newAttackStarted);
// settings
extern bool g_enabled;               // extended buffer on/off
extern int g_window;                 // frames
extern bool g_probe;
std::vector<ProbeRow> Rows();
int GameBufferSlot();                // last value of the game's buffered-move slot (-1 = none)
std::string Status();
// most recent attack button pressed on pad 1 (XInput bits) and the d-pad/stick direction bits held with it
void LastPress(uint16_t& buttons, uint16_t& dirs);
// numbered press history: PressSeq() = number of the latest press; Press(n) = that press (false if too old)
uint32_t PressSeq();
// the mod's own menu owns pad 1 while open: the game sees a neutral pad (and keeps seeing one until every
// button is released after closing, so the press that closed it does not reach the game's menu)
void SetCapture(bool on);
// input changes (extended buffer, scripted test input) are only allowed in practice mode
void SetAllowed(bool on);
void RealPad(uint16_t& buttons, int16_t& lx, int16_t& ly);
bool GetPress(uint32_t n, uint16_t& buttons, uint16_t& dirs);
}
