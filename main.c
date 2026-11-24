#include <raylib.h>
#include <raymath.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <omp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h> 

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-result"
#pragma GCC diagnostic ignored "-Wunused-function"
#define RAYGUI_IMPLEMENTATION
#include "../lib/raygui.h"
#pragma GCC diagnostic pop

#define WINDOW_W 1800
#define WINDOW_H 900
static int screenW = WINDOW_W, screenH = WINDOW_H;
#define WIDTH  screenW
#define HEIGHT screenH
#define PANEL_WIDTH 300
#define SIM_WIDTH (WIDTH - PANEL_WIDTH)

#define MIN_BALL_SIZE 2
#define MAX_BALL_SIZE 32
#define MAX_GRID_CELLS ((SIM_WIDTH / MIN_BALL_SIZE) * (HEIGHT / MIN_BALL_SIZE))

#define MOLECULES_PER_SPECIES 10000
#define MAX_PER_SPECIES       20000
#define MAX_BALL_COUNT        (2 * MAX_PER_SPECIES)

#define DAMPING 1.0
#define RESTITUTION 1.0
#define GRAVITY 0.0

#define SUBSTEPS 16

#define SUB_GRAVITY  (GRAVITY / (SUBSTEPS * SUBSTEPS))
#define SUB_DAMPING  (1.0 - (1.0 - DAMPING) / SUBSTEPS)

#define BALLGRID_MAX_BALLS MAX_BALL_COUNT
#include "../lib/ballgrid.h"
#include "../lib/ballrender.h"

#define BARRIER_X (SIM_WIDTH / 2.0f)
static bool barrierUp = false;

void setBallSize(float size) {
    ballgridSetSize(size, SIM_WIDTH, HEIGHT);
}

typedef enum { RED_M, BLUE_M, PURPLE_M, SPECIES_COUNT } Species;
static const Color speciesColor[SPECIES_COUNT] = {
    { 250,  84,  72, 255 },
    {  56, 152, 255, 255 },
    { 190, 104, 255, 255 },
};

#define COL_TANK      (Color){   9,  10,  14, 255 }
#define COL_GRID      (Color){ 120, 140, 190,  14 }
#define COL_GRID_MAJ  (Color){ 120, 140, 190,  26 }
#define COL_CARD      (Color){  14,  16,  22, 255 }
#define COL_PANEL     (Color){  14,  16,  24, 255 }
#define COL_BORDER    (Color){  52,  57,  74, 255 }
#define COL_PLOT_GRID (Color){ 255, 255, 255,  20 }
#define COL_TEXT      (Color){ 228, 230, 238, 255 }
#define COL_MUTED     (Color){ 132, 137, 156, 255 }

typedef struct {
    float redCount, blueCount;
    float perSpecies;
    float ballSize;
    float spawnSpeed;
    bool  separated;
    float speedLog2;
    bool  paused;
    float temperature;
    float activationKT0;
    float heatKT0;
    float rateWindowSecs;
    bool  showCharts, showBursts, sound;
    bool  energyColors;
    bool  fullscreen;
    float volume;
} Settings;

static Settings settings = {
    .speedLog2 = 0, .temperature = 1, .rateWindowSecs = 2,
    .showCharts = true, .showBursts = true, .sound = true, .volume = 1,
};

typedef struct {
    const char *button, *title;
    const char *note[2];
    const char *names[SPECIES_COUNT];
    float t0Kelvin;
    float activationKT0, heatKT0;
    float perSpecies, ballSize, spawnSpeed;
} Preset;

static const Preset presets[] = {
    { "Generic", "A + B → C", { NULL, NULL }, { "A", "B", "C" },
      0, 3, 0, MOLECULES_PER_SPECIES, 4, 0.01f },
    { "NO + O3", "NO + O_3 → NO_2 + O_2",
      { "E_a = 12.5 kJ/mol   (JPL: k = 3×10^{−12} e^{−1500/T} cm^3 s^{−1})",
        "ΔH = −201 kJ/mol, absorbed by the air: heat set to 0" },
      { "NO", "O_3", "NO_2 + O_2" },
      298, 1500.0f / 298, 0, 5000, 4, 0.1f },
};
#define PRESET_COUNT (int)(sizeof presets / sizeof presets[0])
static int activePreset = 0;

#define GAS_CONSTANT_KJ 8.314e-3f

void applyPreset(int i) {
    const Preset *p = &presets[i];
    activePreset = i;
    settings.activationKT0 = p->activationKT0;
    settings.heatKT0 = p->heatKT0;
    settings.perSpecies = settings.redCount = settings.blueCount = p->perSpecies;
    settings.ballSize = p->ballSize;
    settings.spawnSpeed = p->spawnSpeed;
    settings.separated = false;
}

static const char *temperatureText(float tOverT0) {
    float k = presets[activePreset].t0Kelvin;
    return k > 0 ? TextFormat("%.0f K", tOverT0 * k) : TextFormat("%.2f T_0", tOverT0);
}
static const char *energyText(float inKT0, bool showSign) {
    float k = presets[activePreset].t0Kelvin;
    return k > 0 ? TextFormat(showSign ? "%+.1f kJ/mol" : "%.1f kJ/mol", inKT0 * GAS_CONSTANT_KJ * k)
                 : TextFormat(showSign ? "%+.2f kT_0" : "%.2f kT_0", inKT0);
}

uint8_t      ballSpecies[MAX_BALL_COUNT];
static Color drawColor[MAX_BALL_COUNT];

Texture2D ballTex;

#define BALL_TEX 64

