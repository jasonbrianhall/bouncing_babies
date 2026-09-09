// Baby Bounce Rescue — Copyright 2026 Jason Brian Hall
// Building is fixed on the LEFT, ambulance fixed on the RIGHT.
// A single two-man firefighter team moves between 3 zones (near building,
// mid-screen, near ambulance). A baby falls from the building, and each
// successful catch bounces it one zone further right, until the final
// bounce carries it into the ambulance.
//
// Build: g++ main.cpp -o bouncing_babies `sdl2-config --cflags --libs` -lSDL2_ttf -std=c++17

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "font_data.h"
#include <vector>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <string>
#include <algorithm>
#include <fstream>
#include <sstream>

static const int SCREEN_W = 800;
static const int SCREEN_H = 600;
static const int GROUND_Y = 520;

// The 3 catch zones the firefighter team can occupy, spanning the
// whole width of the screen: near the building, mid-screen, near the ambulance.
static const int ZONE_X[3] = { 230, 440, 630 };

static const int BUILDING_X = 30;
static const int BUILDING_W = 130;
static const int WINDOW_X   = BUILDING_X + BUILDING_W - 50;
static const int WINDOW_Y[4] = { 90, 170, 250, 330 };

static const int AMBULANCE_X = 700;

enum class BabyState { Falling, Bounce1, Bounce2, Bounce3, Delivering, Gone, Splat };

struct Baby {
    float x, y;
    float vx = 0.f, vy = 0.f;
    BabyState state = BabyState::Falling;
    int targetZone = 0;    // zone index the current flight is heading toward
    float splatTimer = 0.f;
};

struct Firefighters {
    int zone = 0;
    float x, y;
};

static SDL_Window* gWindow = nullptr;
static SDL_Renderer* gRenderer = nullptr;
static TTF_Font* gFont = nullptr;
static TTF_Font* gFontBig = nullptr;

static SDL_AudioDeviceID gAudioDev = 0;
static SDL_AudioSpec gAudioSpec;

// Current oversampling factor the loaded fonts were rendered at. When the
// window is bigger than the logical SCREEN_W x SCREEN_H canvas, we reload
// the fonts at a proportionally larger point size so the glyph textures
// have enough pixels to stay sharp once SDL's logical-size scaling stretches
// them up to the real window resolution, instead of just blowing up a
// small, blurry texture.
static float gFontRenderScale = 1.0f;
static const int FONT_BASE_SIZE = 22;
static const int FONT_BASE_SIZE_BIG = 48;

void loadFontsAtScale(float scale) {
    scale = std::clamp(scale, 0.5f, 4.0f);
    if (gFont) { TTF_CloseFont(gFont); gFont = nullptr; }
    if (gFontBig) { TTF_CloseFont(gFontBig); gFontBig = nullptr; }
    int sz = std::max(1, (int)std::lround(FONT_BASE_SIZE * scale));
    int szBig = std::max(1, (int)std::lround(FONT_BASE_SIZE_BIG * scale));
    SDL_RWops* fontRW1 = SDL_RWFromConstMem(bb_font_data, (int)bb_font_data_len);
    gFont = TTF_OpenFontRW(fontRW1, 1 /*freesrc*/, sz);
    SDL_RWops* fontRW2 = SDL_RWFromConstMem(bb_font_data, (int)bb_font_data_len);
    gFontBig = TTF_OpenFontRW(fontRW2, 1 /*freesrc*/, szBig);
    gFontRenderScale = scale;
}

void playTone(float startFreq, float endFreq, float durationSec, float volume = 0.25f) {
    if (gAudioDev == 0) return;
    int sampleRate = gAudioSpec.freq;
    int n = (int)(durationSec * sampleRate);
    std::vector<Sint16> buf(n);
    for (int i = 0; i < n; i++) {
        float tNorm = (float)i / n;
        float freq = startFreq + (endFreq - startFreq) * tNorm;
        float phase = 2.0f * (float)M_PI * freq * ((float)i / sampleRate);
        float sample = std::sin(phase) >= 0 ? 1.0f : -1.0f; // PC-speaker-ish square wave
        float env = 1.0f - tNorm;
        buf[i] = (Sint16)(sample * volume * 32767 * env);
    }
    SDL_QueueAudio(gAudioDev, buf.data(), n * sizeof(Sint16));
}

void playBoing()   { playTone(300, 500, 0.10f); }
void playSplat()   { playTone(180, 60, 0.30f, 0.35f); }
void playDeliver() { playTone(500, 900, 0.15f, 0.2f); }
void playGameOver(){ playTone(400, 100, 0.8f, 0.3f); }

void fillRect(int x, int y, int w, int h, SDL_Color c) {
    SDL_SetRenderDrawColor(gRenderer, c.r, c.g, c.b, c.a);
    SDL_Rect r{ x, y, w, h };
    SDL_RenderFillRect(gRenderer, &r);
}

