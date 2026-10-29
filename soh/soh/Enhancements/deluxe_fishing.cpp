// deluxe_fishing.cpp
// "Fishing Deluxe": swamp-style second pond + Fish Index (120 species) for Ship of Harkinian.
//
// HOW TO HOOK IT UP:
//  1) z_fishing.c edits (5 small ones, see chat): declarations, OnFishInit, BeforeDraw, OnFishCaught,
//     and the extra-fish spawn loop.
//  2) In soh/soh/SohGui/SohGui.cpp, inside SetupGuiElements(), right after
//     gui->AddGuiWindow(mStatsWindow);  add:
//         DeluxeFishing_Init();
//         gui->AddGuiWindow(DeluxeFishing_CreateIndexWindow());
//     and declare both functions above `namespace SohGui {`.
//
// NOTE: the "second pond" is the lily-pad cove inside the vanilla Fishing Pond scene. Fish that live
// there are the 120 deluxe species; the rest of the pond stays vanilla. A fully new map would need
// custom scene assets, which can't be written as plain code.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include <imgui.h>

#include <libultraship/libultraship.h>
#include "soh/cvar_prefixes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
}


enum class Rarity : uint8_t { Common, Uncommon, Rare, Epic, Legendary };

struct FishDef {
    uint16_t id;
    std::string name;
    Rarity rarity;
    float minLen, maxLen; // cm
    uint8_t r, g, b;      // tint for the fish model
};

struct Progress {
    uint32_t count = 0;
    float best = 0.0f;
};

static const char* kRarityName[] = { "Common", "Uncommon", "Rare", "Epic", "Legendary" };
static const ImVec4 kRarityColor[] = {
    { 0.80f, 0.80f, 0.80f, 1 }, { 0.40f, 0.90f, 0.40f, 1 }, { 0.40f, 0.60f, 1.00f, 1 },
    { 0.75f, 0.40f, 1.00f, 1 }, { 1.00f, 0.80f, 0.20f, 1 },
};
static const int kTierWeight[] = { 55, 25, 12, 6, 2 };

static const char* kSaveFile = "deluxe_fishing_index.txt"; // change to your save dir if you like

static std::vector<FishDef> sFish;
static std::vector<Progress> sProg;
static std::mt19937 sRng{ std::random_device{}() };
static bool sEnabled = true;       // player toggle: use swamp pond (on by default)
static bool sMissingOnly = false;
static bool sWholePond = true;    // testing: every fish in the pond is a deluxe fish (untick = swamp cove only)
static std::string sToast;
static double sToastUntil = 0.0;

static void HsvToRgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
    h = h - (int)h;
    int i = (int)(h * 6);
    float f = h * 6 - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    float R, G, B;
    switch (i % 6) {
        case 0: R = v; G = t; B = p; break;
        case 1: R = q; G = v; B = p; break;
        case 2: R = p; G = v; B = t; break;
        case 3: R = p; G = q; B = v; break;
        case 4: R = t; G = p; B = v; break;
        default: R = v; G = p; B = q; break;
    }
    r = (uint8_t)(R * 255); g = (uint8_t)(G * 255); b = (uint8_t)(B * 255);
}

static void BuildTable() {
    // 15 base fish x 8 swamp variants = 120 species
    static const struct { const char* n; float len; } bases[] = {
        { "Carp", 45 },    { "Catfish", 60 }, { "Eel", 80 },      { "Perch", 25 },    { "Pike", 90 },
        { "Gar", 100 },    { "Bass", 40 },    { "Loach", 35 },    { "Tench", 35 },    { "Bowfin", 65 },
        { "Minnow", 8 },   { "Sturgeon", 120 }, { "Piranha", 30 }, { "Lungfish", 70 }, { "Gudgeon", 15 },
    };
    static const struct { const char* n; Rarity r; float s, v; } mods[] = {
        { "Mud", Rarity::Common, 0.45f, 0.55f },     { "Reed", Rarity::Common, 0.50f, 0.75f },
        { "Lily", Rarity::Uncommon, 0.60f, 0.85f },  { "Mossback", Rarity::Uncommon, 0.70f, 0.60f },
        { "Bog", Rarity::Rare, 0.80f, 0.65f },       { "Glow", Rarity::Rare, 0.90f, 1.00f },
        { "Ghost", Rarity::Epic, 0.15f, 1.00f },     { "Golden", Rarity::Legendary, 0.90f, 1.00f },
    };
    uint16_t id = 0;
    for (auto& m : mods) {
        for (int bi = 0; bi < 15; bi++) {
            const auto& b = bases[bi];
            FishDef f;
            f.id = id++;
            f.name = std::string(m.n) + " " + b.n;
            f.rarity = m.r;
            // each base fish gets its own hue; golden ones stay in the gold range
            float hue = (m.r == Rarity::Legendary) ? 0.10f + 0.01f * bi : bi / 15.0f;
            HsvToRgb(hue, m.s, m.v, f.r, f.g, f.b);
            float scale = 1.0f + 0.15f * (int)m.r;
            f.minLen = b.len * 0.6f * scale;
            f.maxLen = b.len * 1.4f * scale;
            sFish.push_back(f);
        }
    }
    sProg.assign(sFish.size(), Progress{});
}