void loadBallTexture(void) {
    Image img = GenImageColor(BALL_TEX, BALL_TEX, BLANK);
    Color *px = img.data;
    const float r = BALL_TEX / 2.0f;
    const Vector3 light = Vector3Normalize((Vector3){ -0.45f, -0.55f, 0.70f });
    const Vector3 half  = Vector3Normalize(Vector3Add(light, (Vector3){ 0, 0, 1 }));
    for (int y = 0; y < BALL_TEX; y++) {
        for (int x = 0; x < BALL_TEX; x++) {
            float nx = (x + 0.5f - r) / (r - 1), ny = (y + 0.5f - r) / (r - 1);
            float d2 = nx * nx + ny * ny;
            float cover = Clamp((1 - sqrtf(d2)) * (r - 1) + 0.5f, 0, 1);
            if (cover <= 0) continue;
            Vector3 n = { nx, ny, sqrtf(fmaxf(0, 1 - d2)) };
            float diffuse  = fmaxf(0, Vector3DotProduct(n, light));
            float specular = powf(fmaxf(0, Vector3DotProduct(n, half)), 40);
            px[y * BALL_TEX + x] = (Color){ (unsigned char)(255 * Clamp(0.42f + 0.68f * diffuse, 0, 1)),
                                            (unsigned char)(255 * 0.55f * specular), 0,
                                            (unsigned char)(255 * cover) };
        }
    }
    ballTex = LoadTextureFromImage(img);
    UnloadImage(img);
    GenTextureMipmaps(&ballTex);
    SetTextureFilter(ballTex, TEXTURE_FILTER_TRILINEAR);
}

static const char *BALL_FS =
    "#version 330\n"
    "in vec2 uv;\n"
    "in vec4 tint;\n"
    "uniform sampler2D tex;\n"
    "out vec4 color;\n"
    "void main(){ vec4 t = texture(tex, uv);"
    " color = vec4(tint.rgb * t.r + t.g, t.a * tint.a); }\n";
    
float thermalEnergy(void);
static float reactionBarrier(void);

static float energyScale(float b) { return fmaxf(6, 1.5f * b); }

static Color energyColor(float e, float barrier, float max) {
    static const Color c0 = { 30, 45, 120, 255 }, c1 = { 70, 190, 200, 255 };
    static const Color h0 = { 255, 170, 40, 255 }, h1 = { 255, 250, 235, 255 };
    if (e < barrier) return ColorLerp(c0, c1, sqrtf(e / barrier));
    return ColorLerp(h0, h1, Clamp((e - barrier) / fmaxf(max - barrier, 1e-6f), 0, 1));
}

void drawAllBalls(void) {
    if (settings.energyColors) {
        float kT = thermalEnergy();
        float invKT = kT > 0 ? 1 / kT : 0, b = reactionBarrier() * invKT, max = energyScale(b);
        #pragma omp parallel for
        for (size_t i = 0; i < ball_count; i++) {
            Vector2 v = Vector2Subtract(balls[i].p, balls[i].prevP);
            drawColor[i] = energyColor(0.5f * (v.x * v.x + v.y * v.y) * invKT, b, max);
        }
    } else {
        #pragma omp parallel for
        for (size_t i = 0; i < ball_count; i++)
            drawColor[i] = speciesColor[ballSpecies[ballId[i]]];
    }
    ballRenderDraw(drawColor, ballTex, ballSize);
}

void integrateBall(Ball *b) {
    Vector2 old = b->p;
    Vector2 v = Vector2Scale(Vector2Subtract(b->p, b->prevP), SUB_DAMPING);
    v.y -= SUB_GRAVITY;
    b->p = Vector2Add(b->p, v);
    b->prevP = old;
}

void integrateAllBalls() {
    for(size_t i = 0; i < ball_count; i++) {
        integrateBall(&balls[i]);
    }
}

void checkBounds(Ball *b) {
    Vector2 v = Vector2Subtract(b->p, b->prevP);
    if(b->p.x < halfSize) {
        b->p.x = halfSize;
        b->prevP.x = b->p.x + v.x * RESTITUTION;
    }
    if(b->p.x > SIM_WIDTH - halfSize) {
        b->p.x = SIM_WIDTH - halfSize;
        b->prevP.x = b->p.x + v.x * RESTITUTION;
    }
    if (barrierUp) {
        if (b->prevP.x < BARRIER_X && b->p.x > BARRIER_X - halfSize) {
            b->p.x = BARRIER_X - halfSize;
            b->prevP.x = b->p.x + v.x * RESTITUTION;
        } else if (b->prevP.x >= BARRIER_X && b->p.x < BARRIER_X + halfSize) {
            b->p.x = BARRIER_X + halfSize;
            b->prevP.x = b->p.x + v.x * RESTITUTION;
        }
    }
    if(b->p.y < halfSize) {
        b->p.y = halfSize;
        b->prevP.y = b->p.y + v.y * RESTITUTION;
    }
    if(b->p.y > HEIGHT - halfSize) {
        b->p.y = HEIGHT - halfSize;
        b->prevP.y = b->p.y + v.y * RESTITUTION;
    }
}

void checkAllBounds() {
    for(size_t i = 0; i < ball_count; i++) {
        checkBounds(&balls[i]);
    }
}

float min(float a, float b) {
    return a < b ? a : b;
}

static unsigned reactionCount = 0;

#define MAX_PENDING_BURSTS 64
static Vector2  pendingBurst[MAX_PENDING_BURSTS];
static unsigned pendingBurstCount = 0;

static float activationEnergy = 0;
static float kT0 = 0;
static float reactionHeat = 0;

static float reactionBarrier(void) { return fmaxf(activationEnergy, -reactionHeat); }