void fillCircle(int cx, int cy, int r, SDL_Color c); // fwd decl - used by drawBuilding/drawFirefighters below

void drawText(const std::string& s, int x, int y, SDL_Color c, TTF_Font* font, bool center = false) {
    if (!font) return;
    SDL_Surface* surf = TTF_RenderText_Blended(font, s.c_str(), c);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(gRenderer, surf);
    // The texture may have been rendered at gFontRenderScale x the logical
    // point size (see loadFontsAtScale); shrink the destination rect back
    // down to logical units so layout is unaffected - SDL's own logical-size
    // scaling then stretches it back up to real pixels using the extra
    // source detail instead of upscaling a small texture.
    int dstW = (int)std::lround(surf->w / gFontRenderScale);
    int dstH = (int)std::lround(surf->h / gFontRenderScale);
    SDL_Rect dst{ x, y, dstW, dstH };
    if (center) dst.x -= dstW / 2;
    SDL_RenderCopy(gRenderer, tex, nullptr, &dst);
    SDL_FreeSurface(surf);
    SDL_DestroyTexture(tex);
}

// ---- High scores ----
// Persisted as plain text under the OS's normal per-user data directory via
// SDL_GetPrefPath, which resolves to the right place on each platform on its
// own (e.g. ~/.local/share/BouncingBabies/ on Linux, %APPDATA%\BouncingBabies\
// on Windows, ~/Library/Application Support/BouncingBabies/ on macOS) so
// there's no manual HOME/APPDATA guessing here. Each entry is written as two
// lines (score, then name) rather than one delimited line, so a name can
// contain spaces or punctuation without needing to escape anything.
static const int MAX_HIGH_SCORES = 10;
static const size_t MAX_NAME_LEN = 16;

struct HighScoreEntry {
    std::string name;
    int score;
};
static std::vector<HighScoreEntry> gHighScores;

std::string highScoreFilePath() {
    char* pref = SDL_GetPrefPath("", "BouncingBabies");
    std::string path = pref ? (std::string(pref) + "highscores.txt") : "highscores.txt";
    if (pref) SDL_free(pref);
    return path;
}

void sortAndTrimHighScores() {
    std::sort(gHighScores.begin(), gHighScores.end(),
        [](const HighScoreEntry& a, const HighScoreEntry& b) { return a.score > b.score; });
    if (gHighScores.size() > (size_t)MAX_HIGH_SCORES) gHighScores.resize(MAX_HIGH_SCORES);
}

void loadHighScores() {
    gHighScores.clear();
    std::ifstream in(highScoreFilePath());
    std::string scoreLine, nameLine;
    while (std::getline(in, scoreLine) && std::getline(in, nameLine)) {
        try {
            gHighScores.push_back({ nameLine, std::stoi(scoreLine) });
        } catch (...) { /* skip a malformed/corrupted entry rather than crash */ }
    }
    sortAndTrimHighScores();
}

void saveHighScores() {
    std::ofstream out(highScoreFilePath(), std::ios::trunc);
    for (const auto& e : gHighScores) out << e.score << "\n" << e.name << "\n";
}

// True if `score` would land somewhere in the top MAX_HIGH_SCORES - used to
// decide whether the player is prompted to enter their name at all.
bool qualifiesForHighScore(int score) {
    if (gHighScores.size() < (size_t)MAX_HIGH_SCORES) return true;
    return score > gHighScores.back().score;
}

// Inserts a name/score pair in sorted position and re-saves the file.
void addHighScore(const std::string& name, int score) {
    std::string displayName = name.empty() ? "Player" : name;
    gHighScores.push_back({ displayName, score });
    sortAndTrimHighScores();
    saveHighScores();
}

// One flickering flame "tongue": a stack of narrowing, wobbling rows that
// go red -> orange -> yellow -> near-white from base to tip, so it reads as
// a flame shape rather than a flat rectangle.
void drawFlameLick(int baseCx, int baseY, int width, int height, float t, float phaseOffset) {
    const int steps = 12;
    for (int i = 0; i < steps; i++) {
        float frac = i / (float)(steps - 1); // 0 = base, 1 = tip
        float wobble = std::sin(t * 9.0f + phaseOffset + frac * 4.0f) * width * 0.18f * frac;
        float w = std::max(1.0f, width * (1.0f - frac * 0.88f));
        int rowH = std::max(1, height / steps + 1);
        int rowY = baseY - (int)(frac * height);
        int rowCx = baseCx + (int)wobble;

        Uint8 r = 255;
        Uint8 g = (Uint8)(50 + frac * 190);
        Uint8 b = (Uint8)(frac * frac * 120);
        Uint8 a = (Uint8)(255 - frac * 40);
        SDL_Color c{ r, g, b, a };
        fillRect(rowCx - (int)(w / 2), rowY, (int)w, rowH, c);
    }
}