static void Save() {
    std::ofstream o(kSaveFile, std::ios::trunc);
    for (size_t i = 0; i < sProg.size(); i++) {
        if (sProg[i].count) o << i << ' ' << sProg[i].count << ' ' << sProg[i].best << '\n';
    }
}

static void Load() {
    std::ifstream in(kSaveFile);
    size_t i; uint32_t c; float b;
    while (in >> i >> c >> b) {
        if (i < sProg.size()) sProg[i] = { c, b };
    }
}

static void EnsureInit() {
    if (!sFish.empty()) return;
    BuildTable();
    Load();
}

static size_t RollSpecies() {
    std::discrete_distribution<int> tierDist(std::begin(kTierWeight), std::end(kTierWeight));
    Rarity tier = (Rarity)tierDist(sRng);
    std::vector<size_t> pool;
    for (size_t i = 0; i < sFish.size(); i++) {
        if (sFish[i].rarity == tier) pool.push_back(i);
    }
    return pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(sRng)];
}

// ---- The "deluxe pond": the lily-pad cove at the far end of the pond, opposite the waterfall stream ----
static const float kCoveX = -560.0f, kCoveZ = -450.0f, kCoveRadius = 190.0f;

static bool InCove(float x, float z) {
    float dx = x - kCoveX, dz = z - kCoveZ;
    return (dx * dx + dz * dz) <= (kCoveRadius * kCoveRadius);
}

// The species lives on the fish actor itself: home.rot.x = species + 1 (0 = plain vanilla fish),
// home.rot.z = 1 means "re-roll once released". The vanilla fish code uses neither field.

// Call once when a pond fish is created. Fish that spawn inside the cove become deluxe fish.
extern "C" void DeluxeFishing_OnFishInit(Actor* a) {
    EnsureInit();
    a->home.rot.z = 0;
    if (sEnabled && (sWholePond || InCove(a->world.pos.x, a->world.pos.z))) {
        a->home.rot.x = (int16_t)(RollSpecies() + 1);
    } else {
        a->home.rot.x = 0;
    }
}

// Call every time a fish is drawn: tints deluxe fish, and re-rolls a released fish.
extern "C" void DeluxeFishing_BeforeDraw(Actor* a, PlayState* play) {
    if (a->home.rot.x <= 0 || a->home.rot.x > (int16_t)sFish.size()) return;

    // caught and released: new species once it is back under water
    if (a->home.rot.z == 1 && a->world.pos.y < play->colCtx.colHeader->waterBoxes->ySurface - 5.0f) {
        a->home.rot.x = (int16_t)(RollSpecies() + 1);
        a->home.rot.z = 0;
    }

    const FishDef& f = sFish[a->home.rot.x - 1];

    // Tint 1: primitive/environment color (works if the fish material uses them)
    OPEN_DISPS(play->state.gfxCtx);
    gDPSetPrimColor(POLY_OPA_DISP++, 0, 0, f.r, f.g, f.b, 255);
    gDPSetEnvColor(POLY_OPA_DISP++, f.r, f.g, f.b, 255);
    CLOSE_DISPS(play->state.gfxCtx);

    // Tint 2: the fish model is lit, so light it with the species color (ambient only, no directional lights)
    auto lift = [](uint8_t c) { return (uint8_t)(70 + (c * 185) / 255); };
    Lights* lights = Lights_New(play->state.gfxCtx, lift(f.r), lift(f.g), lift(f.b));
    Lights_Draw(lights, play->state.gfxCtx);
}