static void releaseHeat(Ball *a, Ball *b, float heat) {
    Vector2 va = Vector2Subtract(a->p, a->prevP), vb = Vector2Subtract(b->p, b->prevP);
    Vector2 cm = Vector2Scale(Vector2Add(va, vb), 0.5f);
    Vector2 u  = Vector2Scale(Vector2Subtract(vb, va), 0.5f);
    float uu = Vector2DotProduct(u, u), target = fmaxf(uu + heat, 0);
    if (uu > 1e-12f) u = Vector2Scale(u, sqrtf(target / uu));
    else             u = Vector2Scale(Vector2Normalize(Vector2Subtract(b->p, a->p)), sqrtf(target));
    a->prevP = Vector2Subtract(a->p, Vector2Subtract(cm, u));
    b->prevP = Vector2Subtract(b->p, Vector2Add(cm, u));
}

void react(uint32_t i0, uint32_t i1, float impactEnergy) {
    if (impactEnergy < reactionBarrier()) return;
    uint8_t *s0 = &ballSpecies[ballId[i0]], *s1 = &ballSpecies[ballId[i1]];
    if ((*s0 == RED_M && *s1 == BLUE_M) || (*s0 == BLUE_M && *s1 == RED_M))
    {
        *s0 = *s1 = PURPLE_M;
        if (reactionHeat != 0) releaseHeat(&balls[i0], &balls[i1], reactionHeat);
        Vector2 at = Vector2Lerp(balls[i0].p, balls[i1].p, 0.5f);
        #pragma omp critical(reaction_log)
        {
            reactionCount++;
            if (pendingBurstCount < MAX_PENDING_BURSTS) pendingBurst[pendingBurstCount++] = at;
        }
    }
}

void checkCollision(uint32_t i0, uint32_t i1) {
    Ball *b0 = &balls[i0], *b1 = &balls[i1];
    if (barrierUp && (b0->prevP.x < BARRIER_X) != (b1->prevP.x < BARRIER_X)) return;
    Vector2 d = Vector2Subtract(b1->p, b0->p);
    float distsqr = d.x * d.x + d.y * d.y;
    if(distsqr < ballSize * ballSize && distsqr > 0) {
        float dist = sqrt(distsqr);
        Vector2 n  = Vector2Scale(d, 1.0f / dist);
        Vector2 v0 = Vector2Subtract(b0->p, b0->prevP);
        Vector2 v1 = Vector2Subtract(b1->p, b1->prevP);

        float ov = min(ballSize - dist, ballSize * 0.25f);
        Vector2 push = Vector2Scale(n, ov * 0.5f);
        b0->p = Vector2Subtract(b0->p, push);
        b1->p = Vector2Add(b1->p, push);

        float vn = Vector2DotProduct(Vector2Subtract(v1, v0), n);
        float impactEnergy = 0;
        if (vn < 0) {
            impactEnergy = 0.25f * vn * vn;
            float j = -(1.0f + RESTITUTION) * vn * 0.5f;
            v0 = Vector2Subtract(v0, Vector2Scale(n, j));
            v1 = Vector2Add(v1, Vector2Scale(n, j));
        }
        b0->prevP = Vector2Subtract(b0->p, v0);
        b1->prevP = Vector2Subtract(b1->p, v1);
        react(i0, i1, impactEnergy);
    }
}

void createBall(float x, float y, float vx, float vy, Species species) {
    if(ball_count >= MAX_BALL_COUNT) { return; }
    uint32_t id = (uint32_t)ball_count;
    Ball *b = &balls[ball_count];
    b->p =     (Vector2){x,  y};
    b->prevP = (Vector2){x - vx, y - vy};
    ballId[ball_count] = id;
    ballSpecies[id] = (uint8_t)species;
    ball_count++;
}

float randf(float a, float b) {
    float scale = rand() / (float)RAND_MAX;
    return a + scale * (b - a);
}

float thermalEnergy(void) {
    if (ball_count == 0) return 0;
    double sum = 0;
    for (size_t i = 0; i < ball_count; i++) {
        Vector2 v = Vector2Subtract(balls[i].p, balls[i].prevP);
        sum += 0.5 * (v.x * v.x + v.y * v.y);
    }
    return (float)(sum / ball_count);
}

void scaleTemperature(float factor) {
    float s = sqrtf(factor);
    for (size_t i = 0; i < ball_count; i++) {
        Vector2 v = Vector2Subtract(balls[i].p, balls[i].prevP);
        balls[i].prevP = Vector2Subtract(balls[i].p, Vector2Scale(v, s));
    }
}

void createRandomBalls(size_t count) {
    for(size_t i = 0; i < count; i++) {
        float x = randf(0,SIM_WIDTH);
        float y = randf(0,HEIGHT);
        float vx = randf(-10,10);
        float vy = randf(-0,0);
        createBall(x, y, vx, vy, BLUE_M);
    }
}

bool trySpawn(float x, float y, float vx, float vy, Species species) {
    if (!spawnFree(x, y) || ball_count >= MAX_BALL_COUNT) return false;
    createBall(x, y, vx, vy, species);
    Cell *c = &grid[ballCellKey(&balls[ball_count - 1])];
    if (c->count < CELL_CAP) c->idx[c->count] = (uint32_t)(ball_count - 1);
    c->count++;
    return true;
}

void countSpecies(int count[SPECIES_COUNT]) {
    for (int k = 0; k < SPECIES_COUNT; k++) count[k] = 0;
    for (size_t i = 0; i < ball_count; i++) count[ballSpecies[i]]++;
}

static Font hudFont;

void loadHudFont(void) {
    static const int extra[] = { 0x2192, 0x2212, 0x0394, 0x00D7, 0x00B7, 0x00B0, 0x00BD };
    int cps[95 + sizeof extra / sizeof extra[0]], n = 0;
    for (int c = 32; c < 127; c++) cps[n++] = c;
    for (size_t i = 0; i < sizeof extra / sizeof extra[0]; i++) cps[n++] = extra[i];
    hudFont = LoadFontEx("lmromandemi10-regular.otf", 64, cps, n);
    SetTextureFilter(hudFont.texture, TEXTURE_FILTER_BILINEAR);
}