void drawBuilding(float flamePhase) {
    SDL_Color brick{ 120, 60, 50, 255 };
    fillRect(BUILDING_X, 40, BUILDING_W, GROUND_Y - 40, brick);
    SDL_Color mortar{ 90, 40, 32, 255 };
    for (int y = 40; y < GROUND_Y; y += 20)
        fillRect(BUILDING_X, y, BUILDING_W, 2, mortar);

    for (int row = 0; row < 4; row++) {
        int wy = WINDOW_Y[row];
        SDL_Color frame{ 40, 25, 20, 255 };
        fillRect(WINDOW_X - 4, wy - 4, 54, 58, frame);

        // Dark interior behind the flames instead of a flat orange fill.
        SDL_Color interior{ 25, 10, 8, 255 };
        fillRect(WINDOW_X, wy, 46, 50, interior);

        float t = flamePhase + row * 1.7f;
        int baseY = wy + 48;

        // Hot glow low in the window, behind the licks.
        float glow = 0.5f + 0.5f * std::sin(t * 5.0f);
        SDL_Color glowC{ 255, (Uint8)(120 + 60 * glow), 40, 200 };
        fillCircle(WINDOW_X + 23, baseY - 6, (int)(16 + 4 * glow), glowC);

        // A cluster of 3 overlapping flame tongues of different heights so
        // the fire has a jagged, moving silhouette instead of one blob.
        drawFlameLick(WINDOW_X + 12, baseY, 20, 34 + (int)(6 * std::sin(t * 3.1f)), t, 0.0f);
        drawFlameLick(WINDOW_X + 23, baseY, 26, 46 + (int)(8 * std::sin(t * 2.3f + 1.5f)), t, 2.1f);
        drawFlameLick(WINDOW_X + 34, baseY, 18, 30 + (int)(6 * std::sin(t * 2.7f + 0.8f)), t, 4.2f);
    }
}

void drawFirefighters(const Firefighters& ff) {
    SDL_Color skin{ 235, 190, 150, 255 };
    SDL_Color uniform{ 40, 40, 200, 255 };
    SDL_Color helmet{ 230, 210, 60, 255 };
    SDL_Color eyeC{ 30, 25, 20, 255 };
    SDL_Color mouthC{ 150, 70, 60, 255 };

    // paramedic 1 (left side, facing right toward stretcher)
    fillRect((int)ff.x - 66, (int)ff.y - 26, 10, 26, uniform); // leg
    fillRect((int)ff.x - 54, (int)ff.y - 26, 10, 26, uniform); // leg
    fillRect((int)ff.x - 64, (int)ff.y - 50, 22, 26, uniform); // torso
    fillRect((int)ff.x - 60, (int)ff.y - 62, 14, 14, skin);    // head
    fillRect((int)ff.x - 62, (int)ff.y - 66, 18, 5, helmet);   // helmet
    fillRect((int)ff.x - 44, (int)ff.y - 22, 14, 8, skin);     // arm gripping stretcher
    // face: looking right (toward the far side), so features sit on the
    // forward half of the head.
    fillRect((int)ff.x - 51, (int)ff.y - 57, 2, 2, eyeC);      // eye
    fillRect((int)ff.x - 51, (int)ff.y - 53, 3, 1, mouthC);    // mouth

    // paramedic 2 (right side, mirrored, facing left toward stretcher)
    fillRect((int)ff.x + 44, (int)ff.y - 26, 10, 26, uniform);
    fillRect((int)ff.x + 56, (int)ff.y - 26, 10, 26, uniform);
    fillRect((int)ff.x + 42, (int)ff.y - 50, 22, 26, uniform);
    fillRect((int)ff.x + 46, (int)ff.y - 62, 14, 14, skin);
    fillRect((int)ff.x + 48, (int)ff.y - 66, 18, 5, helmet);
    fillRect((int)ff.x + 30, (int)ff.y - 22, 14, 8, skin);
    // face: mirrored, looking left toward paramedic 1.
    fillRect((int)ff.x + 49, (int)ff.y - 57, 2, 2, eyeC);
    fillRect((int)ff.x + 48, (int)ff.y - 53, 3, 1, mouthC);

    // stretcher canvas, gripped between the two paramedics' hands
    SDL_Color canvas{ 220, 40, 40, 255 };
    fillRect((int)ff.x - 45, (int)ff.y - 14, 90, 12, canvas);
    SDL_Color rim{ 60, 60, 60, 255 };
    fillRect((int)ff.x - 48, (int)ff.y - 2, 96, 5, rim);
}

void fillCircle(int cx, int cy, int r, SDL_Color c) {
    SDL_SetRenderDrawColor(gRenderer, c.r, c.g, c.b, c.a);
    for (int wy = -r; wy <= r; wy++) {
        int wx = (int)std::sqrt((float)(r*r - wy*wy));
        SDL_RenderDrawLine(gRenderer, cx - wx, cy + wy, cx + wx, cy + wy);
    }
}

