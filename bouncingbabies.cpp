// Bouncing Babies — SDL2 remake
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

void drawText(const std::string& s, int x, int y, SDL_Color c, TTF_Font* font, bool center = false) {
    if (!font) return;
    SDL_Surface* surf = TTF_RenderText_Blended(font, s.c_str(), c);
    if (!surf) return;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(gRenderer, surf);
    SDL_Rect dst{ x, y, surf->w, surf->h };
    if (center) dst.x -= surf->w / 2;
    SDL_RenderCopy(gRenderer, tex, nullptr, &dst);
    SDL_FreeSurface(surf);
    SDL_DestroyTexture(tex);
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
        float flick = 0.5f + 0.5f * std::sin(flamePhase * 6.0f + row * 1.7f);
        SDL_Color fireC{ (Uint8)(200 + 55 * flick), (Uint8)(90 + 60 * flick), 20, 255 };
        fillRect(WINDOW_X, wy, 46, 50, fireC);
        SDL_Color innerFire{ 255, (Uint8)(200 * flick), 60, 255 };
        int fw = 20 + (int)(8 * flick);
        fillRect(WINDOW_X + 23 - fw/2, wy + 25 - fw/2, fw, fw, innerFire);
    }
}

void drawFirefighters(const Firefighters& ff) {
    SDL_Color skin{ 235, 190, 150, 255 };
    SDL_Color uniform{ 40, 40, 200, 255 };
    SDL_Color helmet{ 230, 210, 60, 255 };

    // paramedic 1 (left side, facing right toward stretcher)
    fillRect((int)ff.x - 66, (int)ff.y - 26, 10, 26, uniform); // leg
    fillRect((int)ff.x - 54, (int)ff.y - 26, 10, 26, uniform); // leg
    fillRect((int)ff.x - 64, (int)ff.y - 50, 22, 26, uniform); // torso
    fillRect((int)ff.x - 60, (int)ff.y - 62, 14, 14, skin);    // head
    fillRect((int)ff.x - 62, (int)ff.y - 66, 18, 5, helmet);   // helmet
    fillRect((int)ff.x - 44, (int)ff.y - 22, 14, 8, skin);     // arm gripping stretcher

    // paramedic 2 (right side, mirrored, facing left toward stretcher)
    fillRect((int)ff.x + 44, (int)ff.y - 26, 10, 26, uniform);
    fillRect((int)ff.x + 56, (int)ff.y - 26, 10, 26, uniform);
    fillRect((int)ff.x + 42, (int)ff.y - 50, 22, 26, uniform);
    fillRect((int)ff.x + 46, (int)ff.y - 62, 14, 14, skin);
    fillRect((int)ff.x + 48, (int)ff.y - 66, 18, 5, helmet);
    fillRect((int)ff.x + 30, (int)ff.y - 22, 14, 8, skin);

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

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    if (TTF_Init() != 0) {
        SDL_Log("TTF_Init failed: %s", TTF_GetError());
    }

    gWindow = SDL_CreateWindow("Bouncing Babies",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W, SCREEN_H, SDL_WINDOW_SHOWN);
    gRenderer = SDL_CreateRenderer(gWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    // Font is embedded directly in the binary (font_data.h) so text always
    // renders regardless of what fonts happen to be installed on this
    // machine - no filesystem paths to guess at all.
    SDL_RWops* fontRW1 = SDL_RWFromConstMem(bb_font_data, (int)bb_font_data_len);
    gFont = TTF_OpenFontRW(fontRW1, 1 /*freesrc*/, 22);
    SDL_RWops* fontRW2 = SDL_RWFromConstMem(bb_font_data, (int)bb_font_data_len);
    gFontBig = TTF_OpenFontRW(fontRW2, 1 /*freesrc*/, 48);
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
    float flamePhase = 0.f;
    bool gameOver = false;
    bool running = true;

    Uint32 lastTicks = SDL_GetTicks();

    while (running) {
        Uint32 nowTicks = SDL_GetTicks();
        float dt = (nowTicks - lastTicks) / 1000.0f;
        lastTicks = nowTicks;
        if (dt > 0.05f) dt = 0.05f;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE) running = false;
                if (!gameOver) {
                    if (k == SDLK_1 || k == SDLK_LEFT)  ff.zone = 0;
                    if (k == SDLK_2 || k == SDLK_UP)    ff.zone = 1;
                    if (k == SDLK_3 || k == SDLK_RIGHT) ff.zone = 2;
                }
                if (gameOver && k == SDLK_RETURN) {
                    babies.clear();
                    score = 0;
                    lives = 5;
                    level = 1;
                    spawnInterval = 3.2f;
                    gameOver = false;
                }
            }
        }

        ff.x = (float)ZONE_X[ff.zone];
        flamePhase += dt;

        if (!gameOver) {
            float gravity = 620.f;
            float groundLevel = (float)GROUND_Y - 20;   // true ground height (used for misses)
            float catchY = (float)GROUND_Y - 42;         // height of the stretcher surface (used for catches)

            spawnTimer += dt;
            if (spawnTimer >= spawnInterval) {
                spawnTimer = 0.f;
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
        drawText("Keys: 1=Near bldg 2=Mid 3=Near ambulance", SCREEN_W/2, 15, white, gFont, true);

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
            drawText("Press ENTER to restart", SCREEN_W/2, SCREEN_H/2 + 40, white, gFont, true);
        }

        SDL_RenderPresent(gRenderer);
    }

    if (gAudioDev) SDL_CloseAudioDevice(gAudioDev);
    if (gFont) TTF_CloseFont(gFont);
    if (gFontBig) TTF_CloseFont(gFontBig);
    TTF_Quit();
    SDL_DestroyRenderer(gRenderer);
    SDL_DestroyWindow(gWindow);
    SDL_Quit();
    return 0;
}