static float richText(const char *s, int len, float x, float y, float size, Color c, bool draw) {
    float x0 = x;
    char buf[256];
    for (int i = 0; i < len;) {
        int run = i;
        while (i < len && s[i] != '_' && s[i] != '^') i++;
        if (i > run) {
            int n = i - run < (int)sizeof buf - 1 ? i - run : (int)sizeof buf - 1;
            memcpy(buf, s + run, n);
            buf[n] = 0;
            if (draw) DrawTextEx(hudFont, buf, (Vector2){ x, y }, size, 0, c);
            x += MeasureTextEx(hudFont, buf, size, 0).x;
        }
        if (i >= len) break;
        bool sub = s[i++] == '_';
        int start = i, n;
        if (i < len && s[i] == '{') {
            int depth = 1;
            start = ++i;
            while (i < len && depth) { depth += (s[i] == '{') - (s[i] == '}'); i++; }
            n = i - start - (depth == 0);
        } else {
            GetCodepointNext(s + i, &n);
            i += n;
        }
        float ss = size * 0.68f;
        float baseline = y + 0.75f * size + (sub ? 0.16f : -0.36f) * size;
        x += richText(s + start, n, x, baseline - 0.75f * ss, ss, c, draw);
    }
    return x - x0;
}

void drawLabel(const char *text, float x, float y, float size, Color c) {
    richText(text, (int)strlen(text), x, y, size, c, true);
}

static float labelWidth(const char *text, float size) {
    return richText(text, (int)strlen(text), 0, 0, size, BLANK, false);
}

static void drawLabelRight(const char *text, float right, float y, float size, Color c) {
    drawLabel(text, right - labelWidth(text, size), y, size, c);
}

#define SIM_FPS        60.0f

#define HIST_LEN       600
static int  histCount[HIST_LEN][SPECIES_COUNT];
static int  histLen = 0;
static long histInterval = SUBSTEPS * 4;

void recordHistory(void) {
    if (histLen == HIST_LEN) {
        for (int t = 0; t < HIST_LEN / 2; t++)
            memcpy(histCount[t], histCount[2 * t], sizeof histCount[t]);
        histLen = HIST_LEN / 2;
        histInterval *= 2;
    }
    countSpecies(histCount[histLen++]);
}

static float elapsedSecs(void) {
    return histLen > 1 ? (histLen - 1) * (histInterval / SUBSTEPS) / SIM_FPS : 0;
}

static float histX(int t, Rectangle p) {
    return p.x + (histLen > 1 ? t * p.width / (histLen - 1) : 0);
}

static float niceStep(float range, int ticks) {
    float raw = range / ticks, mag = powf(10, floorf(log10f(raw))), f = raw / mag;
    return (f < 1.5f ? 1 : f < 3.5f ? 2 : f < 7.5f ? 5 : 10) * mag;
}

static void drawAxes(Rectangle p, float yMax, bool percent, float tMax) {
    float ys = niceStep(yMax, 4);
    for (int i = 0; i * ys <= yMax * 1.001f; i++) {
        float v = i * ys, y = p.y + p.height - v / yMax * p.height;
        if (i) DrawLineEx((Vector2){ p.x, y }, (Vector2){ p.x + p.width, y }, 1, COL_PLOT_GRID);
        drawLabelRight(percent ? TextFormat("%.0f%%", 100 * v) : TextFormat("%g", v), p.x - 6, y - 8, 15, COL_MUTED);
    }
    if (tMax > 0) {
        float ts = niceStep(tMax, 5);
        for (int i = 0; i * ts <= tMax * 1.001f; i++) {
            float x = p.x + i * ts / tMax * p.width;
            if (i) DrawLineEx((Vector2){ x, p.y }, (Vector2){ x, p.y + p.height }, 1, COL_PLOT_GRID);
            const char *s = TextFormat("%g", i * ts);
            drawLabel(s, x - labelWidth(s, 15) / 2, p.y + p.height + 3, 15, COL_MUTED);
        }
    }
    DrawLineEx((Vector2){ p.x, p.y }, (Vector2){ p.x, p.y + p.height }, 1, COL_BORDER);
    DrawLineEx((Vector2){ p.x, p.y + p.height }, (Vector2){ p.x + p.width, p.y + p.height }, 1, COL_BORDER);
}

static float drawHistory(Rectangle p, int total) {
    drawAxes(p, 1, true, elapsedSecs());
    if (histLen > 1 && total > 0) {
        for (int k = 0; k < SPECIES_COUNT; k++) {
            for (int t = 1; t < histLen; t++) {
                Vector2 a = { histX(t - 1, p), p.y + p.height - histCount[t - 1][k] * p.height / total };
                Vector2 b = { histX(t,     p), p.y + p.height - histCount[t][k]     * p.height / total };
                DrawLineEx(a, b, 2, speciesColor[k]);
            }
        }
    }
    int now[SPECIES_COUNT];
    countSpecies(now);
    float x = p.x, y = p.y + p.height + 26;
    for (int k = 0; k < SPECIES_COUNT; k++) {
        DrawCircleV((Vector2){ x + 5, y + 10 }, 5, speciesColor[k]);
        const char *name = presets[activePreset].names[k];
        drawLabel(name, x + 15, y, 19, COL_TEXT);
        x += 15 + labelWidth(name, 19) + 6;
        const char *n = TextFormat("%d", now[k]);
        drawLabel(n, x, y, 19, COL_MUTED);
        x += labelWidth(n, 19) + 18;
    }
    return y + 26;
}