void drawBaby(const Baby& b) {
    if (b.state == BabyState::Gone) return;
    SDL_Color skin{ 250, 210, 170, 255 };
    SDL_Color diaper{ 250, 250, 250, 255 };
    SDL_Color face{ 60, 40, 30, 255 };
    SDL_Color blush{ 250, 150, 150, 255 };
    if (b.state == BabyState::Splat) {
        SDL_Color splatColor{ 200, 40, 40, 200 };
        float grow = std::min(1.0f, b.splatTimer / 0.4f);
        int w = (int)(10 + grow * 40);
        fillRect((int)b.x - w/2, GROUND_Y - 6, w, 8, splatColor);
        return;
    }
    int cx = (int)b.x;
    int cy = (int)b.y;

    // tiny arms
    fillCircle(cx - 10, cy - 8, 3, skin);
    fillCircle(cx + 10, cy - 8, 3, skin);

    // round head
    fillCircle(cx, cy - 12, 9, skin);

    // face: two eyes + blush + smile
    fillRect(cx - 5, cy - 14, 2, 2, face);
    fillRect(cx + 3, cy - 14, 2, 2, face);
    fillCircle(cx - 6, cy - 10, 2, blush);
    fillCircle(cx + 6, cy - 10, 2, blush);
    fillRect(cx - 2, cy - 9, 4, 1, face);

    // chubby body + diaper
    fillCircle(cx, cy - 1, 7, skin);
    fillRect(cx - 6, cy - 2, 12, 6, diaper);

    // tiny feet
    fillCircle(cx - 4, cy + 5, 3, skin);
    fillCircle(cx + 4, cy + 5, 3, skin);
}

void drawAmbulance() {
    SDL_Color body{ 250, 250, 250, 255 };
    SDL_Color stripe{ 220, 30, 30, 255 };
    fillRect(AMBULANCE_X, GROUND_Y - 60, 90, 50, body);
    fillRect(AMBULANCE_X, GROUND_Y - 40, 90, 10, stripe);
    SDL_Color wheel{ 20, 20, 20, 255 };
    fillRect(AMBULANCE_X + 10, GROUND_Y - 12, 16, 16, wheel);
    fillRect(AMBULANCE_X + 60, GROUND_Y - 12, 16, 16, wheel);
    SDL_Color cross{ 220, 30, 30, 255 };
    fillRect(AMBULANCE_X + 38, GROUND_Y - 55, 20, 6, cross);
    fillRect(AMBULANCE_X + 45, GROUND_Y - 62, 6, 20, cross);
}

// Set up a bounce flight from the baby's current position toward the given zone.
void launchBounce(Baby& b, int fromZone, int toZone, float vy0, float gravity) {
    b.vy = vy0;
    float flightTime = 2.0f * std::fabs(vy0) / gravity;
    float fromX = (float)ZONE_X[fromZone];
    float toX = (float)ZONE_X[toZone];
    b.vx = (toX - fromX) / flightTime;
    b.targetZone = toZone;
}

