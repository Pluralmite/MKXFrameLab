#pragma once
struct UiState {
    bool classicMeter = false;   // old plain ImGui meter instead of the game-styled panel
    float hudScale = 1.f;
    bool menuOpen = false;       // advanced settings window (ImGui)
    bool gameMenu = false;       // F2: the game-styled FRAME LAB menu (owns pad / keyboard while open)
    int gmSel = 0;
    bool showMeter = true;
    bool showP2Panel = false;
    bool showBoxes = true;
    bool showHurt = true, showHit = true, showThrow = true, showPush = false, showOther = true;
    bool fill = true;
    float hitAlpha = 1.f;        // opacity of hitboxes / projectile shapes (0.1 - 4, 1 = default)
    float hurtAlpha = 1.f;       // opacity of hurtboxes
    bool showMarkers = false;
    bool showExplorer = false;
    bool showWatches = false;
    bool showBones = false;
    bool showHurtPts = true;
    bool showStrike = true;
    int strikeLinger = 8;
    bool showShapes = true;
    bool shapesInactive = false;
    bool hurtLabels = false;
    float zeroRadius = 0.f;
    bool boneLabels = false;
    float boneRadius = 0.f;
    bool showCollList = false;
    int recTicks = 240;
    char recLabel[64] = "move";
};
extern UiState g_ui;
struct ImFont;
extern ImFont* g_hudFont;
extern ImFont* g_uiFont;     // game-like sans (Bahnschrift) for labels / menu
extern ImFont* g_valueFont;  // large numbers
extern ImFont* g_menuFont;   // FRAME LAB menu items (game pause-menu size)
bool UiCapturesInput();

void UiHotkeys();
void UiDraw(float screenW, float screenH);

extern volatile bool g_reloadRequested;   // polled by the loader (hot reload)