static float reactionRate(int t) {
    int window = (int)(settings.rateWindowSecs * SIM_FPS * SUBSTEPS / histInterval + 0.5f);
    if (window < 1) window = 1;
    int t0 = t - window < 0 ? 0 : t - window;
    if (t == t0) return 0;
    float secs = (t - t0) * (histInterval / SUBSTEPS) / SIM_FPS;
    return (histCount[t0][RED_M] - histCount[t][RED_M]) / secs;
}

static void drawRateChart(Rectangle p) {
    float peak = 0;
    for (int t = 1; t < histLen; t++) peak = fmaxf(peak, reactionRate(t));
    float yMax = peak > 0 ? ceilf(peak / niceStep(peak, 3)) * niceStep(peak, 3) : 1;
    drawAxes(p, yMax, false, elapsedSecs());
    if (histLen > 1 && peak > 0) {
        Color c = speciesColor[PURPLE_M];
        for (int t = 1; t < histLen; t++) {
            float x0 = histX(t - 1, p), x1 = histX(t, p);
            float y0 = p.y + p.height - reactionRate(t - 1) / yMax * p.height;
            float y1 = p.y + p.height - reactionRate(t)     / yMax * p.height;
            float base = p.y + p.height;
            DrawTriangle((Vector2){ x0, y0 }, (Vector2){ x0, base }, (Vector2){ x1, base }, Fade(c, 0.18f));
            DrawTriangle((Vector2){ x0, y0 }, (Vector2){ x1, base }, (Vector2){ x1, y1 }, Fade(c, 0.18f));
            DrawLineEx((Vector2){ x0, y0 }, (Vector2){ x1, y1 }, 2, c);
        }
    }
}

static void readoutRow(float x, float right, float *y, const char *name, const char *value) {
    drawLabel(name, x, *y, 20, COL_MUTED);
    drawLabelRight(value, right, *y, 20, COL_TEXT);
    *y += 26;
}

void drawFigure(void) {
    const Preset *pre = &presets[activePreset];
    int notes = (pre->note[0] != NULL) + (pre->note[1] != NULL);
    const float cx = 14, cy = 14, cw = 460, pad = 16, axisW = 40;
    const float ch = 552 + 20 * notes;
    Rectangle card = { cx, cy, cw, ch };
    DrawRectangleRec(card, COL_CARD);
    DrawRectangleLinesEx(card, 1, COL_BORDER);

    float x = cx + pad, right = cx + cw - pad, y = cy + 12;
    drawLabel(pre->title, x, y, 30, COL_TEXT);
    y += 38;
    for (int i = 0; i < 2; i++) if (pre->note[i]) { drawLabel(pre->note[i], x, y, 16, COL_MUTED); y += 20; }
    y += 8;

    float kT = thermalEnergy(), now = histLen > 0 ? reactionRate(histLen - 1) : 0;
    drawLabel("Composition", x, y, 20, COL_TEXT);
    drawLabelRight("fraction of molecules", right, y + 3, 16, COL_MUTED);
    y += 30;
    y = drawHistory((Rectangle){ x + axisW, y, right - x - axisW, 130 }, (int)ball_count) + 6;

    drawLabel("Reaction rate", x, y, 20, COL_TEXT);
    drawLabelRight(TextFormat("now %.1f s^{−1}", now), right, y + 3, 16, COL_MUTED);
    y += 30;
    drawRateChart((Rectangle){ x + axisW, y, right - x - axisW, 100 });
    y += 100 + 22;
    drawLabelRight("time / s", right, y, 15, COL_MUTED);
    y += 26;

    DrawLineEx((Vector2){ x, y }, (Vector2){ right, y }, 1, COL_BORDER);
    y += 10;
    float ratio = kT > 0 ? reactionBarrier() / kT : 0;
    readoutRow(x, right, &y, "Temperature  T", temperatureText(kT / kT0));
    readoutRow(x, right, &y, "Barrier / kT", TextFormat("%.2f", ratio));
    readoutRow(x, right, &y, "Reacting collisions  e^{−E_a/kT}", TextFormat("%.1f%%", 100 * expf(-ratio)));
}

void stepSim(long *step, long n) {
    for (long s = 0; s < n; s++, (*step)++) {
        if (*step % SUBSTEPS == 0) {
            if (*step == 0) grid_clear();
            sortBalls();
        }
        integrateAllBalls();
        buildGrid();
        for (int i = 0; i < 2; i++) checkAllCollisions();
        checkAllBounds();
        if (*step % histInterval == 0) recordHistory();
    }
}

#define POP_VOICES     16
static Sound popVoice[POP_VOICES];
static int   nextVoice = 0;

void loadPopSound(void) {
    popVoice[0] = LoadSound("ball.mp3");
    for (int v = 1; v < POP_VOICES; v++) popVoice[v] = LoadSoundAlias(popVoice[0]);
}

#define MAX_POPS_PER_SEC 120.0f
static float popAllowance = 0;
#define LOUD_RATE      8.0f
static float recentRate = 0;

void playReactionSounds(unsigned reactions) {
    float dt = fmaxf(GetFrameTime(), 1e-3f);
    float k = fminf(dt / 0.5f, 1);
    recentRate += k * (reactions / dt - recentRate);
    float loud = sqrtf(LOUD_RATE / fmaxf(recentRate, LOUD_RATE));
    popAllowance = fminf(popAllowance + dt * MAX_POPS_PER_SEC, 2);

    for (unsigned n = 0; n < reactions && popAllowance >= 1; n++, popAllowance -= 1) {
        float pan = 0;
        if (pendingBurstCount > 0) {
            Vector2 at = pendingBurst[rand() % pendingBurstCount];
            pan = 0.8f * (2 * at.x / SIM_WIDTH - 1);
        }
        Sound s = popVoice[nextVoice];
        nextVoice = (nextVoice + 1) % POP_VOICES;
        SetSoundPitch(s, randf(0.85f, 1.25f));
        SetSoundVolume(s, loud * randf(0.6f, 1.0f));
        SetSoundPan(s, pan);
        PlaySound(s);
    }
}