// Call when a fish is landed. Records the species in the index. Returns its name (or NULL for vanilla fish).
extern "C" const char* DeluxeFishing_OnFishCaught(Actor* a, float lengthCm) {
    if (a->home.rot.x <= 0 || a->home.rot.x > (int16_t)sFish.size()) return nullptr;
    size_t idx = (size_t)(a->home.rot.x - 1);
    const FishDef& f = sFish[idx];
    float t = std::clamp((lengthCm - 30.0f) / 40.0f, 0.0f, 1.0f);
    float len = f.minLen + t * (f.maxLen - f.minLen);

    Progress& p = sProg[idx];
    bool isNew = (p.count == 0);
    p.count++;
    p.best = std::max(p.best, len);
    Save();
    a->home.rot.z = 1; // re-roll species after release

    char buf[160];
    snprintf(buf, sizeof(buf), "%sCaught: %s (%.0f cm) [%s]", isNew ? "NEW! " : "", f.name.c_str(), len,
             kRarityName[(int)f.rarity]);
    sToast = buf;
    sToastUntil = ImGui::GetTime() + 5.0;
    return f.name.c_str();
}

// Extra fish that live in the cove. Call with i = 0, 1, 2... until it returns 0.
// Each one reuses the stats of a vanilla (non-loach) fish through 'param' (100 + that fish's number).
extern "C" int DeluxeFishing_ExtraFish(int i, float* x, float* y, float* z, int16_t* param) {
    static const struct { int16_t x, z, idx; } kExtra[] = {
        { -501, -440, 0 },
        { -473, -360, 1 },
        { -552, -391, 2 },
        { -619, -340, 3 },
        { -614, -424, 4 },
        { -683, -472, 5 },
        { -602, -493, 6 },
        { -577, -574, 7 },
        { -532, -503, 10 },
        { -448, -505, 11 },
    };
    if (!sEnabled || i < 0 || i >= (int)(sizeof(kExtra) / sizeof(kExtra[0]))) return 0;
    *x = kExtra[i].x;
    *y = -45.0f;
    *z = kExtra[i].z;
    *param = (int16_t)(100 + kExtra[i].idx);
    return 1;
}

void DeluxeFishing_Init() {
    EnsureInit();
}

static void DrawToast() {
    if (ImGui::GetTime() < sToastUntil) {
        ImGui::SetNextWindowPos(ImVec2(20, 20));
        ImGui::SetNextWindowBgAlpha(0.7f);
        ImGui::Begin("##fishtoast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextUnformatted(sToast.c_str());
        ImGui::End();
    }
}

static void DrawIndexContents() {
    int caught = 0;
    for (auto& p : sProg) caught += p.count > 0;
    int total = (int)sFish.size();

    ImGui::Text("Caught: %d / %d    Missing: %d", caught, total, total - caught);
    ImGui::ProgressBar((float)caught / total, ImVec2(-1, 0));
    ImGui::Checkbox("Deluxe fish on", &sEnabled);
    ImGui::SameLine();
    ImGui::Checkbox("Whole pond (untick = swamp cove only)", &sWholePond);
    ImGui::Checkbox("Show missing only", &sMissingOnly);
    ImGui::TextDisabled("Leave and re-enter the pond after changing the first two.");

    if (ImGui::BeginTable("fishtable", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Species");
        ImGui::TableSetupColumn("Rarity");
        ImGui::TableSetupColumn("Caught");
        ImGui::TableSetupColumn("Best (cm)");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < sFish.size(); i++) {
            const Progress& p = sProg[i];
            if (sMissingOnly && p.count > 0) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%03d", (int)i + 1);
            ImGui::TableNextColumn();
            if (p.count) {
                ImGui::TextColored(ImVec4(sFish[i].r / 255.f, sFish[i].g / 255.f, sFish[i].b / 255.f, 1), "%s",
                                   sFish[i].name.c_str());
            } else {
                ImGui::TextUnformatted("???");
            }
            ImGui::TableNextColumn();
            ImGui::TextColored(kRarityColor[(int)sFish[i].rarity], "%s", kRarityName[(int)sFish[i].rarity]);
            ImGui::TableNextColumn(); ImGui::Text("%u", p.count);
            ImGui::TableNextColumn();
            if (p.count) ImGui::Text("%.0f", p.best); else ImGui::TextUnformatted("-");
        }
        ImGui::EndTable();
    }
}

// The Fish Index as a regular SoH window (the wrapper handles Begin/End and the close button).
class FishIndexWindow final : public Ship::GuiWindow {
  public:
    using Ship::GuiWindow::GuiWindow;
    void InitElement() override {}
    void UpdateElement() override {}
    void DrawElement() override {
        EnsureInit();
        DrawToast();
        DrawIndexContents();
    }
};

std::shared_ptr<Ship::GuiWindow> DeluxeFishing_CreateIndexWindow() {
    auto w = std::make_shared<FishIndexWindow>(CVAR_WINDOW("FishIndex"), "Fish Index##SoH", ImVec2(520, 560));
    w->Show(); // opens on every launch while testing; close it with the X
    return w;
}