int main(int argc, char** argv) {
    srand((unsigned)time(nullptr));
    loadHighScores();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) {
        SDL_Log("TTF_Init failed: %s", TTF_GetError());
    }

    gWindow = SDL_CreateWindow("Baby Bounce Rescue",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    gRenderer = SDL_CreateRenderer(gWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_SetRenderDrawBlendMode(gRenderer, SDL_BLENDMODE_BLEND); // so alpha in fillRect/overlays actually blends
    // Keep everything drawn at the fixed SCREEN_W x SCREEN_H layout; SDL
    // scales it (letterboxed, aspect preserved) to whatever the window or
    // fullscreen display actually is, so resizing/F11 doesn't require
    // touching any of the drawing code below.
    SDL_RenderSetLogicalSize(gRenderer, SCREEN_W, SCREEN_H);

    // Font is embedded directly in the binary (font_data.h) so text always
    // renders regardless of what fonts happen to be installed on this
    // machine - no filesystem paths to guess at all.
    loadFontsAtScale(1.0f);
    if (!gFont || !gFontBig) {
        SDL_Log("WARNING: embedded font failed to load (%s). Text will not render.",
                TTF_GetError());
    }

    SDL_AudioSpec want{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 2048;
    gAudioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &gAudioSpec, 0);
    if (gAudioDev) SDL_PauseAudioDevice(gAudioDev, 0);

    // Optional joystick/gamepad: opens the first one plugged in, if any.
    // Uses the plain SDL_Joystick API (axis 0 = horizontal stick/d-pad,
    // any button = confirm) rather than the SDL_GameController mapping
    // database, so it works with generic controllers too.
    SDL_Joystick* gJoystick = nullptr;
    if (SDL_NumJoysticks() > 0) {
        gJoystick = SDL_JoystickOpen(0);
        if (gJoystick) SDL_Log("Joystick connected: %s", SDL_JoystickName(gJoystick));
    }
    int joyAxisDir = 0;   // last edge-triggered horizontal direction, so a held stick only moves once
    int joyHatDir = 0;

    Firefighters ff;
    ff.zone = 0;
    ff.x = (float)ZONE_X[0];
    ff.y = (float)GROUND_Y - 20;

    std::vector<Baby> babies;

    int score = 0;
    int lives = 5;
    int level = 1;
    float spawnTimer = 0.f;
    float spawnInterval = 3.2f;
    float doubleSpawnTimer = -1.f; // >=0 while counting down to a level-4+ "double throw" second baby
    float flamePhase = 0.f;
    bool gameOver = false;
    bool running = true;
    bool isFullscreen = false;
    bool introScreen = true;
    bool showHighScores = false;   // toggled with H, only reachable from intro/game-over
    bool highScoreResolved = false; // guards against re-checking the same game-over score every frame
    bool enteringName = false;      // true while the new-high-score name prompt is up
    std::string nameInput;

    Uint32 lastTicks = SDL_GetTicks();

    // Shared input actions so keyboard, mouse, and joystick/controller all
    // drive the exact same behavior instead of duplicating this logic per
    // input device.
    auto doConfirm = [&]() {
        if (introScreen) {
            introScreen = false;
        } else if (gameOver && !enteringName) {
            babies.clear();
            score = 0;
            lives = 5;
            level = 1;
            spawnInterval = 3.2f;
            doubleSpawnTimer = -1.f;
            gameOver = false;
            highScoreResolved = false;
        }
    };
    auto doCycle = [&](int dir) { // dir: -1 = left, +1 = right
        if (introScreen) { introScreen = false; return; }
        if (gameOver) return;
        ff.zone = (ff.zone + dir + 3) % 3;
    };
    auto doSelectZone = [&](int zone) {
        if (introScreen) { introScreen = false; return; }
        if (gameOver) return;
        ff.zone = zone;
    };

    while (running) {
        Uint32 nowTicks = SDL_GetTicks();
        float dt = (nowTicks - lastTicks) / 1000.0f;
        lastTicks = nowTicks;
        if (dt > 0.05f) dt = 0.05f;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;

            // While the new-high-score name prompt is up, keyboard input goes
            // there and nowhere else (no pausing/fullscreen/movement/restart).
            if (enteringName) {
                if (e.type == SDL_TEXTINPUT) {
                    if (nameInput.size() < MAX_NAME_LEN) nameInput += e.text.text;
                } else if (e.type == SDL_KEYDOWN) {
                    SDL_Keycode k = e.key.keysym.sym;
                    if (k == SDLK_BACKSPACE && !nameInput.empty()) {
                        nameInput.pop_back();
                    } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                        addHighScore(nameInput, score);
                        enteringName = false;
                        SDL_StopTextInput();
                    } else if (k == SDLK_ESCAPE) {
                        // skip saving a name rather than force one
                        enteringName = false;
                        SDL_StopTextInput();
                    }
                }
                continue;
            }

            if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE) {
                    if (showHighScores) showHighScores = false; // close the high-score screen instead of quitting
                    else running = false;
                } else if (k == SDLK_h && (introScreen || gameOver)) {
                    showHighScores = !showHighScores;
                } else if (showHighScores) {
                    // any other key just closes the high-score screen
                    showHighScores = false;
                } else if (k == SDLK_F11) {
                    isFullscreen = !isFullscreen;
                    SDL_SetWindowFullscreen(gWindow, isFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                } else if (k == SDLK_LEFT)  doCycle(-1);
                else if (k == SDLK_RIGHT) doCycle(1);
                else if (k == SDLK_1) doSelectZone(0);
                else if (k == SDLK_2 || k == SDLK_UP) doSelectZone(1);
                else if (k == SDLK_3) doSelectZone(2);
                else if (k == SDLK_RETURN) doConfirm();
                else if (introScreen && k != SDLK_ESCAPE) doConfirm(); // any other key on the intro screen starts the game
            } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                if (showHighScores) {
                    showHighScores = false;
                } else if (introScreen || gameOver) {
                    doConfirm();
                } else {
                    float lx, ly;
                    //windowToLogical(e.button.x, e.button.y, lx, ly);
                    lx=float(e.button.x);
                    ly=float(e.button.y);
                    int best = 0; float bestDist = 1e9f;
                    for (int i = 0; i < 3; i++) {
                        float d = std::fabs(lx - (float)ZONE_X[i]);
                        if (d < bestDist) { bestDist = d; best = i; }
                    }
                    doSelectZone(best);
                }
            } else if (e.type == SDL_JOYBUTTONDOWN) {
                doConfirm();
            } else if (e.type == SDL_JOYHATMOTION) {
                int dir = 0;
                if (e.jhat.value & SDL_HAT_LEFT) dir = -1;
                else if (e.jhat.value & SDL_HAT_RIGHT) dir = 1;
                if (dir != 0 && dir != joyHatDir) doCycle(dir);
                joyHatDir = dir;
            } else if (e.type == SDL_JOYAXISMOTION && e.jaxis.axis == 0) {
                const Sint16 deadzone = 12000;
                int dir = 0;
                if (e.jaxis.value < -deadzone) dir = -1;
                else if (e.jaxis.value > deadzone) dir = 1;
                if (dir != 0 && dir != joyAxisDir) doCycle(dir);
                joyAxisDir = dir;
            }
        }

        ff.x = (float)ZONE_X[ff.zone];
        flamePhase += dt;

        // If the window (or fullscreen display) is now noticeably bigger
        // or smaller relative to the logical canvas, reload the fonts at a
        // matching point size so text stays crisp instead of blurry when
        // stretched. Cheap check every frame; the (relatively) expensive
        // font reload only fires on an actual size change.
        {
            int winW = 0, winH = 0;
            SDL_GetWindowSize(gWindow, &winW, &winH);
            float scale = std::min((float)winW / SCREEN_W, (float)winH / SCREEN_H);
            scale = std::clamp(scale, 0.5f, 4.0f);
            if (std::fabs(scale - gFontRenderScale) > 0.05f) {
                loadFontsAtScale(scale);
            }
        }

        if (showHighScores) {
            SDL_SetRenderDrawColor(gRenderer, 15, 15, 30, 255);
            SDL_RenderClear(gRenderer);

            SDL_Color gold{ 230, 210, 60, 255 };
            SDL_Color white{ 255, 255, 255, 255 };
            SDL_Color dim{ 180, 180, 190, 255 };

            drawText("HIGH SCORES", SCREEN_W/2, 70, gold, gFontBig, true);

            int listY = 160;
            if (gHighScores.empty()) {
                drawText("No scores yet - go catch some babies!", SCREEN_W/2, listY, dim, gFont, true);
            } else {
                for (size_t i = 0; i < gHighScores.size(); i++) {
                    std::string rank = std::to_string(i + 1) + ".";
                    SDL_Color rowColor = (i == 0) ? gold : white;
                    drawText(rank + "  " + gHighScores[i].name, SCREEN_W/2 - 220, listY, rowColor, gFont);
                    drawText(std::to_string(gHighScores[i].score), SCREEN_W/2 + 220, listY, rowColor, gFont, true);
                    listY += 34;
                }
            }

            drawText("Press H, ESC, or any key to go back", SCREEN_W/2, SCREEN_H - 60, dim, gFont, true);
            SDL_RenderPresent(gRenderer);
            continue;
        }

        if (introScreen) {
            SDL_SetRenderDrawColor(gRenderer, 15, 15, 30, 255);
            SDL_RenderClear(gRenderer);
            for (int i = 0; i < 6; i++) {
                SDL_Color c{ (Uint8)(15 + i*3), (Uint8)(15 + i*2), (Uint8)(40 + i*5), 255 };
                fillRect(0, i * (GROUND_Y/6), SCREEN_W, GROUND_Y/6 + 1, c);
            }

            // Real scene as a backdrop: burning building, ground, ambulance,
            // and the paramedic team idling at mid-screen - same art as the
            // actual game, just static/no gameplay running yet.
            drawBuilding(flamePhase);
            drawAmbulance();
            SDL_Color groundColor{ 60, 60, 60, 255 };
            fillRect(0, GROUND_Y, SCREEN_W, SCREEN_H - GROUND_Y, groundColor);
            Firefighters introFF;
            introFF.zone = 1;
            introFF.x = (float)ZONE_X[1];
            introFF.y = (float)GROUND_Y - 20;
            drawFirefighters(introFF);

            // Dark translucent panel behind the title/instructions so they
            // stay readable over the busy scene.
            SDL_Color panel{ 10, 10, 20, 165 };
            fillRect(0, SCREEN_H/2 - 190, SCREEN_W, 360, panel);

            SDL_Color white{ 255, 255, 255, 255 };
            SDL_Color gold{ 230, 210, 60, 255 };
            drawText("Baby Bounce Rescue", SCREEN_W/2, SCREEN_H/2 - 150, gold, gFontBig, true);
            drawText("Catch the falling babies and get them to the ambulance!", SCREEN_W/2, SCREEN_H/2 - 60, white, gFont, true);
            drawText("LEFT / RIGHT arrows - move between zones", SCREEN_W/2, SCREEN_H/2 - 20, white, gFont, true);
            drawText("1 / 2 / 3 - jump straight to a zone", SCREEN_W/2, SCREEN_H/2 + 10, white, gFont, true);
            drawText("Click a zone, or use a controller's stick/D-pad", SCREEN_W/2, SCREEN_H/2 + 40, white, gFont, true);
            drawText("F11 - toggle fullscreen        ESC - quit        H - high scores", SCREEN_W/2, SCREEN_H/2 + 70, white, gFont, true);
            drawText("Press any key, click, or press a button to start", SCREEN_W/2, SCREEN_H/2 + 120, gold, gFont, true);
            SDL_RenderPresent(gRenderer);
            continue;
        }

        if (!gameOver) {
            float gravity = 620.f;
            float groundLevel = (float)GROUND_Y - 20;   // true ground height (used for misses)
            float catchY = (float)GROUND_Y - 42;         // height of the stretcher surface (used for catches)

            auto spawnOneBaby = [&]() {
                int row = 0; // topmost window = the 4th floor
                Baby b;
                b.x = (float)WINDOW_X + 23.f;   // start inside the window
                b.y = (float)WINDOW_Y[row] + 25;
                b.state = BabyState::Falling;
                b.targetZone = 0;

                // Solve for the toss velocity that lands the baby exactly at
                // ZONE_X[0] / catchY after flightDuration seconds, so the
                // arc always ends right where the stretcher sits.
                float flightDuration = 1.3f;
                b.vx = ((float)ZONE_X[0] - b.x) / flightDuration;
                b.vy = (catchY - b.y - 0.5f * gravity * flightDuration * flightDuration) / flightDuration;

                babies.push_back(b);
            };

            spawnTimer += dt;
            if (spawnTimer >= spawnInterval) {
                spawnTimer = 0.f;
                spawnOneBaby();

                // From level 4 on, there's a growing chance the throw is a
                // "double" - a second baby tossed out just behind the
                // first, ramping up in both frequency and how tight the
                // gap is as the level climbs, so late levels demand
                // catching two in a row instead of one at a time.
                if (level >= 4) {
                    float doubleChance = std::min(0.85f, (level - 3) * 0.15f);
                    if ((float)rand() / (float)RAND_MAX < doubleChance) {
                        float gap = std::max(0.10f, 0.25f - (level - 4) * 0.02f);
                        doubleSpawnTimer = gap;
                    }
                }
            }

            if (doubleSpawnTimer >= 0.f) {
                doubleSpawnTimer -= dt;
                if (doubleSpawnTimer <= 0.f) {
                    doubleSpawnTimer = -1.f;
                    spawnOneBaby();
                }
            }

            spawnInterval = std::max(0.55f, 3.2f - (level - 1) * 0.22f);

            for (auto& b : babies) {
                if (b.state == BabyState::Falling) {
                    b.vy += gravity * dt;
                    b.x += b.vx * dt;
                    b.y += b.vy * dt;
                    if (b.y >= catchY) {
                        b.x = (float)ZONE_X[0];
                        if (ff.zone == 0) {
                            b.y = catchY;
                            launchBounce(b, 0, 1, -420.f, gravity);
                            b.state = BabyState::Bounce1;
                            playBoing();
                        } else {
                            b.y = groundLevel;
                            b.state = BabyState::Splat;
                            b.splatTimer = 0.f;
                            playSplat();
                            lives--;
                            if (lives <= 0) { gameOver = true; playGameOver(); }
                        }
                    }
                } else if (b.state == BabyState::Bounce1 || b.state == BabyState::Bounce2) {
                    b.vy += gravity * dt;
                    b.x += b.vx * dt;
                    b.y += b.vy * dt;
                    if (b.vy > 0 && b.y >= catchY) {
                        b.x = (float)ZONE_X[b.targetZone];
                        bool caught = (ff.zone == b.targetZone);
                        if (b.state == BabyState::Bounce1) {
                            if (caught) {
                                b.y = catchY;
                                launchBounce(b, 1, 2, -320.f, gravity);
                                b.state = BabyState::Bounce2;
                                playBoing();
                            } else {
                                b.y = groundLevel;
                                b.state = BabyState::Splat; b.splatTimer = 0.f; playSplat();
                                lives--; if (lives <= 0) { gameOver = true; playGameOver(); }
                            }
                        } else { // Bounce2
                            if (caught) {
                                // baby is now safely in the paramedics' hands — the final
                                // toss into the ambulance always succeeds from here on
                                b.y = catchY;
                                b.vy = -220.f;
                                b.vx = 160.f;
                                b.state = BabyState::Bounce3;
                                playBoing();
                            } else {
                                b.y = groundLevel;
                                b.state = BabyState::Splat; b.splatTimer = 0.f; playSplat();
                                lives--; if (lives <= 0) { gameOver = true; playGameOver(); }
                            }
                        }
                    }
                } else if (b.state == BabyState::Bounce3) {
                    // guaranteed final hop into the ambulance — no catch required,
                    // and it's never treated as a miss regardless of trampoline position
                    b.vy += gravity * dt;
                    b.x += b.vx * dt;
                    b.y += b.vy * dt;
                    if (b.vy > 0 && b.y >= catchY) {
                        b.y = catchY;
                        b.state = BabyState::Delivering;
                        playDeliver();
                    }
                } else if (b.state == BabyState::Delivering) {
                    b.x += 180.f * dt;
                    b.y -= 25.f * dt;
                    if (b.x >= (float)AMBULANCE_X + 45.f) {
                        b.state = BabyState::Gone;
                        score += 10 * level;
                    }
                } else if (b.state == BabyState::Splat) {
                    b.splatTimer += dt;
                    if (b.splatTimer > 0.6f) b.state = BabyState::Gone;
                }
            }

            babies.erase(std::remove_if(babies.begin(), babies.end(),
                [](const Baby& b) { return b.state == BabyState::Gone; }), babies.end());

            level = 1 + score / 80;
        }

        if (gameOver && !highScoreResolved) {
            highScoreResolved = true;
            if (qualifiesForHighScore(score)) {
                enteringName = true;
                nameInput.clear();
                SDL_StartTextInput();
            }
        }

        // ---- render ----
        SDL_SetRenderDrawColor(gRenderer, 15, 15, 30, 255);
        SDL_RenderClear(gRenderer);

        for (int i = 0; i < 6; i++) {
            SDL_Color c{ (Uint8)(15 + i*3), (Uint8)(15 + i*2), (Uint8)(40 + i*5), 255 };
            fillRect(0, i * (GROUND_Y/6), SCREEN_W, GROUND_Y/6 + 1, c);
        }

        drawBuilding(flamePhase);
        drawAmbulance();

        SDL_Color groundColor{ 60, 60, 60, 255 };
        fillRect(0, GROUND_Y, SCREEN_W, SCREEN_H - GROUND_Y, groundColor);

        for (auto& b : babies) drawBaby(b);
        drawFirefighters(ff);

        SDL_Color white{ 255, 255, 255, 255 };
        drawText("Lives: " + std::to_string(std::max(0, lives)), 20, 15, white, gFont);
        drawText("Level: " + std::to_string(level), 20, 45, white, gFont);

        // big score text, top right (no box/border)
        {
            int areaW = 210;
            int x = SCREEN_W - areaW - 15;
            int y = 15;
            SDL_Color label{ 200, 200, 210, 255 };
            drawText("SCORE", x + areaW/2, y, label, gFont, true);
            drawText(std::to_string(score), x + areaW/2, y + 24, white, gFontBig, true);
        }

        if (gameOver) {
            SDL_Color overlay{ 0, 0, 0, 180 };
            fillRect(0, 0, SCREEN_W, SCREEN_H, overlay);
            drawText("GAME OVER", SCREEN_W/2, SCREEN_H/2 - 60, white, gFontBig, true);
            drawText("Final Score: " + std::to_string(score), SCREEN_W/2, SCREEN_H/2, white, gFont, true);

            if (enteringName) {
                SDL_Color gold{ 230, 210, 60, 255 };
                drawText("NEW HIGH SCORE! Enter your name:", SCREEN_W/2, SCREEN_H/2 + 40, gold, gFont, true);

                // Blinking cursor, ~2Hz, appended only while it's "on".
                std::string shown = nameInput;
                if ((SDL_GetTicks() / 250) % 2 == 0) shown += "_";

                int boxW = 300, boxH = 40;
                SDL_Color boxColor{ 30, 30, 45, 230 };
                SDL_Color boxBorder{ 230, 210, 60, 255 };
                int boxX = SCREEN_W/2 - boxW/2, boxY = SCREEN_H/2 + 70;
                fillRect(boxX, boxY, boxW, boxH, boxColor);
                fillRect(boxX, boxY, boxW, 2, boxBorder);
                fillRect(boxX, boxY + boxH - 2, boxW, 2, boxBorder);
                fillRect(boxX, boxY, 2, boxH, boxBorder);
                fillRect(boxX + boxW - 2, boxY, 2, boxH, boxBorder);
                drawText(shown, SCREEN_W/2, boxY + 9, white, gFont, true);

                drawText("ENTER to confirm        ESC to skip", SCREEN_W/2, boxY + boxH + 20, white, gFont, true);
            } else {
                drawText("Press ENTER to restart        H - high scores", SCREEN_W/2, SCREEN_H/2 + 40, white, gFont, true);
            }
        }

        SDL_RenderPresent(gRenderer);
    }

    if (gAudioDev) SDL_CloseAudioDevice(gAudioDev);
    if (gJoystick) SDL_JoystickClose(gJoystick);
    if (gFont) TTF_CloseFont(gFont);
    if (gFontBig) TTF_CloseFont(gFontBig);
    TTF_Quit();
    SDL_DestroyRenderer(gRenderer);
    SDL_DestroyWindow(gWindow);
    SDL_Quit();
    return 0;
}