void unloadPopSound(void) {
    for (int v = 1; v < POP_VOICES; v++) UnloadSoundAlias(popVoice[v]);
    UnloadSound(popVoice[0]);
}

#define MAX_BURSTS     64
#define BURST_DURATION 0.6f
typedef struct { Vector2 at; double born; } Burst;
static Burst bursts[MAX_BURSTS];
static int   burstNext = 0;

void clearBursts(void) {
    memset(bursts, 0, sizeof bursts);
    pendingBurstCount = 0;
}

void spawnPendingBursts(void) {
    double now = GetTime();
    for (unsigned i = 0; i < pendingBurstCount; i++) {
        bursts[burstNext] = (Burst){ pendingBurst[i], now };
        burstNext = (burstNext + 1) % MAX_BURSTS;
    }
    pendingBurstCount = 0;
}

void drawBursts(void) {
    double now = GetTime();
    for (int i = 0; i < MAX_BURSTS; i++) {
        if (bursts[i].born <= 0) continue;
        float t = (float)(now - bursts[i].born) / BURST_DURATION;
        if (t >= 1) { bursts[i].born = 0; continue; }
        float ease = 1 - (1 - t) * (1 - t);
        float r = halfSize + ease * ballSize * 2.5f;
        DrawRing(bursts[i].at, r - 2.5f * (1 - t), r, 0, 360, 32,
                 Fade(speciesColor[PURPLE_M], 1 - t));
    }
}

#define MIN_TEMPERATURE 0.05f
#define MAX_TEMPERATURE 20.0f
static float appliedTemperature = 1;

void resetSim(long *step, double *budget) {
    ball_count = 0;
    setBallSize(settings.ballSize);
    barrierUp = settings.separated;
    buildGrid();
    histLen = 0;
    histInterval = SUBSTEPS * 4;
    clearBursts();
    reactionCount = 0;
    *step = 0;
    *budget = 0;

    int want[2] = { (int)settings.redCount, (int)settings.blueCount }, made[2] = { 0, 0 };
    float v = settings.spawnSpeed;
    long maxTries = 20L * (want[0] + want[1]) + 10000;
    for (long tries = 0; (made[0] < want[0] || made[1] < want[1]) && tries < maxTries; tries++) {
        int k = made[0] * want[1] <= made[1] * want[0] ? 0 : 1;
        if (made[k] >= want[k]) k = 1 - k;
        float x0 = halfSize, x1 = SIM_WIDTH - halfSize;
        if (settings.separated) { if (k) x0 = BARRIER_X + halfSize; else x1 = BARRIER_X - halfSize; }
        if (trySpawn(randf(x0, x1), randf(halfSize, HEIGHT - halfSize),
                     randf(-v, v), randf(-v, v), k ? BLUE_M : RED_M)) {
            made[k]++;
        }
    }

    kT0 = thermalEnergy();
    settings.temperature = appliedTemperature = 1;
}

void applySettings(void) {
    activationEnergy = settings.activationKT0 * kT0;
    reactionHeat = settings.heatKT0 * kT0;
    float kT = thermalEnergy();
    if (kT > 0) {
        float target = settings.temperature != appliedTemperature ? settings.temperature
                     : fminf(kT / kT0, MAX_TEMPERATURE);
        if (fabsf(kT / kT0 - target) > 1e-4f * target) scaleTemperature(target * kT0 / kT);
        settings.temperature = appliedTemperature = target;
    }
    SetMasterVolume(settings.volume);
}

void styleGui(void) {
    GuiSetFont(hudFont);
    GuiSetStyle(DEFAULT, TEXT_SIZE, 18);
    GuiSetStyle(DEFAULT, TEXT_SPACING, 0);
    GuiSetStyle(DEFAULT, BACKGROUND_COLOR, 0x0e1018ff);
    GuiSetStyle(DEFAULT, LINE_COLOR,       0x5a6078ff);
    const int pal[12] = {
        0x34394aff, 0x181b26ff, 0xe4e6eeff,
        0x9aa0b2ff, 0x1e212bff, 0xffffffff,
        0xc4c8d4ff, 0x737a8eff, 0xffffffff,
        0x2a2e3cff, 0x12141cff, 0x5c6070ff,
    };
    for (int i = 0; i < 12; i++) GuiSetStyle(DEFAULT, i, pal[i]);
}

static void sliderRow(float x, float *y, float w, const char *name, const char *value,
                      float *v, float lo, float hi) {
    drawLabel(name, x, *y, 18, COL_TEXT);
    drawLabelRight(value, x + w, *y, 18, COL_MUTED);
    GuiSliderBar((Rectangle){ x, *y + 22, w, 14 }, NULL, NULL, v, lo, hi);
    *y += 44;
}

static float roundCount(float v) {
    float step = v < 1000 ? 1 : powf(10, floorf(log10f(v)) - 2);
    return roundf(v / step) * step;
}

static void countRow(float x, float *y, float w, const char *name, float *v) {
    float pos = cbrtf(*v / MAX_PER_SPECIES), before = pos;
    sliderRow(x, y, w, name, TextFormat("%d", (int)*v), &pos, 0, 1);
    if (pos != before) *v = fminf(roundCount(MAX_PER_SPECIES * pos * pos * pos), MAX_PER_SPECIES);
}

static void checkRow(float x, float *y, const char *name, bool *v) {
    GuiCheckBox((Rectangle){ x, *y + 2, 16, 16 }, name, v);
    *y += 26;
}

bool drawSettingsPanel(void) {
    const float px = SIM_WIDTH, x = px + 18, w = PANEL_WIDTH - 36;
    float y = 22;
    DrawRectangle(px, 0, PANEL_WIDTH, HEIGHT, COL_PANEL);
    DrawLineEx((Vector2){ px, 0 }, (Vector2){ px, HEIGHT }, 2, COL_BORDER);

    GuiLine((Rectangle){ x, y, w, 16 }, "Simulation"); y += 22;
    sliderRow(x, &y, w, "Speed (up/down)", TextFormat("%.3gx", exp2f(roundf(settings.speedLog2 * 4) / 4)),
              &settings.speedLog2, -4, 12);
    GuiCheckBox((Rectangle){ x, y + 7, 16, 16 }, "Paused (space)", &settings.paused);
    if (!barrierUp) GuiDisable();
    if (GuiButton((Rectangle){ x + w / 2, y, w / 2, 30 }, "Remove barrier (B)")) barrierUp = false;
    GuiEnable();
    y += 40;

    GuiLine((Rectangle){ x, y, w, 16 }, "Chemistry"); y += 22;
    sliderRow(x, &y, w, "Temperature (left/right)", temperatureText(settings.temperature),
              &settings.temperature, MIN_TEMPERATURE, MAX_TEMPERATURE);
    sliderRow(x, &y, w, "Activation energy", energyText(settings.activationKT0, false),
              &settings.activationKT0, 0, 10);
    sliderRow(x, &y, w, "Reaction heat",
              TextFormat("%s %s", energyText(settings.heatKT0, true), settings.heatKT0 > 0.05f ? "exothermic"
                         : settings.heatKT0 < -0.05f ? "endothermic" : ""),
              &settings.heatKT0, -10, 20);
    float ratio = fmaxf(settings.activationKT0, -settings.heatKT0) / settings.temperature;
    drawLabel("Reacting collisions  e^{−E_a/kT}", x, y, 18, COL_MUTED);
    drawLabelRight(TextFormat("%.2g%%", 100 * expf(-ratio)), x + w, y, 18, COL_TEXT);
    y += 28;

    GuiLine((Rectangle){ x, y, w, 16 }, "New run (applies on Reset)"); y += 22;
    float before = settings.perSpecies;
    countRow(x, &y, w, "Molecules per species", &settings.perSpecies);
    if (settings.perSpecies != before) settings.redCount = settings.blueCount = settings.perSpecies;
    countRow(x, &y, w, TextFormat("%s molecules", presets[activePreset].names[RED_M]),  &settings.redCount);
    countRow(x, &y, w, TextFormat("%s molecules", presets[activePreset].names[BLUE_M]), &settings.blueCount);
    sliderRow(x, &y, w, "Ball size", TextFormat("%d px", (int)settings.ballSize),
              &settings.ballSize, MIN_BALL_SIZE, MAX_BALL_SIZE);
    settings.ballSize = roundf(settings.ballSize);
    sliderRow(x, &y, w, "Starting speed", TextFormat("%.3f px/substep", settings.spawnSpeed),
              &settings.spawnSpeed, 0.002f, 0.5f);
    checkRow(x, &y, "Start separated by a barrier", &settings.separated);
    bool reset = GuiButton((Rectangle){ x, y, w, 30 }, "Reset (R)");
    y += 40;

    GuiLine((Rectangle){ x, y, w, 16 }, "Display"); y += 22;
    sliderRow(x, &y, w, "Rate smoothing", TextFormat("%.1f s", settings.rateWindowSecs),
              &settings.rateWindowSecs, 0.1f, 20);
    GuiCheckBox((Rectangle){ x + w / 2, y + 2, 16, 16 }, "Energy colours (E)", &settings.energyColors);
    checkRow(x, &y, "Charts", &settings.showCharts);
    GuiCheckBox((Rectangle){ x + w / 2, y + 2, 16, 16 }, "Fullscreen (F)", &settings.fullscreen);
    checkRow(x, &y, "Reaction bursts", &settings.showBursts);
    checkRow(x, &y, "Sound", &settings.sound);
    sliderRow(x, &y, w, "Volume", TextFormat("%d%%", (int)(settings.volume * 100 + 0.5f)),
              &settings.volume, 0, 1);

    GuiLine((Rectangle){ x, y, w, 16 }, "Preset (resets)"); y += 22;
    float bw = (w - 8 * (PRESET_COUNT - 1)) / PRESET_COUNT;
    for (int i = 0; i < PRESET_COUNT; i++) {
        if (GuiButton((Rectangle){ x + i * (bw + 8), y, bw, 30 }, presets[i].button)) {
            applyPreset(i);
            reset = true;
        }
    }
    return reset;
}

void drawEnergyKey(void) {
    float kT = thermalEnergy(), b = kT > 0 ? reactionBarrier() / kT : 0, max = energyScale(b);
    const float cw = 460, ch = 100, cx = 14, cy = HEIGHT - 14 - ch, pad = 16;
    Rectangle card = { cx, cy, cw, ch };
    DrawRectangleRec(card, COL_CARD);
    DrawRectangleLinesEx(card, 1, COL_BORDER);

    float x = cx + pad, right = cx + cw - pad;
    drawLabel("Kinetic energy  ½mv^2 / kT", x, cy + 10, 20, COL_TEXT);
    drawLabelRight("hot: E above the barrier", right, cy + 13, 16, COL_MUTED);
    Rectangle bar = { x, cy + 56, right - x, 14 };
    for (int i = 0; i < (int)bar.width; i++)
        DrawRectangle((int)bar.x + i, (int)bar.y, 1, (int)bar.height,
                      energyColor((i + 0.5f) / bar.width * max, b, max));
    DrawRectangleLinesEx(bar, 1, COL_BORDER);

    float step = niceStep(max, 6);
    for (int i = 0; i * step <= max * 1.001f; i++) {
        float tx = bar.x + i * step / max * bar.width;
        DrawLineEx((Vector2){ tx, bar.y + bar.height }, (Vector2){ tx, bar.y + bar.height + 4 }, 1, COL_MUTED);
        const char *t = TextFormat("%g", i * step);
        drawLabel(t, tx - labelWidth(t, 15) / 2, bar.y + bar.height + 5, 15, COL_MUTED);
    }
    float bx = bar.x + b / max * bar.width;
    DrawLineEx((Vector2){ bx, bar.y - 4 }, (Vector2){ bx, bar.y + bar.height + 4 }, 2, COL_TEXT);
    const char *bl = TextFormat("barrier %.2f", b);
    drawLabel(bl, Clamp(bx - labelWidth(bl, 15) / 2, x, right - labelWidth(bl, 15)), bar.y - 21, 15, COL_TEXT);
}

void drawTank(void) {
    DrawRectangle(0, 0, SIM_WIDTH, HEIGHT, COL_TANK);
    for (int x = 50; x < SIM_WIDTH; x += 50)
        DrawLine(x, 0, x, HEIGHT, x % 250 ? COL_GRID : COL_GRID_MAJ);
    for (int y = 50; y < HEIGHT; y += 50)
        DrawLine(0, y, SIM_WIDTH, y, y % 250 ? COL_GRID : COL_GRID_MAJ);
}

void drawBarrier(void) {
    const float x0 = BARRIER_X - 4, x1 = BARRIER_X + 4;
    Color edge = { 190, 196, 214, 220 }, hatch = { 190, 196, 214, 90 };
    DrawRectangle((int)x0, 0, (int)(x1 - x0), HEIGHT, Fade(COL_PANEL, 0.7f));
    for (float y = -8; y < HEIGHT; y += 8)
        DrawLineEx((Vector2){ x0, y + 8 }, (Vector2){ x1, y }, 1, hatch);
    DrawLineEx((Vector2){ x0, 0 }, (Vector2){ x0, HEIGHT }, 1.5f, edge);
    DrawLineEx((Vector2){ x1, 0 }, (Vector2){ x1, HEIGHT }, 1.5f, edge);
}

#define TARGET_FPS   180
#define MAX_FRAME_DT (1.0 / 20)

static bool setFullscreen(bool full) {
    if (full != IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)) ToggleBorderlessWindowed();
    int monitor = GetCurrentMonitor();
    screenW = full ? GetMonitorWidth(monitor)  : WINDOW_W;
    screenH = full ? GetMonitorHeight(monitor) : WINDOW_H;
    if (!ballgridAlloc(MAX_GRID_CELLS)) {
        CloseWindow();
        fprintf(stderr, "out of memory for the collision grid\n");
        return false;
    }
    return true;
}

int main(int argc, char **argv) {
    bool full = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--full") == 0) full = true;
        else { fprintf(stderr, "usage: %s [--full]\n", argv[0]); return 1; }
    }
    omp_set_num_threads(10);
    srand(time(NULL));

    InitWindow(WINDOW_W, WINDOW_H, "Reaction Simulation");
    settings.fullscreen = full;
    if (!setFullscreen(full)) return 1;
    SetTargetFPS(TARGET_FPS);
    InitAudioDevice();
    loadPopSound();
    loadHudFont();
    styleGui();

    loadBallTexture();
    ballRenderInit(BALL_FS);

    long step = 0;
    double budget = 0;
    applyPreset(0);
    resetSim(&step, &budget);

    while(!WindowShouldClose()) {
        if (IsKeyPressed(KEY_UP))    settings.speedLog2 = fminf(settings.speedLog2 + 1, 6);
        if (IsKeyPressed(KEY_DOWN))  settings.speedLog2 = fmaxf(settings.speedLog2 - 1, -4);
        if (IsKeyPressed(KEY_RIGHT)) settings.temperature = fminf(settings.temperature * 1.25f, MAX_TEMPERATURE);
        if (IsKeyPressed(KEY_LEFT))  settings.temperature = fmaxf(settings.temperature / 1.25f, MIN_TEMPERATURE);
        if (IsKeyPressed(KEY_SPACE)) settings.paused = !settings.paused;
        if (IsKeyPressed(KEY_R))     resetSim(&step, &budget);
        if (IsKeyPressed(KEY_B))     barrierUp = false;
        if (IsKeyPressed(KEY_E))     settings.energyColors = !settings.energyColors;
        if (IsKeyPressed(KEY_F))     settings.fullscreen = !settings.fullscreen;
        if (settings.fullscreen != IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)) {
            if (!setFullscreen(settings.fullscreen)) return 1;
            resetSim(&step, &budget);
        }
        applySettings();

        double speed = exp2(roundf(settings.speedLog2 * 4) / 4);
        double dt = fmin(GetFrameTime(), MAX_FRAME_DT);
        if (!settings.paused) budget += SUBSTEPS * SIM_FPS * speed * dt;
        long substeps = (long)budget;
        budget -= substeps;

        stepSim(&step, substeps);
        if (settings.sound) playReactionSounds(reactionCount);
        reactionCount = 0;
        if (settings.showBursts) spawnPendingBursts(); else clearBursts();

        BeginDrawing();
        drawTank();
        drawAllBalls();
        drawBursts();
        if (barrierUp) drawBarrier();
        if (settings.showCharts) drawFigure();
        if (settings.energyColors) drawEnergyKey();
        if (drawSettingsPanel()) resetSim(&step, &budget);
        SetWindowTitle(TextFormat("Fps: %d Balls: %zu Speed: %.3gx%s", GetFPS(), ball_count, speed,
                                  settings.paused ? " (paused)" : ""));

        EndDrawing();
    }
    UnloadFont(hudFont);
    unloadPopSound();
    CloseAudioDevice();
    ballRenderUnload();
    UnloadTexture(ballTex);
    CloseWindow();
    ballgridFree();
    return 0;
}