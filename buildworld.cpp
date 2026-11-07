// ============================================================================
//  BUILDWORLD — мобильная 3D-песочница из кубиков.
//  Один файл, C++14 + SDL2. Всё (текстуры, шрифт, звуки) создаётся кодом.
//  Рендер: SDL_RenderGeometry + сортировка треугольников по глубине.
// ============================================================================
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <map>
#include <algorithm>
#include <initializer_list>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t i32;
typedef uint64_t u64;
typedef int64_t i64;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------- утилиты
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline int clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smooth01(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
static inline int ifloor(double v) { int i = (int)v; return (v < i) ? i - 1 : i; }

struct Rng {
    u64 s;
    explicit Rng(u64 seed = 1) : s(seed * 2654435761ULL + 0x9E3779B97F4A7C15ULL) { next(); }
    u32 next() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return (u32)(s >> 33); }
    float f() { return (next() & 0xFFFFFF) / 16777216.0f; }          // [0,1)
    float sf() { return f() * 2.f - 1.f; }                              // [-1,1)
    int range(int n) { return n <= 0 ? 0 : (int)(next() % (u32)n); }
    int range(int a, int b) { return a + range(b - a + 1); }            // [a,b]
};

static inline u32 hash32(u32 x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x;
}
static inline u32 hash3i(int x, int y, int z, u32 seed) {
    return hash32((u32)x * 73856093U ^ hash32((u32)y * 19349663U ^ hash32((u32)z * 83492791U ^ seed)));
}

// ---------------------------------------------------------------- шум Перлина
struct Noise {
    u8 p[512];
    void init(u32 seed) {
        Rng r(seed);
        for (int i = 0; i < 256; i++) p[i] = (u8)i;
        for (int i = 255; i > 0; i--) { int j = r.range(i + 1); std::swap(p[i], p[j]); }
        for (int i = 0; i < 256; i++) p[256 + i] = p[i];
    }
    static inline float fade(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }
    static inline float grad2(int h, float x, float y) {
        switch (h & 7) {
            case 0: return x + y; case 1: return -x + y; case 2: return x - y; case 3: return -x - y;
            case 4: return x; case 5: return -x; case 6: return y; default: return -y;
        }
    }
    static inline float grad3(int h, float x, float y, float z) {
        int hh = h & 15; float u = hh < 8 ? x : y;
        float v = hh < 4 ? y : (hh == 12 || hh == 14 ? x : z);
        return ((hh & 1) ? -u : u) + ((hh & 2) ? -v : v);
    }
    float n2(float x, float y) const {
        int xi = ifloor(x), yi = ifloor(y);
        float xf = x - xi, yf = y - yi;
        xi &= 255; yi &= 255;
        float u = fade(xf), v = fade(yf);
        int aa = p[p[xi] + yi], ab = p[p[xi] + yi + 1], ba = p[p[xi + 1] + yi], bb = p[p[xi + 1] + yi + 1];
        float x1 = lerpf(grad2(aa, xf, yf), grad2(ba, xf - 1, yf), u);
        float x2 = lerpf(grad2(ab, xf, yf - 1), grad2(bb, xf - 1, yf - 1), u);
        return lerpf(x1, x2, v) * 0.7f;
    }
    float n3(float x, float y, float z) const {
        int xi = ifloor(x), yi = ifloor(y), zi = ifloor(z);
        float xf = x - xi, yf = y - yi, zf = z - zi;
        xi &= 255; yi &= 255; zi &= 255;
        float u = fade(xf), v = fade(yf), w = fade(zf);
        int a = p[xi] + yi, aa = p[a] + zi, ab = p[a + 1] + zi;
        int b = p[xi + 1] + yi, ba = p[b] + zi, bb = p[b + 1] + zi;
        float r = lerpf(lerpf(lerpf(grad3(p[aa], xf, yf, zf), grad3(p[ba], xf - 1, yf, zf), u),
                              lerpf(grad3(p[ab], xf, yf - 1, zf), grad3(p[bb], xf - 1, yf - 1, zf), u), v),
                        lerpf(lerpf(grad3(p[aa + 1], xf, yf, zf - 1), grad3(p[ba + 1], xf - 1, yf, zf - 1), u),
                              lerpf(grad3(p[ab + 1], xf, yf - 1, zf - 1), grad3(p[bb + 1], xf - 1, yf - 1, zf - 1), u), v), w);
        return r;
    }
    float fbm2(float x, float y, int oct) const {
        float s = 0, a = 1, n = 0;
        for (int i = 0; i < oct; i++) { s += n2(x, y) * a; n += a; a *= 0.5f; x *= 2.03f; y *= 2.03f; }
        return s / n;
    }
    float fbm3(float x, float y, float z, int oct) const {
        float s = 0, a = 1, n = 0;
        for (int i = 0; i < oct; i++) { s += n3(x, y, z) * a; n += a; a *= 0.5f; x *= 2.03f; y *= 2.03f; z *= 2.03f; }
        return s / n;
    }
};

// ---------------------------------------------------------------- мир: константы
enum { CW = 16, WH = 128, SECN = WH / 16, SEA = 48, CHUNK_VOL = CW * CW * WH };
static inline int cidx(int x, int y, int z) { return (y << 8) | (z << 4) | x; }

// направления граней: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z
static const int DX[6] = {1, -1, 0, 0, 0, 0};
static const int DY[6] = {0, 0, 1, -1, 0, 0};
static const int DZ[6] = {0, 0, 0, 0, 1, -1};
// горизонтальные направления: 0 +X, 1 +Z, 2 -X, 3 -Z
static const int HDX[4] = {1, 0, -1, 0};
static const int HDZ[4] = {0, 1, 0, -1};

// ---------------------------------------------------------------- блоки
enum Block : u8 {
    B_AIR = 0, B_GRASS, B_DIRT, B_STONE, B_COBBLE, B_SAND, B_GRAVEL, B_LOG, B_LEAVES, B_PLANKS,
    B_GLASS, B_COAL_ORE, B_IRON_ORE, B_GOLD_ORE, B_DIAMOND_ORE, B_BEDROCK, B_WATER, B_LAVA, B_SNOW, B_SNOWGRASS,
    B_ICE, B_CACTUS, B_SANDSTONE, B_PINE_LOG, B_PINE_LEAVES, B_TALLGRASS, B_FLOWER_R, B_FLOWER_Y, B_SAPLING, B_TORCH,
    B_CRAFT, B_FURNACE, B_CHEST, B_DOOR, B_LADDER, B_BED, B_STONEBRICK, B_OBSIDIAN, B_IRON_BLOCK, B_GOLD_BLOCK,
    B_DIAMOND_BLOCK, B_FURNACE_LIT,
    B_COUNT
};

// тайлы атласа (16x16 тайлов по 16 пикселей)
enum Tile {
    T_GRASS_TOP = 0, T_GRASS_SIDE, T_DIRT, T_STONE, T_COBBLE, T_SAND, T_GRAVEL, T_LOG, T_LOG_TOP, T_LEAVES, T_PLANKS,
    T_GLASS, T_COAL_ORE, T_IRON_ORE, T_GOLD_ORE, T_DIAMOND_ORE,
    T_BEDROCK = 16, T_WATER, T_LAVA, T_SNOW, T_SNOW_SIDE, T_ICE, T_CACTUS_SIDE, T_CACTUS_TOP, T_SANDSTONE, T_SANDSTONE_TOP,
    T_PINE_LOG, T_PINE_LEAVES, T_TALLGRASS, T_FLOWER_R, T_FLOWER_Y, T_SAPLING,
    T_TORCH = 32, T_CRAFT_TOP, T_CRAFT_SIDE, T_FURN_FRONT, T_FURN_SIDE, T_FURN_TOP, T_CHEST_FRONT, T_CHEST_SIDE, T_CHEST_TOP,
    T_DOOR_LO, T_DOOR_HI, T_LADDER, T_BED_HEAD, T_BED_FOOT, T_BED_SIDE, T_STONEBRICK,
    T_OBSIDIAN = 48, T_IRON_BLOCK, T_GOLD_BLOCK, T_DIAMOND_BLOCK, T_PINE_TOP, T_FURN_FRONT_LIT, T_BED_SIDE_HEAD, T_WHITE,
    T_CRACK0 = 56, T_SUN = 61, T_MOON = 62, T_FLAME = 63,
    T_STICK = 64, T_COAL, T_IRON_INGOT, T_GOLD_INGOT, T_DIAMOND, T_APPLE, T_RAWMEAT, T_COOKED, T_BONE, T_BONEMEAL,
    T_ROTTEN, T_FLUFF, T_DOOR_ITEM, T_BED_ITEM,
    T_HEART_F = 80, T_HEART_H, T_HEART_E, T_FOOD_F, T_FOOD_H, T_FOOD_E, T_BUBBLE, T_BOOK, T_WATER_FX, T_STRING, T_BOW, T_ARROW,
    T_TOOL0 = 96,                  // 96..115: материал*4 + вид
    T_TOPTUN_SIDE = 128, T_TOPTUN_FACE, T_TOPTUN_LEG, T_PUKH_BODY, T_PUKH_FACE, T_PUKH_LEG,
    T_GNIL_BODY, T_GNIL_FACE, T_GNIL_LEG, T_TEN_BODY, T_TEN_FACE, T_TEN_LEG, T_SKIN, T_SLEEVE, T_HORN,
    T_MUSHCAP, T_GNIL_TOP, T_TEN_SPIKE,
    T_ZOMB_FACE, T_ZOMB_SKIN, T_ZOMB_SHIRT, T_ZOMB_PANTS, T_SKEL_FACE, T_SKEL_BONE, T_SKEL_RIBS, T_SKEL_HOOD,
    T_SPID_FACE, T_SPID_BODY, T_SPID_ABDO, T_SPID_LEG, T_BOWWOOD,
};

enum Shape { SH_NONE = 0, SH_CUBE, SH_CROSS, SH_TORCH, SH_DOOR, SH_LADDER, SH_BED, SH_FLUID };
enum ToolKind { TK_PICK = 0, TK_AXE, TK_SHOVEL, TK_SWORD, TK_NONE = 255 };
enum SoundMat { SM_STONE = 0, SM_GRASS, SM_SAND, SM_WOOD, SM_GLASS, SM_SNOW };

struct BlockDef {
    const char* name;
    u8 tex[6];       // +X -X +Y -Y +Z -Z
    u8 shape;
    bool opaque;     // полностью непрозрачный куб (скрывает соседние грани, глушит свет)
    bool solid;      // есть столкновение
    u8 group;        // >0: грани между блоками одной группы не рисуются (стекло, листва, вода)
    float hardness;  // секунды рукой; <0 — неразрушимый
    u8 tool;         // подходящий инструмент
    u8 tier;         // требуемый уровень кирки (0 — любая/рука если tool!=pick), 255 — без требований
    u8 light;        // излучение света
    u8 sound;
    bool replaceable;
    bool needPick;   // без кирки ничего не выпадет
};

static BlockDef BD[B_COUNT];

static void defBlock(int id, const char* name, u8 t0, u8 t1, u8 t2, u8 t3, u8 t4, u8 t5, u8 shape, bool opaque, bool solid,
                     u8 group, float hard, u8 tool, u8 tier, u8 light, u8 snd, bool repl = false, bool needPick = false) {
    BlockDef& d = BD[id];
    d.name = name; d.tex[0] = t0; d.tex[1] = t1; d.tex[2] = t2; d.tex[3] = t3; d.tex[4] = t4; d.tex[5] = t5;
    d.shape = shape; d.opaque = opaque; d.solid = solid; d.group = group; d.hardness = hard; d.tool = tool; d.tier = tier;
    d.light = light; d.sound = snd; d.replaceable = repl; d.needPick = needPick;
}
static void defCube(int id, const char* name, u8 t, float hard, u8 tool, u8 snd, bool needPick = false, u8 tier = 0) {
    defBlock(id, name, t, t, t, t, t, t, SH_CUBE, true, true, 0, hard, tool, tier, 0, snd, false, needPick);
}

static void initBlocks() {
    defBlock(B_AIR, "ВОЗДУХ", 0, 0, 0, 0, 0, 0, SH_NONE, false, false, 0, 0, TK_NONE, 0, 0, SM_STONE, true);
    defBlock(B_GRASS, "ДЁРН", T_GRASS_SIDE, T_GRASS_SIDE, T_GRASS_TOP, T_DIRT, T_GRASS_SIDE, T_GRASS_SIDE, SH_CUBE, true, true, 0, 0.6f, TK_SHOVEL, 0, 0, SM_GRASS);
    defCube(B_DIRT, "ЗЕМЛЯ", T_DIRT, 0.5f, TK_SHOVEL, SM_GRASS);
    defCube(B_STONE, "КАМЕНЬ", T_STONE, 1.5f, TK_PICK, SM_STONE, true);
    defCube(B_COBBLE, "БУЛЫЖНИК", T_COBBLE, 2.0f, TK_PICK, SM_STONE, true);
    defCube(B_SAND, "ПЕСОК", T_SAND, 0.5f, TK_SHOVEL, SM_SAND);
    defCube(B_GRAVEL, "ГРАВИЙ", T_GRAVEL, 0.6f, TK_SHOVEL, SM_SAND);
    defBlock(B_LOG, "БРЕВНО", T_LOG, T_LOG, T_LOG_TOP, T_LOG_TOP, T_LOG, T_LOG, SH_CUBE, true, true, 0, 2.0f, TK_AXE, 0, 0, SM_WOOD);
    defBlock(B_LEAVES, "ЛИСТВА", T_LEAVES, T_LEAVES, T_LEAVES, T_LEAVES, T_LEAVES, T_LEAVES, SH_CUBE, false, true, 2, 0.2f, TK_NONE, 0, 0, SM_GRASS);
    defCube(B_PLANKS, "ДОСКИ", T_PLANKS, 2.0f, TK_AXE, SM_WOOD);
    defBlock(B_GLASS, "СТЕКЛО", T_GLASS, T_GLASS, T_GLASS, T_GLASS, T_GLASS, T_GLASS, SH_CUBE, false, true, 1, 0.3f, TK_NONE, 0, 0, SM_GLASS);
    defCube(B_COAL_ORE, "УГОЛЬНАЯ РУДА", T_COAL_ORE, 3.0f, TK_PICK, SM_STONE, true, 0);
    defCube(B_IRON_ORE, "ЖЕЛЕЗНАЯ РУДА", T_IRON_ORE, 3.0f, TK_PICK, SM_STONE, true, 1);
    defCube(B_GOLD_ORE, "ЗОЛОТАЯ РУДА", T_GOLD_ORE, 3.0f, TK_PICK, SM_STONE, true, 2);
    defCube(B_DIAMOND_ORE, "АЛМАЗНАЯ РУДА", T_DIAMOND_ORE, 3.0f, TK_PICK, SM_STONE, true, 2);
    defCube(B_BEDROCK, "ТВЕРДЬ", T_BEDROCK, -1, TK_NONE, SM_STONE);
    defBlock(B_WATER, "ВОДА", T_WATER, T_WATER, T_WATER, T_WATER, T_WATER, T_WATER, SH_FLUID, false, false, 3, -1, TK_NONE, 0, 0, SM_STONE, true);
    defBlock(B_LAVA, "ЛАВА", T_LAVA, T_LAVA, T_LAVA, T_LAVA, T_LAVA, T_LAVA, SH_FLUID, false, false, 4, -1, TK_NONE, 0, 15, SM_STONE, true);
    defCube(B_SNOW, "СНЕГ", T_SNOW, 0.3f, TK_SHOVEL, SM_SNOW);
    defBlock(B_SNOWGRASS, "СНЕЖНЫЙ ДЁРН", T_SNOW_SIDE, T_SNOW_SIDE, T_SNOW, T_DIRT, T_SNOW_SIDE, T_SNOW_SIDE, SH_CUBE, true, true, 0, 0.6f, TK_SHOVEL, 0, 0, SM_SNOW);
    defBlock(B_ICE, "ЛЁД", T_ICE, T_ICE, T_ICE, T_ICE, T_ICE, T_ICE, SH_CUBE, false, true, 5, 0.5f, TK_PICK, 0, 0, SM_GLASS);
    defBlock(B_CACTUS, "КАКТУС", T_CACTUS_SIDE, T_CACTUS_SIDE, T_CACTUS_TOP, T_CACTUS_TOP, T_CACTUS_SIDE, T_CACTUS_SIDE, SH_CUBE, false, true, 0, 0.4f, TK_NONE, 0, 0, SM_GRASS);
    defBlock(B_SANDSTONE, "ПЕСЧАНИК", T_SANDSTONE, T_SANDSTONE, T_SANDSTONE_TOP, T_SANDSTONE_TOP, T_SANDSTONE, T_SANDSTONE, SH_CUBE, true, true, 0, 0.8f, TK_PICK, 0, 0, SM_STONE, false, true);
    defBlock(B_PINE_LOG, "ЕЛОВОЕ БРЕВНО", T_PINE_LOG, T_PINE_LOG, T_PINE_TOP, T_PINE_TOP, T_PINE_LOG, T_PINE_LOG, SH_CUBE, true, true, 0, 2.0f, TK_AXE, 0, 0, SM_WOOD);
    defBlock(B_PINE_LEAVES, "ХВОЯ", T_PINE_LEAVES, T_PINE_LEAVES, T_PINE_LEAVES, T_PINE_LEAVES, T_PINE_LEAVES, T_PINE_LEAVES, SH_CUBE, false, true, 6, 0.2f, TK_NONE, 0, 0, SM_GRASS);
    defBlock(B_TALLGRASS, "ВЫСОКАЯ ТРАВА", T_TALLGRASS, 0, 0, 0, 0, 0, SH_CROSS, false, false, 0, 0, TK_NONE, 0, 0, SM_GRASS, true);
    defBlock(B_FLOWER_R, "АЛЫЙ ЦВЕТОК", T_FLOWER_R, 0, 0, 0, 0, 0, SH_CROSS, false, false, 0, 0, TK_NONE, 0, 0, SM_GRASS);
    defBlock(B_FLOWER_Y, "ЖЁЛТЫЙ ЦВЕТОК", T_FLOWER_Y, 0, 0, 0, 0, 0, SH_CROSS, false, false, 0, 0, TK_NONE, 0, 0, SM_GRASS);
    defBlock(B_SAPLING, "САЖЕНЕЦ", T_SAPLING, 0, 0, 0, 0, 0, SH_CROSS, false, false, 0, 0, TK_NONE, 0, 0, SM_GRASS);
    defBlock(B_TORCH, "ФАКЕЛ", T_TORCH, 0, 0, 0, 0, 0, SH_TORCH, false, false, 0, 0, TK_NONE, 0, 14, SM_WOOD);
    defBlock(B_CRAFT, "ВЕРСТАК", T_CRAFT_SIDE, T_CRAFT_SIDE, T_CRAFT_TOP, T_PLANKS, T_CRAFT_SIDE, T_CRAFT_SIDE, SH_CUBE, true, true, 0, 2.5f, TK_AXE, 0, 0, SM_WOOD);
    defBlock(B_FURNACE, "ПЕЧЬ", T_FURN_SIDE, T_FURN_SIDE, T_FURN_TOP, T_FURN_TOP, T_FURN_SIDE, T_FURN_SIDE, SH_CUBE, true, true, 0, 3.5f, TK_PICK, 0, 0, SM_STONE, false, true);
    defBlock(B_FURNACE_LIT, "ПЕЧЬ", T_FURN_SIDE, T_FURN_SIDE, T_FURN_TOP, T_FURN_TOP, T_FURN_SIDE, T_FURN_SIDE, SH_CUBE, true, true, 0, 3.5f, TK_PICK, 0, 13, SM_STONE, false, true);
    defBlock(B_CHEST, "СУНДУК", T_CHEST_SIDE, T_CHEST_SIDE, T_CHEST_TOP, T_CHEST_TOP, T_CHEST_SIDE, T_CHEST_SIDE, SH_CUBE, true, true, 0, 2.5f, TK_AXE, 0, 0, SM_WOOD);
    defBlock(B_DOOR, "ДВЕРЬ", T_DOOR_LO, 0, 0, 0, 0, 0, SH_DOOR, false, true, 0, 3.0f, TK_AXE, 0, 0, SM_WOOD);
    defBlock(B_LADDER, "ЛЕСТНИЦА", T_LADDER, 0, 0, 0, 0, 0, SH_LADDER, false, false, 0, 0.4f, TK_AXE, 0, 0, SM_WOOD);
    defBlock(B_BED, "КРОВАТЬ", T_BED_SIDE, 0, 0, 0, 0, 0, SH_BED, false, true, 0, 0.2f, TK_NONE, 0, 0, SM_WOOD);
    defCube(B_STONEBRICK, "КАМЕННЫЙ КИРПИЧ", T_STONEBRICK, 1.5f, TK_PICK, SM_STONE, true);
    defCube(B_OBSIDIAN, "ВУЛКАНИТ", T_OBSIDIAN, 40.0f, TK_PICK, SM_STONE, true, 3);
    defCube(B_IRON_BLOCK, "ЖЕЛЕЗНЫЙ БЛОК", T_IRON_BLOCK, 5.0f, TK_PICK, SM_STONE, true, 1);
    defCube(B_GOLD_BLOCK, "ЗОЛОТОЙ БЛОК", T_GOLD_BLOCK, 3.0f, TK_PICK, SM_STONE, true, 2);
    defCube(B_DIAMOND_BLOCK, "АЛМАЗНЫЙ БЛОК", T_DIAMOND_BLOCK, 5.0f, TK_PICK, SM_STONE, true, 2);
}

// ---------------------------------------------------------------- предметы
enum Item : u16 {
    I_STICK = 64, I_COAL, I_IRON, I_GOLD, I_DIAMOND, I_APPLE, I_RAWMEAT, I_COOKED, I_BONE, I_BONEMEAL, I_ROTTEN, I_FLUFF,
    I_STRING, I_BOW, I_ARROW,
    I_TOOL0 = 80,   // 80..99: материал*4 + вид
    I_END = 100
};
enum Mat { MT_WOOD = 0, MT_STONE, MT_IRON, MT_GOLD, MT_DIAMOND };
static const char* MAT_NAME_M[5] = {"ДЕРЕВЯННЫЙ", "КАМЕННЫЙ", "ЖЕЛЕЗНЫЙ", "ЗОЛОТОЙ", "АЛМАЗНЫЙ"};
static const char* MAT_NAME_F[5] = {"ДЕРЕВЯННАЯ", "КАМЕННАЯ", "ЖЕЛЕЗНАЯ", "ЗОЛОТАЯ", "АЛМАЗНАЯ"};
static const int MAT_DUR[5] = {60, 132, 251, 33, 1562};
static const float MAT_SPEED[5] = {2.f, 4.f, 6.f, 12.f, 8.f};
static const int MAT_TIER[5] = {0, 1, 2, 0, 3};
static const float SWORD_DMG[5] = {4, 5, 6, 4, 7};

static inline bool isTool(int id) { return id >= I_TOOL0 && id < I_TOOL0 + 20; }
static inline int toolMat(int id) { return (id - I_TOOL0) / 4; }
static inline int toolKind(int id) { return (id - I_TOOL0) % 4; }
static inline int toolId(int mat, int kind) { return I_TOOL0 + mat * 4 + kind; }
static inline bool isBlockItem(int id) { return id > 0 && id < B_COUNT; }

static std::string itemName(int id) {
    if (isBlockItem(id)) return BD[id].name;
    if (isTool(id)) {
        int m = toolMat(id), k = toolKind(id);
        switch (k) {
            case TK_PICK: return std::string(MAT_NAME_F[m]) + " КИРКА";
            case TK_AXE: return std::string(MAT_NAME_M[m]) + " ТОПОР";
            case TK_SHOVEL: return std::string(MAT_NAME_F[m]) + " ЛОПАТА";
            default: return std::string(MAT_NAME_M[m]) + " МЕЧ";
        }
    }
    switch (id) {
        case I_STICK: return "ПАЛКА"; case I_COAL: return "УГОЛЬ"; case I_IRON: return "ЖЕЛЕЗНЫЙ СЛИТОК";
        case I_GOLD: return "ЗОЛОТОЙ СЛИТОК"; case I_DIAMOND: return "АЛМАЗ"; case I_APPLE: return "ЯБЛОКО";
        case I_RAWMEAT: return "СЫРОЕ МЯСО"; case I_COOKED: return "ЖАРЕНОЕ МЯСО"; case I_BONE: return "КОСТЬ";
        case I_BONEMEAL: return "КОСТНАЯ МУКА"; case I_ROTTEN: return "ГНИЛАЯ ПЛОТЬ"; case I_FLUFF: return "ПУХ";
        case I_STRING: return "НИТЬ"; case I_BOW: return "ЛУК"; case I_ARROW: return "СТРЕЛА";
    }
    return "?";
}
static int itemTile(int id) {
    if (id == B_DOOR) return T_DOOR_ITEM;
    if (id == B_BED) return T_BED_ITEM;
    if (isBlockItem(id)) {
        const BlockDef& d = BD[id];
        if (d.shape == SH_CUBE || d.shape == SH_FLUID) return d.tex[4];
        return d.tex[0];
    }
    if (isTool(id)) return T_TOOL0 + (id - I_TOOL0);
    if (id >= I_STICK && id <= I_FLUFF) return T_STICK + (id - I_STICK);
    if (id == I_STRING) return T_STRING;
    if (id == I_BOW) return T_BOW;
    if (id == I_ARROW) return T_ARROW;
    return T_WHITE;
}
static inline int maxStack(int id) { return (isTool(id) || id == I_BOW) ? 1 : 64; }
static inline int foodValue(int id) {
    switch (id) { case I_APPLE: return 4; case I_RAWMEAT: return 3; case I_COOKED: return 8; case I_ROTTEN: return 2; }
    return 0;
}
static inline bool itemIsCubeIcon(int id) {
    if (!isBlockItem(id)) return false;
    int s = BD[id].shape;
    return s == SH_CUBE || s == SH_FLUID;
}

struct Stack {
    u16 id = 0, cnt = 0, dmg = 0;
    bool empty() const { return id == 0 || cnt == 0; }
    void clear() { id = cnt = dmg = 0; }
};
static inline Stack mkStack(int id, int cnt) { Stack s; s.id = (u16)id; s.cnt = (u16)cnt; s.dmg = 0; return s; }

// ============================================================================
//  ТЕКСТУРЫ: атлас 256x256 (16x16 тайлов по 16x16 пикселей), рисуется кодом
// ============================================================================
enum { ATLAS = 256 };
static u32 g_atlasPx[ATLAS * ATLAS];

static inline u32 RGBA(int r, int g, int b, int a = 255) {
    r = clampi(r, 0, 255); g = clampi(g, 0, 255); b = clampi(b, 0, 255); a = clampi(a, 0, 255);
    return ((u32)a << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;   // SDL_PIXELFORMAT_ABGR8888
}
static inline int pR(u32 c) { return c & 255; }
static inline int pG(u32 c) { return (c >> 8) & 255; }
static inline int pB(u32 c) { return (c >> 16) & 255; }
static inline int pA(u32 c) { return (c >> 24) & 255; }

struct TP {   // рисовальщик одного тайла
    int tx, ty; Rng r;
    explicit TP(int tile) : tx((tile & 15) * 16), ty((tile >> 4) * 16), r(1000 + tile * 7919) {}
    void set(int x, int y, u32 c) { if (x >= 0 && y >= 0 && x < 16 && y < 16) g_atlasPx[(ty + y) * ATLAS + tx + x] = c; }
    u32 get(int x, int y) { x &= 15; y &= 15; return g_atlasPx[(ty + y) * ATLAS + tx + x]; }
    void fill(int R, int G, int B, int var, int a = 255) {
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
            int v = (int)(r.sf() * var);
            set(x, y, RGBA(R + v, G + v, B + v, a));
        }
    }
    void clear() { for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) set(x, y, 0); }
    void shade(int x, int y, int d) {
        u32 c = get(x, y); set(x, y, RGBA(pR(c) + d, pG(c) + d, pB(c) + d, pA(c)));
    }
    void rect(int x0, int y0, int x1, int y1, u32 c) { for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) set(x, y, c); }
    void line(int x0, int y0, int x1, int y1, u32 c) {
        int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
        for (;;) { set(x0, y0, c); if (x0 == x1 && y0 == y1) break; int e2 = 2 * e; if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; } }
    }
    void copyFrom(int tile) {
        int sx = (tile & 15) * 16, sy = (tile >> 4) * 16;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) set(x, y, g_atlasPx[(sy + y) * ATLAS + sx + x]);
    }
};

// ---------------------------------------------------------------- инструменты пиксель-арта
static inline u32 HEX(u32 h, int a = 255) { return RGBA((int)((h >> 16) & 255), (int)((h >> 8) & 255), (int)(h & 255), a); }
static inline u32 mixc(u32 a, u32 b, float t) {
    return RGBA((int)lerpf((float)pR(a), (float)pR(b), t), (int)lerpf((float)pG(a), (float)pG(b), t),
                (int)lerpf((float)pB(a), (float)pB(b), t), (int)lerpf((float)pA(a), (float)pA(b), t));
}
static inline u32 shadeC(u32 c, int d) { return RGBA(pR(c) + d, pG(c) + d, pB(c) + d, pA(c)); }
typedef std::vector<u32> Pal;
static Pal mkPal(std::initializer_list<u32> hx) { Pal p; for (u32 h : hx) p.push_back(HEX(h)); return p; }
static const float BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
// квантование в палитру с лёгким упорядоченным размытием границ
static inline u32 rampPick(const Pal& p, float t, int x, int y, float dither = 0.5f) {
    int n = (int)p.size();
    t += (BAYER4[y & 3][x & 3] / 16.f - 0.5f) * dither / n;
    return p[clampi((int)(t * n), 0, n - 1)];
}
// бесшовный сглаженный шум 16x16, нормированный в 0..1
struct TN {
    float v[16][16];
    float at(int x, int y) const { return v[y & 15][x & 15]; }
};
static TN makeNoise(u32 seed, std::initializer_list<std::pair<int, float>> octaves) {
    TN n; memset(n.v, 0, sizeof(n.v));
    int o = 0;
    for (auto& oc : octaves) {
        int cell = oc.first, g = 16 / cell;
        float grid[16][16];
        for (int j = 0; j < g; j++) for (int i = 0; i < g; i++) grid[j][i] = (hash32(seed * 7919u + o * 104729u + j * 31u + i) & 0xFFFF) / 65535.f;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
            float u = (x + 0.5f) / cell - 0.5f, w = (y + 0.5f) / cell - 0.5f;
            int i0 = ifloor(u), j0 = ifloor(w);
            float fu = u - i0, fw = w - j0;
            fu = fu * fu * (3 - 2 * fu); fw = fw * fw * (3 - 2 * fw);
            int a0 = ((i0 % g) + g) % g, a1 = (a0 + 1) % g, b0 = ((j0 % g) + g) % g, b1 = (b0 + 1) % g;
            float val = lerpf(lerpf(grid[b0][a0], grid[b0][a1], fu), lerpf(grid[b1][a0], grid[b1][a1], fu), fw);
            n.v[y][x] += val * oc.second;
        }
        o++;
    }
    float mn = 1e9f, mx = -1e9f;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { mn = std::min(mn, n.v[y][x]); mx = std::max(mx, n.v[y][x]); }
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) n.v[y][x] = (n.v[y][x] - mn) / std::max(1e-5f, mx - mn);
    return n;
}
static void fillNoise(TP& t, const Pal& p, const TN& n, float dither = 0.5f, int a = 255) {
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { u32 c = rampPick(p, n.at(x, y), x, y, dither); t.set(x, y, RGBA(pR(c), pG(c), pB(c), a)); }
}
static void bevel(TP& t, int x0, int y0, int x1, int y1, int hl, int sh) {
    for (int x = x0; x <= x1; x++) { t.shade(x, y0, hl); t.shade(x, y1, sh); }
    for (int y = y0 + 1; y < y1; y++) { t.shade(x0, y, hl); t.shade(x1, y, sh); }
}
// камни по диаграмме Вороного (бесшовно): каждый камень своего тона, со швами и фаской
static void stonesTex(TP& t, int nSeeds, u32 seed, const Pal& pal, u32 mortar, float mortarW, int hl, int sh, const Pal* alt = nullptr) {
    float sx[24], sy[24]; int n = 0;
    Rng r(seed);
    for (int tries = 0; tries < 2000 && n < nSeeds; tries++) {
        float x = r.f() * 16, y = r.f() * 16; bool ok = true;
        for (int i = 0; i < n && ok; i++) {
            float dx = fabsf(x - sx[i]), dy = fabsf(y - sy[i]); dx = std::min(dx, 16 - dx); dy = std::min(dy, 16 - dy);
            if (dx * dx + dy * dy < (16.f * 16.f / nSeeds) * 0.55f) ok = false;
        }
        if (ok) { sx[n] = x; sy[n] = y; n++; }
    }
    int id[16][16]; bool mor[16][16];
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        float b1 = 1e9f, b2 = 1e9f; int bi = 0;
        for (int i = 0; i < n; i++) {
            float dx = fabsf(x + 0.5f - sx[i]), dy = fabsf(y + 0.5f - sy[i]); dx = std::min(dx, 16 - dx); dy = std::min(dy, 16 - dy);
            float d = sqrtf(dx * dx + dy * dy);
            if (d < b1) { b2 = b1; b1 = d; bi = i; } else if (d < b2) b2 = d;
        }
        id[y][x] = bi; mor[y][x] = (b2 - b1) < mortarW;
    }
    TN nz = makeNoise(seed + 5, {{4, 0.5f}, {2, 0.35f}, {1, 0.15f}});
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        if (mor[y][x]) { t.set(x, y, mortar); continue; }
        int s = id[y][x];
        const Pal& p = (alt && (hash32(seed + s * 17) % 3 == 0)) ? *alt : pal;
        float base = (hash32(seed * 3 + s * 101) % 1000) / 1000.f;
        float v = clampf(base * 0.65f + nz.at(x, y) * 0.35f, 0, 0.999f);
        u32 c = rampPick(p, v, x, y, 0.35f);
        bool up = mor[(y + 15) & 15][x], lf = mor[y][(x + 15) & 15], dn = mor[(y + 1) & 15][x], rt = mor[y][(x + 1) & 15];
        if (up || lf) c = shadeC(c, hl);
        else if (dn || rt) c = shadeC(c, -sh);
        t.set(x, y, c);
    }
}
// самоцвет/вкрапление: маска с объёмом (светлый верх-лево, тёмный низ-право) и тенью на камне
static void gem(TP& t, int ox, int oy, const char* const* mask, int h, u32 dark, u32 mid, u32 light, u32 glint) {
    auto in = [&](int x, int y) { if (y < 0 || y >= h || x < 0 || x >= (int)strlen(mask[y])) return false; return mask[y][x] == '#'; };
    int w = 0; for (int y = 0; y < h; y++) w = std::max(w, (int)strlen(mask[y]));
    // тень на камне снизу и справа
    for (int y = 0; y <= h; y++) for (int x = 0; x <= w; x++)
        if (!in(x, y) && (in(x - 1, y) || in(x, y - 1))) t.shade(ox + x, oy + y, -26);
    bool glinted = false;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        if (!in(x, y)) continue;
        u32 c = mid;
        bool topL = !in(x, y - 1) || !in(x - 1, y), botR = !in(x, y + 1) || !in(x + 1, y);
        if (botR && !topL) c = dark; else if (topL && !botR) c = light;
        if (!glinted && topL) { c = glint; glinted = true; }
        t.set(ox + x, oy + y, c);
    }
}
static void placeGems(TP& t, u32 seed, int count, float minDist, const char* const* const* masks, const int* heights, int nMasks,
                      u32 dark, u32 mid, u32 light, u32 glint) {
    Rng r(seed); int px[16], py[16], n = 0;
    for (int tries = 0; tries < 3000 && n < count; tries++) {
        int x = r.range(1, 12), y = r.range(1, 12); bool ok = true;
        for (int i = 0; i < n && ok; i++) if ((x - px[i]) * (x - px[i]) + (y - py[i]) * (y - py[i]) < minDist * minDist) ok = false;
        if (!ok) continue;
        px[n] = x; py[n] = y; n++;
        int m = r.range(nMasks);
        gem(t, x, y, masks[m], heights[m], dark, mid, light, glint);
    }
}
// кора: вертикальные борозды, слегка петляющие
static void barkTex(TP& t, const Pal& p, u32 seed) {
    TN n = makeNoise(seed, {{4, 0.6f}, {2, 0.4f}});
    const int grooves[4] = {1, 5, 9, 13};
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        float v = 0.35f + n.at(x * 3, y) * 0.5f;
        for (int g = 0; g < 4; g++) {
            int gx = grooves[g] + (int)(n.at(g * 4, y) * 2.2f) - 1;
            int d = abs(((x - gx) + 16 + 8) % 16 - 8);
            if (d == 0) v = 0.02f; else if (d == 1 && ((x - gx + 16) % 16) == 1) v = std::min(v + 0.35f, 0.99f);
        }
        t.set(x, y, rampPick(p, v, x, y, 0.3f));
    }
}
// торец бревна: кольца и кора по краю
static void ringsTex(TP& t, const Pal& rings, u32 barkD, u32 barkL) {
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        float dx = x - 7.5f, dy = y - 7.5f;
        float cheb = std::max(fabsf(dx), fabsf(dy)), eu = sqrtf(dx * dx + dy * dy);
        float d = cheb * 0.55f + eu * 0.45f;
        if (cheb >= 7.0f) { t.set(x, y, ((x + y) & 1) ? barkD : barkL); continue; }
        int ring = (int)(d * 0.9f + 0.25f * sinf(atan2f(dy, dx) * 3.f));
        int idx = (ring % 2 == 0) ? (int)rings.size() - 1 : (int)rings.size() - 2;
        if (cheb >= 6.0f) idx = 0;
        if (d < 1.2f) idx = 1;
        t.set(x, y, rings[clampi(idx, 0, (int)rings.size() - 1)]);
    }
}
// доски: 4 доски с фаской, волокнами и стыками
static void planksTex(TP& t, const Pal& p, u32 seam, u32 seed) {
    TN grain = makeNoise(seed, {{8, 0.5f}, {2, 0.5f}});
    const int joint[4] = {11, 3, 14, 7};
    for (int b = 0; b < 4; b++) {
        int y0 = b * 4;
        float tone = 0.25f + (hash32(seed + b * 31) % 100) / 100.f * 0.4f;
        for (int y = y0; y < y0 + 4; y++) for (int x = 0; x < 16; x++) {
            float g = grain.at(x / 3 + b * 5, y * 4);
            float v = clampf(tone + (g - 0.5f) * 0.5f, 0, 0.999f);
            u32 c = rampPick(p, v, x, y, 0.25f);
            if (y == y0) c = shadeC(c, 16);
            if (y == y0 + 3) c = seam;
            if (x == joint[b]) c = (y == y0 + 3) ? seam : shadeC(seam, 12);
            if (x == joint[b] + 1 && y != y0 + 3) c = shadeC(c, 14);
            t.set(x, y, c);
        }
    }
}
// кромка травы/снега, свисающая на боковину земли
static void overhang(TP& t, const Pal& p, u32 seed, int minD, int maxD, u32 edge) {
    TN n = makeNoise(seed, {{4, 0.6f}, {1, 0.4f}});
    for (int x = 0; x < 16; x++) {
        int d = minD + (int)(n.at(x, 3) * (maxD - minD + 1));
        if (hash32(seed + x * 13) % 5 == 0) d += 2;   // капли
        for (int y = 0; y < d; y++) t.set(x, y, rampPick(p, 1.f - (float)y / (d + 1) * 0.8f + (n.at(x, y) - 0.5f) * 0.3f, x, y, 0.4f));
        t.set(x, d, edge);
        t.shade(x, d + 1, -18);
    }
}

// ---------------------------------------------------------------- палитры
static const Pal& palStone() { static Pal p = mkPal({0x5f6166, 0x6d6f74, 0x7b7d82, 0x888a8f, 0x96989d, 0xa3a5a9}); return p; }
static void stoneBase(TP& t, u32 seed) {
    TN n = makeNoise(seed, {{8, 0.45f}, {4, 0.35f}, {2, 0.2f}});
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) t.set(x, y, rampPick(palStone(), 0.15f + n.at(x, y) * 0.7f, x, y, 0.6f));
    // тонкие трещинки
}

// ---------------------------------------------------------------- блоки
static void genBlockTextures() {
    // ЗЕМЛЯ
    const Pal dirtP = mkPal({0x4f3423, 0x60402a, 0x734e33, 0x85603e, 0x97704a});
    { TP t(T_DIRT); TN n = makeNoise(11, {{4, 0.5f}, {2, 0.3f}, {1, 0.2f}}); fillNoise(t, dirtP, n, 0.7f);
      const int pb[5][2] = {{3, 3}, {11, 5}, {6, 10}, {13, 12}, {1, 13}};
      for (auto& q : pb) { t.set(q[0], q[1], HEX(0x9c8c7a)); t.set(q[0] + 1, q[1], HEX(0x7d6f60)); t.set(q[0], q[1] + 1, HEX(0x3e2a1c)); } }
    // ДЁРН
    const Pal grassP = mkPal({0x3b7329, 0x468532, 0x52983a, 0x60aa43, 0x72bd50});
    { TP t(T_GRASS_TOP); TN n = makeNoise(12, {{4, 0.45f}, {2, 0.35f}, {1, 0.2f}}); fillNoise(t, grassP, n, 0.8f);
      Rng r(5); for (int i = 0; i < 9; i++) { int x = r.range(16), y = r.range(16); t.set(x, y, HEX(0x8fd062)); t.set(x, (y + 1) & 15, HEX(0x6cb64a)); t.set(x, (y + 2) & 15, HEX(0x356a25)); } }
    { TP t(T_GRASS_SIDE); t.copyFrom(T_DIRT); overhang(t, grassP, 13, 2, 4, HEX(0x2f5f21)); }
    // КАМЕНЬ
    { TP t(T_STONE); stoneBase(t, 14); }
    // БУЛЫЖНИК
    { TP t(T_COBBLE); Pal p = mkPal({0x5c5e63, 0x6c6e73, 0x7d7f84, 0x8e9095, 0x9fa1a5}); stonesTex(t, 7, 15, p, HEX(0x3c3d41), 0.9f, 22, 18); }
    // ПЕСОК
    { TP t(T_SAND); Pal p = mkPal({0xcdb57c, 0xd8c28a, 0xe1cd98, 0xe9d7a6, 0xf0e1b5});
      TN n = makeNoise(16, {{8, 0.3f}, {2, 0.4f}, {1, 0.3f}});
      for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
          float rip = sinf((x * 0.4f + y) * 0.785f) * 0.12f;
          t.set(x, y, rampPick(p, 0.2f + n.at(x, y) * 0.6f + rip, x, y, 0.7f)); }
      const int sp[6][2] = {{2, 2}, {9, 4}, {13, 9}, {5, 12}, {11, 14}, {7, 7}}; for (auto& q : sp) t.set(q[0], q[1], HEX(0xb89c62)); }
    // ГРАВИЙ
    { TP t(T_GRAVEL); Pal p = mkPal({0x6b6560, 0x7b746e, 0x8b847d, 0x9a938b}); Pal a = mkPal({0x6f5d4e, 0x82705f, 0x968371, 0xa89582});
      stonesTex(t, 13, 17, p, HEX(0x4a4540), 0.75f, 26, 20, &a); }
    // БРЕВНО
    const Pal barkP = mkPal({0x2f2013, 0x4a3320, 0x5d412a, 0x6e4f33, 0x7f5d3d});
    { TP t(T_LOG); barkTex(t, barkP, 18); }
    const Pal ringP = mkPal({0x8c6a3f, 0x7a5a33, 0xb08a55, 0xc79f66});
    { TP t(T_LOG_TOP); ringsTex(t, ringP, HEX(0x3b2817), HEX(0x5a3e26)); }
    // ЛИСТВА: грозди листьев с объёмом и просветами
    { TP t(T_LEAVES);
      TN n = makeNoise(19, {{4, 0.5f}, {2, 0.5f}});
      Pal p = mkPal({0x23481b, 0x2f6224, 0x3d7a2d, 0x4d9338, 0x63ab47});
      for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
          float v = n.at(x, y);
          if (v < 0.13f) { t.set(x, y, 0); continue; }
          t.set(x, y, rampPick(p, v, x, y, 0.9f)); }
      const int lf[10][2] = {{2, 1}, {8, 2}, {13, 4}, {4, 6}, {10, 8}, {1, 10}, {6, 12}, {12, 12}, {14, 0}, {8, 14}};
      for (auto& q : lf) { t.set(q[0], q[1], HEX(0x7cc25a)); t.set(q[0] + 1, q[1], HEX(0x63ab47)); t.set(q[0], q[1] + 1, HEX(0x4d9338)); t.set(q[0] + 1, q[1] + 1, HEX(0x23481b)); } }
    // ДОСКИ
    const Pal plankP = mkPal({0x8a6538, 0x9c7443, 0xaf844f, 0xc1955c, 0xcfa368});
    { TP t(T_PLANKS); planksTex(t, plankP, HEX(0x5a3f22), 20); }
    // СТЕКЛО
    { TP t(T_GLASS); t.clear();
      for (int y = 1; y < 15; y++) for (int x = 1; x < 15; x++) t.set(x, y, RGBA(205, 232, 244, 28));
      for (int i = 0; i < 16; i++) { t.set(i, 0, HEX(0xeef8fc, 235)); t.set(0, i, HEX(0xeef8fc, 235)); t.set(i, 15, HEX(0x8fb6c8, 235)); t.set(15, i, HEX(0x8fb6c8, 235)); }
      t.set(15, 0, HEX(0xc6e0ea, 235)); t.set(0, 15, HEX(0xc6e0ea, 235));
      for (int i = 0; i < 16; i++) { t.set(i, 1, RGBA(255, 255, 255, 60)); t.set(1, i, RGBA(255, 255, 255, 60)); }
      t.line(3, 6, 6, 3, RGBA(255, 255, 255, 190)); t.line(3, 8, 8, 3, RGBA(255, 255, 255, 120)); t.line(10, 12, 12, 10, RGBA(255, 255, 255, 140)); }
    // РУДЫ
    static const char* mLump1[] = {"##.", "###", ".##"};
    static const char* mLump2[] = {"##", "##"};
    static const char* mLump3[] = {".##", "###", "##."};
    static const char* mLump4[] = {"###", "##."};
    static const char* mFlake1[] = {"#.", "##", ".#"};
    static const char* mFlake2[] = {".#", "##", "#."};
    static const char* mGem1[] = {".#.", "###", ".#."};
    static const char* mGem2[] = {".##", "###", "##."};
    static const char* const* lumps[4] = {mLump1, mLump2, mLump3, mLump4}; static const int lumpsH[4] = {3, 2, 3, 2};
    static const char* const* flakes[3] = {mFlake1, mFlake2, mLump2}; static const int flakesH[3] = {3, 3, 2};
    static const char* const* gems[2] = {mGem1, mGem2}; static const int gemsH[2] = {3, 3};
    { TP t(T_COAL_ORE); stoneBase(t, 21); placeGems(t, 1, 6, 4.2f, lumps, lumpsH, 4, HEX(0x101013), HEX(0x26262b), HEX(0x3d3d45), HEX(0x6a6a78)); }
    { TP t(T_IRON_ORE); stoneBase(t, 22); placeGems(t, 2, 5, 4.6f, lumps, lumpsH, 4, HEX(0x8c5a3c), HEX(0xcf9a74), HEX(0xe6bd98), HEX(0xfbe6d2)); }
    { TP t(T_GOLD_ORE); stoneBase(t, 23); placeGems(t, 3, 6, 4.2f, flakes, flakesH, 3, HEX(0x9c6c0c), HEX(0xe0b021), HEX(0xf7d749), HEX(0xfff6b8)); }
    { TP t(T_DIAMOND_ORE); stoneBase(t, 24); placeGems(t, 4, 5, 4.8f, gems, gemsH, 2, HEX(0x157f8a), HEX(0x33c4d1), HEX(0x86eef2), HEX(0xffffff)); }
    // ТВЕРДЬ
    { TP t(T_BEDROCK); Pal p = mkPal({0x1d1d20, 0x2d2d31, 0x404045, 0x55555b, 0x6c6c72}); stonesTex(t, 9, 25, p, HEX(0x141416), 0.6f, 18, 14); }
    // ВОДА
    { TP t(T_WATER); Pal p = mkPal({0x2a54c4, 0x3061d0, 0x386ddb, 0x437be4, 0x5089ea});
      TN n = makeNoise(26, {{8, 0.5f}, {4, 0.5f}});
      for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
          float w = sinf((x + n.at(x, y) * 6.f) * 0.785f + y * 1.57f);
          float v = 0.25f + n.at(x, y) * 0.5f + (w > 0.85f ? 0.3f : 0.f);
          t.set(x, y, rampPick(p, v, x, y, 0.5f)); }
      for (int i = 0; i < 4; i++) { int x = (i * 5 + 2) & 15, y = i * 4 + 1; t.set(x, y, HEX(0x8fb9f5)); t.set(x + 1, y, HEX(0x6d9ef0)); } }
    // ЛАВА
    { TP t(T_LAVA); Pal p = mkPal({0x8f1f08, 0xb8300b, 0xdb4d0f, 0xf07a17, 0xfba52a, 0xffd04a, 0xfff0a0});
      TN n = makeNoise(27, {{8, 0.4f}, {4, 0.4f}, {2, 0.2f}}); fillNoise(t, p, n, 0.5f); }
    // СНЕГ
    const Pal snowP = mkPal({0xd9e3ee, 0xe5edf5, 0xf0f5fa, 0xfafcfe});
    { TP t(T_SNOW); TN n = makeNoise(28, {{4, 0.5f}, {2, 0.5f}}); fillNoise(t, snowP, n, 0.7f);
      const int sp[5][2] = {{3, 4}, {10, 2}, {13, 10}, {6, 13}, {8, 8}}; for (auto& q : sp) t.set(q[0], q[1], HEX(0xbcd4ec)); }
    { TP t(T_SNOW_SIDE); t.copyFrom(T_DIRT); overhang(t, snowP, 29, 3, 4, HEX(0xb8c6d4)); }
    // ЛЁД
    { TP t(T_ICE); Pal p = mkPal({0x86b0e6, 0x95bdec, 0xa6c9f1, 0xb8d6f6});
      TN n = makeNoise(30, {{8, 0.6f}, {4, 0.4f}});
      for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { u32 c = rampPick(p, n.at(x, y), x, y, 0.5f); t.set(x, y, RGBA(pR(c), pG(c), pB(c), 200)); }
      t.line(2, 13, 7, 6, HEX(0xe8f3ff, 230)); t.line(7, 6, 9, 7, HEX(0xe8f3ff, 230)); t.line(9, 14, 14, 8, HEX(0xdcecfc, 210)); t.line(4, 2, 6, 4, HEX(0xdcecfc, 200));
      bevel(t, 0, 0, 15, 15, 30, 20); }
    // КАКТУС
    { TP t(T_CACTUS_SIDE);
      for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
          int k = x % 5; u32 c = k == 0 ? HEX(0x2c6427) : (k == 1 ? HEX(0x3f8433) : (k == 2 ? HEX(0x56a043) : (k == 3 ? HEX(0x4a9139) : HEX(0x3a7a30))));
          if ((y * 7 + x) % 11 == 0) c = shadeC(c, -10);
          t.set(x, y, c); }
      for (int y = 1; y < 16; y += 4) for (int x = 0; x < 16; x += 5) { int yy = y + ((x / 5) & 1) * 2; t.set(x, yy, HEX(0xece4b8)); t.set(x, yy + 1, HEX(0x234f1f)); } }
    { TP t(T_CACTUS_TOP);
      for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
          float d = std::max(fabsf(x - 7.5f), fabsf(y - 7.5f));
          t.set(x, y, d > 6.5f ? HEX(0x2c6427) : (d > 4.5f ? HEX(0x4a9139) : (d > 2.5f ? HEX(0x56a043) : HEX(0x6cb553)))); }
      t.set(7, 7, HEX(0xece4b8)); t.set(3, 3, HEX(0xece4b8)); t.set(12, 12, HEX(0xece4b8)); t.set(12, 3, HEX(0xece4b8)); t.set(3, 12, HEX(0xece4b8)); }
    // ПЕСЧАНИК: слои
    const Pal sstP = mkPal({0xc5ac6e, 0xd3bb7d, 0xdfc88c, 0xe9d49b});
    { TP t(T_SANDSTONE); TN n = makeNoise(31, {{8, 0.4f}, {1, 0.6f}});
      for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
          float v = 0.3f + n.at(x, y) * 0.4f;
          if (y < 3) v += 0.35f; else if (y >= 13) v -= 0.1f;
          u32 c = rampPick(sstP, v, x, y, 0.5f);
          if (y == 3 || y == 12) c = HEX(0xa98f55);
          if (y == 2 || y == 11) c = shadeC(c, 14);
          if (y >= 5 && y <= 10 && (x * 3 + y * 5) % 17 == 0) c = shadeC(c, -16);
          t.set(x, y, c); } }
    { TP t(T_SANDSTONE_TOP); TN n = makeNoise(32, {{4, 0.5f}, {1, 0.5f}}); fillNoise(t, sstP, n, 0.6f); bevel(t, 0, 0, 15, 15, 14, 22); }
    // ЕЛЬ
    { TP t(T_PINE_LOG); barkTex(t, mkPal({0x24160d, 0x3a2416, 0x4a2f1d, 0x5a3a25, 0x6a462d}), 33); }
    { TP t(T_PINE_TOP); ringsTex(t, mkPal({0x7a5530, 0x684726, 0x9c7443, 0xb08550}), HEX(0x2a190e), HEX(0x4a2f1d)); }
    { TP t(T_PINE_LEAVES);
      for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) t.set(x, y, HEX(0x1c3f2a));
      Pal p = mkPal({0x285a3a, 0x33704a, 0x3f855a, 0x50996b});
      Rng r(34);
      for (int i = 0; i < 46; i++) { int x = r.range(16), y = r.range(16); u32 c = p[r.range(4)]; int dir = r.range(2) ? 1 : -1;
          t.set(x, y, c); t.set((x + dir) & 15, (y + 1) & 15, c); t.set((x + 2 * dir) & 15, (y + 2) & 15, shadeC(c, -14)); }
      const int holes[7][2] = {{3, 2}, {11, 4}, {7, 9}, {14, 12}, {1, 13}, {9, 15}, {5, 5}}; for (auto& q : holes) t.set(q[0], q[1], 0); }
    // ВЫСОКАЯ ТРАВА
    { TP t(T_TALLGRASS); t.clear();
      const int bl[8][3] = {{2, 9, -1}, {4, 13, 0}, {6, 7, 1}, {8, 11, 0}, {9, 14, -1}, {11, 8, 1}, {13, 12, 0}, {14, 6, -1}};
      for (auto& b : bl) for (int y = 0; y < b[1]; y++) {
          float k = (float)y / b[1]; int x = b[0] + (k > 0.6f ? b[2] : 0);
          t.set(x, 15 - y, mixc(HEX(0x3b7329), HEX(0x86c95d), k)); } }
    // ЦВЕТЫ
    { TP t(T_FLOWER_R); t.clear();
      for (int y = 8; y < 16; y++) t.set(7, y, HEX(0x3d7a2c)); t.set(8, 12, HEX(0x52983a)); t.set(9, 11, HEX(0x60aa43)); t.set(6, 13, HEX(0x52983a));
      static const char* pet[5] = {".###.", "##.##", "#.#.#", "##.##", ".###."};
      for (int y = 0; y < 5; y++) for (int x = 0; x < 5; x++) if (pet[y][x] == '#') t.set(5 + x, 3 + y, (x + y < 4) ? HEX(0xf0464a) : HEX(0xc81e2c));
      t.set(7, 5, HEX(0xf2c230)); t.set(6, 4, HEX(0xff8a8a)); }
    { TP t(T_FLOWER_Y); t.clear();
      for (int y = 9; y < 16; y++) t.set(8, y, HEX(0x3d7a2c)); t.set(7, 13, HEX(0x52983a)); t.set(6, 12, HEX(0x60aa43));
      static const char* pet[5] = {"#.#.#", ".###.", "##.##", ".###.", "#.#.#"};
      for (int y = 0; y < 5; y++) for (int x = 0; x < 5; x++) if (pet[y][x] == '#') t.set(6 + x, 4 + y, (x + y < 4) ? HEX(0xffe45c) : HEX(0xe8b81e));
      t.set(8, 6, HEX(0xd2701c)); }
    // САЖЕНЕЦ
    { TP t(T_SAPLING); t.clear();
      for (int y = 9; y < 16; y++) t.set(7, y, HEX(0x6e4b2c)); t.set(8, 11, HEX(0x5d3f24));
      static const char* cr[7] = {"..###..", ".#####.", "#######", "#######", ".#####.", "..#.#..", "......."};
      for (int y = 0; y < 7; y++) for (int x = 0; x < 7; x++) if (cr[y][x] == '#') t.set(4 + x, 3 + y, (x + y < 5) ? HEX(0x6cb64a) : ((x + y < 8) ? HEX(0x4d9338) : HEX(0x2f6224)));
      t.set(6, 4, HEX(0x9bd86b)); }
    // ФАКЕЛ (палочка в столбцах 7-8, пламя в строках 3-5 — так ожидает модель)
    { TP t(T_TORCH); t.clear();
      for (int y = 6; y < 16; y++) { t.set(7, y, y < 8 ? HEX(0x3a2614) : HEX(0x9a7246)); t.set(8, y, y < 8 ? HEX(0x2a1a0c) : HEX(0x6e4f2e)); }
      t.set(7, 3, HEX(0xfff6c8)); t.set(8, 3, HEX(0xffd86a)); t.set(7, 4, HEX(0xffd86a)); t.set(8, 4, HEX(0xffa630)); t.set(7, 5, HEX(0xff8a1e)); t.set(8, 5, HEX(0xe35a10)); }
    // ВЕРСТАК
    { TP t(T_CRAFT_TOP); planksTex(t, plankP, HEX(0x5a3f22), 35);
      for (int i = 0; i < 16; i++) { t.set(i, 0, HEX(0x4a321a)); t.set(0, i, HEX(0x4a321a)); t.set(i, 15, HEX(0x3a2614)); t.set(15, i, HEX(0x3a2614)); }
      for (int y = 3; y <= 12; y++) for (int x = 3; x <= 12; x++) {
          bool line = (x == 3 || x == 12 || y == 3 || y == 12 || x == 6 || x == 9 || y == 6 || y == 9);
          if (line) t.set(x, y, HEX(0x5a3f22)); else t.shade(x, y, -10); }
      bevel(t, 1, 1, 14, 14, 12, 14); }
    { TP t(T_CRAFT_SIDE); planksTex(t, plankP, HEX(0x5a3f22), 36);
      for (int x = 0; x < 16; x++) { t.set(x, 0, HEX(0x7a5530)); t.set(x, 1, HEX(0x5a3f22)); t.set(x, 2, HEX(0x3a2614)); }
      // молоток
      t.rect(2, 5, 5, 6, HEX(0x8f9297)); t.set(2, 5, HEX(0xc9ccd0)); t.rect(3, 7, 3, 12, HEX(0x6e4b2c)); t.set(4, 7, HEX(0x4a321a));
      // пила
      t.rect(8, 5, 13, 7, HEX(0xb7bbc0)); t.rect(8, 5, 13, 5, HEX(0xdfe2e5)); for (int x = 8; x <= 13; x += 2) t.set(x, 8, HEX(0x8f9297));
      t.rect(13, 8, 14, 11, HEX(0x6e4b2c)); t.set(14, 11, HEX(0x4a321a)); }
    // ПЕЧЬ
    { TP t(T_FURN_SIDE); Pal p = mkPal({0x5a5c61, 0x6a6c71, 0x7a7c81, 0x8a8c91}); stonesTex(t, 8, 37, p, HEX(0x3a3b3f), 0.8f, 18, 16);
      for (int x = 0; x < 16; x++) { t.set(x, 0, HEX(0x9fa1a5)); t.set(x, 1, HEX(0x7a7c81)); t.set(x, 14, HEX(0x4a4b50)); t.set(x, 15, HEX(0x34353a)); } }
    { TP t(T_FURN_TOP); Pal p = mkPal({0x75777c, 0x808287, 0x8b8d92, 0x96989d}); TN n = makeNoise(38, {{4, 0.5f}, {1, 0.5f}}); fillNoise(t, p, n, 0.6f);
      bevel(t, 0, 0, 15, 15, 20, 30); bevel(t, 3, 3, 12, 12, -16, -16); }
    { TP t(T_FURN_FRONT); t.copyFrom(T_FURN_SIDE);
      // арка топки
      static const char* arch[9] = {"..######..", ".########.", "##########", "##########", "##########", "##########", "##########", "##########", "##########"};
      for (int y = 0; y < 9; y++) for (int x = 0; x < 10; x++) if (arch[y][x] == '#') t.set(3 + x, 5 + y, y < 2 ? HEX(0x18181a) : HEX(0x0f0f11));
      for (int x = 3; x < 13; x++) t.set(x, 13, HEX(0x4a4b50));
      for (int x = 4; x < 12; x += 2) for (int y = 10; y < 13; y++) t.set(x, y, HEX(0x34353a));   // решётка
      bevel(t, 2, 4, 13, 14, 20, 0); t.rect(4, 2, 11, 2, HEX(0x4a4b50)); }
    { TP t(T_FURN_FRONT_LIT); t.copyFrom(T_FURN_FRONT);
      Pal f = mkPal({0x9a2a08, 0xd8460c, 0xf57d18, 0xffb52e, 0xffe27a});
      for (int y = 7; y < 13; y++) for (int x = 4; x < 12; x++) {
          float h = 0.5f + 0.5f * sinf(x * 1.7f) ; float v = clampf((y - 7 + h * 2.f) / 7.f, 0, 0.999f);
          if (y < 9 && ((x + y) & 1)) continue;
          t.set(x, y, f[clampi((int)(v * 5), 0, 4)]); }
      for (int x = 4; x < 12; x += 2) t.set(x, 12, HEX(0xffe27a)); }
    // СУНДУК
    { TP t(T_CHEST_SIDE); planksTex(t, mkPal({0x7a4f22, 0x8c5b28, 0x9e6830, 0xae7638}), HEX(0x4a2f12), 39);
      for (int x = 0; x < 16; x++) { t.set(x, 5, HEX(0x2e1c0a)); t.set(x, 6, HEX(0x5a3a18)); }
      for (int i = 0; i < 16; i++) { t.set(0, i, HEX(0x3a3b3f)); t.set(15, i, HEX(0x3a3b3f)); t.set(i, 0, HEX(0x3a3b3f)); t.set(i, 15, HEX(0x2a2b2e)); }
      t.set(1, 1, HEX(0x9fa1a5)); t.set(14, 1, HEX(0x9fa1a5)); t.set(1, 14, HEX(0x9fa1a5)); t.set(14, 14, HEX(0x9fa1a5)); }
    { TP t(T_CHEST_TOP); planksTex(t, mkPal({0x7a4f22, 0x8c5b28, 0x9e6830, 0xae7638}), HEX(0x4a2f12), 40);
      for (int i = 0; i < 16; i++) { t.set(0, i, HEX(0x3a3b3f)); t.set(15, i, HEX(0x2a2b2e)); t.set(i, 0, HEX(0x3a3b3f)); t.set(i, 15, HEX(0x2a2b2e)); } }
    { TP t(T_CHEST_FRONT); t.copyFrom(T_CHEST_SIDE);
      t.rect(6, 4, 9, 8, HEX(0xc89a2a)); t.rect(6, 4, 9, 4, HEX(0xf2d066)); t.rect(9, 5, 9, 8, HEX(0x8a6514)); t.rect(7, 6, 8, 7, HEX(0x2a1a06)); }
    // ДВЕРЬ
    const Pal doorP = mkPal({0x7d5a32, 0x8f683b, 0xa07645, 0xb0844f});
    { TP t(T_DOOR_LO); planksTex(t, doorP, HEX(0x4f381e), 41);
      for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) if (x < 2 || x > 13 || y > 13) t.set(x, y, (x == 0 || y == 15) ? HEX(0x4f381e) : HEX(0x6e4f2c));
      t.rect(4, 2, 11, 11, HEX(0x6e4f2c)); t.rect(5, 3, 10, 10, HEX(0x9a7244)); bevel(t, 4, 2, 11, 11, -14, 16);
      t.rect(12, 6, 13, 7, HEX(0x3a3b3f)); t.set(12, 6, HEX(0xc9ccd0)); }
    { TP t(T_DOOR_HI); planksTex(t, doorP, HEX(0x4f381e), 42);
      for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) if (x < 2 || x > 13 || y < 2) t.set(x, y, (x == 0 || y == 0) ? HEX(0x4f381e) : HEX(0x6e4f2c));
      t.rect(3, 4, 12, 12, HEX(0x5a3f22));
      for (int py = 0; py < 2; py++) for (int px = 0; px < 2; px++) {
          int x0 = 4 + px * 5, y0 = 5 + py * 4;
          for (int y = y0; y < y0 + 3; y++) for (int x = x0; x < x0 + 4; x++) t.set(x, y, mixc(HEX(0xbfe4f2), HEX(0x86b9d6), (float)(y - y0 + x - x0) / 5.f));
          t.set(x0, y0, HEX(0xffffff)); } }
    // ЛЕСТНИЦА
    { TP t(T_LADDER); t.clear();
      for (int y = 0; y < 16; y++) { t.set(2, y, HEX(0xa07645)); t.set(3, y, HEX(0x7d5a32)); t.set(12, y, HEX(0xa07645)); t.set(13, y, HEX(0x7d5a32)); }
      for (int y = 2; y < 16; y += 4) for (int x = 1; x < 15; x++) { t.set(x, y, HEX(0xb88a52)); t.set(x, y + 1, HEX(0x6e4f2c)); }
      for (int y = 2; y < 16; y += 4) { t.set(2, y, HEX(0x3a3b3f)); t.set(13, y, HEX(0x3a3b3f)); } }
    // КРОВАТЬ (верх: изголовье и изножье; бок рисуется в строках 7-15 — высота кровати 9/16)
    { TP t(T_BED_FOOT);
      for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
          u32 c = ((x / 4 + y / 4) & 1) ? HEX(0xb8262e) : HEX(0xc93139);
          if (x == 0 || x == 15) c = HEX(0x8a1a22);
          if (y == 15) c = HEX(0x6e4f2c);
          if ((x % 4 == 2) && (y % 4 == 2)) c = HEX(0xf2c24a);
          t.set(x, y, c); } }
    { TP t(T_BED_HEAD); t.copyFrom(T_BED_FOOT);
      for (int y = 0; y < 6; y++) for (int x = 0; x < 16; x++) t.set(x, y, y == 5 ? HEX(0xc8c4b8) : (x == 0 || x == 15 ? HEX(0xd8d4c8) : HEX(0xf4f1e8)));
      bevel(t, 1, 0, 14, 4, 10, 14); t.set(0, 0, HEX(0x6e4f2c)); t.set(15, 0, HEX(0x6e4f2c));
      for (int x = 0; x < 16; x++) t.set(x, 6, HEX(0xe8e4d8)); }
    { TP t(T_BED_SIDE); t.clear();
      for (int y = 7; y < 16; y++) for (int x = 0; x < 16; x++) {
          u32 c;
          if (y == 7) c = HEX(0xe8e4d8);
          else if (y <= 11) c = (y == 11) ? HEX(0x8a1a22) : HEX(0xc0293a - 0);
          else if (y <= 13) c = (y == 12) ? HEX(0xa07645) : HEX(0x7d5a32);
          else c = (x < 3 || x > 12) ? HEX(0x5a3f22) : HEX(0x3a2614);
          t.set(x, y, c); }
      for (int x = 1; x < 16; x += 4) t.set(x, 9, HEX(0xa81e2a)); }
    { TP t(T_BED_SIDE_HEAD); t.copyFrom(T_BED_SIDE);
      for (int y = 7; y < 10; y++) for (int x = 0; x < 6; x++) t.set(x, y, y == 9 ? HEX(0xc8c4b8) : HEX(0xf4f1e8)); }
    // КАМЕННЫЙ КИРПИЧ
    { TP t(T_STONEBRICK); TN n = makeNoise(43, {{4, 0.5f}, {1, 0.5f}});
      Pal p = mkPal({0x75777c, 0x82848a, 0x8f9196, 0x9b9da2});
      for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) t.set(x, y, rampPick(p, 0.2f + n.at(x, y) * 0.6f, x, y, 0.6f));
      for (int row = 0; row < 2; row++) {
          int y0 = row * 8, off = row ? 4 : 0;
          for (int x = 0; x < 16; x++) { t.set(x, y0 + 7, HEX(0x45464a)); }
          int sx = off, ex = (off + 7) & 15;
          for (int y = y0; y < y0 + 7; y++) { t.set(ex, y, HEX(0x45464a)); t.set((ex + 8) & 15, y, HEX(0x45464a)); }
          for (int b = 0; b < 2; b++) {
              int bx0 = (sx + b * 8) & 15;
              for (int k = 0; k < 7; k++) { int x = (bx0 + k) & 15; t.shade(x, y0, 18); t.shade(x, y0 + 6, -16); }
              for (int y = y0 + 1; y < y0 + 6; y++) { t.shade(bx0, y, 18); t.shade((bx0 + 6) & 15, y, -16); }
          }
      }
    }
    // ВУЛКАНИТ
    { TP t(T_OBSIDIAN); Pal p = mkPal({0x0f0a17, 0x171022, 0x21172f, 0x2c1f3e});
      TN n = makeNoise(44, {{8, 0.5f}, {2, 0.5f}}); fillNoise(t, p, n, 0.6f);
      const int gl[6][2] = {{3, 3}, {10, 2}, {6, 8}, {13, 9}, {2, 12}, {9, 13}};
      for (auto& q : gl) { t.set(q[0], q[1], HEX(0x7a4fc0)); t.set(q[0] + 1, q[1], HEX(0x4e2f86)); t.set(q[0], q[1] + 1, HEX(0x3a2266)); } }
    // МЕТАЛЛИЧЕСКИЕ БЛОКИ
    auto metal = [](int tile, const Pal& p, u32 seed, int kind) {
        TP t(tile);
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
            float v = 0.55f + ((x + y) % 7 == 0 ? 0.25f : 0.f) + ((hash32(seed + x * 16 + y) % 100) / 100.f - 0.5f) * 0.12f;
            t.set(x, y, rampPick(p, clampf(v, 0, 0.999f), x, y, 0.3f)); }
        bevel(t, 0, 0, 15, 15, 40, 50);
        bevel(t, 1, 1, 14, 14, 18, 22);
        if (kind == 0) { bevel(t, 4, 4, 11, 11, -20, -14 * -1); t.rect(7, 2, 8, 2, p[0]); t.rect(7, 13, 8, 13, p[0]); }
        else if (kind == 1) { for (int i = 3; i < 13; i++) { t.set(i, 15 - i, p[p.size() - 1]); t.set(i + 1, 15 - i, p[p.size() - 2]); } }
        else { for (int y = 2; y < 14; y++) for (int x = 2; x < 14; x++) if (abs(x - 7) + abs(y - 7) == 5 || abs(x - 8) + abs(y - 8) == 5) t.set(x, y, p[0]); t.set(5, 5, HEX(0xffffff)); }
    };
    metal(T_IRON_BLOCK, mkPal({0x9a9ca2, 0xb8babf, 0xcfd1d5, 0xe2e4e7, 0xf2f3f5}), 45, 0);
    metal(T_GOLD_BLOCK, mkPal({0xb88a12, 0xd8a91f, 0xeec43a, 0xf7d95a, 0xfff0a0}), 46, 1);
    metal(T_DIAMOND_BLOCK, mkPal({0x1e9aa6, 0x3cc4cf, 0x6ad9e0, 0x9aeaee, 0xd8fbfc}), 47, 2);
}

static void genAtlas() {
    memset(g_atlasPx, 0, sizeof(g_atlasPx));
    genBlockTextures();
    { TP t(T_WHITE); t.fill(255, 255, 255, 0); }
    for (int s = 0; s < 5; s++) {   // трещины
        TP t(T_CRACK0 + s); t.clear(); Rng r(77);
        int lines = 2 + s * 3;
        for (int l = 0; l < lines; l++) {
            int x = 8 + r.range(5) - 2, y = 8 + r.range(5) - 2;
            for (int k = 0; k < 3 + s * 2; k++) { int nx = x + r.range(5) - 2, ny = y + r.range(5) - 2; t.line(x, y, nx, ny, RGBA(20, 20, 20, 210)); x = clampi(nx, 0, 15); y = clampi(ny, 0, 15); }
        }
    }
    { TP t(T_SUN); t.clear(); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        float dx = x - 7.5f, dy = y - 7.5f; float d = fmaxf(fabsf(dx), fabsf(dy));
        if (d < 5) t.set(x, y, RGBA(255, 250, 200)); else if (d < 6.5f) t.set(x, y, RGBA(255, 220, 110, 230)); else t.set(x, y, RGBA(255, 210, 100, (int)(110 * (8 - d) / 1.5f))); } }
    { TP t(T_MOON); t.clear(); t.rect(3, 3, 12, 12, RGBA(220, 225, 235)); t.rect(5, 5, 6, 6, RGBA(180, 185, 200)); t.rect(9, 8, 11, 10, RGBA(180, 185, 200)); t.set(7, 10, RGBA(190, 195, 210)); }
    { TP t(T_FLAME); t.clear(); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { float dx = (x - 7.5f) / 7.5f, dy = (y - 9.f) / 7.f; float d = dx * dx + dy * dy * 0.7f; if (d < 1) t.set(x, y, d < 0.3f ? RGBA(255, 240, 150, 230) : RGBA(255, 130, 20, 200)); } }

    // ---------- предметы
    { TP t(T_STICK); t.clear(); for (int i = 0; i < 12; i++) { t.set(3 + i, 14 - i, RGBA(130, 94, 54)); t.set(4 + i, 14 - i, RGBA(96, 68, 36)); } }
    { TP t(T_COAL); t.clear(); for (int y = 4; y < 13; y++) for (int x = 3; x < 13; x++) { float dx = x - 7.5f, dy = y - 8.f; float d = dx * dx * 0.8f + dy * dy; if (d < 20) { int v = (x + y) % 3 == 0 ? 40 : 0; t.set(x, y, d > 14 ? RGBA(20, 20, 22) : RGBA(52 + v + t.r.range(16), 52 + v + t.r.range(12), 58 + v + t.r.range(12))); } }
      t.set(6, 6, RGBA(150, 150, 160)); t.set(7, 6, RGBA(120, 120, 130)); t.set(9, 9, RGBA(130, 130, 140)); }
    for (int m = 0; m < 2; m++) { TP t(m == 0 ? T_IRON_INGOT : T_GOLD_INGOT); t.clear();
        int R = m ? 250 : 210, G = m ? 205 : 210, B = m ? 40 : 215;
        for (int y = 6; y < 12; y++) for (int x = 2 + (11 - y) / 2; x < 14 - (11 - y) / 2; x++) t.set(x, y, RGBA(R - (y - 6) * 8, G - (y - 6) * 8, B - (y - 6) * 6));
        t.line(4, 6, 11, 6, RGBA(255, 255, 240)); }
    { TP t(T_DIAMOND); t.clear(); for (int y = 3; y < 14; y++) { int w = y < 7 ? (y - 2) * 1 + 2 : (14 - y); for (int x = 8 - w; x < 8 + w; x++) t.set(x, y, RGBA(90 + (x * 9 % 60), 220, 230)); } t.set(6, 5, RGBA(255, 255, 255)); t.set(7, 6, RGBA(230, 255, 255)); }
    { TP t(T_APPLE); t.clear(); for (int y = 4; y < 15; y++) for (int x = 2; x < 14; x++) { float dx = x - 7.5f, dy = y - 9.f; if (dx * dx + dy * dy < 26) t.set(x, y, RGBA(210 - (int)(dy * 4), 30, 40)); } t.set(5, 7, RGBA(255, 160, 160)); t.set(8, 2, RGBA(90, 60, 30)); t.set(8, 3, RGBA(90, 60, 30)); t.rect(9, 2, 10, 3, RGBA(60, 150, 40)); }
    for (int m = 0; m < 2; m++) { TP t(m ? T_COOKED : T_RAWMEAT); t.clear();
        for (int y = 4; y < 13; y++) for (int x = 2; x < 14; x++) { float dx = x - 7.5f, dy = y - 8.f; if (dx * dx * 0.6f + dy * dy < 18) t.set(x, y, m ? RGBA(150 - y * 3, 90 - y * 3, 50) : RGBA(220 - y * 3, 90, 100)); }
        for (int x = 4; x < 12; x++) t.set(x, 6 + (x % 3 == 0), m ? RGBA(200, 150, 90) : RGBA(250, 230, 220)); }
    { TP t(T_BONE); t.clear(); t.line(4, 11, 11, 4, RGBA(235, 232, 220)); t.line(5, 11, 12, 4, RGBA(210, 205, 190)); t.rect(2, 11, 4, 13, RGBA(235, 232, 220)); t.rect(11, 2, 13, 4, RGBA(235, 232, 220)); }
    { TP t(T_BONEMEAL); t.clear(); for (int i = 0; i < 40; i++) { int x = 3 + t.r.range(10), y = 6 + t.r.range(8); if (abs(x - 8) < (y - 4)) t.set(x, y, RGBA(240, 240, 235 - t.r.range(30))); } }
    { TP t(T_ROTTEN); t.clear(); for (int y = 4; y < 13; y++) for (int x = 3; x < 13; x++) { float dx = x - 7.5f, dy = y - 8.f; if (dx * dx + dy * dy * 1.3f < 20) t.set(x, y, RGBA(120 + t.r.range(30), 140, 70)); } t.set(6, 7, RGBA(70, 90, 40)); t.set(9, 9, RGBA(70, 90, 40)); }
    { TP t(T_FLUFF); t.clear(); for (int c = 0; c < 5; c++) { int cx = 4 + t.r.range(8), cy = 5 + t.r.range(6); for (int y = -3; y <= 3; y++) for (int x = -3; x <= 3; x++) if (x * x + y * y < 9) t.set(cx + x, cy + y, RGBA(250, 235 - t.r.range(20), 240)); } }
    { TP t(T_DOOR_ITEM); t.clear(); t.rect(4, 1, 11, 14, RGBA(150, 112, 66)); t.rect(5, 2, 10, 6, RGBA(170, 210, 230)); t.rect(5, 8, 10, 13, RGBA(130, 96, 54)); t.set(10, 9, RGBA(220, 220, 220)); }
    { TP t(T_BED_ITEM); t.clear(); t.rect(1, 7, 14, 11, RGBA(180, 36, 40)); t.rect(1, 7, 4, 9, RGBA(240, 240, 235)); t.rect(1, 12, 2, 13, RGBA(110, 80, 50)); t.rect(13, 12, 14, 13, RGBA(110, 80, 50)); }
    // ---------- значки интерфейса
    static const char* heart[7] = {".##.##.", "#######", "#######", "#######", ".#####.", "..###..", "...#..."};
    for (int k = 0; k < 3; k++) { TP t(T_HEART_F + k); t.clear();
        for (int y = 0; y < 7; y++) for (int x = 0; x < 7; x++) if (heart[y][x] == '#') {
            bool full = (k == 0) || (k == 1 && x < 4);
            for (int sy = 0; sy < 2; sy++) for (int sx = 0; sx < 2; sx++) t.set(1 + x * 2 + sx, 1 + y * 2 + sy, full ? RGBA(230, 30, 40) : RGBA(50, 20, 22, 200));
        }
        if (k < 2) { t.set(3, 3, RGBA(255, 180, 180)); t.set(4, 3, RGBA(255, 180, 180)); } }
    static const char* food[7] = {"....##.", "...####", "..#####", ".#####.", "####...", "##.....", "#......"};
    for (int k = 0; k < 3; k++) { TP t(T_FOOD_F + k); t.clear();
        for (int y = 0; y < 7; y++) for (int x = 0; x < 7; x++) if (food[y][x] == '#') {
            bool full = (k == 0) || (k == 1 && x >= 3);
            u32 c = (x + y < 4) ? (full ? RGBA(235, 230, 220) : RGBA(50, 40, 30, 200)) : (full ? RGBA(180, 100, 50) : RGBA(50, 30, 20, 200));
            for (int sy = 0; sy < 2; sy++) for (int sx = 0; sx < 2; sx++) t.set(1 + x * 2 + sx, 1 + y * 2 + sy, c);
        } }
    { TP t(T_BUBBLE); t.clear(); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { float dx = x - 7.5f, dy = y - 7.5f, d = dx * dx + dy * dy; if (d < 36 && d > 20) t.set(x, y, RGBA(150, 200, 255)); else if (d <= 20) t.set(x, y, RGBA(60, 120, 220, 120)); } t.set(5, 5, RGBA(255, 255, 255)); }
    { TP t(T_BOOK); t.clear(); t.rect(2, 2, 13, 13, RGBA(120, 50, 30)); t.rect(4, 3, 13, 12, RGBA(240, 230, 200)); t.rect(2, 2, 3, 13, RGBA(90, 34, 20)); for (int y = 5; y < 11; y += 2) t.line(6, y, 11, y, RGBA(110, 100, 90)); }
    // ---------- инструменты
    const int MC[5][3] = {{168, 130, 78}, {130, 130, 132}, {220, 220, 225}, {250, 210, 40}, {90, 225, 230}};
    for (int m = 0; m < 5; m++) for (int k = 0; k < 4; k++) {
        TP t(T_TOOL0 + m * 4 + k); t.clear();
        u32 hd = RGBA(MC[m][0], MC[m][1], MC[m][2]), hl = RGBA(MC[m][0] + 40, MC[m][1] + 40, MC[m][2] + 40), dk = RGBA(MC[m][0] - 60, MC[m][1] - 60, MC[m][2] - 60);
        u32 st = RGBA(120, 86, 48), st2 = RGBA(90, 62, 34);
        if (k != TK_SWORD) for (int i = 0; i < 11; i++) { t.set(2 + i, 13 - i, st); t.set(3 + i, 13 - i, st2); }
        if (k == TK_PICK) { t.line(4, 2, 8, 3, hd); t.line(8, 3, 11, 5, hd); t.line(11, 5, 12, 9, hd); t.line(13, 11, 12, 8, dk); t.line(3, 3, 7, 3, hl); t.set(10, 6, dk); t.set(9, 4, hd); t.set(10, 4, hd); t.set(11, 6, hd); }
        else if (k == TK_AXE) { t.rect(7, 2, 10, 6, hd); t.rect(6, 3, 6, 7, hd); t.line(7, 2, 10, 2, hl); t.line(6, 7, 9, 7, dk); }
        else if (k == TK_SHOVEL) { t.rect(10, 2, 13, 5, hd); t.line(10, 2, 13, 2, hl); t.line(13, 3, 13, 5, dk); t.set(9, 6, dk); }
        else { for (int i = 0; i < 9; i++) { t.set(5 + i, 10 - i, hd); t.set(6 + i, 10 - i, hl); t.set(5 + i, 11 - i, dk); }
               t.line(2, 9, 6, 13, RGBA(90, 70, 40)); t.line(3, 13, 1, 15, st); t.line(4, 13, 2, 15, st2); t.set(3, 12, st); }
    }
    // ---------- шкуры мобов
    { TP t(T_TOPTUN_SIDE); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { int v = (int)(t.r.sf() * 16) + ((y + x / 3) % 4 == 0 ? -16 : 0); t.set(x, y, RGBA(150 + v, 96 + v, 52 + v)); } }
    { TP t(T_TOPTUN_FACE); t.copyFrom(T_TOPTUN_SIDE); t.rect(2, 5, 4, 7, RGBA(20, 20, 20)); t.rect(11, 5, 13, 7, RGBA(20, 20, 20)); t.set(2, 5, RGBA(240, 240, 240)); t.set(11, 5, RGBA(240, 240, 240));
      t.rect(4, 10, 11, 14, RGBA(215, 180, 140)); t.rect(5, 11, 6, 12, RGBA(70, 40, 30)); t.rect(9, 11, 10, 12, RGBA(70, 40, 30)); }
    { TP t(T_TOPTUN_LEG); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { int v = (int)(t.r.sf() * 10); t.set(x, y, y > 12 ? RGBA(50, 40, 30) : RGBA(110 + v, 70 + v, 40 + v)); } }
    { TP t(T_HORN); t.fill(235, 225, 200, 8); }
    { TP t(T_PUKH_BODY); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { int v = (int)(t.r.sf() * 18); t.set(x, y, RGBA(245 + v / 3, 210 + v, 225 + v)); } for (int i = 0; i < 20; i++) t.shade(t.r.range(16), t.r.range(16), -22); }
    { TP t(T_PUKH_FACE); t.copyFrom(T_PUKH_BODY); t.rect(3, 5, 4, 7, RGBA(30, 20, 40)); t.rect(11, 5, 12, 7, RGBA(30, 20, 40)); t.rect(6, 8, 9, 10, RGBA(250, 150, 40)); t.rect(7, 11, 8, 11, RGBA(220, 110, 30)); t.rect(1, 9, 2, 10, RGBA(255, 160, 180)); t.rect(13, 9, 14, 10, RGBA(255, 160, 180)); }
    { TP t(T_PUKH_LEG); t.fill(240, 150, 40, 10); }
    // ГНИЛЬ — ходячий гнилой пень: кора, мох, грибы
    { TP t(T_GNIL_BODY); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        int v = (int)(t.r.sf() * 10) + ((x * 3 + y / 4) % 5 == 0 ? -22 : 0);
        bool moss = ((x * 7 + y * 3) % 11 < 3 && y > 6) || t.r.range(100) < 8;
        t.set(x, y, moss ? RGBA(70 + v, 112 + v, 46) : RGBA(78 + v, 60 + v, 44 + v)); } }
    { TP t(T_GNIL_FACE); t.copyFrom(T_GNIL_BODY);
      t.rect(3, 4, 5, 6, RGBA(30, 18, 10)); t.rect(10, 4, 12, 6, RGBA(30, 18, 10));
      t.rect(4, 5, 4, 5, RGBA(255, 150, 40)); t.rect(11, 5, 11, 5, RGBA(255, 150, 40));
      t.line(3, 11, 6, 9, RGBA(25, 15, 8)); t.line(6, 9, 9, 11, RGBA(25, 15, 8)); t.line(9, 11, 12, 9, RGBA(25, 15, 8)); }
    { TP t(T_GNIL_LEG); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { int v = (int)(t.r.sf() * 10) + (x % 3 == 0 ? -14 : 0); t.set(x, y, RGBA(64 + v, 48 + v, 34 + v)); } }
    { TP t(T_GNIL_TOP); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        float dx = x - 7.5f, dy = y - 7.5f; int ring = ((int)(sqrtf(dx * dx + dy * dy) * 1.3f)) % 2 ? -16 : 0; int v = (int)(t.r.sf() * 8);
        t.set(x, y, t.r.range(100) < 35 ? RGBA(66 + v, 120 + v, 44) : RGBA(120 + ring + v, 96 + ring + v, 64 + ring)); } }
    { TP t(T_MUSHCAP); t.fill(200, 40, 36, 10); for (int i = 0; i < 7; i++) { int x = t.r.range(14), y = t.r.range(14); t.rect(x, y, x + 1, y + 1, RGBA(245, 240, 225)); } }
    // ТЕНЕВИК — четвероногий зверь тени с одним глазом и светящимися шипами
    { TP t(T_TEN_BODY); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { int v = (int)(t.r.sf() * 8) + ((x + y * 2) % 7 == 0 ? 10 : 0); t.set(x, y, RGBA(28 + v, 34 + v, 52 + v)); } }
    { TP t(T_TEN_FACE); t.copyFrom(T_TEN_BODY);
      t.rect(4, 3, 11, 9, RGBA(20, 24, 36)); t.rect(5, 4, 10, 8, RGBA(70, 230, 230)); t.rect(6, 5, 9, 7, RGBA(170, 255, 255)); t.rect(7, 5, 8, 7, RGBA(10, 40, 50));
      for (int x = 3; x < 13; x += 2) { t.set(x, 12, RGBA(220, 230, 235)); t.set(x + 1, 13, RGBA(220, 230, 235)); } t.rect(3, 13, 12, 13, RGBA(15, 15, 20)); }
    { TP t(T_TEN_LEG); t.fill(22, 26, 40, 6); }
    { TP t(T_TEN_SPIKE); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) t.set(x, y, RGBA(60 + y * 6, 220, 230)); }
    // ЗОМБИ: землисто-серая кожа, рваная охристая рубаха, штаны в заплатах
    { TP t(T_ZOMB_SKIN); TN n = makeNoise(201, {{4, 0.5f}, {2, 0.3f}, {1, 0.2f}}); fillNoise(t, mkPal({0x6c7560, 0x78816b, 0x848c76, 0x909882}), n, 0.6f);
      const int br[4][2] = {{3, 3}, {11, 6}, {6, 11}, {13, 13}}; for (auto& q : br) { t.set(q[0], q[1], HEX(0x5d5870)); t.set(q[0] + 1, q[1], HEX(0x6a6478)); } }
    { TP t(T_ZOMB_FACE); t.copyFrom(T_ZOMB_SKIN);
      for (int x = 2; x < 14; x++) t.set(x, 4, HEX(0x4d5545));
      t.rect(3, 5, 5, 7, HEX(0x262920)); t.rect(10, 5, 12, 7, HEX(0x262920)); t.set(4, 6, HEX(0xffb23a)); t.set(11, 6, HEX(0xffb23a)); t.set(4, 5, HEX(0x6b5520)); t.set(11, 5, HEX(0x6b5520));
      t.set(7, 8, HEX(0x58614c)); t.set(8, 9, HEX(0x58614c));
      for (int x = 4; x <= 11; x++) t.set(x, 11, HEX(0x3a2f2a));
      for (int x = 5; x <= 11; x += 2) { t.set(x, 10, HEX(0xb8b0a0)); t.set(x, 12, HEX(0xb8b0a0)); }
      t.line(9, 1, 12, 3, HEX(0x8f5f5f)); }
    { TP t(T_ZOMB_SHIRT); TN n = makeNoise(202, {{4, 0.5f}, {1, 0.5f}}); fillNoise(t, mkPal({0x6e4f26, 0x7d5c2d, 0x8c6934, 0x9a763c}), n, 0.6f);
      for (int x = 0; x < 16; x++) { t.set(x, 12, HEX(0x3e2a18)); t.set(x, 13, HEX(0x33220f)); }
      t.rect(7, 12, 8, 13, HEX(0xc0a060));
      const int tear[5][2] = {{3, 4}, {4, 5}, {11, 8}, {12, 8}, {12, 9}}; for (auto& q : tear) t.set(q[0], q[1], HEX(0x7b846c));
      for (int x = 0; x < 16; x += 3) t.set(x, 15, HEX(0x4a3518));
      t.line(2, 1, 5, 3, HEX(0x5c4220)); t.line(13, 2, 10, 6, HEX(0x5c4220)); }
    { TP t(T_ZOMB_PANTS); TN n = makeNoise(203, {{4, 0.5f}, {1, 0.5f}}); fillNoise(t, mkPal({0x35322e, 0x3f3b36, 0x48443f, 0x524d47}), n, 0.6f);
      t.rect(3, 3, 7, 7, HEX(0x5f6b78)); bevel(t, 3, 3, 7, 7, 14, 14); for (int i = 3; i <= 7; i += 2) { t.set(i, 2, HEX(0xb8b0a0)); t.set(i, 8, HEX(0xb8b0a0)); }
      t.rect(10, 9, 12, 10, HEX(0x7b846c)); for (int x = 0; x < 16; x++) { t.set(x, 14, HEX(0x6c7560)); t.set(x, 15, HEX(0x5d6652)); } }
    // СКЕЛЕТ: слоновая кость, тёмно-багряный капюшон, синие огоньки в глазницах
    const Pal boneP = mkPal({0xbdb59c, 0xcfc8b0, 0xdcd6c0, 0xe8e3d0, 0xf2eee0});
    { TP t(T_SKEL_BONE); TN n = makeNoise(204, {{4, 0.5f}, {1, 0.5f}}); fillNoise(t, boneP, n, 0.6f);
      for (int x = 0; x < 16; x++) { t.set(x, 5, HEX(0xa59d84)); t.set(x, 11, HEX(0xa59d84)); }
      t.line(4, 1, 6, 3, HEX(0x9c947c)); t.line(11, 13, 12, 15, HEX(0x9c947c)); }
    { TP t(T_SKEL_FACE); TN n = makeNoise(205, {{4, 0.5f}, {1, 0.5f}}); fillNoise(t, boneP, n, 0.5f);
      t.rect(2, 4, 6, 8, HEX(0x15130f)); t.rect(9, 4, 13, 8, HEX(0x15130f)); t.set(2, 4, HEX(0xcfc8b0)); t.set(13, 4, HEX(0xcfc8b0));
      t.rect(4, 6, 4, 6, HEX(0x6ad0ff)); t.rect(11, 6, 11, 6, HEX(0x6ad0ff)); t.set(3, 6, HEX(0x2a6a88)); t.set(12, 6, HEX(0x2a6a88));
      t.set(7, 9, HEX(0x2a251d)); t.set(8, 9, HEX(0x2a251d)); t.set(7, 10, HEX(0x2a251d));
      for (int x = 3; x <= 12; x++) t.set(x, 12, (x & 1) ? HEX(0xf2eee0) : HEX(0x2a251d));
      for (int x = 3; x <= 12; x++) t.set(x, 13, HEX(0x8c856e));
      t.line(10, 1, 12, 2, HEX(0x9c947c)); }
    { TP t(T_SKEL_RIBS); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) t.set(x, y, HEX(0x1a1814));
      for (int r = 0; r < 4; r++) { int y = 2 + r * 3; for (int x = 2; x <= 13; x++) { t.set(x, y, boneP[3]); t.set(x, y + 1, boneP[1]); } t.set(2, y, boneP[1]); t.set(13, y, boneP[1]); }
      for (int y = 0; y < 16; y++) { t.set(7, y, boneP[4]); t.set(8, y, boneP[2]); } }
    { TP t(T_SKEL_HOOD); TN n = makeNoise(206, {{4, 0.6f}, {2, 0.4f}}); fillNoise(t, mkPal({0x3e1013, 0x4a1418, 0x5c1a1f, 0x6e2026, 0x80282e}), n, 0.6f);
      for (int y = 0; y < 16; y += 4) for (int x = 0; x < 16; x++) if ((x + y / 4) % 5 == 0) t.set(x, y, HEX(0x2e0c0e));
      for (int x = 0; x < 16; x += 2) t.set(x, 15, HEX(0x2e0c0e)); }
    // ПАУК: мохнатый бурый, янтарные глаза, оранжевые шевроны на брюшке
    const Pal spP = mkPal({0x342619, 0x403021, 0x4d3a29, 0x5b4531, 0x6a5039});
    { TP t(T_SPID_BODY); TN n = makeNoise(207, {{2, 0.5f}, {1, 0.5f}}); fillNoise(t, spP, n, 0.8f);
      Rng r(208); for (int i = 0; i < 18; i++) { int x = r.range(16), y = r.range(15); t.set(x, y, HEX(0x6b5238)); t.set(x, y + 1, HEX(0x3a2b1e)); } }
    { TP t(T_SPID_ABDO); t.copyFrom(T_SPID_BODY);
      for (int c = 0; c < 3; c++) { int y0 = 2 + c * 5; for (int i = 0; i < 5; i++) { t.set(7 - i, y0 + i / 2, HEX(0xc8782a)); t.set(8 + i, y0 + i / 2, HEX(0xc8782a)); t.set(7 - i, y0 + i / 2 + 1, HEX(0x8a4e1c)); t.set(8 + i, y0 + i / 2 + 1, HEX(0x8a4e1c)); } } }
    { TP t(T_SPID_FACE); t.copyFrom(T_SPID_BODY);
      t.rect(4, 5, 5, 6, HEX(0xffb030)); t.rect(10, 5, 11, 6, HEX(0xffb030)); t.set(4, 5, HEX(0xfff0b0)); t.set(10, 5, HEX(0xfff0b0));
      const int se[4][2] = {{2, 3}, {13, 3}, {6, 2}, {9, 2}}; for (auto& q : se) t.set(q[0], q[1], HEX(0xe08a20));
      t.rect(5, 10, 6, 13, HEX(0x1a120c)); t.rect(9, 10, 10, 13, HEX(0x1a120c)); t.set(6, 14, HEX(0xd8d0b8)); t.set(9, 14, HEX(0xd8d0b8)); }
    { TP t(T_SPID_LEG); TN n = makeNoise(209, {{2, 0.5f}, {1, 0.5f}}); fillNoise(t, spP, n, 0.6f);
      for (int y = 0; y < 16; y += 5) for (int x = 0; x < 16; x++) t.set(x, y, HEX(0x7a5c3a)); }
    { TP t(T_BOWWOOD); TN n = makeNoise(210, {{8, 0.5f}, {1, 0.5f}}); fillNoise(t, mkPal({0x4f3420, 0x5e3f26, 0x6e4b2c, 0x7d5734}), n, 0.5f); }
    // предметы: нить, лук, стрела
    { TP t(T_STRING); t.clear();   // катушка ниток
      t.rect(3, 2, 12, 3, HEX(0x8a6538)); t.rect(3, 2, 12, 2, HEX(0xb08a55)); t.rect(3, 12, 12, 13, HEX(0x8a6538)); t.rect(3, 13, 12, 13, HEX(0x5d4224));
      for (int y = 4; y <= 11; y++) for (int x = 4; x <= 11; x++) t.set(x, y, ((y + (x > 7 ? 1 : 0)) & 1) ? HEX(0xe8e4da) : HEX(0xc9c4b8));
      for (int y = 4; y <= 11; y++) { t.set(4, y, HEX(0xf6f3ec)); t.set(11, y, HEX(0xa8a398)); }
      t.line(11, 9, 13, 11, HEX(0xe8e4da)); t.line(13, 11, 14, 14, HEX(0xe8e4da)); t.set(14, 15, HEX(0xc9c4b8)); }
    { TP t(T_BOW); t.clear();
      static const int arc[13][2] = {{9, 1}, {7, 2}, {6, 3}, {5, 4}, {4, 5}, {4, 6}, {3, 7}, {3, 8}, {4, 9}, {4, 10}, {5, 11}, {6, 12}, {7, 13}};
      for (auto& q : arc) { t.set(q[0], q[1], HEX(0x7d5734)); t.set(q[0] + 1, q[1], HEX(0x4f3420)); }
      t.set(10, 1, HEX(0x4f3420)); t.set(8, 14, HEX(0x4f3420)); t.set(9, 14, HEX(0x7d5734));
      for (int y = 2; y <= 13; y++) t.set(10, y, HEX(0xeceae2));
      t.rect(3, 7, 4, 8, HEX(0xb08a55)); }
    { TP t(T_ARROW); t.clear();
      for (int i = 0; i < 10; i++) { t.set(3 + i, 12 - i, HEX(0x8a6538)); t.set(4 + i, 12 - i, HEX(0x5d4224)); }
      t.set(13, 2, HEX(0xf2eee0)); t.set(14, 1, HEX(0xf2eee0)); t.set(12, 2, HEX(0xcfc8b0)); t.set(13, 3, HEX(0xcfc8b0)); t.set(14, 2, HEX(0x9c947c));
      t.rect(1, 12, 2, 13, HEX(0xf6d2e0)); t.rect(2, 14, 3, 14, HEX(0xf6d2e0)); t.set(1, 14, HEX(0xe8b0c6)); t.set(3, 11, HEX(0xe8b0c6)); }
    { TP t(T_SKIN); t.fill(222, 170, 130, 6); }
    { TP t(T_SLEEVE); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { int v = (int)(t.r.sf() * 8) + ((x + y) % 4 == 0 ? -8 : 0); t.set(x, y, RGBA(48 + v, 96 + v, 150 + v)); } }
}

// ============================================================================
//  ШРИФТ 5x7 с кириллицей
// ============================================================================
struct GlyphDef { u32 cp; const char* rows[7]; };
static const GlyphDef GLYPHS[] = {
    {0x410, {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {0x411, {"#####", "#....", "#....", "####.", "#...#", "#...#", "####."}},
    {0x412, {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {0x413, {"#####", "#....", "#....", "#....", "#....", "#....", "#...."}},
    {0x414, {"..##.", ".#.#.", ".#.#.", ".#.#.", ".#.#.", "#####", "#...#"}},
    {0x415, {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}},
    {0x401, {".#.#.", ".....", "#####", "#....", "####.", "#....", "#####"}},
    {0x416, {"#.#.#", "#.#.#", ".###.", "..#..", ".###.", "#.#.#", "#.#.#"}},
    {0x417, {".###.", "#...#", "....#", "..##.", "....#", "#...#", ".###."}},
    {0x418, {"#...#", "#...#", "#..##", "#.#.#", "##..#", "#...#", "#...#"}},
    {0x419, {".#.#.", "..#..", "#...#", "#..##", "#.#.#", "##..#", "#...#"}},
    {0x41A, {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {0x41B, {"..###", ".#..#", ".#..#", ".#..#", ".#..#", ".#..#", "#...#"}},
    {0x41C, {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {0x41D, {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {0x41E, {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {0x41F, {"#####", "#...#", "#...#", "#...#", "#...#", "#...#", "#...#"}},
    {0x420, {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}},
    {0x421, {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}},
    {0x422, {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {0x423, {"#...#", "#...#", "#...#", ".####", "....#", "#...#", ".###."}},
    {0x424, {"..#..", ".###.", "#.#.#", "#.#.#", "#.#.#", ".###.", "..#.."}},
    {0x425, {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}},
    {0x426, {"#..#.", "#..#.", "#..#.", "#..#.", "#..#.", "#####", "....#"}},
    {0x427, {"#...#", "#...#", "#...#", ".####", "....#", "....#", "....#"}},
    {0x428, {"#.#.#", "#.#.#", "#.#.#", "#.#.#", "#.#.#", "#.#.#", "#####"}},
    {0x429, {"#.#.#", "#.#.#", "#.#.#", "#.#.#", "#.#.#", "#####", "....#"}},
    {0x42A, {"##...", ".#...", ".#...", ".###.", ".#..#", ".#..#", ".###."}},
    {0x42B, {"#...#", "#...#", "#...#", "##..#", "#.#.#", "#.#.#", "##..#"}},
    {0x42C, {"#....", "#....", "#....", "####.", "#...#", "#...#", "####."}},
    {0x42D, {".###.", "#...#", "....#", ".####", "....#", "#...#", ".###."}},
    {0x42E, {"#..#.", "#.#.#", "#.#.#", "###.#", "#.#.#", "#.#.#", "#..#."}},
    {0x42F, {".####", "#...#", "#...#", ".####", "..#.#", ".#..#", "#...#"}},
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', {"####.", "....#", "....#", ".###.", "....#", "....#", "####."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', {".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."}},
    {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
    {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}},
    {'N', {"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"}},
    {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}},
    {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}},
    {',', {".....", ".....", ".....", ".....", ".....", "..#..", ".#..."}},
    {':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
    {';', {".....", ".##..", ".##..", ".....", ".##..", "..#..", ".#..."}},
    {'!', {"..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."}},
    {'?', {".###.", "#...#", "....#", "..##.", "..#..", ".....", "..#.."}},
    {'-', {".....", ".....", ".....", ".###.", ".....", ".....", "....."}},
    {'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
    {'=', {".....", ".....", "#####", ".....", "#####", ".....", "....."}},
    {'/', {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."}},
    {'(', {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."}},
    {')', {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."}},
    {'%', {"##..#", "##..#", "...#.", "..#..", ".#...", "#..##", "#..##"}},
    {'"', {".#.#.", ".#.#.", ".....", ".....", ".....", ".....", "....."}},
    {'\'', {"..#..", "..#..", ".....", ".....", ".....", ".....", "....."}},
    {0xAB, {".....", "..#.#", ".#.#.", "#.#..", ".#.#.", "..#.#", "....."}},
    {0xBB, {".....", "#.#..", ".#.#.", "..#.#", ".#.#.", "#.#..", "....."}},
    {'<', {"...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."}},
    {'>', {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."}},
    {'_', {".....", ".....", ".....", ".....", ".....", ".....", "#####"}},
    {'*', {".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", "....."}},
    {'#', {".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#."}},
    {'[', {".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###."}},
    {']', {".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###."}},
    {'|', {"..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
};
enum { FONT_TEX_W = 128, FONT_TEX_H = 128, GLYPH_CW = 6, GLYPH_CH = 8 };
static u32 g_fontPx[FONT_TEX_W * FONT_TEX_H];
static std::unordered_map<u32, int> g_glyphIndex;

static void genFont() {
    memset(g_fontPx, 0, sizeof(g_fontPx));
    int n = (int)(sizeof(GLYPHS) / sizeof(GLYPHS[0]));
    for (int i = 0; i < n; i++) {
        int gx = (i % 21) * GLYPH_CW, gy = (i / 21) * GLYPH_CH;
        for (int y = 0; y < 7; y++) for (int x = 0; x < 5; x++)
            if (GLYPHS[i].rows[y][x] == '#') g_fontPx[(gy + y) * FONT_TEX_W + gx + x] = 0xFFFFFFFFu;
        g_glyphIndex[GLYPHS[i].cp] = i;
    }
    // латиница, совпадающая по начертанию с кириллицей
    const char* lat = "ABCEHKMOPTX";
    const u32 cyr[] = {0x410, 0x412, 0x421, 0x415, 0x41D, 0x41A, 0x41C, 0x41E, 0x420, 0x422, 0x425};
    for (int i = 0; lat[i]; i++) g_glyphIndex[(u32)lat[i]] = g_glyphIndex[cyr[i]];
    g_glyphIndex[0x2014] = g_glyphIndex['-']; g_glyphIndex[0x2013] = g_glyphIndex['-'];
}
static u32 utf8Next(const char*& s) {
    const u8* p = (const u8*)s;
    u32 c = *p;
    if (c < 0x80) { s += 1; return c; }
    if ((c & 0xE0) == 0xC0 && p[1]) { s += 2; return ((c & 0x1F) << 6) | (p[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0 && p[1] && p[2]) { s += 3; return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); }
    s += 1; return '?';
}
static int glyphFor(u32 c) {
    if (c >= 0x430 && c <= 0x44F) c -= 0x20;
    else if (c == 0x451) c = 0x401;
    else if (c >= 'a' && c <= 'z') c -= 32;
    auto it = g_glyphIndex.find(c);
    return it == g_glyphIndex.end() ? -1 : it->second;
}

// ============================================================================
//  ЗВУК: синтез при запуске + простой микшер в аудио-колбэке
// ============================================================================
enum { AUDIO_RATE = 22050, MAX_VOICES = 24 };
enum Snd {
    SND_STEP0 = 0, SND_DIG0 = 6, SND_BREAK0 = 12,
    SND_PLACE = 18, SND_PICKUP, SND_HIT, SND_HURT, SND_CLICK, SND_SPLASH, SND_TOPTUN, SND_PUKH, SND_GNIL, SND_TEN,
    SND_EAT, SND_POOF, SND_FIZZ, SND_SWING, SND_ZOMB, SND_SKEL, SND_SPID, SND_BOW, SND_COUNT
};
static std::vector<float> g_snd[SND_COUNT];
struct Voice { int snd; float pos, rate, vol; bool on; };
static Voice g_voices[MAX_VOICES];
static SDL_AudioDeviceID g_audioDev = 0;
static float g_masterVol = 0.8f;

static void audioCallback(void*, Uint8* stream, int len) {
    Sint16* out = (Sint16*)stream;
    int n = len / 2;
    for (int i = 0; i < n; i++) {
        float s = 0;
        for (int v = 0; v < MAX_VOICES; v++) {
            Voice& vo = g_voices[v];
            if (!vo.on) continue;
            const std::vector<float>& b = g_snd[vo.snd];
            int ip = (int)vo.pos;
            if (ip + 1 >= (int)b.size()) { vo.on = false; continue; }
            float f = vo.pos - ip;
            s += (b[ip] * (1 - f) + b[ip + 1] * f) * vo.vol;
            vo.pos += vo.rate;
        }
        s *= g_masterVol;
        s = s / (1.f + fabsf(s) * 0.3f);   // мягкий лимитер
        int iv = (int)(s * 30000.f);
        out[i] = (Sint16)clampi(iv, -32767, 32767);
    }
}

static void playSound(int snd, float vol = 1.f, float pitch = 1.f) {
    if (!g_audioDev || snd < 0 || snd >= SND_COUNT || g_snd[snd].empty() || vol <= 0.01f) return;
    SDL_LockAudioDevice(g_audioDev);
    int best = -1; float bestPos = -1;
    for (int v = 0; v < MAX_VOICES; v++) {
        if (!g_voices[v].on) { best = v; break; }
        if (g_voices[v].pos > bestPos) { bestPos = g_voices[v].pos; best = v; }
    }
    if (best >= 0) { g_voices[best].snd = snd; g_voices[best].pos = 0; g_voices[best].rate = pitch; g_voices[best].vol = vol; g_voices[best].on = true; }
    SDL_UnlockAudioDevice(g_audioDev);
}

struct Synth {
    std::vector<float> b; Rng r;
    Synth(float sec, u32 seed) : b((size_t)(sec * AUDIO_RATE), 0.f), r(seed) {}
    // отфильтрованный шум: lp — коэффициент низких частот (0..1), hp — подавление низов
    void noise(float t0, float t1, float amp, float lp, float hp, float attack, float decay) {
        int a = (int)(t0 * AUDIO_RATE), e = std::min((int)(t1 * AUDIO_RATE), (int)b.size());
        float l = 0, h = 0;
        for (int i = a; i < e; i++) {
            float t = (i - a) / (float)AUDIO_RATE;
            float env = (t < attack ? t / attack : expf(-(t - attack) / decay));
            float n = r.sf();
            l += (n - l) * lp;
            float o = l - h; h += (l - h) * hp;
            b[i] += o * amp * env;
        }
    }
    void tone(float t0, float t1, float f0, float f1, float amp, float attack, float decay, int wave = 0) {
        int a = (int)(t0 * AUDIO_RATE), e = std::min((int)(t1 * AUDIO_RATE), (int)b.size());
        float ph = 0; int len = e - a;
        for (int i = a; i < e; i++) {
            float t = (i - a) / (float)AUDIO_RATE;
            float k = len > 0 ? (i - a) / (float)len : 0;
            float f = f0 + (f1 - f0) * k;
            ph += f / AUDIO_RATE; ph -= floorf(ph);
            float w = wave == 0 ? sinf(ph * 6.2831853f) : (wave == 1 ? (ph * 2 - 1) : (ph < 0.5f ? 1.f : -1.f));
            float env = (t < attack ? t / attack : expf(-(t - attack) / decay));
            b[i] += w * amp * env;
        }
    }
};

static void genSounds() {
    // параметры материалов: lp, hp, длительность
    const float LP[6] = {0.55f, 0.18f, 0.35f, 0.30f, 0.80f, 0.12f};
    const float HP[6] = {0.02f, 0.01f, 0.03f, 0.01f, 0.10f, 0.01f};
    for (int m = 0; m < 6; m++) {
        { Synth s(0.12f, 10 + m); s.noise(0, 0.12f, 0.55f, LP[m], HP[m], 0.004f, 0.03f); if (m == SM_WOOD) s.tone(0, 0.08f, 180, 120, 0.25f, 0.002f, 0.025f); g_snd[SND_STEP0 + m] = s.b; }
        { Synth s(0.16f, 20 + m); s.noise(0, 0.16f, 0.75f, LP[m], HP[m], 0.002f, 0.04f);
          if (m == SM_STONE) s.tone(0, 0.06f, 900, 600, 0.12f, 0.001f, 0.015f);
          if (m == SM_WOOD) s.tone(0, 0.1f, 240, 160, 0.3f, 0.001f, 0.03f);
          if (m == SM_GLASS) s.tone(0, 0.1f, 2400, 2000, 0.15f, 0.001f, 0.02f);
          g_snd[SND_DIG0 + m] = s.b; }
        { Synth s(0.35f, 30 + m); s.noise(0, 0.35f, 0.9f, LP[m] * 0.8f, HP[m], 0.002f, 0.08f); s.noise(0.05f, 0.3f, 0.4f, LP[m], HP[m], 0.01f, 0.06f);
          if (m == SM_GLASS) for (int k = 0; k < 5; k++) s.tone(0.02f * k, 0.02f * k + 0.15f, 2000 + k * 450, 1800 + k * 300, 0.12f, 0.001f, 0.05f);
          g_snd[SND_BREAK0 + m] = s.b; }
    }
    { Synth s(0.18f, 40); s.tone(0, 0.15f, 140, 70, 0.6f, 0.002f, 0.04f); s.noise(0, 0.1f, 0.4f, 0.3f, 0.01f, 0.001f, 0.025f); g_snd[SND_PLACE] = s.b; }
    { Synth s(0.14f, 41); s.tone(0, 0.12f, 500, 1300, 0.4f, 0.003f, 0.05f); g_snd[SND_PICKUP] = s.b; }
    { Synth s(0.2f, 42); s.noise(0, 0.2f, 0.9f, 0.4f, 0.02f, 0.001f, 0.04f); s.tone(0, 0.15f, 160, 60, 0.5f, 0.001f, 0.05f); g_snd[SND_HIT] = s.b; }
    { Synth s(0.3f, 43); s.tone(0, 0.3f, 320, 180, 0.4f, 0.005f, 0.1f, 1); s.noise(0, 0.15f, 0.3f, 0.3f, 0.02f, 0.002f, 0.05f); g_snd[SND_HURT] = s.b; }
    { Synth s(0.06f, 44); s.tone(0, 0.06f, 1200, 900, 0.35f, 0.001f, 0.015f, 2); g_snd[SND_CLICK] = s.b; }
    { Synth s(0.5f, 45); s.noise(0, 0.5f, 0.8f, 0.5f, 0.05f, 0.01f, 0.12f); g_snd[SND_SPLASH] = s.b; }
    { Synth s(0.6f, 46); s.tone(0, 0.6f, 95, 70, 0.6f, 0.05f, 0.25f, 1); s.tone(0, 0.6f, 190, 140, 0.2f, 0.05f, 0.2f); g_snd[SND_TOPTUN] = s.b; }
    { Synth s(0.25f, 47); s.tone(0, 0.1f, 900, 1400, 0.35f, 0.005f, 0.04f); s.tone(0.12f, 0.24f, 1000, 1500, 0.3f, 0.005f, 0.04f); g_snd[SND_PUKH] = s.b; }
    { Synth s(0.7f, 48); s.tone(0, 0.7f, 80, 60, 0.5f, 0.1f, 0.3f, 2); s.noise(0, 0.7f, 0.25f, 0.1f, 0.0f, 0.1f, 0.3f); g_snd[SND_GNIL] = s.b; }
    { Synth s(0.6f, 49); s.noise(0, 0.6f, 0.4f, 0.9f, 0.3f, 0.1f, 0.2f); s.tone(0, 0.6f, 600, 300, 0.15f, 0.05f, 0.2f); g_snd[SND_TEN] = s.b; }
    { Synth s(0.25f, 50); for (int k = 0; k < 3; k++) s.noise(k * 0.08f, k * 0.08f + 0.07f, 0.6f, 0.3f, 0.02f, 0.002f, 0.02f); g_snd[SND_EAT] = s.b; }
    { Synth s(0.4f, 51); s.noise(0, 0.4f, 0.7f, 0.6f, 0.05f, 0.005f, 0.1f); s.tone(0, 0.3f, 400, 100, 0.3f, 0.002f, 0.08f); g_snd[SND_POOF] = s.b; }
    { Synth s(0.5f, 52); s.noise(0, 0.5f, 0.6f, 0.9f, 0.4f, 0.01f, 0.15f); g_snd[SND_FIZZ] = s.b; }
    { Synth s(0.15f, 53); s.noise(0, 0.15f, 0.35f, 0.25f, 0.05f, 0.03f, 0.04f); g_snd[SND_SWING] = s.b; }
    { Synth s(0.9f, 54); s.tone(0, 0.9f, 118, 82, 0.42f, 0.12f, 0.35f, 1); s.tone(0.05f, 0.85f, 176, 120, 0.15f, 0.12f, 0.3f); s.noise(0, 0.9f, 0.18f, 0.08f, 0.0f, 0.15f, 0.3f); g_snd[SND_ZOMB] = s.b; }
    { Synth s(0.42f, 55); for (int k = 0; k < 6; k++) { float t0 = k * 0.06f + (k % 2) * 0.012f; s.tone(t0, t0 + 0.03f, 1500 + k * 90, 1100, 0.22f, 0.001f, 0.008f, 2); s.noise(t0, t0 + 0.03f, 0.25f, 0.7f, 0.2f, 0.001f, 0.01f); } g_snd[SND_SKEL] = s.b; }
    { Synth s(0.55f, 56); s.noise(0, 0.55f, 0.55f, 0.95f, 0.35f, 0.04f, 0.16f); s.tone(0, 0.3f, 240, 160, 0.12f, 0.01f, 0.1f, 1); g_snd[SND_SPID] = s.b; }
    { Synth s(0.32f, 57); s.tone(0, 0.3f, 310, 190, 0.5f, 0.001f, 0.08f); s.tone(0, 0.2f, 620, 380, 0.15f, 0.001f, 0.04f); s.noise(0, 0.06f, 0.3f, 0.5f, 0.1f, 0.001f, 0.02f); g_snd[SND_BOW] = s.b; }
}

static void initAudio() {
    genSounds();
    SDL_AudioSpec want, have;
    SDL_memset(&want, 0, sizeof(want));
    want.freq = AUDIO_RATE; want.format = AUDIO_S16SYS; want.channels = 1; want.samples = 1024; want.callback = audioCallback;
    g_audioDev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audioDev) SDL_PauseAudioDevice(g_audioDev, 0);
}

// ============================================================================
//  МИР: чанки, генерация, доступ к блокам, обновления (жидкости, песок)
// ============================================================================
struct MQuad {
    float p[4][3];       // координаты внутри чанка
    u16 tile;
    u8 u[4], v[4];       // пиксели внутри тайла (0..16)
    u8 sky[4], blk[4];   // свет 0..15
    u8 shade[4];         // направление * затенение углов (0..255)
    u8 nrm;              // 0..5, 6 = двусторонний
    u8 flags;            // 1 = вода/полупрозрачный, 2 = светится, 4 = лава
};
struct Section { std::vector<MQuad> q; std::vector<u16> lights; u64 conn = ~0ULL; int vis = -1; };   // lights: x | z<<4 | (y&15)<<8
struct Chunk {
    int cx = 0, cz = 0;
    u8 blk[CHUNK_VOL];
    u8 meta[CHUNK_VOL];
    u8 light[CHUNK_VOL];   // старшие 4 бита — небо, младшие — блоки
    Section sec[SECN];
    bool modified = false, meshed = false, dirty = true;
    int emitters = 0;
};
static inline i64 ckey(int cx, int cz) { return ((i64)(u32)cx << 32) | (u32)cz; }
static inline i64 posKey(int x, int y, int z) {
    return ((i64)(x & 0x3FFFFFF) << 38) | ((i64)(z & 0x3FFFFFF) << 12) | (i64)(y & 0xFFF);
}
static inline void unpackPos(i64 k, int& x, int& y, int& z) {
    x = (int)((k >> 38) & 0x3FFFFFF); if (x & 0x2000000) x -= 0x4000000;
    z = (int)((k >> 12) & 0x3FFFFFF); if (z & 0x2000000) z -= 0x4000000;
    y = (int)(k & 0xFFF);
}

struct Furnace { Stack in, fuel, out; float burn = 0, burnMax = 0, cook = 0; };
struct Grow { int x, y, z; float t; };

enum Biome { BI_PLAINS = 0, BI_FOREST, BI_DESERT, BI_SNOW, BI_MOUNT, BI_BEACH, BI_OCEAN };

static u32 g_seed = 12345;
static Noise nCont, nHill, nMount, nTemp, nHum, nRiver, nCave1, nCave2, nCave3, nLake, nDetail;
static std::unordered_map<i64, Chunk*> g_chunks;
static std::unordered_map<i64, std::vector<u8>> g_savedChunks;   // RLE изменённых чанков
static std::map<i64, std::vector<Stack>> g_chests;
static std::map<i64, Furnace> g_furnaces;
static std::vector<Grow> g_grow;
static i64 g_tick = 0;

static void worldInitNoise(u32 seed) {
    g_seed = seed;
    nCont.init(seed * 3 + 1); nHill.init(seed * 5 + 2); nMount.init(seed * 7 + 3); nTemp.init(seed * 11 + 4);
    nHum.init(seed * 13 + 5); nRiver.init(seed * 17 + 6); nCave1.init(seed * 19 + 7); nCave2.init(seed * 23 + 8);
    nCave3.init(seed * 29 + 9); nLake.init(seed * 31 + 10); nDetail.init(seed * 37 + 11);
}

static Chunk* g_lastChunk = nullptr;
static inline Chunk* getChunk(int cx, int cz) {
    if (g_lastChunk && g_lastChunk->cx == cx && g_lastChunk->cz == cz) return g_lastChunk;
    auto it = g_chunks.find(ckey(cx, cz));
    if (it == g_chunks.end()) return nullptr;
    g_lastChunk = it->second;
    return it->second;
}
static inline u8 getBlock(int x, int y, int z) {
    if (y < 0) return B_BEDROCK;
    if (y >= WH) return B_AIR;
    Chunk* c = getChunk(x >> 4, z >> 4);
    if (!c) return B_BEDROCK;   // незагруженное — непроходимо
    return c->blk[cidx(x & 15, y, z & 15)];
}
static inline u8 getMeta(int x, int y, int z) {
    if (y < 0 || y >= WH) return 0;
    Chunk* c = getChunk(x >> 4, z >> 4);
    if (!c) return 0;
    return c->meta[cidx(x & 15, y, z & 15)];
}
static inline int getLightAt(int x, int y, int z, int& sky, int& blk) {
    if (y >= WH) { sky = 15; blk = 0; return 15; }
    if (y < 0) { sky = 0; blk = 0; return 0; }
    Chunk* c = getChunk(x >> 4, z >> 4);
    if (!c) { sky = 15; blk = 0; return 15; }
    u8 l = c->light[cidx(x & 15, y, z & 15)];
    sky = l >> 4; blk = l & 15;
    return std::max(sky, blk);
}
static inline bool isLoaded(int x, int z) { return getChunk(x >> 4, z >> 4) != nullptr; }

// ---------------------------------------------------------------- рельеф
struct ColInfo { int h; u8 biome; float temp, hum; };
static ColInfo columnInfo(int x, int z) {
    ColInfo ci;
    float cont = nCont.fbm2(x * 0.0016f, z * 0.0016f, 4);
    float hills = nHill.fbm2(x * 0.009f, z * 0.009f, 4);
    float mnt = nMount.fbm2(x * 0.0032f, z * 0.0032f, 4);
    float m = smooth01(0.08f, 0.36f, mnt);
    float ridge = 1.f - fabsf(nMount.n2(x * 0.013f + 33.f, z * 0.013f - 71.f) * 1.4f);
    ridge = clampf(ridge, 0, 1); ridge *= ridge;
    float h = 57.f + cont * 38.f + hills * (6.f + m * 6.f) + m * (24.f + ridge * 26.f);
    float rv = fabsf(nRiver.fbm2(x * 0.0026f, z * 0.0026f, 3));
    if (rv < 0.04f && h > SEA - 3) {
        float k = smooth01(0.0f, 0.04f, rv);
        h = lerpf(SEA - 3.f, h, k * k);
    }
    ci.h = clampi((int)h, 6, WH - 12);
    ci.temp = nTemp.fbm2(x * 0.0021f + 100.f, z * 0.0021f, 3) * 1.6f - std::max(0, ci.h - 70) * 0.006f;
    ci.hum = nHum.fbm2(x * 0.0019f - 300.f, z * 0.0019f, 3) * 1.6f;
    if (ci.h < SEA - 3) ci.biome = BI_OCEAN;
    else if (ci.h <= SEA + 1) ci.biome = ci.temp < -0.2f ? BI_SNOW : BI_BEACH;
    else if (ci.h > 92 && m > 0.5f) ci.biome = BI_MOUNT;
    else if (ci.temp < -0.18f) ci.biome = BI_SNOW;
    else if (ci.temp > 0.16f && ci.hum < 0.05f) ci.biome = BI_DESERT;
    else if (ci.hum > 0.08f) ci.biome = BI_FOREST;
    else ci.biome = BI_PLAINS;
    return ci;
}

// ---------------------------------------------------------------- деревья
template <class SetF, class GetF>
static bool growTree(int kind, int x, int y, int z, Rng& r, SetF set, GetF get) {
    // kind: 0 — лиственное, 1 — ель
    if (kind == 0) {
        int h = r.range(4, 6);
        if (y + h + 2 >= WH) return false;
        for (int i = 1; i <= h; i++) { u8 b = get(x, y + i, z); if (b != B_AIR && !BD[b].replaceable && b != B_LEAVES && b != B_SAPLING) return false; }
        for (int dy = h - 2; dy <= h + 1; dy++) {
            int rad = dy >= h ? 1 : 2;
            for (int dz = -rad; dz <= rad; dz++) for (int dx = -rad; dx <= rad; dx++) {
                if (rad == 2 && abs(dx) == 2 && abs(dz) == 2 && r.range(2)) continue;
                if (dy == h + 1 && abs(dx) + abs(dz) > 1) continue;
                u8 b = get(x + dx, y + dy, z + dz);
                if (b == B_AIR || BD[b].replaceable) set(x + dx, y + dy, z + dz, B_LEAVES);
            }
        }
        for (int i = 0; i < h; i++) set(x, y + i, z, B_LOG);
    } else {
        int h = r.range(6, 9);
        if (y + h + 2 >= WH) return false;
        for (int i = 1; i <= h; i++) { u8 b = get(x, y + i, z); if (b != B_AIR && !BD[b].replaceable && b != B_PINE_LEAVES && b != B_SAPLING) return false; }
        for (int dy = 2; dy <= h + 1; dy++) {
            int rad = (dy == h + 1) ? 0 : ((h - dy) % 3 == 0 ? 1 : 2);
            if (dy > h - 2) rad = std::min(rad, 1);
            for (int dz = -rad; dz <= rad; dz++) for (int dx = -rad; dx <= rad; dx++) {
                if (abs(dx) + abs(dz) > rad + (rad > 1 ? 0 : 1)) continue;
                u8 b = get(x + dx, y + dy, z + dz);
                if (b == B_AIR || BD[b].replaceable) set(x + dx, y + dy, z + dz, B_PINE_LEAVES);
            }
        }
        for (int i = 0; i < h; i++) set(x, y + i, z, B_PINE_LOG);
    }
    return true;
}

// ---------------------------------------------------------------- генерация чанка
static void generateChunk(Chunk* c) {
    const int bx = c->cx * 16, bz = c->cz * 16;
    memset(c->blk, 0, sizeof(c->blk));
    memset(c->meta, 0, sizeof(c->meta));
    memset(c->light, 0, sizeof(c->light));
    ColInfo col[16][16];
    for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) col[z][x] = columnInfo(bx + x, bz + z);

    // 1. слои земли
    for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
        const ColInfo& ci = col[z][x];
        int h = ci.h;
        u32 hs = hash3i(bx + x, 0, bz + z, g_seed);
        int dirtDepth = 3 + (int)(hs % 2);
        for (int y = 0; y <= h; y++) {
            u8 b = B_STONE;
            int d = h - y;
            switch (ci.biome) {
                case BI_DESERT: b = d < 4 ? B_SAND : (d < 7 ? B_SANDSTONE : B_STONE); break;
                case BI_BEACH: b = d < 4 ? B_SAND : B_STONE; break;
                case BI_OCEAN: b = d < 3 ? ((hs >> 8) % 3 == 0 ? B_GRAVEL : ((hs >> 9) % 2 ? B_SAND : B_DIRT)) : B_STONE; break;
                case BI_SNOW: b = d == 0 ? (h > SEA ? B_SNOWGRASS : B_DIRT) : (d < dirtDepth ? B_DIRT : B_STONE); break;
                case BI_MOUNT: b = (d == 0 && h > 104) ? B_SNOW : B_STONE; if (d < 2 && h < 98 && (hs % 3)) b = d == 0 ? B_GRASS : B_DIRT; break;
                default: b = d == 0 ? B_GRASS : (d < dirtDepth ? B_DIRT : B_STONE); break;
            }
            if (ci.biome != BI_OCEAN && d == 0 && h < SEA) b = B_DIRT;   // дно рек и озёр
            if (ci.biome != BI_OCEAN && ci.biome != BI_DESERT && ci.biome != BI_BEACH && h <= SEA && d < 3 && (hs >> 4) % 4 == 0) b = B_SAND;
            c->blk[cidx(x, y, z)] = b;
        }
        for (int y = h + 1; y <= SEA; y++) c->blk[cidx(x, y, z)] = B_WATER;
        if (h < SEA && ci.temp < -0.2f) c->blk[cidx(x, SEA, z)] = B_ICE;
        c->blk[cidx(x, 0, z)] = B_BEDROCK;
        for (int y = 1; y <= 3; y++) if (hash3i(bx + x, y, bz + z, g_seed ^ 0xBED) % (u32)(y + 1) == 0) c->blk[cidx(x, y, z)] = B_BEDROCK;
    }

    // 2. пещеры: шум на сетке 4x4x4 с трилинейной интерполяцией
    static float gT[5][33][5], gC[5][33][5];
    for (int gz = 0; gz < 5; gz++) for (int gy = 0; gy < 33; gy++) for (int gx = 0; gx < 5; gx++) {
        float wx = (float)(bx + gx * 4), wy = (float)(gy * 4), wz = (float)(bz + gz * 4);
        float a = nCave1.n3(wx * 0.022f, wy * 0.04f, wz * 0.022f);
        float b = nCave2.n3(wx * 0.022f + 50.f, wy * 0.04f, wz * 0.022f);
        gT[gx][gy][gz] = a * a + b * b;
        gC[gx][gy][gz] = nCave3.fbm3(wx * 0.016f, wy * 0.028f, wz * 0.016f, 2);
    }
    for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
        int h = col[z][x].h;
        bool wet = h <= SEA + 1;
        int gx = x >> 2, gz = z >> 2; float fx = (x & 3) / 4.f, fz = (z & 3) / 4.f;
        float lakeN = nLake.n2((bx + x) * 0.03f, (bz + z) * 0.03f);
        for (int y = 1; y < std::min(h + 1, WH - 1); y++) {
            int gy = y >> 2; float fy = (y & 3) / 4.f;
            #define TRI(G) ( lerpf(lerpf(lerpf(G[gx][gy][gz], G[gx+1][gy][gz], fx), lerpf(G[gx][gy+1][gz], G[gx+1][gy+1][gz], fx), fy), \
                                  lerpf(lerpf(G[gx][gy][gz+1], G[gx+1][gy][gz+1], fx), lerpf(G[gx][gy+1][gz+1], G[gx+1][gy+1][gz+1], fx), fy), fz) )
            float t = TRI(gT);
            float ch = TRI(gC);
            #undef TRI
            int topLimit = wet ? h - 4 : h;
            bool tunnel = t < 0.010f && y <= topLimit;
            float chThr = 0.30f + std::max(0, y - 40) * 0.01f;
            bool cave = ch > chThr && y < h - 7;
            if (!(tunnel || cave)) continue;
            u8& cell = c->blk[cidx(x, y, z)];
            if (cell == B_BEDROCK || cell == B_WATER || cell == B_ICE) continue;
            if (y <= 10) cell = B_LAVA;
            else if (cave && y <= 22 && lakeN > 0.22f) cell = B_WATER;
            else cell = B_AIR;
            // песок, повисший над пещерой, превращаем в песчаник
            if (y + 1 < WH && c->blk[cidx(x, y + 1, z)] == B_SAND) c->blk[cidx(x, y + 1, z)] = B_SANDSTONE;
            if (y + 1 < WH && c->blk[cidx(x, y + 1, z)] == B_GRAVEL) c->blk[cidx(x, y + 1, z)] = B_STONE;
        }
    }

    // 3. руды жилами
    Rng r(((u64)(u32)c->cx * 341873128712ULL) ^ ((u64)(u32)c->cz * 132897987541ULL) ^ g_seed);
    struct OreSpec { u8 id; int tries, ymin, ymax, smin, smax; int chance; };
    const OreSpec ores[4] = {
        {B_COAL_ORE, 30, 5, 120, 4, 12, 100},
        {B_IRON_ORE, 11, 5, 62, 3, 7, 100},
        {B_GOLD_ORE, 3, 4, 30, 3, 6, 100},
        {B_DIAMOND_ORE, 1, 2, 14, 2, 5, 60},
    };
    for (int o = 0; o < 4; o++) {
        const OreSpec& s = ores[o];
        for (int t = 0; t < s.tries; t++) {
            if (r.range(100) >= s.chance) continue;
            int x = r.range(16), z = r.range(16), y = r.range(s.ymin, s.ymax);
            int n = r.range(s.smin, s.smax);
            for (int k = 0; k < n; k++) {
                if (y > 0 && y < WH && c->blk[cidx(x, y, z)] == B_STONE) c->blk[cidx(x, y, z)] = s.id;
                int d = r.range(6);
                x = clampi(x + DX[d], 0, 15); y = clampi(y + DY[d], 1, WH - 1); z = clampi(z + DZ[d], 0, 15);
            }
        }
    }
    // гравий и камни-пятна в толще
    for (int t = 0; t < 4; t++) {
        int x = r.range(16), z = r.range(16), y = r.range(10, 70), n = r.range(6, 14);
        for (int k = 0; k < n; k++) {
            if (c->blk[cidx(x, y, z)] == B_STONE) c->blk[cidx(x, y, z)] = B_GRAVEL;
            int d = r.range(6); x = clampi(x + DX[d], 0, 15); y = clampi(y + DY[d], 1, WH - 1); z = clampi(z + DZ[d], 0, 15);
        }
    }

    // 4. растительность
    auto lset = [&](int x, int y, int z, u8 b) { if (x >= 0 && x < 16 && z >= 0 && z < 16 && y > 0 && y < WH) c->blk[cidx(x, y, z)] = b; };
    auto lget = [&](int x, int y, int z) -> u8 { if (x < 0 || x > 15 || z < 0 || z > 15 || y < 0 || y >= WH) return B_STONE; return c->blk[cidx(x, y, z)]; };
    int treeCount = 0; int treeKind = 0;
    u8 cb = col[8][8].biome;
    if (cb == BI_FOREST) { treeCount = r.range(5, 9); treeKind = col[8][8].temp < -0.05f ? 1 : 0; }
    else if (cb == BI_PLAINS) treeCount = r.range(4) == 0 ? 1 : 0;
    else if (cb == BI_SNOW) { treeCount = r.range(1, 3); treeKind = 1; }
    else if (cb == BI_MOUNT) { treeCount = r.range(0, 2); treeKind = 1; }
    for (int t = 0; t < treeCount; t++) {
        int x = r.range(2, 13), z = r.range(2, 13), h = col[z][x].h;
        u8 g = lget(x, h, z);
        if ((g == B_GRASS || g == B_SNOWGRASS) && h > SEA && lget(x, h + 1, z) == B_AIR) {
            if (growTree(treeKind, x, h + 1, z, r, lset, lget)) lset(x, h, z, B_DIRT);
        }
    }
    if (cb == BI_DESERT) {
        int n = r.range(0, 3);
        for (int t = 0; t < n; t++) {
            int x = r.range(1, 14), z = r.range(1, 14), h = col[z][x].h;
            if (lget(x, h, z) != B_SAND || h <= SEA) continue;
            int ch = r.range(1, 3);
            for (int i = 1; i <= ch; i++) if (lget(x, h + i, z) == B_AIR && lget(x + 1, h + i, z) == B_AIR && lget(x - 1, h + i, z) == B_AIR) lset(x, h + i, z, B_CACTUS);
        }
    }
    for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
        int h = col[z][x].h;
        if (h + 1 >= WH || lget(x, h, z) != B_GRASS || lget(x, h + 1, z) != B_AIR) continue;
        u32 hv = hash3i(bx + x, h, bz + z, g_seed ^ 0xF10);
        int pr = hv % 100;
        int grassChance = cb == BI_FOREST ? 18 : (cb == BI_PLAINS ? 22 : 8);
        if (pr < 2) lset(x, h + 1, z, (hv >> 8) & 1 ? B_FLOWER_R : B_FLOWER_Y);
        else if (pr < 2 + grassChance) lset(x, h + 1, z, B_TALLGRASS);
    }
    c->emitters = 0;
    for (int i = 0; i < CHUNK_VOL; i++) if (BD[c->blk[i]].light) c->emitters++;
}

// ---------------------------------------------------------------- RLE сжатие
static void rleEncode(const u8* d, int n, std::vector<u8>& out) {
    int i = 0;
    while (i < n) {
        u8 v = d[i]; int run = 1;
        while (i + run < n && d[i + run] == v && run < 255) run++;
        out.push_back((u8)run); out.push_back(v); i += run;
    }
}
static bool rleDecode(const u8*& p, const u8* end, u8* d, int n) {
    int i = 0;
    while (i < n) {
        if (p + 2 > end) return false;
        int run = p[0]; u8 v = p[1]; p += 2;
        if (run == 0 || i + run > n) return false;
        memset(d + i, v, run); i += run;
    }
    return true;
}
static void encodeChunk(Chunk* c, std::vector<u8>& out) {
    out.clear();
    rleEncode(c->blk, CHUNK_VOL, out);
    rleEncode(c->meta, CHUNK_VOL, out);
}
static bool decodeChunk(Chunk* c, const std::vector<u8>& in) {
    const u8* p = in.data(); const u8* e = p + in.size();
    if (!rleDecode(p, e, c->blk, CHUNK_VOL)) return false;
    if (!rleDecode(p, e, c->meta, CHUNK_VOL)) return false;
    c->emitters = 0;
    for (int i = 0; i < CHUNK_VOL; i++) if (BD[c->blk[i] < B_COUNT ? c->blk[i] : 0].light) c->emitters++;
    for (int i = 0; i < CHUNK_VOL; i++) if (c->blk[i] >= B_COUNT) c->blk[i] = B_AIR;
    return true;
}

// ---------------------------------------------------------------- загрузка/выгрузка чанков
static Chunk* loadOrGenChunk(int cx, int cz) {
    Chunk* c = new Chunk();
    c->cx = cx; c->cz = cz;
    auto it = g_savedChunks.find(ckey(cx, cz));
    bool ok = false;
    if (it != g_savedChunks.end()) {
        ok = decodeChunk(c, it->second);
        if (ok) {
            c->modified = true;
            memset(c->light, 0, sizeof(c->light));
            for (int i = 0; i < CHUNK_VOL; i++) if (c->blk[i] == B_SAPLING) {
                int x = i & 15, z = (i >> 4) & 15, y = i >> 8;
                Grow g; g.x = cx * 16 + x; g.y = y; g.z = cz * 16 + z; g.t = 30.f + (hash32(i) % 60);
                g_grow.push_back(g);
            }
        }
    }
    if (!ok) generateChunk(c);
    g_chunks[ckey(cx, cz)] = c;
    // соседям нужно перестроить края
    for (int d = 0; d < 4; d++) { Chunk* n = getChunk(cx + HDX[d], cz + HDZ[d]); if (n) n->dirty = true; }
    for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) { Chunk* n = getChunk(cx + dx, cz + dz); if (n) n->dirty = true; }
    return c;
}
static void unloadChunk(Chunk* c) {
    if (c->modified) { std::vector<u8> v; encodeChunk(c, v); g_savedChunks[ckey(c->cx, c->cz)] = v; }
    g_chunks.erase(ckey(c->cx, c->cz));
    if (g_lastChunk == c) g_lastChunk = nullptr;
    delete c;
}
static void freeAllChunks() {
    for (auto& kv : g_chunks) delete kv.second;
    g_chunks.clear(); g_lastChunk = nullptr;
}

// ---------------------------------------------------------------- отложенные обновления блоков
struct Upd { int x, y, z; };
static std::map<i64, std::vector<Upd>> g_sched;
static std::unordered_map<i64, i64> g_pending;   // posKey -> tick

static int updateDelayFor(u8 b) {
    if (b == B_WATER) return 5;
    if (b == B_LAVA) return 30;
    if (b == B_SAND || b == B_GRAVEL) return 2;
    return 1;
}
static bool needsUpdate(u8 b) {
    return b == B_WATER || b == B_LAVA || b == B_SAND || b == B_GRAVEL || b == B_TORCH || b == B_TALLGRASS ||
           b == B_FLOWER_R || b == B_FLOWER_Y || b == B_SAPLING || b == B_LADDER || b == B_DOOR || b == B_CACTUS || b == B_BED;
}
static void scheduleUpdate(int x, int y, int z, int delay = -1) {
    if (y < 0 || y >= WH || !isLoaded(x, z)) return;
    u8 b = getBlock(x, y, z);
    if (!needsUpdate(b)) return;
    i64 k = posKey(x, y, z);
    if (g_pending.count(k)) return;
    if (delay < 0) delay = updateDelayFor(b);
    i64 t = g_tick + delay;
    g_pending[k] = t;
    Upd u; u.x = x; u.y = y; u.z = z;
    g_sched[t].push_back(u);
}

static void markLightNeighborsDirty(int cx, int cz) {
    for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) { Chunk* n = getChunk(cx + dx, cz + dz); if (n) n->dirty = true; }
}

// прямое изменение блока (без каскада)
static void setBlockRaw(int x, int y, int z, u8 b, u8 m) {
    if (y < 0 || y >= WH) return;
    Chunk* c = getChunk(x >> 4, z >> 4);
    if (!c) return;
    int i = cidx(x & 15, y, z & 15);
    u8 old = c->blk[i];
    if (old == b && c->meta[i] == m) return;
    if (BD[old].light) c->emitters--;
    if (BD[b].light) c->emitters++;
    c->blk[i] = b; c->meta[i] = m;
    c->modified = true; c->dirty = true;
    int lx = x & 15, lz = z & 15;
    bool lightChange = BD[old].light != BD[b].light;
    bool opacChange = BD[old].opaque != BD[b].opaque || BD[old].shape != BD[b].shape;
    if (lightChange) { markLightNeighborsDirty(c->cx, c->cz); return; }
    // по умолчанию перестраивается только этот чанк; соседи — лишь если блок на границе
    if (lx == 0) { Chunk* n = getChunk(c->cx - 1, c->cz); if (n) n->dirty = true; }
    if (lx == 15) { Chunk* n = getChunk(c->cx + 1, c->cz); if (n) n->dirty = true; }
    if (lz == 0) { Chunk* n = getChunk(c->cx, c->cz - 1); if (n) n->dirty = true; }
    if (lz == 15) { Chunk* n = getChunk(c->cx, c->cz + 1); if (n) n->dirty = true; }
    if ((lx == 0 || lx == 15) && (lz == 0 || lz == 15)) {
        Chunk* n = getChunk(c->cx + (lx ? 1 : -1), c->cz + (lz ? 1 : -1)); if (n) n->dirty = true;
    }
    if (opacChange) {   // тень от блока может изменить свет факелов в соседних чанках
        for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) {
            Chunk* n = getChunk(c->cx + dx, c->cz + dz);
            if (n && n->emitters > 0) { markLightNeighborsDirty(c->cx, c->cz); return; }
        }
    }
}
static void setBlock(int x, int y, int z, u8 b, u8 m = 0) {
    setBlockRaw(x, y, z, b, m);
    scheduleUpdate(x, y, z);
    for (int d = 0; d < 6; d++) scheduleUpdate(x + DX[d], y + DY[d], z + DZ[d]);
}

// ============================================================================
//  СВЕТ И МЕШИ: только видимые грани, сглаженный свет и затенение углов
// ============================================================================
enum { PW = 18, PH = WH + 2, BPAD = 14, BW = 16 + BPAD * 2 };
static u8 g_pb[PW * PW * PH];     // блоки с запасом в 1 клетку
static u8 g_pm[PW * PW * PH];     // мета
static u8 g_psky[PW * PW * PH];   // небесный свет
static u8 g_pbl[BW * BW * WH];    // свет от факелов/лавы с запасом 14
static int g_queue[BW * BW * WH];
static Chunk* g_nb[3][3];

static inline int pidx(int x, int y, int z) { return ((y + 1) * PW + (z + 1)) * PW + (x + 1); }
static inline int bidx(int x, int y, int z) { return (y * BW + (z + BPAD)) * BW + (x + BPAD); }

static inline u8 nbBlock(int x, int y, int z) {   // x,z в диапазоне [-16, 31]
    if (y < 0) return B_BEDROCK;
    if (y >= WH) return B_AIR;
    Chunk* c = g_nb[(x + 16) >> 4][(z + 16) >> 4];
    if (!c) return B_STONE;
    return c->blk[cidx(x & 15, y, z & 15)];
}
static inline bool lightAtten(u8 b, int& extra) {
    if (b == B_LEAVES || b == B_PINE_LEAVES || b == B_ICE) { extra = 1; return true; }
    if (b == B_WATER) { extra = 2; return true; }
    extra = 0; return false;
}

static void computeLight(Chunk* c) {
    // заполнить блоки с запасом
    for (int z = -1; z <= 16; z++) for (int x = -1; x <= 16; x++)
        for (int y = -1; y <= WH; y++) {
            int i = pidx(x, y, z);
            if (y < 0) { g_pb[i] = B_BEDROCK; g_pm[i] = 0; continue; }
            if (y >= WH) { g_pb[i] = B_AIR; g_pm[i] = 0; continue; }
            Chunk* cc = g_nb[(x + 16) >> 4][(z + 16) >> 4];
            if (!cc) { g_pb[i] = B_STONE; g_pm[i] = 0; continue; }
            int ci = cidx(x & 15, y, z & 15);
            g_pb[i] = cc->blk[ci]; g_pm[i] = cc->meta[ci];
        }
    // --- небесный свет
    int qh = 0, qt = 0;
    for (int z = -1; z <= 16; z++) for (int x = -1; x <= 16; x++) {
        int s = 15;
        for (int y = WH; y >= -1; y--) {
            int i = pidx(x, y, z); u8 b = g_pb[i];
            int ex;
            if (BD[b].opaque) s = 0;
            else if (lightAtten(b, ex)) s = std::max(0, s - ex);
            g_psky[i] = (u8)s;
        }
    }
    for (int z = -1; z <= 16; z++) for (int x = -1; x <= 16; x++)
        for (int y = 0; y < WH; y++) {
            int i = pidx(x, y, z); int s = g_psky[i];
            if (s < 2) continue;
            bool seed = false;
            for (int d = 0; d < 6 && !seed; d++) {
                if (d == 2 || d == 3) continue;
                int nx = x + DX[d], nz = z + DZ[d];
                if (nx < -1 || nx > 16 || nz < -1 || nz > 16) continue;
                int j = pidx(nx, y, nz);
                if (!BD[g_pb[j]].opaque && g_psky[j] < s - 1) seed = true;
            }
            if (seed) g_queue[qt++] = i;
        }
    while (qh < qt) {
        int i = g_queue[qh++];
        int y = i / (PW * PW) - 1, rem = i % (PW * PW), z = rem / PW - 1, x = rem % PW - 1;
        int l = g_psky[i] - 1;
        if (l <= 0) continue;
        for (int d = 0; d < 6; d++) {
            int nx = x + DX[d], ny = y + DY[d], nz = z + DZ[d];
            if (nx < -1 || nx > 16 || nz < -1 || nz > 16 || ny < 0 || ny >= WH) continue;
            int j = pidx(nx, ny, nz);
            u8 nb = g_pb[j];
            if (BD[nb].opaque) continue;
            int ex; lightAtten(nb, ex);
            int nl = l - ex;
            if (nl > g_psky[j]) { g_psky[j] = (u8)nl; if (qt < BW * BW * WH) g_queue[qt++] = j; }
        }
    }
    // --- свет от источников
    memset(g_pbl, 0, sizeof(g_pbl));
    qh = qt = 0;
    for (int gz = 0; gz < 3; gz++) for (int gx = 0; gx < 3; gx++) {
        Chunk* cc = g_nb[gx][gz];
        if (!cc || cc->emitters <= 0) continue;
        int ox = (gx - 1) * 16, oz = (gz - 1) * 16;
        for (int i = 0; i < CHUNK_VOL; i++) {
            u8 em = BD[cc->blk[i]].light;
            if (!em) continue;
            int x = (i & 15) + ox, z = ((i >> 4) & 15) + oz, y = i >> 8;
            if (x < -BPAD || x >= 16 + BPAD || z < -BPAD || z >= 16 + BPAD) continue;
            int bi = bidx(x, y, z);
            if (g_pbl[bi] < em) { g_pbl[bi] = em; g_queue[qt++] = bi; }
        }
    }
    while (qh < qt) {
        int i = g_queue[qh++];
        int y = i / (BW * BW), rem = i % (BW * BW), z = rem / BW - BPAD, x = rem % BW - BPAD;
        int l = g_pbl[i] - 1;
        if (l <= 0) continue;
        for (int d = 0; d < 6; d++) {
            int nx = x + DX[d], ny = y + DY[d], nz = z + DZ[d];
            if (nx < -BPAD || nx >= 16 + BPAD || nz < -BPAD || nz >= 16 + BPAD || ny < 0 || ny >= WH) continue;
            int j = bidx(nx, ny, nz);
            if (g_pbl[j] >= l) continue;
            u8 nb = nbBlock(nx, ny, nz);
            if (BD[nb].opaque) continue;
            g_pbl[j] = (u8)l;
            if (qt < BW * BW * WH) g_queue[qt++] = j;
        }
    }
    for (int y = 0; y < WH; y++) for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++)
        c->light[cidx(x, y, z)] = (u8)((g_psky[pidx(x, y, z)] << 4) | g_pbl[bidx(x, y, z)]);
}

static inline u8 PB(int x, int y, int z) { return g_pb[pidx(x, y, z)]; }
static inline u8 PM(int x, int y, int z) { return g_pm[pidx(x, y, z)]; }
static inline int PSKY(int x, int y, int z) { if (y >= WH) return 15; if (y < 0) return 0; return g_psky[pidx(x, y, z)]; }
static inline int PBL(int x, int y, int z) { if (y < 0 || y >= WH) return 0; return g_pbl[bidx(x, y, z)]; }

// углы граней в порядке: лево-верх, право-верх, право-низ, лево-низ (вид снаружи)
static const float FACE_C[6][4][3] = {
    {{1, 1, 0}, {1, 1, 1}, {1, 0, 1}, {1, 0, 0}},
    {{0, 1, 1}, {0, 1, 0}, {0, 0, 0}, {0, 0, 1}},
    {{0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1}},
    {{0, 0, 1}, {1, 0, 1}, {1, 0, 0}, {0, 0, 0}},
    {{1, 1, 1}, {0, 1, 1}, {0, 0, 1}, {1, 0, 1}},
    {{0, 1, 0}, {1, 1, 0}, {1, 0, 0}, {0, 0, 0}},
};
static const float FACE_SHADE[6] = {0.80f, 0.80f, 1.0f, 0.55f, 0.68f, 0.68f};
static const float AO_MUL[4] = {0.50f, 0.68f, 0.84f, 1.0f};

static inline void faceUV(int d, float x, float y, float z, float& u, float& v) {
    switch (d) {
        case 0: u = z; v = 1 - y; break;
        case 1: u = 1 - z; v = 1 - y; break;
        case 2: u = x; v = z; break;
        case 3: u = x; v = 1 - z; break;
        case 4: u = 1 - x; v = 1 - y; break;
        default: u = x; v = 1 - y; break;
    }
}
static inline int hdirToFace(int h) { static const int m[4] = {0, 4, 1, 5}; return m[h & 3]; }

static bool faceVisible(u8 b, u8 n) {
    if (n == B_AIR) return true;
    const BlockDef& dn = BD[n];
    if (dn.opaque) return false;
    if (BD[b].group && BD[b].group == dn.group) return false;
    return true;
}

// грань полного блока со сглаженным светом и AO
static void emitCubeFace(std::vector<MQuad>& out, int x, int y, int z, int d, int tile, u8 flags) {
    MQuad q;
    int ox = x + DX[d], oy = y + DY[d], oz = z + DZ[d];
    int a1, a2;   // оси касательных
    if (d < 2) { a1 = 1; a2 = 2; } else if (d < 4) { a1 = 0; a2 = 2; } else { a1 = 0; a2 = 1; }
    int ao[4];
    for (int k = 0; k < 4; k++) {
        const float* cc = FACE_C[d][k];
        q.p[k][0] = x + cc[0]; q.p[k][1] = y + cc[1]; q.p[k][2] = z + cc[2];
        float u, v; faceUV(d, cc[0], cc[1], cc[2], u, v);
        q.u[k] = (u8)(u * 16); q.v[k] = (u8)(v * 16);
        int s1[3] = {0, 0, 0}, s2[3] = {0, 0, 0};
        s1[a1] = cc[a1] > 0.5f ? 1 : -1;
        s2[a2] = cc[a2] > 0.5f ? 1 : -1;
        int x1 = ox + s1[0], y1 = oy + s1[1], z1 = oz + s1[2];
        int x2 = ox + s2[0], y2 = oy + s2[1], z2 = oz + s2[2];
        int x3 = ox + s1[0] + s2[0], y3 = oy + s1[1] + s2[1], z3 = oz + s1[2] + s2[2];
        bool o1 = y1 >= 0 && y1 < WH && BD[PB(x1, y1, z1)].opaque;
        bool o2 = y2 >= 0 && y2 < WH && BD[PB(x2, y2, z2)].opaque;
        bool o3 = y3 >= 0 && y3 < WH && BD[PB(x3, y3, z3)].opaque;
        ao[k] = (o1 && o2) ? 0 : 3 - ((int)o1 + (int)o2 + (int)o3);
        int ss = PSKY(ox, oy, oz), sb = PBL(ox, oy, oz), n = 1;
        if (!o1) { ss += PSKY(x1, y1, z1); sb += PBL(x1, y1, z1); n++; }
        if (!o2) { ss += PSKY(x2, y2, z2); sb += PBL(x2, y2, z2); n++; }
        if (!o3 && !(o1 && o2)) { ss += PSKY(x3, y3, z3); sb += PBL(x3, y3, z3); n++; }
        q.sky[k] = (u8)((ss + n / 2) / n); q.blk[k] = (u8)((sb + n / 2) / n);
        q.shade[k] = (u8)(FACE_SHADE[d] * AO_MUL[ao[k]] * 255.f);
    }
    q.tile = (u16)tile; q.nrm = (u8)d; q.flags = flags;
    if (ao[0] + ao[2] < ao[1] + ao[3]) {   // поворот диагонали для ровного AO
        MQuad r = q;
        for (int k = 0; k < 4; k++) {
            int s = (k + 1) & 3;
            for (int j = 0; j < 3; j++) r.p[k][j] = q.p[s][j];
            r.u[k] = q.u[s]; r.v[k] = q.v[s]; r.sky[k] = q.sky[s]; r.blk[k] = q.blk[s]; r.shade[k] = q.shade[s];
        }
        q = r;
    }
    out.push_back(q);
}

// параллелепипед (дверь, кровать, факел)
static void emitBox(std::vector<MQuad>& out, int x, int y, int z, float x0, float y0, float z0, float x1, float y1, float z1,
                    const int tiles[6], int sky, int blk, u8 flags, u8 skipMask = 0) {
    float mn[3] = {x0, y0, z0}, mx[3] = {x1, y1, z1};
    for (int d = 0; d < 6; d++) {
        if (skipMask & (1 << d)) continue;
        if (tiles[d] < 0) continue;
        MQuad q;
        for (int k = 0; k < 4; k++) {
            const float* cc = FACE_C[d][k];
            float px = cc[0] > 0.5f ? mx[0] : mn[0], py = cc[1] > 0.5f ? mx[1] : mn[1], pz = cc[2] > 0.5f ? mx[2] : mn[2];
            q.p[k][0] = x + px; q.p[k][1] = y + py; q.p[k][2] = z + pz;
            float u, v; faceUV(d, px, py, pz, u, v);
            q.u[k] = (u8)clampi((int)lroundf(u * 16), 0, 16); q.v[k] = (u8)clampi((int)lroundf(v * 16), 0, 16);
            q.sky[k] = (u8)sky; q.blk[k] = (u8)blk; q.shade[k] = (u8)(FACE_SHADE[d] * 255.f);
        }
        q.tile = (u16)tiles[d]; q.nrm = (u8)d; q.flags = flags;
        out.push_back(q);
    }
}

static void emitCross(std::vector<MQuad>& out, int x, int y, int z, int tile, int sky, int blk) {
    const float P[2][4][3] = {{{0.15f, 1, 0.15f}, {0.85f, 1, 0.85f}, {0.85f, 0, 0.85f}, {0.15f, 0, 0.15f}},
                              {{0.85f, 1, 0.15f}, {0.15f, 1, 0.85f}, {0.15f, 0, 0.85f}, {0.85f, 0, 0.15f}}};
    const u8 U[4] = {0, 16, 16, 0}, V[4] = {0, 0, 16, 16};
    for (int s = 0; s < 2; s++) {
        MQuad q;
        for (int k = 0; k < 4; k++) {
            q.p[k][0] = x + P[s][k][0]; q.p[k][1] = y + P[s][k][1]; q.p[k][2] = z + P[s][k][2];
            q.u[k] = U[k]; q.v[k] = V[k]; q.sky[k] = (u8)sky; q.blk[k] = (u8)blk; q.shade[k] = 230;
        }
        q.tile = (u16)tile; q.nrm = 6; q.flags = 0;
        out.push_back(q);
    }
}

static float fluidHeight(u8 b, u8 m, u8 above) {
    if (above == b) return 1.0f;
    if ((m & 7) == 0 || (m & 8)) return 0.875f;
    return std::max(0.12f, (8 - (m & 7)) / 8.f * 0.875f);
}

static void buildSection(Chunk* c, int s) {
    Section& sec = c->sec[s];
    sec.q.clear();
    int y0 = s * 16;
    bool any = false;
    for (int y = y0; y < y0 + 16 && !any; y++) for (int i = 0; i < 256; i++) if (c->blk[(y << 8) | i]) { any = true; break; }
    if (!any) { sec.conn = ~0ULL; sec.lights.clear(); return; }
    std::vector<MQuad>& out = sec.q;
    sec.lights.clear();
    for (int y = y0; y < y0 + 16; y++) for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
        u8 b = PB(x, y, z);
        if (b == B_AIR) continue;
        if (b == B_TORCH || b == B_FURNACE_LIT) sec.lights.push_back((u16)(x | (z << 4) | ((y & 15) << 8)));
        const BlockDef& bd = BD[b];
        u8 m = PM(x, y, z);
        int sky = PSKY(x, y, z), bl = PBL(x, y, z);
        switch (bd.shape) {
            case SH_CUBE: {
                int frontFace = -1, frontTile = 0;
                if (b == B_FURNACE || b == B_FURNACE_LIT) { frontFace = hdirToFace(m); frontTile = b == B_FURNACE_LIT ? T_FURN_FRONT_LIT : T_FURN_FRONT; }
                if (b == B_CHEST) { frontFace = hdirToFace(m); frontTile = T_CHEST_FRONT; }
                for (int d = 0; d < 6; d++) {
                    u8 n = PB(x + DX[d], y + DY[d], z + DZ[d]);
                    if (y + DY[d] < 0) continue;
                    if (!faceVisible(b, n)) continue;
                    int t = d == frontFace ? frontTile : bd.tex[d];
                    u8 fl = (b == B_ICE) ? 1 : 0;
                    if (b == B_CACTUS && d != 2 && d != 3) {
                        // кактус немного тоньше блока
                        int tiles[6] = {-1, -1, -1, -1, -1, -1}; tiles[d] = t;
                        emitBox(out, x, y, z, 1 / 16.f, 0, 1 / 16.f, 15 / 16.f, 1, 15 / 16.f, tiles, sky, bl, 0);
                        continue;
                    }
                    emitCubeFace(out, x, y, z, d, t, fl);
                }
                break;
            }
            case SH_FLUID: {
                u8 above = PB(x, y + 1, z);
                float h = fluidHeight(b, m, above);
                u8 fl = b == B_WATER ? 1 : 6;
                int lsky = std::max(sky, PSKY(x, y + 1, z)), lbl = std::max(bl, PBL(x, y + 1, z));
                if (b == B_LAVA) { lsky = 15; lbl = 15; }
                for (int d = 0; d < 6; d++) {
                    u8 n = PB(x + DX[d], y + DY[d], z + DZ[d]);
                    if (n == b) continue;
                    if (BD[n].opaque) continue;
                    if (d == 3 && y == 0) continue;
                    if (d == 2 && b == B_WATER && n == B_ICE) continue;
                    MQuad q;
                    int ns = std::max(lsky, PSKY(x + DX[d], y + DY[d], z + DZ[d])), nb2 = std::max(lbl, PBL(x + DX[d], y + DY[d], z + DZ[d]));
                    for (int k = 0; k < 4; k++) {
                        const float* cc = FACE_C[d][k];
                        float py = cc[1] > 0.5f ? h : 0.f;
                        q.p[k][0] = x + cc[0]; q.p[k][1] = y + py; q.p[k][2] = z + cc[2];
                        float u, v; faceUV(d, cc[0], py, cc[2], u, v);
                        q.u[k] = (u8)clampi((int)(u * 16), 0, 16); q.v[k] = (u8)clampi((int)(v * 16), 0, 16);
                        q.sky[k] = (u8)ns; q.blk[k] = (u8)nb2; q.shade[k] = (u8)(FACE_SHADE[d] * 255.f);
                    }
                    q.tile = bd.tex[d]; q.nrm = (d == 2) ? 6 : (u8)d; q.flags = fl;
                    out.push_back(q);
                }
                break;
            }
            case SH_CROSS: emitCross(out, x, y, z, bd.tex[0], sky, bl); break;
            case SH_TORCH: {
                float cx = 0.5f, cz = 0.5f, yb = 0.f;
                if (m >= 1 && m <= 4) { int hd = m - 1; cx += HDX[hd] * 0.34f; cz += HDZ[hd] * 0.34f; yb = 0.2f; }
                const int tiles[6] = {T_TORCH, T_TORCH, T_TORCH, -1, T_TORCH, T_TORCH};
                size_t st = out.size();
                emitBox(out, x, y, z, cx - 1 / 16.f, yb, cz - 1 / 16.f, cx + 1 / 16.f, yb + 10 / 16.f, cz + 1 / 16.f, tiles, 15, 15, 2);
                for (size_t i = st; i < out.size(); i++) {   // подогнать текстуру под тонкую палочку
                    MQuad& q = out[i];
                    for (int k = 0; k < 4; k++) {
                        if (q.nrm == 2) { q.u[k] = q.u[k] < 8 ? 7 : 9; q.v[k] = q.v[k] < 8 ? 4 : 6; }
                        else { q.u[k] = q.u[k] < 8 ? 7 : 9; q.v[k] = q.v[k] < 12 ? 3 : 13; }
                    }
                }
                break;
            }
            case SH_LADDER: {
                int hd = m & 3;
                MQuad q;
                const float in = 1 / 16.f;
                for (int k = 0; k < 4; k++) {
                    float a = (k == 0 || k == 3) ? 0.f : 1.f, yy = (k < 2) ? 1.f : 0.f;
                    float px, pz;
                    switch (hd) {
                        case 0: px = 1 - in; pz = a; break;
                        case 2: px = in; pz = 1 - a; break;
                        case 1: px = 1 - a; pz = 1 - in; break;
                        default: px = a; pz = in; break;
                    }
                    q.p[k][0] = x + px; q.p[k][1] = y + yy; q.p[k][2] = z + pz;
                    q.u[k] = (u8)(a * 16); q.v[k] = (u8)((1 - yy) * 16);
                    q.sky[k] = (u8)sky; q.blk[k] = (u8)bl; q.shade[k] = 210;
                }
                q.tile = T_LADDER; q.nrm = 6; q.flags = 0;
                out.push_back(q);
                break;
            }
            case SH_DOOR: {
                int f = m & 3; bool open = (m & 4) != 0, upper = (m & 8) != 0;
                int side = open ? (f + 1) & 3 : (f + 2) & 3;
                float x0 = 0, x1 = 1, z0 = 0, z1 = 1, t = 3 / 16.f;
                if (side == 0) x0 = 1 - t; else if (side == 2) x1 = t; else if (side == 1) z0 = 1 - t; else z1 = t;
                int tl = upper ? T_DOOR_HI : T_DOOR_LO;
                const int tiles[6] = {tl, tl, T_PLANKS, T_PLANKS, tl, tl};
                emitBox(out, x, y, z, x0, 0, z0, x1, 1, z1, tiles, sky, bl, 0, upper ? 8 : 4);
                break;
            }
            case SH_BED: {
                bool head = (m & 4) != 0;
                int tiles[6] = {T_BED_SIDE, T_BED_SIDE, head ? T_BED_HEAD : T_BED_FOOT, T_PLANKS, T_BED_SIDE, T_BED_SIDE};
                if (head) for (int d = 0; d < 6; d++) if (d != 2 && d != 3) tiles[d] = T_BED_SIDE_HEAD;
                emitBox(out, x, y, z, 0, 0, 0, 1, 9 / 16.f, 1, tiles, std::max(sky, PSKY(x, y + 1, z)), std::max(bl, PBL(x, y + 1, z)), 0);
                break;
            }
            default: break;
        }
    }
    // связность граней секции (для отсечения пещер)
    static u8 vis[4096]; static short stk[4096];
    memset(vis, 0, sizeof(vis));
    u64 conn = 0;
    for (int i = 0; i < 4096; i++) {
        if (vis[i]) continue;
        int x = i & 15, z = (i >> 4) & 15, yy = i >> 8;
        if (BD[c->blk[cidx(x, y0 + yy, z)]].opaque) { vis[i] = 1; continue; }
        int sp = 0; stk[sp++] = (short)i; vis[i] = 1; int faces = 0;
        while (sp) {
            int j = stk[--sp];
            int jx = j & 15, jz = (j >> 4) & 15, jy = j >> 8;
            faces |= (jx == 15 ? 1 : 0) | (jx == 0 ? 2 : 0) | (jy == 15 ? 4 : 0) | (jy == 0 ? 8 : 0) | (jz == 15 ? 16 : 0) | (jz == 0 ? 32 : 0);
            for (int d = 0; d < 6; d++) {
                int nx = jx + DX[d], ny = jy + DY[d], nz = jz + DZ[d];
                if (nx < 0 || nx > 15 || ny < 0 || ny > 15 || nz < 0 || nz > 15) continue;
                int k = (ny << 8) | (nz << 4) | nx;
                if (vis[k]) continue;
                vis[k] = 1;
                if (BD[c->blk[cidx(nx, y0 + ny, nz)]].opaque) continue;
                stk[sp++] = (short)k;
            }
        }
        for (int a = 0; a < 6; a++) if (faces & (1 << a)) for (int b2 = 0; b2 < 6; b2++) if (faces & (1 << b2)) conn |= 1ULL << (a * 6 + b2);
    }
    sec.conn = conn;
}

static bool neighborsLoaded(int cx, int cz) {
    for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) if (!getChunk(cx + dx, cz + dz)) return false;
    return true;
}
static void buildChunkMesh(Chunk* c) {
    for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) g_nb[dx + 1][dz + 1] = getChunk(c->cx + dx, c->cz + dz);
    computeLight(c);
    for (int s = 0; s < SECN; s++) buildSection(c, s);
    c->meshed = true; c->dirty = false;
}

// ============================================================================
//  РЕНДЕР: проекция на CPU, отсечение ближней плоскостью, сортировка по глубине,
//  вывод одним вызовом SDL_RenderGeometry с атласом
// ============================================================================
static SDL_Window* g_win = nullptr;
static SDL_Renderer* g_ren = nullptr;
static SDL_Texture* g_atlasTex = nullptr;
static SDL_Texture* g_fontTex = nullptr;
static SDL_Texture* g_glowTex = nullptr;   // мягкое пятно света для ореолов (аддитивно)
static int g_W = 1280, g_H = 720;

struct Camera {
    double x = 0, y = 70, z = 0;
    float yaw = 0, pitch = 0;
    float r[3], u[3], f[3];
    float focal = 600, cx = 640, cy = 360, fovY = 70.f, nearZ = 0.06f;
    float tanX = 1, tanY = 1;
};
static Camera g_cam;

static void camUpdate() {
    Camera& c = g_cam;
    float cp = cosf(c.pitch), sp = sinf(c.pitch), cyw = cosf(c.yaw), syw = sinf(c.yaw);
    c.f[0] = cp * syw; c.f[1] = sp; c.f[2] = cp * cyw;
    c.r[0] = cyw; c.r[1] = 0; c.r[2] = -syw;
    // up = f x r
    c.u[0] = c.f[1] * c.r[2] - c.f[2] * c.r[1];
    c.u[1] = c.f[2] * c.r[0] - c.f[0] * c.r[2];
    c.u[2] = c.f[0] * c.r[1] - c.f[1] * c.r[0];
    c.cx = g_W * 0.5f; c.cy = g_H * 0.5f;
    c.focal = (g_H * 0.5f) / tanf(c.fovY * 0.5f * (float)M_PI / 180.f);
    c.tanX = c.cx / c.focal; c.tanY = c.cy / c.focal;
}
static inline void toCam(float dx, float dy, float dz, float* o) {
    const Camera& c = g_cam;
    o[0] = dx * c.r[0] + dy * c.r[1] + dz * c.r[2];
    o[1] = dx * c.u[0] + dy * c.u[1] + dz * c.u[2];
    o[2] = dx * c.f[0] + dy * c.f[1] + dz * c.f[2];
}

// ---------------------------------------------------------------- буферы кадра
struct Poly { float key; int v0; int n; };
static std::vector<SDL_Vertex> g_vb;
static std::vector<Poly> g_polys;
static std::vector<int> g_ib;
static std::vector<Poly> g_polysTmp;

static inline SDL_Color mkColor(float r, float g, float b, float a) {
    SDL_Color c;
    c.r = (Uint8)clampi((int)(r * 255.f), 0, 255); c.g = (Uint8)clampi((int)(g * 255.f), 0, 255);
    c.b = (Uint8)clampi((int)(b * 255.f), 0, 255); c.a = (Uint8)clampi((int)(a * 255.f), 0, 255);
    return c;
}

// многоугольник в пространстве камеры: P[n][3], UV[n][2] (норм.), C[n][4]
static void emitPolyCam(int n, const float (*P)[3], const float (*UV)[2], const float (*C)[4], float key) {
    float cp[8][3], cuv[8][2], cc[8][4];
    int m = 0;
    const float nz = g_cam.nearZ;
    bool allIn = true, allOut = true;
    for (int i = 0; i < n; i++) { if (P[i][2] < nz) allIn = false; else allOut = false; }
    if (allOut) return;
    if (allIn) {
        for (int i = 0; i < n; i++) { memcpy(cp[i], P[i], 12); memcpy(cuv[i], UV[i], 8); memcpy(cc[i], C[i], 16); }
        m = n;
    } else {
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            bool ii = P[i][2] >= nz, jj = P[j][2] >= nz;
            if (ii) { memcpy(cp[m], P[i], 12); memcpy(cuv[m], UV[i], 8); memcpy(cc[m], C[i], 16); m++; }
            if (ii != jj && m < 8) {
                float t = (nz - P[i][2]) / (P[j][2] - P[i][2]);
                for (int k = 0; k < 3; k++) cp[m][k] = P[i][k] + (P[j][k] - P[i][k]) * t;
                for (int k = 0; k < 2; k++) cuv[m][k] = UV[i][k] + (UV[j][k] - UV[i][k]) * t;
                for (int k = 0; k < 4; k++) cc[m][k] = C[i][k] + (C[j][k] - C[i][k]) * t;
                m++;
            }
        }
        if (m < 3) return;
    }
    const Camera& c = g_cam;
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    int base = (int)g_vb.size();
    for (int i = 0; i < m; i++) {
        float iz = 1.f / cp[i][2];
        SDL_Vertex v;
        v.position.x = c.cx + cp[i][0] * iz * c.focal;
        v.position.y = c.cy - cp[i][1] * iz * c.focal;
        v.tex_coord.x = cuv[i][0]; v.tex_coord.y = cuv[i][1];
        v.color = mkColor(cc[i][0], cc[i][1], cc[i][2], cc[i][3]);
        minx = std::min(minx, v.position.x); maxx = std::max(maxx, v.position.x);
        miny = std::min(miny, v.position.y); maxy = std::max(maxy, v.position.y);
        g_vb.push_back(v);
    }
    if (maxx < 0 || minx > g_W || maxy < 0 || miny > g_H) { g_vb.resize(base); return; }
    Poly p; p.key = key; p.v0 = base; p.n = m;
    g_polys.push_back(p);
}

// четырёхугольник в пространстве камеры с разбиением вблизи (борьба с аффинными искажениями)
static void emitQuadCam(const float (*P)[3], const float (*UV)[2], const float (*C)[4], float keyBias = 0.f) {
    float minz = std::min(std::min(P[0][2], P[1][2]), std::min(P[2][2], P[3][2]));
    float maxz = std::max(std::max(P[0][2], P[1][2]), std::max(P[2][2], P[3][2]));
    if (maxz < g_cam.nearZ) return;
    int N = 1;
    if (minz < 3.0f) {
        float e1 = fabsf(P[0][0] - P[2][0]) + fabsf(P[0][1] - P[2][1]) + fabsf(P[0][2] - P[2][2]);
        if (e1 > 0.4f) N = minz < 1.2f ? 4 : 2;
    }
    if (N == 1) {
        float mx = (P[0][0] + P[1][0] + P[2][0] + P[3][0]) * 0.25f, my = (P[0][1] + P[1][1] + P[2][1] + P[3][1]) * 0.25f,
              mz = (P[0][2] + P[1][2] + P[2][2] + P[3][2]) * 0.25f;
        emitPolyCam(4, P, UV, C, mx * mx + my * my + mz * mz + keyBias);
        return;
    }
    float inv = 1.f / N;
    for (int j = 0; j < N; j++) for (int i = 0; i < N; i++) {
        float sp[4][3], suv[4][2], sc[4][4];
        const float st[4][2] = {{(float)i, (float)j}, {(float)(i + 1), (float)j}, {(float)(i + 1), (float)(j + 1)}, {(float)i, (float)(j + 1)}};
        for (int k = 0; k < 4; k++) {
            float s = st[k][0] * inv, t = st[k][1] * inv;
            float w0 = (1 - s) * (1 - t), w1 = s * (1 - t), w2 = s * t, w3 = (1 - s) * t;
            for (int a = 0; a < 3; a++) sp[k][a] = P[0][a] * w0 + P[1][a] * w1 + P[2][a] * w2 + P[3][a] * w3;
            for (int a = 0; a < 2; a++) suv[k][a] = UV[0][a] * w0 + UV[1][a] * w1 + UV[2][a] * w2 + UV[3][a] * w3;
            for (int a = 0; a < 4; a++) sc[k][a] = C[0][a] * w0 + C[1][a] * w1 + C[2][a] * w2 + C[3][a] * w3;
        }
        float mx = (sp[0][0] + sp[2][0]) * 0.5f, my = (sp[0][1] + sp[2][1]) * 0.5f, mz = (sp[0][2] + sp[2][2]) * 0.5f;
        emitPolyCam(4, sp, suv, sc, mx * mx + my * my + mz * mz + keyBias);
    }
}
// четырёхугольник в мировых координатах относительно камеры
static void emitQuadRel(const float (*W)[3], const float (*UV)[2], const float (*C)[4], float keyBias = 0.f) {
    float P[4][3];
    for (int k = 0; k < 4; k++) toCam(W[k][0], W[k][1], W[k][2], P[k]);
    emitQuadCam(P, UV, C, keyBias);
}
static inline void tileUV(int tile, float pu, float pv, float* o) {
    pu = clampf(pu, 0.03f, 15.97f); pv = clampf(pv, 0.03f, 15.97f);
    o[0] = ((tile & 15) * 16 + pu) / (float)ATLAS;
    o[1] = ((tile >> 4) * 16 + pv) / (float)ATLAS;
}

// сортировка многоугольников (дальние раньше) поразрядной сортировкой
static void flushPolys(SDL_Texture* tex) {
    size_t n = g_polys.size();
    if (n == 0) { g_vb.clear(); return; }
    g_polysTmp.resize(n);
    Poly* a = g_polys.data(); Poly* b = g_polysTmp.data();
    for (int pass = 0; pass < 4; pass++) {
        int sh = pass * 8; size_t cnt[256]; memset(cnt, 0, sizeof(cnt));
        for (size_t i = 0; i < n; i++) { u32 k; memcpy(&k, &a[i].key, 4); k = ~k; cnt[(k >> sh) & 255]++; }
        size_t sum = 0; for (int i = 0; i < 256; i++) { size_t t = cnt[i]; cnt[i] = sum; sum += t; }
        for (size_t i = 0; i < n; i++) { u32 k; memcpy(&k, &a[i].key, 4); k = ~k; b[cnt[(k >> sh) & 255]++] = a[i]; }
        std::swap(a, b);
    }
    g_ib.clear();
    for (size_t i = 0; i < n; i++) {
        const Poly& p = a[i];
        for (int k = 1; k + 1 < p.n; k++) { g_ib.push_back(p.v0); g_ib.push_back(p.v0 + k); g_ib.push_back(p.v0 + k + 1); }
    }
    SDL_RenderGeometry(g_ren, tex, g_vb.data(), (int)g_vb.size(), g_ib.data(), (int)g_ib.size());
    g_vb.clear(); g_polys.clear();
}

// ---------------------------------------------------------------- освещение
static float g_lightLUT[256];
static float g_blockLUT[16];   // свет факелов и лавы: мягче, чем небесный
static float g_dayLight = 1.f;          // 0.15..1
static float g_skyCol[3] = {0.5f, 0.7f, 1.f}, g_zenCol[3] = {0.3f, 0.5f, 0.9f};
static float g_fogStart = 60, g_fogEnd = 80;

// ---------------------------------------------------------------- «шейдеры»: состояние эффектов
// 0 — выключены, 1 — лёгкие (цветной свет, свет по направлению солнца, колышущаяся листва и трава,
// волны и блики на воде, мерцание факелов), 2 — полные (+ ореолы факелов, сияние солнца, лучи и блики в объективе, виньетка)
static int g_shaders = 2;
static float g_fxTime = 0;
struct FXState {
    float sun[3], moon[3], light[3];   // направления
    float sunCol[3];                   // цвет небесного света (тёплый на закате, голубой ночью)
    float faceMul[6];                  // яркость граней по направлению к светилу
    float wind;                        // сила ветра
    float sunUp;                       // 0..1 — солнце над горизонтом
    float sunset;                      // 0..1 — рассвет/закат
};
static FXState g_fx;
static void computeFX(float dayTime, float t) {
    float ang = dayTime * 2.f * (float)M_PI, sh = sinf(ang), sc = cosf(ang);
    float l = sqrtf(sc * sc + sh * sh + 0.22f * 0.22f);
    g_fx.sun[0] = sc / l; g_fx.sun[1] = sh / l; g_fx.sun[2] = 0.22f / l;
    for (int i = 0; i < 3; i++) g_fx.moon[i] = -g_fx.sun[i];
    g_fx.moon[2] = g_fx.sun[2];
    bool day = sh > -0.05f;
    for (int i = 0; i < 3; i++) g_fx.light[i] = day ? g_fx.sun[i] : g_fx.moon[i];
    g_fx.sunUp = smooth01(-0.12f, 0.08f, sh);
    g_fx.sunset = clampf(1.f - fabsf(sh) * 3.2f, 0, 1);
    const float noon[3] = {1.00f, 0.97f, 0.90f}, dusk[3] = {1.00f, 0.66f, 0.42f}, night[3] = {0.58f, 0.70f, 1.00f};
    float k = smooth01(0.0f, 0.45f, sh);
    for (int i = 0; i < 3; i++) g_fx.sunCol[i] = lerpf(night[i], lerpf(dusk[i], noon[i], k), g_fx.sunUp);
    float strength = day ? smooth01(-0.05f, 0.25f, sh) : 0.45f * smooth01(0.05f, 0.3f, -sh);
    static const float N6[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int d = 0; d < 6; d++) {
        float lam = std::max(0.f, N6[d][0] * g_fx.light[0] + N6[d][1] * g_fx.light[1] + N6[d][2] * g_fx.light[2]);
        g_fx.faceMul[d] = 1.f + strength * (0.30f * lam - 0.10f);
    }
    g_fx.wind = 0.65f + 0.35f * sinf(t * 0.31f) * sinf(t * 0.17f + 1.f);
}
static void initLightLUT() {
    for (int i = 0; i < 256; i++) {
        float l = i / 255.f;
        g_lightLUT[i] = 0.055f + 0.945f * powf(0.80f, (1.f - l) * 15.f);
    }
    for (int i = 0; i < 16; i++) g_blockLUT[i] = i == 0 ? 0.f : 0.05f + 0.95f * powf(0.855f, (float)(15 - i));
}
static inline float lightCurve(float l) { return g_lightLUT[clampi((int)(l * 255.f), 0, 255)]; }

// dirMul — яркость грани по направлению к солнцу, flick — мерцание факелов
static inline void litColorFX(float sky15, float blk15, float shade, float dirMul, float flick, float* out) {
    float s = lightCurve(sky15 / 15.f) * g_dayLight;   // дневной множитель: ночью всё темнеет, но видно
    float b = g_blockLUT[clampi((int)(blk15 + 0.5f), 0, 15)] * flick;
    float m = lightCurve(0);                          // минимальная яркость в глубине пещер
    float sr, sg, sb;
    if (g_shaders > 0) { float sd = s * dirMul; sr = sd * g_fx.sunCol[0]; sg = sd * g_fx.sunCol[1]; sb = sd * g_fx.sunCol[2]; }
    else { sr = s * (0.72f + 0.28f * g_dayLight); sg = s * (0.80f + 0.20f * g_dayLight); sb = s; }   // ночью — холодный оттенок
    out[0] = std::max(std::max(sr, b * 1.00f), m * 0.9f) * shade;
    out[1] = std::max(std::max(sg, b * 0.90f), m * 0.9f) * shade;
    out[2] = std::max(std::max(sb, b * 0.74f), m) * shade;
}
static inline void litColor(float sky15, float blk15, float shade, float* out) { litColorFX(sky15, blk15, shade, 1.f, 1.f, out); }
static inline float fogAlpha(float d2) {
    if (d2 < g_fogStart * g_fogStart) return 1.f;
    float d = sqrtf(d2);
    return 1.f - smooth01(g_fogStart, g_fogEnd, d);
}

// ---------------------------------------------------------------- видимость секций (отсечение пещер)
static bool sphereVisible(float dx, float dy, float dz, float rad) {
    float p[3]; toCam(dx, dy, dz, p);
    const Camera& c = g_cam;
    if (p[2] < -rad) return false;
    float kx = sqrtf(1 + c.tanX * c.tanX), ky = sqrtf(1 + c.tanY * c.tanY);
    if (p[0] - p[2] * c.tanX > rad * kx) return false;
    if (-p[0] - p[2] * c.tanX > rad * kx) return false;
    if (p[1] - p[2] * c.tanY > rad * ky) return false;
    if (-p[1] - p[2] * c.tanY > rad * ky) return false;
    return true;
}
struct VisSec { Chunk* c; int s; };
static std::vector<VisSec> g_visible;
static int g_frameNo = 0;
static int g_renderDist = 6;

static void findVisibleSections() {
    g_visible.clear();
    g_frameNo++;
    int ccx = ifloor(g_cam.x) >> 4, ccz = ifloor(g_cam.z) >> 4;
    int csy = clampi(ifloor(g_cam.y) >> 4, 0, SECN - 1);
    struct QE { int cx, sy, cz; int from; int mask; };
    static std::vector<QE> q; q.clear();
    Chunk* c0 = getChunk(ccx, ccz);
    if (!c0) return;
    c0->sec[csy].vis = g_frameNo;
    QE s0; s0.cx = ccx; s0.sy = csy; s0.cz = ccz; s0.from = -1; s0.mask = 0;
    q.push_back(s0);
    size_t head = 0;
    while (head < q.size()) {
        QE e = q[head++];
        Chunk* c = getChunk(e.cx, e.cz);
        if (!c) continue;
        if (c->meshed && !c->sec[e.sy].q.empty()) { VisSec v; v.c = c; v.s = e.sy; g_visible.push_back(v); }
        u64 conn = c->meshed ? c->sec[e.sy].conn : ~0ULL;
        for (int d = 0; d < 6; d++) {
            if (e.mask & (1 << (d ^ 1))) continue;
            if (e.from >= 0 && !(conn & (1ULL << (e.from * 6 + d)))) continue;
            int nx = e.cx + DX[d], ny = e.sy + DY[d], nz = e.cz + DZ[d];
            if (ny < 0 || ny >= SECN) continue;
            if (abs(nx - ccx) > g_renderDist || abs(nz - ccz) > g_renderDist) continue;
            int ddx = nx - ccx, ddz = nz - ccz;
            if (ddx * ddx + ddz * ddz > (g_renderDist + 1) * (g_renderDist + 1)) continue;
            Chunk* n = getChunk(nx, nz);
            if (!n) continue;
            if (n->sec[ny].vis == g_frameNo) continue;
            float scx = (float)(nx * 16 + 8 - g_cam.x), scy = (float)(ny * 16 + 8 - g_cam.y), scz = (float)(nz * 16 + 8 - g_cam.z);
            if (!sphereVisible(scx, scy, scz, 14.f)) continue;
            n->sec[ny].vis = g_frameNo;
            QE ne; ne.cx = nx; ne.sy = ny; ne.cz = nz; ne.from = d ^ 1; ne.mask = e.mask | (1 << d);
            q.push_back(ne);
        }
    }
}

static const float NRM[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
static int g_quadsDrawn = 0;

static inline bool isPlantTile(int t) { return t == T_TALLGRASS || t == T_FLOWER_R || t == T_FLOWER_Y || t == T_SAPLING; }
static void drawSections() {
    g_quadsDrawn = 0;
    const bool fx = g_shaders > 0;
    const float t = g_fxTime, wind = g_fx.wind;
    const Camera& cm = g_cam;
    for (const VisSec& vs : g_visible) {
        Chunk* c = vs.c;
        const float wx0 = (float)(c->cx * 16), wz0 = (float)(c->cz * 16);
        float ox = (float)(c->cx * 16 - g_cam.x), oy = (float)(-g_cam.y), oz = (float)(c->cz * 16 - g_cam.z);
        // базис камеры, сдвинутый в начало чанка
        float o[3]; toCam(ox, oy, oz, o);
        for (const MQuad& q : c->sec[vs.s].q) {
            if (q.nrm < 6) {
                const float* nn = NRM[q.nrm];
                float d = (q.p[0][0] + ox) * nn[0] + (q.p[0][1] + oy) * nn[1] + (q.p[0][2] + oz) * nn[2];
                if (d >= 0) continue;
            }
            // вид эффекта: 1 — растение, 2 — листва, 3 — гладь воды
            int kind = 0;
            float L[4][3];
            memcpy(L, q.p, sizeof(L));
            if (fx) {
                if (q.nrm == 6 && isPlantTile(q.tile)) kind = 1;
                else if (q.tile == T_LEAVES || q.tile == T_PINE_LEAVES) kind = 2;
                else if (q.tile == T_WATER && q.p[0][1] == q.p[1][1] && q.p[1][1] == q.p[2][1] && q.p[2][1] == q.p[3][1]) kind = 3;
                for (int k = 0; k < 4 && kind; k++) {
                    float wx = wx0 + L[k][0], wy = L[k][1], wz = wz0 + L[k][2];
                    if (kind == 1) {
                        if (q.v[k] != 0) continue;   // качаются только верхушки
                        float ph = t * 1.9f + wx * 0.8f + wz * 0.6f;
                        L[k][0] += sinf(ph) * 0.075f * wind; L[k][2] += cosf(ph * 0.8f + 1.3f) * 0.055f * wind;
                    } else if (kind == 2) {
                        float ph = t * 1.4f + wx * 0.9f + wy * 0.7f + wz * 0.8f;
                        L[k][0] += sinf(ph) * 0.032f * wind; L[k][1] += cosf(ph * 1.3f) * 0.02f * wind; L[k][2] += sinf(ph * 0.9f + 2.f) * 0.03f * wind;
                    } else {
                        float w = sinf(t * 1.7f + wx * 0.9f + wz * 0.5f) * 0.5f + sinf(t * 2.6f - wx * 0.4f + wz * 1.1f) * 0.5f;
                        L[k][1] += (w - 1.f) * 0.035f;   // волны только вниз, чтобы не отрываться от берега
                    }
                }
            }
            float P[4][3], UV[4][2], C[4][4];
            float ccx = 0, ccy = 0, ccz = 0;
            for (int k = 0; k < 4; k++) {
                const float* lp = L[k];
                P[k][0] = o[0] + lp[0] * cm.r[0] + lp[1] * cm.r[1] + lp[2] * cm.r[2];
                P[k][1] = o[1] + lp[0] * cm.u[0] + lp[1] * cm.u[1] + lp[2] * cm.u[2];
                P[k][2] = o[2] + lp[0] * cm.f[0] + lp[1] * cm.f[1] + lp[2] * cm.f[2];
                ccx += P[k][0]; ccy += P[k][1]; ccz += P[k][2];
            }
            ccx *= 0.25f; ccy *= 0.25f; ccz *= 0.25f;
            float d2 = ccx * ccx + ccy * ccy + ccz * ccz;
            float fa = fogAlpha(d2);
            if (fa <= 0.01f) continue;
            if (kind == 3) {
                // вода: отражение неба по Френелю и солнечный блик
                for (int k = 0; k < 4; k++) {
                    tileUV(T_WATER_FX, q.u[k], q.v[k], UV[k]);
                    float vx = L[k][0] + ox, vy = L[k][1] + oy, vz = L[k][2] + oz;
                    float vl = sqrtf(vx * vx + vy * vy + vz * vz) + 1e-4f; vx /= vl; vy /= vl; vz /= vl;
                    float cosv = fabsf(vy);
                    float fres = 0.12f + 0.88f * powf(1.f - cosv, 4.f);
                    float base[3]; litColor(q.sky[k], q.blk[k], 1.f, base);
                    const float deep[3] = {0.13f, 0.32f, 0.66f};
                    float spec = 0;
                    if (vy < 0) {   // смотрим на воду сверху: отражённый луч
                        float rx = vx, ry = -vy, rz = vz;
                        float dp = rx * g_fx.sun[0] + ry * g_fx.sun[1] + rz * g_fx.sun[2];
                        if (dp > 0.9f) spec = powf(dp, 90.f) * g_fx.sunUp * (q.sky[k] / 15.f) * 1.4f;
                    }
                    for (int i = 0; i < 3; i++) {
                        float col = deep[i] * base[i] * 1.25f;
                        col = lerpf(col, g_skyCol[i] * 0.95f, fres * 0.75f * (q.sky[k] / 15.f));
                        C[k][i] = std::min(1.f, col + spec * g_fx.sunCol[i]);
                    }
                    C[k][3] = fa * clampf(0.58f + fres * 0.38f + spec * 0.3f, 0, 0.97f);
                }
            } else {
                float alpha = fa * ((q.flags & 1) ? (q.tile == T_WATER ? 0.72f : 0.85f) : 1.f);
                float dirMul = (fx && q.nrm < 6) ? g_fx.faceMul[q.nrm] : 1.f;
                float flick = 1.f;
                if (fx && (q.blk[0] | q.blk[1] | q.blk[2] | q.blk[3]) > 2)
                    flick = 0.95f + 0.05f * sinf(t * 7.3f + (wx0 + q.p[0][0]) * 1.7f + (wz0 + q.p[0][2]) * 2.3f) * sinf(t * 3.1f + q.p[0][1]);
                for (int k = 0; k < 4; k++) {
                    tileUV(q.tile, q.u[k], q.v[k], UV[k]);
                    float sh = q.shade[k] / 255.f;
                    if (q.flags & 2) { C[k][0] = C[k][1] = C[k][2] = (q.flags & 4) ? 1.f : sh * 1.05f; }
                    else litColorFX(q.sky[k], q.blk[k], sh, dirMul, flick, C[k]);
                    C[k][3] = alpha;
                }
            }
            emitQuadCam(P, UV, C);
            g_quadsDrawn++;
        }
    }
}

// ---------------------------------------------------------------- небо
static float g_dayTime = 0.05f;       // 0..1 (0 — рассвет, 0.25 — полдень, 0.5 — закат, 0.75 — полночь)
static const float DAY_LEN = 720.f;   // секунд на сутки
struct Star { float x, y, z, b; };
static std::vector<Star> g_stars;
static void initStars() {
    Rng r(4242);
    for (int i = 0; i < 300; i++) {
        float x, y, z, l;
        do { x = r.sf(); y = r.sf(); z = r.sf(); l = x * x + y * y + z * z; } while (l > 1 || l < 0.05f);
        l = sqrtf(l); Star s; s.x = x / l; s.y = y / l; s.z = z / l; s.b = 0.4f + r.f() * 0.6f; g_stars.push_back(s);
    }
}
static float sunHeight() { return sinf(g_dayTime * 2.f * (float)M_PI); }
static bool isNight() { return sunHeight() < -0.08f; }

static void updateSkyColors(bool underwater) {
    float sh = sunHeight();
    float dl = smooth01(-0.18f, 0.28f, sh);
    g_dayLight = 0.26f + 0.74f * dl;
    float day[3] = {0.53f, 0.74f, 0.98f}, night[3] = {0.03f, 0.04f, 0.09f};
    float zday[3] = {0.30f, 0.50f, 0.92f}, znight[3] = {0.01f, 0.01f, 0.04f};
    for (int i = 0; i < 3; i++) { g_skyCol[i] = lerpf(night[i], day[i], dl); g_zenCol[i] = lerpf(znight[i], zday[i], dl); }
    float sunset = clampf(1.f - fabsf(sh) * 4.f, 0, 1) * 0.65f;
    g_skyCol[0] = lerpf(g_skyCol[0], 0.98f, sunset); g_skyCol[1] = lerpf(g_skyCol[1], 0.55f, sunset); g_skyCol[2] = lerpf(g_skyCol[2], 0.30f, sunset);
    if (underwater) { g_skyCol[0] = 0.05f; g_skyCol[1] = 0.18f; g_skyCol[2] = 0.45f * g_dayLight + 0.05f; for (int i = 0; i < 3; i++) g_zenCol[i] = g_skyCol[i]; }
}

static void drawSkyGlow();
static void drawSky() {
    const Camera& c = g_cam;
    // градиент по углу возвышения
    const int BANDS = 12;
    SDL_Vertex v[(BANDS + 1) * 2]; int idx[BANDS * 6];
    for (int i = 0; i <= BANDS; i++) {
        float y = g_H * i / (float)BANDS;
        float elev = atanf((c.cy - y) / c.focal) + c.pitch;
        float t = clampf(elev / 0.9f, 0, 1); t = sqrtf(t);
        float col[3];
        for (int k = 0; k < 3; k++) col[k] = elev < 0 ? g_skyCol[k] * (1.f - clampf(-elev, 0, 1) * 0.35f) : lerpf(g_skyCol[k], g_zenCol[k], t);
        for (int s = 0; s < 2; s++) {
            SDL_Vertex& vv = v[i * 2 + s];
            vv.position.x = s ? (float)g_W : 0.f; vv.position.y = y;
            vv.color = mkColor(col[0], col[1], col[2], 1); vv.tex_coord.x = vv.tex_coord.y = 0;
        }
    }
    for (int i = 0; i < BANDS; i++) {
        int a = i * 2; int* p = idx + i * 6;
        p[0] = a; p[1] = a + 1; p[2] = a + 2; p[3] = a + 1; p[4] = a + 3; p[5] = a + 2;
    }
    SDL_RenderGeometry(g_ren, NULL, v, (BANDS + 1) * 2, idx, BANDS * 6);

    float ang = g_dayTime * 2.f * (float)M_PI;
    float sh = sinf(ang), sc = cosf(ang);
    // звёзды
    float starA = clampf(-sh * 3.f, 0, 1);
    if (starA > 0.02f) {
        SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
        for (const Star& s : g_stars) {
            // вращение вокруг оси Z вместе с небом
            float x = s.x * sc - s.y * sh, y = s.x * sh + s.y * sc, z = s.z;
            float p[3]; toCam(x, y, z, p);
            if (p[2] < 0.05f) continue;
            float sx = c.cx + p[0] / p[2] * c.focal, sy = c.cy - p[1] / p[2] * c.focal;
            if (sx < 0 || sy < 0 || sx > g_W || sy > g_H) continue;
            SDL_SetRenderDrawColor(g_ren, 255, 255, 240, (Uint8)(255 * starA * s.b));
            int sz = g_H > 800 ? 3 : 2;
            SDL_Rect rr = {(int)sx, (int)sy, sz, sz};
            SDL_RenderFillRect(g_ren, &rr);
        }
    }
    drawSkyGlow();
    // солнце и луна
    for (int body = 0; body < 2; body++) {
        float dx = body ? -sc : sc, dy = body ? -sh : sh, dz = 0.22f;
        float l = sqrtf(dx * dx + dy * dy + dz * dz); dx /= l; dy /= l; dz /= l;
        float p[3]; toCam(dx, dy, dz, p);
        if (p[2] < 0.05f) continue;
        float sx = c.cx + p[0] / p[2] * c.focal, sy = c.cy - p[1] / p[2] * c.focal;
        float size = c.focal * (body ? 0.10f : 0.16f) / p[2];
        int tile = body ? T_MOON : T_SUN;
        SDL_Rect src = {(tile & 15) * 16, (tile >> 4) * 16, 16, 16};
        SDL_Rect dst = {(int)(sx - size / 2), (int)(sy - size / 2), (int)size, (int)size};
        SDL_SetTextureColorMod(g_atlasTex, 255, 255, 255);
        SDL_SetTextureAlphaMod(g_atlasTex, 255);
        SDL_RenderCopy(g_ren, g_atlasTex, &src, &dst);
    }
}

// ---------------------------------------------------------------- «шейдеры»: анимированные тайлы, свечение, постобработка
static u32 g_animWater[16][256], g_animWaterFX[16][256], g_animLava[16][256];
static int g_animFrameW = -1, g_animFrameL = -1;
static void initAnimTiles() {
    TN n1 = makeNoise(26, {{8, 0.5f}, {4, 0.5f}}), n2 = makeNoise(61, {{4, 0.6f}, {2, 0.4f}});
    TN l1 = makeNoise(27, {{8, 0.4f}, {4, 0.4f}, {2, 0.2f}}), l2 = makeNoise(62, {{4, 0.6f}, {2, 0.4f}});
    Pal wp = mkPal({0x2a54c4, 0x3061d0, 0x386ddb, 0x437be4, 0x5089ea});
    Pal fp = mkPal({0xaab5c6, 0xbac4d4, 0xcad3e0, 0xdbe2eb, 0xeef2f7});   // светлая основа — цвет воде задаёт свет
    Pal lp = mkPal({0x8f1f08, 0xb8300b, 0xdb4d0f, 0xf07a17, 0xfba52a, 0xffd04a, 0xfff0a0});
    for (int f = 0; f < 16; f++) {
        float ph = f / 16.f * 2.f * (float)M_PI;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
            float w = sinf((x + n1.at(x, y) * 6.f) * 0.785f + y * 1.571f - ph);
            float v = 0.18f + n1.at(x, y) * 0.35f + n2.at(x + f, y) * 0.3f + (w > 0.82f ? 0.28f : 0.f);
            g_animWater[f][y * 16 + x] = rampPick(wp, v, x, y, 0.5f);
            g_animWaterFX[f][y * 16 + x] = rampPick(fp, v, x, y, 0.5f);
            float lv = l1.at(x + f, y + f) * 0.6f + l2.at(x - f, y) * 0.4f;
            g_animLava[f][y * 16 + x] = rampPick(lp, lv * 1.08f - 0.04f, x, y, 0.5f);
        }
    }
}
static void uploadTile(int tile, const u32* px) {
    int tx = (tile & 15) * 16, ty = (tile >> 4) * 16;
    for (int y = 0; y < 16; y++) memcpy(&g_atlasPx[(ty + y) * ATLAS + tx], px + y * 16, 64);
    if (g_atlasTex) { SDL_Rect r = {tx, ty, 16, 16}; SDL_UpdateTexture(g_atlasTex, &r, px, 64); }
}
static void updateAnimTiles(float t) {
    int fw = g_shaders ? ((int)(t * 7.f)) & 15 : 0, fl = g_shaders ? ((int)(t * 4.f)) & 15 : 0;
    if (fw != g_animFrameW) { uploadTile(T_WATER, g_animWater[fw]); uploadTile(T_WATER_FX, g_animWaterFX[fw]); g_animFrameW = fw; }
    if (fl != g_animFrameL) { uploadTile(T_LAVA, g_animLava[fl]); g_animFrameL = fl; }
}
static void createGlowTexture() {
    if (g_glowTex) SDL_DestroyTexture(g_glowTex);
    static u32 px[64 * 64];
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        float dx = (x + 0.5f - 32.f) / 32.f, dy = (y + 0.5f - 32.f) / 32.f;
        float d = sqrtf(dx * dx + dy * dy);
        float a = d >= 1.f ? 0.f : powf(1.f - d, 2.2f);
        px[y * 64 + x] = RGBA(255, 255, 255, (int)(a * 255.f));
    }
    g_glowTex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, 64, 64);
    if (!g_glowTex) return;
    SDL_UpdateTexture(g_glowTex, NULL, px, 64 * 4);
    SDL_SetTextureBlendMode(g_glowTex, SDL_BLENDMODE_ADD);
}
static void drawGlow(float x, float y, float r, const float* col, float a) {
    if (!g_glowTex || a <= 0.004f || r < 1.f) return;
    SDL_SetTextureColorMod(g_glowTex, (Uint8)(clampf(col[0], 0, 1) * 255), (Uint8)(clampf(col[1], 0, 1) * 255), (Uint8)(clampf(col[2], 0, 1) * 255));
    SDL_SetTextureAlphaMod(g_glowTex, (Uint8)(clampf(a, 0, 1) * 255));
    SDL_Rect d = {(int)(x - r), (int)(y - r), (int)(2 * r), (int)(2 * r)};
    SDL_RenderCopy(g_ren, g_glowTex, NULL, &d);
}
// направление → точка на экране
static bool projectDir(const float* dir, float& sx, float& sy) {
    float p[3]; toCam(dir[0], dir[1], dir[2], p);
    if (p[2] < 0.05f) return false;
    sx = g_cam.cx + p[0] / p[2] * g_cam.focal; sy = g_cam.cy - p[1] / p[2] * g_cam.focal;
    return true;
}
// сколько света проходит по лучу (0 — перекрыт, листва и стекло пропускают частично)
static float rayTransmit(double ox, double oy, double oz, float dx, float dy, float dz, float maxD) {
    float tr = 1.f; int lx = 1 << 30, ly = 0, lz = 0;
    for (float s = 0.3f; s < maxD; s += 0.4f) {
        int x = ifloor(ox + dx * s), y = ifloor(oy + dy * s), z = ifloor(oz + dz * s);
        if (x == lx && y == ly && z == lz) continue;
        lx = x; ly = y; lz = z;
        if (y >= WH || y < 0 || !isLoaded(x, z)) break;
        u8 b = getBlock(x, y, z);
        if (b == B_AIR) continue;
        if (BD[b].opaque) return 0.f;
        if (b == B_LEAVES || b == B_PINE_LEAVES) tr *= 0.55f;
        else if (BD[b].solid || b == B_WATER) tr *= 0.85f;
        if (tr < 0.03f) return 0.f;
    }
    return tr;
}
// сияние у солнца и луны, зарево на горизонте (рисуется за ландшафтом)
static void drawSkyGlow() {
    if (g_shaders < 1) return;
    float sx, sy;
    if (projectDir(g_fx.sun, sx, sy)) {
        drawGlow(sx, sy, g_cam.focal * 0.62f, g_fx.sunCol, 0.50f * g_fx.sunUp);
        float hz[3] = {g_fx.sun[0], 0.f, g_fx.sun[2]}; float l = sqrtf(hz[0] * hz[0] + hz[2] * hz[2]) + 1e-4f; hz[0] /= l; hz[2] /= l;
        float hx, hy; const float dusk[3] = {1.f, 0.42f, 0.14f};
        if (projectDir(hz, hx, hy)) drawGlow(hx, hy, g_W * 0.8f, dusk, 0.62f * g_fx.sunset * smooth01(-0.2f, 0.05f, g_fx.sun[1]));
    }
    if (projectDir(g_fx.moon, sx, sy)) { const float mc[3] = {0.6f, 0.72f, 1.f}; drawGlow(sx, sy, g_cam.focal * 0.28f, mc, 0.30f * (1.f - g_fx.sunUp)); }
}
// ореолы факелов и горящих печей (после мира, до руки)
static void drawLightHalos(double ex, double ey, double ez) {
    if (g_shaders < 2) return;
    struct HL { float x, y, z, d2; };
    static std::vector<HL> hl; hl.clear();
    for (const VisSec& vs : g_visible) {
        const Section& sec = vs.c->sec[vs.s];
        for (u16 p : sec.lights) {
            int x = vs.c->cx * 16 + (p & 15), z = vs.c->cz * 16 + ((p >> 4) & 15), y = vs.s * 16 + (p >> 8);
            u8 b = getBlock(x, y, z); u8 m = getMeta(x, y, z);
            HL h; h.x = x + 0.5f; h.y = y + 0.72f; h.z = z + 0.5f;
            if (b == B_TORCH) { if (m >= 1 && m <= 4) { h.x += HDX[m - 1] * 0.34f; h.z += HDZ[m - 1] * 0.34f; h.y += 0.2f; } }
            else if (b == B_FURNACE_LIT) { int f = hdirToFace(m); h.x += DX[f] * 0.56f; h.z += DZ[f] * 0.56f; h.y = y + 0.4f; }
            else continue;
            float dx = (float)(h.x - ex), dy = (float)(h.y - ey), dz = (float)(h.z - ez);
            h.d2 = dx * dx + dy * dy + dz * dz;
            if (h.d2 < 24.f * 24.f) hl.push_back(h);
        }
    }
    std::sort(hl.begin(), hl.end(), [](const HL& a, const HL& b) { return a.d2 < b.d2; });
    const float warm[3] = {1.f, 0.60f, 0.24f}, core[3] = {1.f, 0.86f, 0.6f};
    int n = 0;
    for (const HL& h : hl) {
        if (n >= 32) break;
        float rel[3] = {(float)(h.x - ex), (float)(h.y - ey), (float)(h.z - ez)};
        float p[3]; toCam(rel[0], rel[1], rel[2], p);
        if (p[2] < 0.15f) continue;
        float d = sqrtf(h.d2);
        float vis = rayTransmit(ex, ey, ez, rel[0] / d, rel[1] / d, rel[2] / d, d - 0.45f);
        if (vis <= 0.f) continue;
        n++;
        float sx = g_cam.cx + p[0] / p[2] * g_cam.focal, sy = g_cam.cy - p[1] / p[2] * g_cam.focal;
        float r = std::min(g_cam.focal * 1.4f / p[2], g_H * 0.8f);
        float fl = 0.88f + 0.12f * sinf(g_fxTime * 9.f + h.x * 3.1f + h.z * 1.7f);
        float a = vis * fl * (1.f - d / 24.f) * (0.45f - 0.25f * g_fx.sunUp);
        drawGlow(sx, sy, r, warm, a * 0.95f);
        drawGlow(sx, sy, r * 0.32f, core, a * 1.3f);
    }
}
// лучи и блики солнца в объективе, виньетка (поверх всего мира)
static void drawLensFX(double ex, double ey, double ez, bool underwater) {
    if (g_shaders < 2) return;
    float sx, sy;
    if (!underwater && g_fx.sunUp > 0.02f && projectDir(g_fx.sun, sx, sy) && sx > -g_W * 0.2f && sx < g_W * 1.2f && sy > -g_H * 0.2f && sy < g_H * 1.2f) {
        const Camera& c = g_cam;
        float T = 0;
        const float off[5][2] = {{0, 0}, {0.012f, 0}, {-0.012f, 0}, {0, 0.012f}, {0, -0.012f}};
        for (auto& o : off) {
            float d[3]; for (int i = 0; i < 3; i++) d[i] = g_fx.sun[i] + c.r[i] * o[0] + c.u[i] * o[1];
            float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            T += rayTransmit(ex, ey, ez, d[0] / l, d[1] / l, d[2] / l, std::min(96.f, g_renderDist * 16.f));
        }
        T /= 5.f;
        if (T > 0.01f) {
            float k = T * g_fx.sunUp;
            const float white[3] = {1.f, 0.97f, 0.9f};
            drawGlow(sx, sy, g_H * 0.5f, g_fx.sunCol, 0.30f * k);
            drawGlow(sx, sy, g_H * 0.11f, white, 0.55f * k);
            // лучи
            const int NR = 16;
            SDL_Vertex v[NR * 3]; int idx[NR * 3];
            for (int i = 0; i < NR; i++) {
                float a = i * 6.2831853f / NR + g_fxTime * 0.03f + (hash32(i * 7 + 1) % 100) * 0.004f;
                float len = g_H * (0.45f + (hash32(i * 13 + 5) % 100) / 100.f * 0.55f), wdt = 0.025f + (hash32(i * 3) % 100) * 0.0003f;
                float al = 0.16f * k * (0.6f + 0.4f * sinf(g_fxTime * 0.7f + i * 1.9f));
                SDL_Color cc = mkColor(g_fx.sunCol[0], g_fx.sunCol[1], g_fx.sunCol[2], al), ce = mkColor(g_fx.sunCol[0], g_fx.sunCol[1], g_fx.sunCol[2], 0);
                v[i * 3].position.x = sx; v[i * 3].position.y = sy; v[i * 3].color = cc;
                v[i * 3 + 1].position.x = sx + cosf(a - wdt) * len; v[i * 3 + 1].position.y = sy + sinf(a - wdt) * len; v[i * 3 + 1].color = ce;
                v[i * 3 + 2].position.x = sx + cosf(a + wdt) * len; v[i * 3 + 2].position.y = sy + sinf(a + wdt) * len; v[i * 3 + 2].color = ce;
                for (int j = 0; j < 3; j++) { v[i * 3 + j].tex_coord.x = v[i * 3 + j].tex_coord.y = 0; idx[i * 3 + j] = i * 3 + j; }
            }
            SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_ADD);
            SDL_RenderGeometry(g_ren, NULL, v, NR * 3, idx, NR * 3);
            SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
            // блики объектива на линии «солнце — центр экрана»
            float vx = g_cam.cx - sx, vy = g_cam.cy - sy;
            float centerK = 1.f - clampf(sqrtf(vx * vx + vy * vy) / (g_W * 0.8f), 0, 1);
            const float fk[4] = {0.55f, 1.15f, 1.6f, 2.1f}, fr[4] = {0.035f, 0.07f, 0.025f, 0.11f};
            const float fc[4][3] = {{1.f, 0.8f, 0.4f}, {0.5f, 0.8f, 1.f}, {0.9f, 0.5f, 1.f}, {0.4f, 1.f, 0.6f}};
            for (int i = 0; i < 4; i++) drawGlow(sx + vx * fk[i], sy + vy * fk[i], g_H * fr[i], fc[i], 0.16f * k * centerK);
        }
    }
    // виньетка
    const int N = 28;
    SDL_Vertex v[N * 2]; int idx[N * 6];
    float cx = g_W * 0.5f, cy = g_H * 0.5f, rx = g_W * 0.42f, ry = g_H * 0.42f;
    float va = underwater ? 0.6f : 0.36f;
    for (int i = 0; i < N; i++) {
        float a = i * 6.2831853f / N, ca = cosf(a), sa = sinf(a);
        v[i * 2].position.x = cx + ca * rx; v[i * 2].position.y = cy + sa * ry; v[i * 2].color = mkColor(0, 0, 0, 0);
        v[i * 2 + 1].position.x = cx + ca * rx * 1.8f; v[i * 2 + 1].position.y = cy + sa * ry * 1.8f; v[i * 2 + 1].color = mkColor(0, 0, 0.02f, va);
        v[i * 2].tex_coord.x = v[i * 2].tex_coord.y = v[i * 2 + 1].tex_coord.x = v[i * 2 + 1].tex_coord.y = 0;
        int a0 = i * 2, a1 = ((i + 1) % N) * 2; int* p = idx + i * 6;
        p[0] = a0; p[1] = a0 + 1; p[2] = a1; p[3] = a0 + 1; p[4] = a1 + 1; p[5] = a1;
    }
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(g_ren, NULL, v, N * 2, idx, N * 6);
}

static void emitClouds(float gameTime) {
    const float CY = 118.f, CELL = 8.f;
    float drift = gameTime * 0.6f;
    int R = (int)((g_renderDist * 16 + 24) / CELL);
    int bx = ifloor((g_cam.x + drift) / CELL), bz = ifloor(g_cam.z / CELL);
    float cl = 0.55f + 0.45f * g_dayLight;
    for (int dz = -R; dz <= R; dz++) for (int dx = -R; dx <= R; dx++) {
        if (dx * dx + dz * dz > R * R) continue;
        int gx = bx + dx, gz = bz + dz;
        float n = nDetail.n2(gx * 0.17f, gz * 0.17f) + nDetail.n2(gx * 0.5f + 7, gz * 0.5f) * 0.3f;
        if (n < 0.18f) continue;
        float x0 = gx * CELL - drift - (float)g_cam.x, z0 = gz * CELL - (float)g_cam.z, y = CY - (float)g_cam.y;
        float W[4][3] = {{x0, y, z0}, {x0 + CELL, y, z0}, {x0 + CELL, y, z0 + CELL}, {x0, y, z0 + CELL}};
        float d2 = (x0 + 4) * (x0 + 4) + (z0 + 4) * (z0 + 4) + y * y;
        float fa = 1.f - smooth01(g_fogEnd * 0.8f, g_fogEnd + 30.f, sqrtf(d2));
        if (fa <= 0.01f) continue;
        float UV[4][2], C[4][4];
        for (int k = 0; k < 4; k++) {
            tileUV(T_WHITE, 8, 8, UV[k]);
            for (int i = 0; i < 3; i++) C[k][i] = g_shaders ? cl * (0.62f + 0.38f * g_fx.sunCol[i]) : cl;
            C[k][2] *= 1.02f; C[k][3] = 0.78f * fa;
        }
        emitQuadRel(W, UV, C);
    }
}

// рамка выделенного блока
static void drawBlockOutline(int bx, int by, int bz, float x0, float y0, float z0, float x1, float y1, float z1) {
    float pts[8][3]; bool ok[8]; float sx[8], sy[8];
    for (int i = 0; i < 8; i++) {
        float wx = bx + ((i & 1) ? x1 : x0) - (float)g_cam.x, wy = by + ((i & 2) ? y1 : y0) - (float)g_cam.y, wz = bz + ((i & 4) ? z1 : z0) - (float)g_cam.z;
        toCam(wx, wy, wz, pts[i]);
        ok[i] = pts[i][2] > 0.05f;
        if (ok[i]) { sx[i] = g_cam.cx + pts[i][0] / pts[i][2] * g_cam.focal; sy[i] = g_cam.cy - pts[i][1] / pts[i][2] * g_cam.focal; }
    }
    static const int E[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_ren, 10, 10, 10, 200);
    for (int e = 0; e < 12; e++) {
        int a = E[e][0], b = E[e][1];
        if (!ok[a] || !ok[b]) continue;
        SDL_RenderDrawLine(g_ren, (int)sx[a], (int)sy[a], (int)sx[b], (int)sy[b]);
        SDL_RenderDrawLine(g_ren, (int)sx[a] + 1, (int)sy[a], (int)sx[b] + 1, (int)sy[b]);
    }
}


// ============================================================================
//  ИГРА: игрок, физика, мобы, предметы, частицы, копание, крафт, печи
// ============================================================================
enum GameMode { GM_SURVIVAL = 0, GM_CREATIVE = 1 };
static int g_mode = GM_SURVIVAL;
static std::string g_msg; static float g_msgT = 0;
// всплывающее название предмета в инвентаре
static std::string g_tipText; static float g_tipX = 0, g_tipY = 0; static Uint32 g_tipUntil = 0;
static void showTip(int id, float x, float y) { if (id <= 0) return; g_tipText = itemName(id); g_tipX = x; g_tipY = y; g_tipUntil = SDL_GetTicks() + 2200; }
// «+3 УГОЛЬ» над панелью, когда что-то подобрал
struct PickNote { int id, cnt; float t; };
static std::vector<PickNote> g_picks;
static void notePickup(int id, int cnt) {
    if (cnt <= 0) return;
    for (auto& p : g_picks) if (p.id == id) { p.cnt += cnt; p.t = 3.f; return; }
    PickNote n; n.id = id; n.cnt = cnt; n.t = 3.f; g_picks.push_back(n);
    if (g_picks.size() > 4) g_picks.erase(g_picks.begin());
}
static void showMsg(const std::string& s, float t = 2.5f) { g_msg = s; g_msgT = t; }

struct Player {
    double x = 0, y = 80, z = 0;
    float vx = 0, vy = 0, vz = 0;
    bool onGround = false, inWater = false, headInWater = false, inLava = false, onLadder = false, flying = false;
    float health = 20, hunger = 20, exhaustion = 0, air = 10;
    float fallStartY = 0; bool falling = false;
    float regenT = 0, starveT = 0, hurtCd = 0, drownT = 0, cactusT = 0, hurtFlash = 0;
    float bobPhase = 0, bobAmt = 0, stepDist = 0;
    int spawnX = 0, spawnY = 80, spawnZ = 0;
    Stack inv[36]; int sel = 0;
    bool dead = false;
    float swing = 0;   // анимация взмаха
    float eatT = 0;
    float bowCharge = 0;   // натяжение лука 0..1
};
static Player P;
static const float PL_HW = 0.3f, PL_H = 1.8f, PL_EYE = 1.62f;

// ---------------------------------------------------------------- инвентарь
static int addItem(Stack s) {   // возвращает остаток
    if (s.empty()) return 0;
    int ms = maxStack(s.id);
    for (int pass = 0; pass < 2 && s.cnt > 0; pass++) {
        for (int i = 0; i < 36 && s.cnt > 0; i++) {
            Stack& d = P.inv[i];
            if (pass == 0) {
                if (d.empty() || d.id != s.id || ms == 1) continue;
                int can = std::min((int)s.cnt, ms - d.cnt);
                if (can > 0) { d.cnt += can; s.cnt -= can; }
            } else if (d.empty()) {
                d = s; int put = std::min((int)s.cnt, ms); d.cnt = put; s.cnt -= put;
            }
        }
    }
    return s.cnt;
}
static int countItem(int id) { int n = 0; for (int i = 0; i < 36; i++) if (P.inv[i].id == id) n += P.inv[i].cnt; return n; }
static void removeItem(int id, int n) {
    for (int i = 35; i >= 0 && n > 0; i--) {
        Stack& s = P.inv[i];
        if (s.id != id || s.empty()) continue;
        int t = std::min((int)s.cnt, n); s.cnt -= t; n -= t; if (s.cnt == 0) s.clear();
    }
}
static Stack& heldStack() { return P.inv[P.sel]; }

// ---------------------------------------------------------------- рецепты
struct Recipe { int w, h; u16 cell[9]; u16 out; u8 cnt; };
static std::vector<Recipe> g_recipes;
static void addRecipe(std::initializer_list<const char*> rows, std::initializer_list<std::pair<char, int>> keys, int out, int cnt) {
    Recipe r; r.h = (int)rows.size(); r.w = 0; memset(r.cell, 0, sizeof(r.cell));
    int y = 0;
    for (const char* row : rows) {
        int w = (int)strlen(row); r.w = std::max(r.w, w);
        for (int x = 0; x < w; x++) {
            u16 id = 0;
            for (auto& k : keys) if (k.first == row[x]) id = (u16)k.second;
            r.cell[y * 3 + x] = id;
        }
        y++;
    }
    r.out = (u16)out; r.cnt = (u8)cnt;
    g_recipes.push_back(r);
}
static void initRecipes() {
    addRecipe({"L"}, {{'L', B_LOG}}, B_PLANKS, 4);
    addRecipe({"L"}, {{'L', B_PINE_LOG}}, B_PLANKS, 4);
    addRecipe({"P", "P"}, {{'P', B_PLANKS}}, I_STICK, 4);
    addRecipe({"PP", "PP"}, {{'P', B_PLANKS}}, B_CRAFT, 1);
    addRecipe({"C", "S"}, {{'C', I_COAL}, {'S', I_STICK}}, B_TORCH, 4);
    addRecipe({"CCC", "C.C", "CCC"}, {{'C', B_COBBLE}}, B_FURNACE, 1);
    addRecipe({"PPP", "P.P", "PPP"}, {{'P', B_PLANKS}}, B_CHEST, 1);
    const int mats[5] = {B_PLANKS, B_COBBLE, I_IRON, I_GOLD, I_DIAMOND};
    for (int m = 0; m < 5; m++) {
        addRecipe({"MMM", ".S.", ".S."}, {{'M', mats[m]}, {'S', I_STICK}}, toolId(m, TK_PICK), 1);
        addRecipe({"MM", "MS", ".S"}, {{'M', mats[m]}, {'S', I_STICK}}, toolId(m, TK_AXE), 1);
        addRecipe({"M", "S", "S"}, {{'M', mats[m]}, {'S', I_STICK}}, toolId(m, TK_SHOVEL), 1);
        addRecipe({"M", "M", "S"}, {{'M', mats[m]}, {'S', I_STICK}}, toolId(m, TK_SWORD), 1);
    }
    addRecipe({"PP", "PP", "PP"}, {{'P', B_PLANKS}}, B_DOOR, 3);
    addRecipe({"S.S", "SSS", "S.S"}, {{'S', I_STICK}}, B_LADDER, 3);
    addRecipe({"FFF", "PPP"}, {{'F', I_FLUFF}, {'P', B_PLANKS}}, B_BED, 1);
    addRecipe({"NNN", "PPP"}, {{'N', I_STRING}, {'P', B_PLANKS}}, B_BED, 1);
    addRecipe({".SN", "S.N", ".SN"}, {{'S', I_STICK}, {'N', I_STRING}}, I_BOW, 1);
    addRecipe({"B", "S", "F"}, {{'B', I_BONE}, {'S', I_STICK}, {'F', I_FLUFF}}, I_ARROW, 4);
    addRecipe({"SS", "SS"}, {{'S', B_SAND}}, B_SANDSTONE, 1);
    addRecipe({"SS", "SS"}, {{'S', B_STONE}}, B_STONEBRICK, 4);
    addRecipe({"III", "III", "III"}, {{'I', I_IRON}}, B_IRON_BLOCK, 1);
    addRecipe({"III", "III", "III"}, {{'I', I_GOLD}}, B_GOLD_BLOCK, 1);
    addRecipe({"III", "III", "III"}, {{'I', I_DIAMOND}}, B_DIAMOND_BLOCK, 1);
    addRecipe({"B"}, {{'B', B_IRON_BLOCK}}, I_IRON, 9);
    addRecipe({"B"}, {{'B', B_GOLD_BLOCK}}, I_GOLD, 9);
    addRecipe({"B"}, {{'B', B_DIAMOND_BLOCK}}, I_DIAMOND, 9);
    addRecipe({"B"}, {{'B', I_BONE}}, I_BONEMEAL, 3);
}
// совпадение сетки gw x gw с рецептом
static int matchRecipe(const Stack* grid, int gw, int* outCnt) {
    int minx = 9, miny = 9, maxx = -1, maxy = -1;
    for (int y = 0; y < gw; y++) for (int x = 0; x < gw; x++) if (!grid[y * gw + x].empty()) {
        minx = std::min(minx, x); maxx = std::max(maxx, x); miny = std::min(miny, y); maxy = std::max(maxy, y);
    }
    if (maxx < 0) return 0;
    int w = maxx - minx + 1, h = maxy - miny + 1;
    for (const Recipe& r : g_recipes) {
        if (r.w != w || r.h != h) continue;
        for (int mir = 0; mir < 2; mir++) {
            bool ok = true;
            for (int y = 0; y < h && ok; y++) for (int x = 0; x < w && ok; x++) {
                int rx = mir ? (w - 1 - x) : x;
                const Stack& g = grid[(miny + y) * gw + minx + x];
                int gid = g.empty() ? 0 : g.id;
                if (gid != r.cell[y * 3 + rx]) ok = false;
            }
            if (ok) { *outCnt = r.cnt; return r.out; }
        }
    }
    return 0;
}

// ---------------------------------------------------------------- печь
static int smeltResult(int id) {
    switch (id) {
        case B_IRON_ORE: return I_IRON; case B_GOLD_ORE: return I_GOLD; case B_SAND: return B_GLASS; case B_COBBLE: return B_STONE;
        case I_RAWMEAT: return I_COOKED; case B_LOG: case B_PINE_LOG: return I_COAL; case B_COAL_ORE: return I_COAL;
        case B_DIAMOND_ORE: return I_DIAMOND;
    }
    return 0;
}
static float fuelTime(int id) {
    if (id == I_COAL) return 40.f;
    if (id == B_LOG || id == B_PINE_LOG || id == B_PLANKS || id == B_CRAFT || id == B_CHEST) return 7.5f;
    if (id == I_STICK || id == B_SAPLING || id == B_LADDER) return 2.5f;
    if (isTool(id) && toolMat(id) == MT_WOOD) return 5.f;
    if (id == B_DOOR) return 5.f;
    return 0;
}
static const float SMELT_TIME = 5.f;

static void tickFurnaces(float dt) {
    for (auto& kv : g_furnaces) {
        Furnace& f = kv.second;
        int fx, fy, fz; unpackPos(kv.first, fx, fy, fz);
        bool wasBurning = f.burn > 0;
        if (f.burn > 0) f.burn -= dt;
        int res = f.in.empty() ? 0 : smeltResult(f.in.id);
        bool canOut = res && (f.out.empty() || (f.out.id == res && f.out.cnt < maxStack(res)));
        if (canOut && f.burn <= 0 && !f.fuel.empty() && fuelTime(f.fuel.id) > 0) {
            f.burnMax = f.burn = fuelTime(f.fuel.id);
            f.fuel.cnt--; if (f.fuel.cnt == 0) f.fuel.clear();
        }
        if (canOut && f.burn > 0) {
            f.cook += dt;
            if (f.cook >= SMELT_TIME) {
                f.cook = 0;
                if (f.out.empty()) f.out = mkStack(res, 1); else f.out.cnt++;
                f.in.cnt--; if (f.in.cnt == 0) f.in.clear();
            }
        } else f.cook = std::max(0.f, f.cook - dt * 2);
        if (f.burn < 0) f.burn = 0;
        bool burning = f.burn > 0;
        if (burning != wasBurning && isLoaded(fx, fz)) {
            u8 b = getBlock(fx, fy, fz);
            if (b == B_FURNACE || b == B_FURNACE_LIT) setBlockRaw(fx, fy, fz, burning ? B_FURNACE_LIT : B_FURNACE, getMeta(fx, fy, fz));
        }
    }
}

// ---------------------------------------------------------------- столкновения
struct AABB { double x0, y0, z0, x1, y1, z1; };
static int blockBoxes(int x, int y, int z, AABB* out) {
    u8 b = getBlock(x, y, z);
    const BlockDef& d = BD[b];
    if (!d.solid) return 0;
    AABB a = {(double)x, (double)y, (double)z, x + 1.0, y + 1.0, z + 1.0};
    if (b == B_CACTUS) { a.x0 += 1 / 16.0; a.z0 += 1 / 16.0; a.x1 -= 1 / 16.0; a.z1 -= 1 / 16.0; }
    else if (b == B_BED) { a.y1 = y + 9 / 16.0; }
    else if (b == B_DOOR) {
        u8 m = getMeta(x, y, z); int f = m & 3; bool open = (m & 4) != 0;
        int side = open ? (f + 1) & 3 : (f + 2) & 3; double t = 3 / 16.0;
        if (side == 0) a.x0 = x + 1 - t; else if (side == 2) a.x1 = x + t; else if (side == 1) a.z0 = z + 1 - t; else a.z1 = z + t;
    }
    out[0] = a; return 1;
}
static double sweepAxis(double x, double y, double z, float hw, float h, int axis, double delta) {
    double bx0 = x - hw, by0 = y, bz0 = z - hw, bx1 = x + hw, by1 = y + h, bz1 = z + hw;
    double mn[3] = {bx0, by0, bz0}, mx[3] = {bx1, by1, bz1};
    double lo[3] = {bx0, by0, bz0}, hi[3] = {bx1, by1, bz1};
    if (delta > 0) hi[axis] += delta; else lo[axis] += delta;
    int ix0 = ifloor(lo[0]) - 1, ix1 = ifloor(hi[0]) + 1, iy0 = ifloor(lo[1]) - 1, iy1 = ifloor(hi[1]) + 1, iz0 = ifloor(lo[2]) - 1, iz1 = ifloor(hi[2]) + 1;
    const double E = 1e-7;
    AABB boxes[2];
    for (int ix = ix0; ix <= ix1; ix++) for (int iy = iy0; iy <= iy1; iy++) for (int iz = iz0; iz <= iz1; iz++) {
        int n = blockBoxes(ix, iy, iz, boxes);
        for (int k = 0; k < n; k++) {
            const AABB& b = boxes[k];
            double bmn[3] = {b.x0, b.y0, b.z0}, bmx[3] = {b.x1, b.y1, b.z1};
            bool ov = true;
            for (int a = 0; a < 3; a++) {
                if (a == axis) continue;
                if (bmx[a] <= mn[a] + E || bmn[a] >= mx[a] - E) { ov = false; break; }
            }
            if (!ov) continue;
            if (delta > 0 && bmn[axis] >= mx[axis] - E) delta = std::min(delta, bmn[axis] - mx[axis]);
            else if (delta < 0 && bmx[axis] <= mn[axis] + E) delta = std::max(delta, bmx[axis] - mn[axis]);
        }
    }
    return delta;
}
// перемещение с учётом столкновений; возвращает биты: 1 — упёрлись по горизонтали, 2 — встали на землю, 4 — удар головой
static int moveBody(double& x, double& y, double& z, float& vx, float& vy, float& vz, float hw, float h, float dt) {
    int res = 0;
    double dx = vx * dt, dy = vy * dt, dz = vz * dt;
    int steps = (int)(std::max(fabs(dx), std::max(fabs(dy), fabs(dz))) / 0.4) + 1;
    dx /= steps; dy /= steps; dz /= steps;
    for (int s = 0; s < steps; s++) {
        double ny = sweepAxis(x, y, z, hw, h, 1, dy);
        if (ny != dy) { if (dy < 0) res |= 2; else res |= 4; vy = 0; dy = 0; }
        y += ny;
        double nx = sweepAxis(x, y, z, hw, h, 0, dx);
        if (nx != dx) { res |= 1; vx = 0; dx = 0; }
        x += nx;
        double nz = sweepAxis(x, y, z, hw, h, 2, dz);
        if (nz != dz) { res |= 1; vz = 0; dz = 0; }
        z += nz;
    }
    return res;
}
static bool boxTouchesBlock(double x, double y, double z, float hw, float h, u8 id, float grow = 0.f) {
    int x0 = ifloor(x - hw - grow), x1 = ifloor(x + hw + grow), y0 = ifloor(y - grow), y1 = ifloor(y + h + grow), z0 = ifloor(z - hw - grow), z1 = ifloor(z + hw + grow);
    for (int ix = x0; ix <= x1; ix++) for (int iy = y0; iy <= y1; iy++) for (int iz = z0; iz <= z1; iz++) if (getBlock(ix, iy, iz) == id) return true;
    return false;
}

// ---------------------------------------------------------------- частицы и выпавшие предметы
struct Particle { float x, y, z, vx, vy, vz, life, maxLife, size; int tile; float u, v; float r, g, b; bool glow; };
static std::vector<Particle> g_parts;
static void spawnBreakParticles(int bx, int by, int bz, int tile, int n = 14) {
    Rng r(SDL_GetTicks() + bx * 13 + bz * 7 + by);
    for (int i = 0; i < n; i++) {
        Particle p;
        p.x = bx + 0.15f + r.f() * 0.7f; p.y = by + 0.15f + r.f() * 0.7f; p.z = bz + 0.15f + r.f() * 0.7f;
        p.vx = r.sf() * 2.5f; p.vy = r.f() * 4.f; p.vz = r.sf() * 2.5f;
        p.life = p.maxLife = 0.5f + r.f() * 0.5f; p.size = 0.07f + r.f() * 0.05f; p.tile = tile;
        p.u = (float)r.range(12); p.v = (float)r.range(12); p.r = p.g = p.b = 1; p.glow = false;
        g_parts.push_back(p);
    }
}
static void spawnPuff(float x, float y, float z, int n, float r0, float g0, float b0, bool glow = false, float up = 1.5f) {
    Rng r(SDL_GetTicks() * 31 + (u32)(x * 100));
    for (int i = 0; i < n; i++) {
        Particle p;
        p.x = x + r.sf() * 0.4f; p.y = y + r.f() * 0.5f; p.z = z + r.sf() * 0.4f;
        p.vx = r.sf() * 1.2f; p.vy = r.f() * up; p.vz = r.sf() * 1.2f;
        p.life = p.maxLife = 0.6f + r.f() * 0.5f; p.size = 0.08f + r.f() * 0.08f; p.tile = T_WHITE; p.u = 8; p.v = 8;
        p.r = r0; p.g = g0; p.b = b0; p.glow = glow;
        g_parts.push_back(p);
    }
}
struct Drop { Stack s; double x, y, z; float vx, vy, vz; float age; bool onGround; };
static std::vector<Drop> g_drops;
static void spawnDrop(Stack s, double x, double y, double z, float vx = 1e9f, float vy = 3.f, float vz = 0) {
    if (s.empty()) return;
    Drop d; d.s = s; d.x = x; d.y = y; d.z = z; d.age = 0; d.onGround = false;
    Rng r(SDL_GetTicks() + (u32)g_drops.size() * 77);
    if (vx > 1e8f) { d.vx = r.sf() * 1.5f; d.vz = r.sf() * 1.5f; } else { d.vx = vx; d.vz = vz; }
    d.vy = vy;
    g_drops.push_back(d);
}

// ---------------------------------------------------------------- урон игроку
static void playerHurt(float amount, double fromX = 1e9, double fromZ = 0, bool ignoreCd = false) {
    if (P.dead || g_mode == GM_CREATIVE) return;
    if (P.hurtCd > 0 && !ignoreCd) return;
    P.health -= amount; P.hurtCd = 0.5f; P.hurtFlash = 0.35f;
    playSound(SND_HURT, 0.8f, 0.9f + (SDL_GetTicks() % 20) / 100.f);
    if (fromX < 1e8) {
        float dx = (float)(P.x - fromX), dz = (float)(P.z - fromZ), l = sqrtf(dx * dx + dz * dz) + 0.001f;
        P.vx += dx / l * 6.f; P.vz += dz / l * 6.f; P.vy = std::max(P.vy, 5.f);
    }
    if (P.health <= 0) {
        P.health = 0; P.dead = true;
        for (int i = 0; i < 36; i++) if (!P.inv[i].empty()) { spawnDrop(P.inv[i], P.x, P.y + 1, P.z); P.inv[i].clear(); }
    }
}

// ---------------------------------------------------------------- мобы
enum MobType { MOB_TOPTUN = 0, MOB_PUKH, MOB_GNIL, MOB_TEN, MOB_ZOMBIE, MOB_SKELETON, MOB_SPIDER, MOB_COUNT };
struct MobDef { const char* name; float hp, hw, h, speed; bool hostile; float dmg; int snd; bool burns; };
static const MobDef MD[MOB_COUNT] = {
    {"ТОПТУН", 10, 0.45f, 1.3f, 1.6f, false, 0, SND_TOPTUN, false},
    {"ПУХЛЯК", 4, 0.3f, 0.75f, 2.0f, false, 0, SND_PUKH, false},
    {"ГНИЛЬ", 20, 0.42f, 1.45f, 2.2f, true, 3, SND_GNIL, true},
    {"ТЕНЕВИК", 14, 0.35f, 1.1f, 4.0f, true, 3, SND_TEN, true},
    {"ЗОМБИ", 20, 0.3f, 1.9f, 2.3f, true, 3, SND_ZOMB, true},       // медленный, бьёт в ближнем бою
    {"СКЕЛЕТ", 16, 0.3f, 1.95f, 2.6f, true, 0, SND_SKEL, true},     // держит дистанцию и стреляет из лука
    {"ПАУК", 14, 0.65f, 0.85f, 3.6f, true, 2, SND_SPID, false},     // быстрый, прыгает, лазает по стенам; днём мирный
};
struct Mob {
    int type; double x, y, z; float vx = 0, vy = 0, vz = 0; float yaw = 0, wishYaw = 0; float hp; float hurtT = 0;
    float walk = 0; float aiT = 0; bool moving = false; bool onGround = false; float atkCd = 0; float sndT = 5; float burnT = 0; float fleeT = 0;
    float angryT = 0, leapCd = 0, strafeT = 0;
    bool dead = false;
};
static std::vector<Mob> g_mobs;

// ---------------------------------------------------------------- стрелы
static const float ARROW_G = 20.f;
struct Arrow { double x, y, z; float vx, vy, vz; float life, dmg; bool fromPlayer, stuck; float dir[3]; };
static std::vector<Arrow> g_arrows;
static void spawnArrow(double x, double y, double z, float vx, float vy, float vz, float dmg, bool fromPlayer) {
    Arrow a; a.x = x; a.y = y; a.z = z; a.vx = vx; a.vy = vy; a.vz = vz; a.life = 30.f; a.dmg = dmg; a.fromPlayer = fromPlayer; a.stuck = false;
    float l = sqrtf(vx * vx + vy * vy + vz * vz) + 1e-5f; a.dir[0] = vx / l; a.dir[1] = vy / l; a.dir[2] = vz / l;
    if (g_arrows.size() >= 160) g_arrows.erase(g_arrows.begin());
    g_arrows.push_back(a);
}

static void mobDie(Mob& m) {
    m.dead = true;
    playSound(SND_POOF, 0.7f);
    spawnPuff((float)m.x, (float)m.y + 0.3f, (float)m.z, 12, 0.9f, 0.9f, 0.9f);
    Rng r(SDL_GetTicks() + (u32)(m.x * 10));
    if (g_mode == GM_CREATIVE) return;
    switch (m.type) {
        case MOB_TOPTUN: spawnDrop(mkStack(I_RAWMEAT, r.range(1, 3)), m.x, m.y + 0.5, m.z); break;
        case MOB_PUKH: spawnDrop(mkStack(I_RAWMEAT, 1), m.x, m.y + 0.5, m.z); { int f = r.range(0, 2); if (f) spawnDrop(mkStack(I_FLUFF, f), m.x, m.y + 0.5, m.z); } break;
        case MOB_GNIL: { int n = r.range(0, 2); if (n) spawnDrop(mkStack(I_ROTTEN, n), m.x, m.y + 0.5, m.z); } break;
        case MOB_TEN: { int n = r.range(0, 2); if (n) spawnDrop(mkStack(I_BONE, n), m.x, m.y + 0.5, m.z); } break;
        case MOB_ZOMBIE: { int n = r.range(0, 2); if (n) spawnDrop(mkStack(I_ROTTEN, n), m.x, m.y + 0.5, m.z); if (r.range(100) < 4) spawnDrop(mkStack(I_IRON, 1), m.x, m.y + 0.5, m.z); } break;
        case MOB_SKELETON: { int n = r.range(0, 2); if (n) spawnDrop(mkStack(I_BONE, n), m.x, m.y + 0.5, m.z); int a = r.range(0, 2); if (a) spawnDrop(mkStack(I_ARROW, a), m.x, m.y + 0.5, m.z); } break;
        case MOB_SPIDER: { int n = r.range(0, 2); if (n) spawnDrop(mkStack(I_STRING, n), m.x, m.y + 0.5, m.z); } break;
    }
}
static void mobHurt(Mob& m, float dmg, double fromX, double fromZ) {
    m.hp -= dmg; m.hurtT = 0.4f;
    float dx = (float)(m.x - fromX), dz = (float)(m.z - fromZ), l = sqrtf(dx * dx + dz * dz) + 0.001f;
    m.vx = dx / l * 7.f; m.vz = dz / l * 7.f; m.vy = 5.5f;
    playSound(SND_HIT, 0.8f);
    playSound(MD[m.type].snd, 0.5f, 1.15f);
    if (!MD[m.type].hostile) m.fleeT = 4.f;
    if (m.type == MOB_SPIDER) m.angryT = 30.f;   // обиженный паук нападает и днём
    if (m.hp <= 0) mobDie(m);
}

static void updateMob(Mob& m, float dt, Rng& r) {
    const MobDef& d = MD[m.type];
    m.hurtT = std::max(0.f, m.hurtT - dt);
    m.atkCd = std::max(0.f, m.atkCd - dt);
    float pdx = (float)(P.x - m.x), pdz = (float)(P.z - m.z), pdy = (float)(P.y - m.y);
    float pdist = sqrtf(pdx * pdx + pdz * pdz);
    bool chasing = false;
    bool useMove = false; float moveYaw = 0;   // скелет смотрит на игрока, а идёт в другую сторону
    m.leapCd = std::max(0.f, m.leapCd - dt);
    bool calm = false;
    if (m.type == MOB_SPIDER) {   // днём на свету паук спокоен, пока его не тронуть
        m.angryT = std::max(0.f, m.angryT - dt);
        int sk, bl2; getLightAt(ifloor(m.x), ifloor(m.y + 0.5), ifloor(m.z), sk, bl2);
        calm = !isNight() && sk >= 10 && m.angryT <= 0;
    }
    if (d.hostile && !calm && !P.dead && g_mode == GM_SURVIVAL && pdist < 20.f && fabsf(pdy) < 10.f) {
        chasing = true;
        m.wishYaw = atan2f(pdx, pdz); m.moving = pdist > 0.8f;
        if (m.type == MOB_SKELETON) {
            useMove = true; m.moving = true; m.strafeT += dt;
            if (pdist < 5.5f) moveYaw = atan2f(-pdx, -pdz) + sinf(m.strafeT * 2.f) * 0.5f;          // отступает
            else if (pdist > 11.f) moveYaw = m.wishYaw;                                               // подходит
            else { moveYaw = m.wishYaw + 1.5708f * (((int)(m.strafeT / 3.f)) % 2 ? 1.f : -1.f); m.moving = fmodf(m.strafeT, 3.f) < 1.6f; }   // кружит
            float dist3 = sqrtf(pdist * pdist + pdy * pdy);
            if (m.atkCd <= 0 && dist3 < 16.f) {
                double ex = m.x, ey = m.y + 1.55, ez = m.z;
                float dx = (float)(P.x - ex), dyy = (float)(P.y + 1.2 - ey), dz = (float)(P.z - ez);
                float L = sqrtf(dx * dx + dyy * dyy + dz * dz) + 1e-4f;
                if (rayTransmit(ex, ey, ez, dx / L, dyy / L, dz / L, L - 0.6f) > 0.5f) {
                    const float SPD = 20.f; float tt = L / SPD;
                    float vx = dx / tt + r.sf() * 1.0f, vz = dz / tt + r.sf() * 1.0f, vy = dyy / tt + 0.5f * ARROW_G * tt + r.sf() * 0.7f;
                    spawnArrow(ex + dx / L * 0.5, ey, ez + dz / L * 0.5, vx, vy, vz, 3.f, false);
                    m.atkCd = 2.0f + r.f() * 0.8f;
                    playSound(SND_BOW, 0.7f * (1.f - std::min(1.f, L / 24.f)), 1.f + r.sf() * 0.1f);
                } else m.atkCd = 0.4f;
            }
        } else {
            if (pdist < 1.3f + d.hw && fabsf(pdy) < 1.6f && m.atkCd <= 0) {
                m.atkCd = 1.0f;
                playerHurt(d.dmg, m.x, m.z);
            }
            if (m.type == MOB_SPIDER && m.onGround && pdist > 1.8f && pdist < 4.5f && m.leapCd <= 0) {   // прыжок на жертву
                m.vy = 6.5f; m.vx = pdx / pdist * 7.5f; m.vz = pdz / pdist * 7.5f; m.leapCd = 2.5f; m.onGround = false;
                playSound(SND_SPID, 0.5f, 1.3f);
            }
        }
    } else if (m.fleeT > 0) {
        m.fleeT -= dt;
        m.wishYaw = atan2f(-pdx, -pdz) + sinf(m.fleeT * 3.f) * 0.5f; m.moving = true;
    } else {
        m.aiT -= dt;
        if (m.aiT <= 0) {
            m.aiT = 2.f + r.f() * 5.f;
            m.moving = r.range(100) < 55;
            m.wishYaw = r.f() * 6.2831853f;
        }
    }
    // поворот
    float dy = m.wishYaw - m.yaw;
    while (dy > M_PI) dy -= 2 * (float)M_PI;
    while (dy < -M_PI) dy += 2 * (float)M_PI;
    m.yaw += dy * std::min(1.f, dt * 6.f);
    float spd = d.speed * (m.fleeT > 0 ? 1.8f : 1.f) * (chasing ? 1.1f : 1.f);
    bool inWater = getBlock(ifloor(m.x), ifloor(m.y + 0.4), ifloor(m.z)) == B_WATER;
    bool inLava = getBlock(ifloor(m.x), ifloor(m.y + 0.4), ifloor(m.z)) == B_LAVA;
    float myaw = useMove ? moveYaw : m.yaw;
    float wx = m.moving ? sinf(myaw) * spd : 0, wz = m.moving ? cosf(myaw) * spd : 0;
    if (inWater) { wx *= 0.5f; wz *= 0.5f; }
    float k = m.onGround ? std::min(1.f, dt * 10.f) : std::min(1.f, dt * 2.f);
    m.vx += (wx - m.vx) * k; m.vz += (wz - m.vz) * k;
    if (inWater) { m.vy += 14.f * dt; m.vy *= expf(-3.f * dt); if (m.vy > 2.5f) m.vy = 2.5f; }
    else m.vy -= 28.f * dt;
    if (m.vy < -50) m.vy = -50;
    int res = moveBody(m.x, m.y, m.z, m.vx, m.vy, m.vz, d.hw, d.h, dt);
    m.onGround = (res & 2) != 0;
    if (m.type == MOB_SPIDER && (res & 1) && m.moving) m.vy = std::max(m.vy, 3.6f);   // лезет по стене
    else if ((res & 1) && m.onGround && m.moving) m.vy = 8.6f;
    if (m.moving && m.onGround) m.walk += dt * spd * 3.f; else m.walk *= expf(-dt * 5);
    // звуки
    m.sndT -= dt;
    if (m.sndT <= 0) {
        m.sndT = 6.f + r.f() * 10.f;
        float dist = sqrtf(pdist * pdist + pdy * pdy);
        if (dist < 24) playSound(d.snd, 0.6f * (1.f - dist / 24.f), 0.9f + r.f() * 0.2f);
    }
    // солнце и лава
    if (d.burns && !isNight()) {
        int sky, bl; getLightAt(ifloor(m.x), ifloor(m.y + d.h - 0.2), ifloor(m.z), sky, bl);
        if (sky >= 12) {
            m.burnT += dt;
            if (r.range(100) < 30) spawnPuff((float)m.x, (float)m.y + d.h * 0.6f, (float)m.z, 1, 1.f, 0.6f, 0.2f, true, 2.f);
            if (m.burnT > 1.f) { m.burnT = 0; m.hp -= 2; m.hurtT = 0.2f; if (m.hp <= 0) mobDie(m); }
        }
    }
    if (inLava) { m.burnT += dt; if (m.burnT > 0.5f) { m.burnT = 0; m.hp -= 4; m.hurtT = 0.2f; if (m.hp <= 0) mobDie(m); } }
    if (m.y < -20) m.dead = true;
}

static float g_spawnT = 0;
static int pickHostile(Rng& r) {
    int v = r.range(100);
    if (v < 18) return MOB_GNIL;
    if (v < 32) return MOB_TEN;
    if (v < 58) return MOB_ZOMBIE;
    if (v < 80) return MOB_SKELETON;
    return MOB_SPIDER;
}
static bool spawnSpaceOK(int x, int y, int z, int t) {
    int h = MD[t].h > 1.f ? 2 : 1, rr = MD[t].hw > 0.5f ? 1 : 0;
    for (int dz = -rr; dz <= rr; dz++) for (int dx = -rr; dx <= rr; dx++) for (int dy = 0; dy < h; dy++)
        if (BD[getBlock(x + dx, y + dy, z + dz)].solid || getBlock(x + dx, y + dy, z + dz) == B_WATER) return false;
    return true;
}
static void spawnMobs(Rng& r) {
    int peaceful = 0, hostile = 0;
    for (const Mob& m : g_mobs) { if (MD[m.type].hostile) hostile++; else peaceful++; }
    for (int attempt = 0; attempt < 6; attempt++) {
        float ang = r.f() * 6.2831853f, dist = 18.f + r.f() * 26.f;
        int x = ifloor(P.x + sinf(ang) * dist), z = ifloor(P.z + cosf(ang) * dist);
        Chunk* c = getChunk(x >> 4, z >> 4);
        if (!c || !c->meshed) continue;
        // поверхность
        int top = WH - 2;
        while (top > 1 && !BD[getBlock(x, top, z)].solid && getBlock(x, top, z) != B_WATER) top--;
        u8 ground = getBlock(x, top, z);
        bool night = isNight();
        if (r.range(100) < 50) {
            // на поверхности
            if (getBlock(x, top + 1, z) != B_AIR || getBlock(x, top + 2, z) != B_AIR) continue;
            if (!night && peaceful < 10 && (ground == B_GRASS || ground == B_SNOWGRASS)) {
                int t = r.range(100) < 55 ? MOB_TOPTUN : MOB_PUKH;
                int n = r.range(1, 3);
                for (int i = 0; i < n && peaceful < 10; i++) {
                    Mob m; m.type = t; m.x = x + 0.5 + r.sf(); m.y = top + 1.05; m.z = z + 0.5 + r.sf(); m.hp = MD[t].hp; m.yaw = r.f() * 6.28f; m.wishYaw = m.yaw;
                    if (BD[getBlock(ifloor(m.x), ifloor(m.y), ifloor(m.z))].solid) continue;
                    g_mobs.push_back(m); peaceful++;
                }
            } else if (night && hostile < 10 && BD[ground].solid && ground != B_LEAVES && ground != B_PINE_LEAVES) {
                int t = pickHostile(r);
                if (!spawnSpaceOK(x, top + 1, z, t)) continue;
                Mob m; m.type = t; m.x = x + 0.5; m.y = top + 1.05; m.z = z + 0.5; m.hp = MD[t].hp; m.yaw = r.f() * 6.28f; m.wishYaw = m.yaw;
                g_mobs.push_back(m); hostile++;
            }
        } else if (hostile < 10) {
            // в пещере: тёмная полость под землёй
            int y = r.range(8, std::max(9, top - 6));
            if (getBlock(x, y, z) != B_AIR || getBlock(x, y + 1, z) != B_AIR || !BD[getBlock(x, y - 1, z)].solid) continue;
            int sky, bl; getLightAt(x, y, z, sky, bl);
            if (sky > 3 || bl > 5) continue;
            int t = pickHostile(r);
            if (!spawnSpaceOK(x, y, z, t)) continue;
            Mob m; m.type = t; m.x = x + 0.5; m.y = y + 0.02; m.z = z + 0.5; m.hp = MD[t].hp; m.yaw = r.f() * 6.28f; m.wishYaw = m.yaw;
            g_mobs.push_back(m); hostile++;
        }
    }
}

static void updateArrows(float dt) {
    for (size_t i = 0; i < g_arrows.size();) {
        Arrow& a = g_arrows[i];
        bool remove = false;
        if (a.stuck) {
            a.life -= dt;
            double dx = P.x - a.x, dy = P.y + 0.8 - a.y, dz = P.z - a.z;
            if (a.fromPlayer && !P.dead && g_mode == GM_SURVIVAL && dx * dx + dy * dy + dz * dz < 2.2 && addItem(mkStack(I_ARROW, 1)) == 0) {
                notePickup(I_ARROW, 1);
                playSound(SND_PICKUP, 0.5f, 1.1f); remove = true;
            }
            if (a.life <= 0) remove = true;
            // блок, в который воткнулась стрела, исчез — падает
            if (!remove && !BD[getBlock(ifloor(a.x + a.dir[0] * 0.2), ifloor(a.y + a.dir[1] * 0.2), ifloor(a.z + a.dir[2] * 0.2))].solid) { a.stuck = false; a.vx = a.vy = a.vz = 0; }
        } else {
            a.life -= dt;
            const int STEPS = 4; float h = dt / STEPS;
            for (int s = 0; s < STEPS && !remove && !a.stuck; s++) {
                a.vy -= ARROW_G * h;
                double nx = a.x + a.vx * h, ny = a.y + a.vy * h, nz = a.z + a.vz * h;
                if (!a.fromPlayer) {
                    if (!P.dead && nx > P.x - PL_HW - 0.05 && nx < P.x + PL_HW + 0.05 && nz > P.z - PL_HW - 0.05 && nz < P.z + PL_HW + 0.05 && ny > P.y && ny < P.y + PL_H) {
                        playerHurt(a.dmg, a.x - a.vx, a.z - a.vz); remove = true; break;
                    }
                } else {
                    for (Mob& m : g_mobs) {
                        if (m.dead) continue;
                        const MobDef& md = MD[m.type];
                        if (nx > m.x - md.hw - 0.1 && nx < m.x + md.hw + 0.1 && nz > m.z - md.hw - 0.1 && nz < m.z + md.hw + 0.1 && ny > m.y && ny < m.y + md.h) {
                            mobHurt(m, a.dmg, a.x - a.vx * 0.1, a.z - a.vz * 0.1); remove = true; break;
                        }
                    }
                    if (remove) break;
                }
                u8 b = getBlock(ifloor(nx), ifloor(ny), ifloor(nz));
                if (BD[b].solid) {
                    a.stuck = true; a.life = a.fromPlayer ? 60.f : 5.f;
                    a.x += (nx - a.x) * 0.7; a.y += (ny - a.y) * 0.7; a.z += (nz - a.z) * 0.7;
                    double ddx = a.x - P.x, ddz = a.z - P.z;
                    playSound(SND_DIG0 + SM_WOOD, 0.5f * (1.f - std::min(1.f, (float)sqrt(ddx * ddx + ddz * ddz) / 20.f)), 1.6f);
                    break;
                }
                if (b == B_WATER) { float k = expf(-4.f * h); a.vx *= k; a.vy *= k; a.vz *= k; }
                a.x = nx; a.y = ny; a.z = nz;
                float l = sqrtf(a.vx * a.vx + a.vy * a.vy + a.vz * a.vz);
                if (l > 0.5f) { a.dir[0] = a.vx / l; a.dir[1] = a.vy / l; a.dir[2] = a.vz / l; }
            }
            if (a.y < -10 || a.life <= 0) remove = true;
        }
        if (remove) { g_arrows[i] = g_arrows.back(); g_arrows.pop_back(); } else i++;
    }
}

// ---------------------------------------------------------------- луч взгляда
struct Hit { bool hit = false; int x = 0, y = 0, z = 0, face = 2; float dist = 0; };
static bool selectable(u8 b) { return b != B_AIR && BD[b].shape != SH_FLUID; }
static Hit raycast(double ox, double oy, double oz, float dx, float dy, float dz, float maxD) {
    Hit h;
    int x = ifloor(ox), y = ifloor(oy), z = ifloor(oz);
    int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
    float tdx = dx != 0 ? fabsf(1.f / dx) : 1e9f, tdy = dy != 0 ? fabsf(1.f / dy) : 1e9f, tdz = dz != 0 ? fabsf(1.f / dz) : 1e9f;
    float tmx = dx != 0 ? (float)((dx > 0 ? (x + 1 - ox) : (ox - x)) * tdx) : 1e9f;
    float tmy = dy != 0 ? (float)((dy > 0 ? (y + 1 - oy) : (oy - y)) * tdy) : 1e9f;
    float tmz = dz != 0 ? (float)((dz > 0 ? (z + 1 - oz) : (oz - z)) * tdz) : 1e9f;
    float t = 0; int face = 2;
    for (int i = 0; i < 64; i++) {
        u8 b = getBlock(x, y, z);
        if (y >= 0 && y < WH && selectable(b) && isLoaded(x, z)) { h.hit = true; h.x = x; h.y = y; h.z = z; h.face = face; h.dist = t; return h; }
        if (tmx < tmy && tmx < tmz) { t = tmx; tmx += tdx; x += sx; face = sx > 0 ? 1 : 0; }
        else if (tmy < tmz) { t = tmy; tmy += tdy; y += sy; face = sy > 0 ? 3 : 2; }
        else { t = tmz; tmz += tdz; z += sz; face = sz > 0 ? 5 : 4; }
        if (t > maxD) break;
    }
    return h;
}
static int rayMob(double ox, double oy, double oz, float dx, float dy, float dz, float maxD, float& tOut) {
    int best = -1; float bt = maxD;
    for (size_t i = 0; i < g_mobs.size(); i++) {
        const Mob& m = g_mobs[i]; if (m.dead) continue;
        const MobDef& d = MD[m.type];
        double b0[3] = {m.x - d.hw - 0.1, m.y, m.z - d.hw - 0.1}, b1[3] = {m.x + d.hw + 0.1, m.y + d.h, m.z + d.hw + 0.1};
        double o[3] = {ox, oy, oz}; float dd[3] = {dx, dy, dz};
        float t0 = 0, t1 = bt; bool ok = true;
        for (int a = 0; a < 3 && ok; a++) {
            if (fabsf(dd[a]) < 1e-6f) { if (o[a] < b0[a] || o[a] > b1[a]) ok = false; continue; }
            float ta = (float)((b0[a] - o[a]) / dd[a]), tb = (float)((b1[a] - o[a]) / dd[a]);
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta); t1 = std::min(t1, tb);
            if (t0 > t1) ok = false;
        }
        if (ok && t0 < bt) { bt = t0; best = (int)i; }
    }
    tOut = bt;
    return best;
}

// ---------------------------------------------------------------- копание
static float breakTime(u8 b, const Stack& tool, bool* canHarvest) {
    const BlockDef& d = BD[b];
    *canHarvest = true;
    if (d.hardness < 0) { *canHarvest = false; return 1e9f; }
    if (d.hardness == 0) return 0.05f;
    int tk = isTool(tool.id) ? toolKind(tool.id) : -1;
    int tm = isTool(tool.id) ? toolMat(tool.id) : -1;
    float speed = 1.f;
    if (tk >= 0 && tk == d.tool) speed = MAT_SPEED[tm];
    if (tk == TK_SWORD && (b == B_LEAVES || b == B_PINE_LEAVES || b == B_CACTUS)) speed = 1.5f;
    if (d.needPick) {
        bool ok = tk == TK_PICK && MAT_TIER[tm] >= d.tier;
        if (!ok) { *canHarvest = false; return d.hardness * 5.f / (tk == TK_PICK ? MAT_SPEED[tm] * 0.5f : 1.f); }
    }
    return d.hardness * 1.5f / speed;
}
static Stack dropFor(u8 b, Rng& r) {
    switch (b) {
        case B_STONE: return mkStack(B_COBBLE, 1);
        case B_GRASS: case B_SNOWGRASS: return mkStack(B_DIRT, 1);
        case B_COAL_ORE: return mkStack(I_COAL, 1);
        case B_DIAMOND_ORE: return mkStack(I_DIAMOND, 1);
        case B_LEAVES: case B_PINE_LEAVES: {
            int v = r.range(100);
            if (v < 8) return mkStack(B_SAPLING, 1);
            if (b == B_LEAVES && v < 13) return mkStack(I_APPLE, 1);
            return Stack();
        }
        case B_TALLGRASS: case B_GLASS: case B_ICE: case B_BEDROCK: case B_WATER: case B_LAVA: return Stack();
        case B_FURNACE_LIT: return mkStack(B_FURNACE, 1);
        default: return mkStack(b, 1);
    }
}
static void damageTool(Stack& s, int amount = 1) {
    if (!isTool(s.id) || g_mode == GM_CREATIVE) return;
    s.dmg += amount;
    if (s.dmg >= MAT_DUR[toolMat(s.id)]) { s.clear(); playSound(SND_BREAK0 + SM_GLASS, 0.6f, 0.6f); showMsg("ИНСТРУМЕНТ СЛОМАЛСЯ"); }
}

static void breakBlockAt(int x, int y, int z, bool harvest) {
    u8 b = getBlock(x, y, z); u8 m = getMeta(x, y, z);
    if (b == B_AIR || BD[b].hardness < 0) return;
    playSound(SND_BREAK0 + BD[b].sound, 0.8f, 0.9f + (SDL_GetTicks() % 20) / 100.f);
    spawnBreakParticles(x, y, z, BD[b].shape == SH_CUBE ? BD[b].tex[0] : BD[b].tex[0]);
    Rng r(SDL_GetTicks() ^ (u32)(x * 31 + z * 17 + y));
    bool surv = g_mode == GM_SURVIVAL;
    if (surv && harvest) { Stack s = dropFor(b, r); if (!s.empty()) spawnDrop(s, x + 0.5, y + 0.4, z + 0.5); }
    i64 k = posKey(x, y, z);
    if (b == B_CHEST) {
        auto it = g_chests.find(k);
        if (it != g_chests.end()) { for (auto& s : it->second) if (!s.empty()) spawnDrop(s, x + 0.5, y + 0.5, z + 0.5); g_chests.erase(it); }
    }
    if (b == B_FURNACE || b == B_FURNACE_LIT) {
        auto it = g_furnaces.find(k);
        if (it != g_furnaces.end()) { spawnDrop(it->second.in, x + 0.5, y + 0.5, z + 0.5); spawnDrop(it->second.fuel, x + 0.5, y + 0.5, z + 0.5); spawnDrop(it->second.out, x + 0.5, y + 0.5, z + 0.5); g_furnaces.erase(it); }
    }
    u8 repl = B_AIR;
    if (b == B_ICE && surv && y > 0 && BD[getBlock(x, y - 1, z)].solid) repl = B_WATER;
    setBlock(x, y, z, repl, 0);
    if (b == B_DOOR) { int oy = (m & 8) ? y - 1 : y + 1; if (getBlock(x, oy, z) == B_DOOR) setBlock(x, oy, z, B_AIR); }
    if (b == B_BED) {
        int f = m & 3; int sgn = (m & 4) ? -1 : 1;
        int ox = x + HDX[f] * sgn, oz = z + HDZ[f] * sgn;
        if (getBlock(x, y, z) == B_AIR && getBlock(ox, y, oz) == B_BED) setBlock(ox, y, oz, B_AIR);
    }
}

// поддержка: факелы, цветы и т.п. отпадают без опоры
static bool hasSupport(int x, int y, int z, u8 b, u8 m) {
    switch (b) {
        case B_TORCH: {
            if (m == 0) { u8 below = getBlock(x, y - 1, z); return BD[below].opaque || below == B_GLASS; }
            int hd = m - 1; return BD[getBlock(x + HDX[hd], y, z + HDZ[hd])].opaque;
        }
        case B_LADDER: { int hd = m & 3; return BD[getBlock(x + HDX[hd], y, z + HDZ[hd])].opaque; }
        case B_TALLGRASS: case B_FLOWER_R: case B_FLOWER_Y: case B_SAPLING: {
            u8 g = getBlock(x, y - 1, z); return g == B_GRASS || g == B_DIRT || g == B_SNOWGRASS;
        }
        case B_CACTUS: { u8 g = getBlock(x, y - 1, z); return g == B_SAND || g == B_CACTUS; }
        case B_DOOR: {
            if (m & 8) return getBlock(x, y - 1, z) == B_DOOR;
            return getBlock(x, y + 1, z) == B_DOOR && BD[getBlock(x, y - 1, z)].solid;
        }
        default: return true;
    }
}

// ---------------------------------------------------------------- обработка отложенных обновлений
static bool fluidCanEnter(u8 n) { return n == B_AIR || (BD[n].replaceable && BD[n].shape != SH_FLUID); }
static void fluidUpdate(int x, int y, int z, u8 b, u8 m) {
    bool water = b == B_WATER;
    int step = water ? 1 : 2;
    // контакт воды и лавы
    if (!water) {
        for (int d = 0; d < 6; d++) {
            if (d == 3) continue;
            if (getBlock(x + DX[d], y + DY[d], z + DZ[d]) == B_WATER) {
                setBlock(x, y, z, ((m & 7) == 0 && !(m & 8)) ? B_OBSIDIAN : B_COBBLE);
                playSound(SND_FIZZ, 0.6f); spawnPuff(x + 0.5f, y + 1.f, z + 0.5f, 8, 0.8f, 0.8f, 0.8f);
                return;
            }
        }
    }
    bool source = (m & 7) == 0 && !(m & 8);
    if (!source) {
        u8 above = getBlock(x, y + 1, z);
        int nm;
        if (above == b) nm = 8;
        else {
            int best = 99, sources = 0;
            for (int h = 0; h < 4; h++) {
                int nx = x + HDX[h], nz = z + HDZ[h];
                if (getBlock(nx, y, nz) != b) continue;
                u8 mm = getMeta(nx, y, nz);
                int nl = (mm & 8) ? 0 : (mm & 7);
                if (nl == 0 && !(mm & 8)) sources++;
                best = std::min(best, nl + step);
            }
            nm = best;
            if (water && sources >= 2) {
                u8 below = getBlock(x, y - 1, z);
                if (BD[below].solid || (below == B_WATER && getMeta(x, y - 1, z) == 0)) nm = 0;
            }
        }
        if (nm > 7) { setBlock(x, y, z, B_AIR); return; }
        if (nm != m) { setBlock(x, y, z, b, (u8)nm); m = (u8)nm; }
    }
    // растекание
    u8 below = getBlock(x, y - 1, z);
    if (y > 0 && fluidCanEnter(below)) { setBlock(x, y - 1, z, b, 8); return; }
    if (y > 0 && below == B_LAVA && water) { scheduleUpdate(x, y - 1, z, 1); return; }
    if (below == b && (getMeta(x, y - 1, z) & 7) != 0) return;   // течёт вниз в поток
    int eff = (m & 8) ? 0 : (m & 7);
    int nl = eff + step;
    if (nl > 7) return;
    for (int h = 0; h < 4; h++) {
        int nx = x + HDX[h], nz = z + HDZ[h];
        if (!isLoaded(nx, nz)) continue;
        u8 n = getBlock(nx, y, nz);
        if (fluidCanEnter(n)) {
            setBlock(nx, y, nz, b, (u8)nl);
        } else if (n == b) {
            u8 mm = getMeta(nx, y, nz);
            if (!(mm & 8) && (mm & 7) > nl) setBlock(nx, y, nz, b, (u8)nl);
        }
    }
}
static void processUpdate(int x, int y, int z) {
    u8 b = getBlock(x, y, z); u8 m = getMeta(x, y, z);
    if (b == B_WATER || b == B_LAVA) { fluidUpdate(x, y, z, b, m); return; }
    if (b == B_SAND || b == B_GRAVEL) {
        u8 below = getBlock(x, y - 1, z);
        if (y > 0 && (below == B_AIR || BD[below].shape == SH_FLUID || (BD[below].replaceable && below != B_AIR))) {
            setBlock(x, y, z, B_AIR); setBlock(x, y - 1, z, b);
        }
        return;
    }
    if (!hasSupport(x, y, z, b, m)) {
        Rng r(x * 13 + y * 7 + z);
        if (g_mode == GM_SURVIVAL) {
            Stack s = (b == B_DOOR) ? ((m & 8) ? Stack() : mkStack(B_DOOR, 1)) : dropFor(b, r);
            if (!s.empty()) spawnDrop(s, x + 0.5, y + 0.3, z + 0.5);
        }
        setBlock(x, y, z, B_AIR);
    }
}
static void runScheduledUpdates() {
    int budget = 3000;
    while (!g_sched.empty() && budget > 0) {
        auto it = g_sched.begin();
        if (it->first > g_tick) break;
        std::vector<Upd> list; list.swap(it->second);
        g_sched.erase(it);
        for (size_t i = 0; i < list.size(); i++) {
            const Upd& u = list[i];
            g_pending.erase(posKey(u.x, u.y, u.z));
            if (budget-- <= 0) { scheduleUpdate(u.x, u.y, u.z, 1); continue; }
            if (!isLoaded(u.x, u.z)) continue;
            processUpdate(u.x, u.y, u.z);
        }
    }
}

// ---------------------------------------------------------------- рост саженцев
static bool growSaplingAt(int x, int y, int z) {
    if (getBlock(x, y, z) != B_SAPLING) return true;
    ColInfo ci = columnInfo(x, z);
    int kind = (ci.biome == BI_SNOW || ci.biome == BI_MOUNT) ? 1 : 0;
    Rng r(hash3i(x, y, z, g_seed) + SDL_GetTicks());
    setBlockRaw(x, y, z, B_AIR, 0);
    auto wset = [](int a, int b, int c, u8 id) { setBlockRaw(a, b, c, id, 0); };
    auto wget = [](int a, int b, int c) -> u8 { return getBlock(a, b, c); };
    if (growTree(kind, x, y, z, r, wset, wget)) {
        if (getBlock(x, y - 1, z) == B_GRASS) setBlockRaw(x, y - 1, z, B_DIRT, 0);
        return true;
    }
    setBlockRaw(x, y, z, B_SAPLING, 0);
    return false;
}
static void tickGrowth(float dt) {
    for (size_t i = 0; i < g_grow.size();) {
        Grow& g = g_grow[i];
        g.t -= dt;
        if (g.t <= 0) {
            if (!isLoaded(g.x, g.z)) { g.t = 10; i++; continue; }
            if (getBlock(g.x, g.y, g.z) != B_SAPLING || growSaplingAt(g.x, g.y, g.z)) { g_grow[i] = g_grow.back(); g_grow.pop_back(); continue; }
            g.t = 30;
        }
        i++;
    }
}

// ============================================================================
//  ИГРОК: управление, физика, взаимодействие; отрисовка сущностей; сохранение
// ============================================================================
struct Input {
    float mx = 0, mz = 0;         // стрейф / вперёд, -1..1
    bool jump = false, jumpPressed = false, dig = false, digPressed = false, place = false, placePressed = false, down = false;
    bool sprint = false;
};
static Input g_in;
static bool g_autoJump = true;
static Hit g_target; static int g_targetMob = -1;
static struct { bool active = false; int x = 0, y = 0, z = 0; float prog = 0; float sndT = 0; } g_dig;
static float g_attackCd = 0, g_placeCd = 0;

static int yawToHdir(float yaw) {
    float fx = sinf(yaw), fz = cosf(yaw);
    if (fabsf(fx) > fabsf(fz)) return fx > 0 ? 0 : 2;
    return fz > 0 ? 1 : 3;
}
static bool playerIntersects(int x, int y, int z) {
    if (P.x + PL_HW <= x || P.x - PL_HW >= x + 1 || P.z + PL_HW <= z || P.z - PL_HW >= z + 1 || P.y + PL_H <= y || P.y >= y + 1) return false;
    return true;
}
static bool anyMobIntersects(int x, int y, int z) {
    for (const Mob& m : g_mobs) {
        const MobDef& d = MD[m.type];
        if (m.x + d.hw <= x || m.x - d.hw >= x + 1 || m.z + d.hw <= z || m.z - d.hw >= z + 1 || m.y + d.h <= y || m.y >= y + 1) continue;
        return true;
    }
    return false;
}

// открытие интерфейсов (реализовано в части интерфейса)
static void openCraftingTable();
static void openFurnace(int x, int y, int z);
static void openChest(int x, int y, int z);
static void sleepInBed(int x, int y, int z);

static void respawnPlayer() {
    P.dead = false; P.health = 20; P.hunger = 20; P.air = 10; P.exhaustion = 0;
    P.x = P.spawnX + 0.5; P.y = P.spawnY + 0.01; P.z = P.spawnZ + 0.5; P.vx = P.vy = P.vz = 0; P.falling = false;
    P.hurtCd = 1.f;
}

static void updatePlayerPhysics(float dt) {
    if (P.dead) return;
    P.hurtCd = std::max(0.f, P.hurtCd - dt);
    P.hurtFlash = std::max(0.f, P.hurtFlash - dt);
    int fx = ifloor(P.x), fz = ifloor(P.z);
    u8 feet = getBlock(fx, ifloor(P.y + 0.1), fz), waist = getBlock(fx, ifloor(P.y + 0.9), fz), head = getBlock(fx, ifloor(P.y + PL_EYE), fz);
    P.inWater = feet == B_WATER || waist == B_WATER;
    P.headInWater = head == B_WATER;
    P.inLava = feet == B_LAVA || waist == B_LAVA;
    P.onLadder = feet == B_LADDER || waist == B_LADDER;
    float yaw = g_cam.yaw;
    float fwx = sinf(yaw), fwz = cosf(yaw), rx = cosf(yaw), rz = -sinf(yaw);
    float mx = g_in.mx, mz = g_in.mz;
    float ml = sqrtf(mx * mx + mz * mz); if (ml > 1) { mx /= ml; mz /= ml; ml = 1; }
    bool creative = g_mode == GM_CREATIVE;
    if (!creative) P.flying = false;
    if (P.flying) {
        float sp = g_in.sprint ? 16.f : 10.f;
        float wx = (fwx * mz + rx * mx) * sp, wz = (fwz * mz + rz * mx) * sp;
        float wy = (g_in.jump ? 8.f : 0.f) - (g_in.down ? 8.f : 0.f);
        float k = std::min(1.f, dt * 8.f);
        P.vx += (wx - P.vx) * k; P.vz += (wz - P.vz) * k; P.vy += (wy - P.vy) * k;
        int res = moveBody(P.x, P.y, P.z, P.vx, P.vy, P.vz, PL_HW, PL_H, dt);
        if (res & 2) P.flying = false;
        P.onGround = false; P.falling = false;
        return;
    }
    float speed = 4.3f * (g_in.sprint && mz > 0.5f ? 1.3f : 1.f);
    if (P.inWater) speed = 2.2f;
    if (P.inLava) speed = 1.4f;
    float wx = (fwx * mz + rx * mx) * speed, wz = (fwz * mz + rz * mx) * speed;
    float acc = P.onGround ? 14.f : (P.inWater ? 6.f : 3.5f);
    float k = std::min(1.f, dt * acc);
    P.vx += (wx - P.vx) * k; P.vz += (wz - P.vz) * k;
    if (P.inWater || P.inLava) {
        P.vy -= 9.f * dt;
        if (g_in.jump) P.vy += 22.f * dt;
        P.vy *= expf(-2.5f * dt);
        if (P.vy > 3.5f) P.vy = 3.5f;
        if (P.vy < -4.f) P.vy = -4.f;
        P.falling = false;
    } else if (P.onLadder) {
        bool climb = g_in.jump || ml > 0.2f;
        P.vy = climb ? 2.8f : std::max(P.vy - 28.f * dt, -2.0f);
        P.falling = false;
    } else {
        P.vy -= 28.f * dt;
        if (P.vy < -55.f) P.vy = -55.f;
    }
    if (g_in.jump && P.onGround && !P.inWater) { P.vy = 8.6f; P.exhaustion += 0.05f; }
    bool wasGround = P.onGround;
    double oldX = P.x, oldZ = P.z;
    int res = moveBody(P.x, P.y, P.z, P.vx, P.vy, P.vz, PL_HW, PL_H, dt);
    P.onGround = (res & 2) != 0;
    // автопрыжок на ступеньку в 1 блок
    if (g_autoJump && (res & 1) && wasGround && ml > 0.3f && !P.inWater) {
        int tx = ifloor(P.x + fwx * mz * 0.6f + rx * mx * 0.6f), tz = ifloor(P.z + fwz * mz * 0.6f + rz * mx * 0.6f);
        int ty = ifloor(P.y + 0.01);
        if (BD[getBlock(tx, ty, tz)].solid && !BD[getBlock(tx, ty + 1, tz)].solid && !BD[getBlock(tx, ty + 2, tz)].solid && !BD[getBlock(ifloor(P.x), ty + 2, ifloor(P.z))].solid)
            P.vy = 8.6f;
    }
    if (P.inWater && (res & 1) && g_in.jump) P.vy = 4.f;   // выбраться из воды на берег
    // падение
    if (!P.onGround && !P.inWater && !P.onLadder) {
        if (!P.falling) { P.falling = true; P.fallStartY = (float)P.y; }
        P.fallStartY = std::max(P.fallStartY, (float)P.y);
    }
    if (P.onGround && P.falling) {
        float h = P.fallStartY - (float)P.y;
        if (h > 3.4f) { playerHurt(floorf(h - 3.f), 1e9, 0, true); playSound(SND_HIT, 0.7f, 0.7f); }
        P.falling = false;
    }
    if (P.inWater || P.onLadder) P.falling = false;
    // шаги и покачивание
    float moved = (float)sqrt((P.x - oldX) * (P.x - oldX) + (P.z - oldZ) * (P.z - oldZ));
    if (P.onGround && moved > 0.001f) {
        P.stepDist += moved; P.bobPhase += moved * 2.4f;
        P.bobAmt = std::min(1.f, P.bobAmt + dt * 4);
        P.exhaustion += moved * (g_in.sprint ? 0.08f : 0.01f);
        if (P.stepDist > 2.0f) {
            P.stepDist = 0;
            u8 under = getBlock(ifloor(P.x), ifloor(P.y - 0.2), ifloor(P.z));
            if (under != B_AIR) playSound(SND_STEP0 + BD[under].sound, 0.45f, 0.9f + (SDL_GetTicks() % 25) / 100.f);
        }
    } else P.bobAmt = std::max(0.f, P.bobAmt - dt * 4);
    if (P.inWater && !wasGround && moved > 0.05f && (SDL_GetTicks() / 400) % 3 == 0 && P.stepDist > 1.5f) { P.stepDist = 0; playSound(SND_SPLASH, 0.25f); }
    else if (P.inWater) P.stepDist += moved;
}

static void updateSurvival(float dt) {
    if (P.dead || g_mode != GM_SURVIVAL) return;
    P.exhaustion += dt * 0.004f;
    while (P.exhaustion >= 4.f) { P.exhaustion -= 4.f; P.hunger = std::max(0.f, P.hunger - 1.f); }
    if (P.hunger >= 18 && P.health < 20) {
        P.regenT += dt;
        if (P.regenT >= 3.f) { P.regenT = 0; P.health = std::min(20.f, P.health + 1); P.exhaustion += 1.5f; }
    } else P.regenT = 0;
    if (P.hunger <= 0) {
        P.starveT += dt;
        if (P.starveT >= 4.f) { P.starveT = 0; if (P.health > 1) playerHurt(1, 1e9, 0, true); }
    }
    if (P.headInWater) {
        P.air -= dt;
        if (P.air < 0) { P.air = 0; P.drownT += dt; if (P.drownT > 1.f) { P.drownT = 0; playerHurt(2, 1e9, 0, true); } }
    } else { P.air = std::min(10.f, P.air + dt * 4.f); P.drownT = 0; }
    if (P.inLava) playerHurt(4);
    if (boxTouchesBlock(P.x, P.y, P.z, PL_HW, PL_H, B_CACTUS, 0.04f)) { P.cactusT += dt; if (P.cactusT > 0.5f) { P.cactusT = 0; playerHurt(1); } }
    if (P.y < -16) playerHurt(4, 1e9, 0, true);
}

static bool eatHeld() {
    Stack& s = heldStack();
    int fv = foodValue(s.id);
    if (!fv) return false;
    if (g_mode == GM_SURVIVAL && P.hunger >= 20) return false;
    P.hunger = std::min(20.f, P.hunger + fv);
    if (s.id == I_ROTTEN && (SDL_GetTicks() % 3) == 0) { P.hunger = std::max(0.f, P.hunger - 2); showMsg("ФУ, ГНИЛОЕ..."); }
    if (g_mode == GM_SURVIVAL) { s.cnt--; if (s.cnt == 0) s.clear(); }
    playSound(SND_EAT, 0.7f);
    return true;
}

static bool tryPlace(const Hit& h) {
    Stack& s = heldStack();
    if (s.empty()) return false;
    int id = s.id;
    if (id == I_BONEMEAL) {
        u8 b = getBlock(h.x, h.y, h.z);
        bool used = false;
        if (b == B_SAPLING) { used = growSaplingAt(h.x, h.y, h.z); spawnPuff(h.x + 0.5f, h.y + 0.3f, h.z + 0.5f, 8, 0.6f, 1.f, 0.5f); }
        else if (b == B_GRASS) {
            Rng r(SDL_GetTicks());
            for (int i = 0; i < 12; i++) {
                int x = h.x + r.range(-3, 3), z = h.z + r.range(-3, 3);
                for (int y = h.y + 2; y >= h.y - 2; y--) if (getBlock(x, y, z) == B_GRASS && getBlock(x, y + 1, z) == B_AIR) {
                    int v = r.range(100); setBlock(x, y + 1, z, v < 70 ? B_TALLGRASS : (v < 85 ? B_FLOWER_R : B_FLOWER_Y)); break;
                }
            }
            used = true; spawnPuff(h.x + 0.5f, h.y + 1.f, h.z + 0.5f, 10, 0.6f, 1.f, 0.5f);
        }
        if (used && g_mode == GM_SURVIVAL) { s.cnt--; if (!s.cnt) s.clear(); }
        return used;
    }
    if (!isBlockItem(id)) return false;
    int nx = DX[h.face], ny = DY[h.face], nz = DZ[h.face];
    int tx = h.x + nx, ty = h.y + ny, tz = h.z + nz;
    u8 hb = getBlock(h.x, h.y, h.z);
    if (BD[hb].replaceable && hb != B_WATER && hb != B_LAVA) { tx = h.x; ty = h.y; tz = h.z; }
    if (ty < 1 || ty >= WH - 1) return false;
    u8 cur = getBlock(tx, ty, tz);
    if (!BD[cur].replaceable) return false;
    const BlockDef& d = BD[id];
    if (d.solid && (playerIntersects(tx, ty, tz) || anyMobIntersects(tx, ty, tz))) return false;
    int pf = yawToHdir(g_cam.yaw);
    u8 meta = 0;
    switch (id) {
        case B_TORCH: {
            if (h.face == 3) return false;
            if (h.face == 2) { if (!BD[getBlock(tx, ty - 1, tz)].opaque) return false; meta = 0; }
            else { int wall = -1; for (int k = 0; k < 4; k++) if (HDX[k] == -nx && HDZ[k] == -nz) wall = k; if (wall < 0 || !BD[getBlock(tx + HDX[wall], ty, tz + HDZ[wall])].opaque) return false; meta = (u8)(wall + 1); }
            break;
        }
        case B_LADDER: {
            if (h.face == 2 || h.face == 3) return false;
            int wall = -1; for (int k = 0; k < 4; k++) if (HDX[k] == -nx && HDZ[k] == -nz) wall = k;
            if (wall < 0 || !BD[getBlock(tx + HDX[wall], ty, tz + HDZ[wall])].opaque) return false;
            meta = (u8)wall; break;
        }
        case B_DOOR: {
            if (!BD[getBlock(tx, ty - 1, tz)].solid || !BD[getBlock(tx, ty + 1, tz)].replaceable || playerIntersects(tx, ty + 1, tz)) return false;
            setBlock(tx, ty, tz, B_DOOR, (u8)pf); setBlock(tx, ty + 1, tz, B_DOOR, (u8)(pf | 8));
            playSound(SND_PLACE, 0.7f);
            if (g_mode == GM_SURVIVAL) { s.cnt--; if (!s.cnt) s.clear(); }
            return true;
        }
        case B_BED: {
            int hx = tx + HDX[pf], hz = tz + HDZ[pf];
            if (!BD[getBlock(tx, ty - 1, tz)].solid || !BD[getBlock(hx, ty - 1, hz)].solid || !BD[getBlock(hx, ty, hz)].replaceable || playerIntersects(hx, ty, hz)) return false;
            setBlock(tx, ty, tz, B_BED, (u8)pf); setBlock(hx, ty, hz, B_BED, (u8)(pf | 4));
            playSound(SND_PLACE, 0.7f);
            if (g_mode == GM_SURVIVAL) { s.cnt--; if (!s.cnt) s.clear(); }
            return true;
        }
        case B_FURNACE: case B_CHEST: meta = (u8)((pf + 2) & 3); break;
        case B_SAPLING: case B_FLOWER_R: case B_FLOWER_Y: case B_TALLGRASS: {
            u8 g = getBlock(tx, ty - 1, tz); if (g != B_GRASS && g != B_DIRT && g != B_SNOWGRASS) return false; break;
        }
        case B_CACTUS: { u8 g = getBlock(tx, ty - 1, tz); if (g != B_SAND && g != B_CACTUS) return false; break; }
        case B_WATER: case B_LAVA: meta = 0; break;
    }
    setBlock(tx, ty, tz, (u8)id, meta);
    if (id == B_SAPLING) { Grow g; g.x = tx; g.y = ty; g.z = tz; g.t = 40.f + (SDL_GetTicks() % 60); g_grow.push_back(g); }
    if (id == B_CHEST) g_chests[posKey(tx, ty, tz)] = std::vector<Stack>(27);
    if (id == B_FURNACE) g_furnaces[posKey(tx, ty, tz)] = Furnace();
    // дёрн под блоком превращается в землю
    if (BD[id].opaque && getBlock(tx, ty - 1, tz) == B_GRASS) setBlock(tx, ty - 1, tz, B_DIRT);
    playSound(SND_PLACE, 0.7f, 0.9f + (SDL_GetTicks() % 20) / 100.f);
    playSound(SND_STEP0 + BD[id].sound, 0.4f);
    if (g_mode == GM_SURVIVAL) { s.cnt--; if (!s.cnt) s.clear(); }
    return true;
}

static bool tryInteract(const Hit& h) {
    u8 b = getBlock(h.x, h.y, h.z);
    switch (b) {
        case B_CRAFT: openCraftingTable(); return true;
        case B_FURNACE: case B_FURNACE_LIT: openFurnace(h.x, h.y, h.z); return true;
        case B_CHEST: openChest(h.x, h.y, h.z); return true;
        case B_DOOR: {
            u8 m = getMeta(h.x, h.y, h.z);
            int by = (m & 8) ? h.y - 1 : h.y;
            if (getBlock(h.x, by, h.z) != B_DOOR) return true;
            u8 lm = getMeta(h.x, by, h.z) ^ 4;
            setBlockRaw(h.x, by, h.z, B_DOOR, lm & ~8);
            if (getBlock(h.x, by + 1, h.z) == B_DOOR) setBlockRaw(h.x, by + 1, h.z, B_DOOR, (lm & ~8) | 8);
            playSound(SND_DIG0 + SM_WOOD, 0.7f, (lm & 4) ? 0.8f : 1.1f);
            return true;
        }
        case B_BED: sleepInBed(h.x, h.y, h.z); return true;
    }
    return false;
}

static void updateInteraction(float dt) {
    g_attackCd = std::max(0.f, g_attackCd - dt);
    g_placeCd = std::max(0.f, g_placeCd - dt);
    P.swing = std::max(0.f, P.swing - dt * 3.f);
    if (P.dead) { g_target.hit = false; g_targetMob = -1; return; }
    double ex = P.x, ey = P.y + PL_EYE, ez = P.z;
    float reach = g_mode == GM_CREATIVE ? 6.f : 5.f;
    g_target = raycast(ex, ey, ez, g_cam.f[0], g_cam.f[1], g_cam.f[2], reach);
    float mt; g_targetMob = rayMob(ex, ey, ez, g_cam.f[0], g_cam.f[1], g_cam.f[2], reach - 1.5f, mt);
    if (g_targetMob >= 0 && g_target.hit && g_target.dist < mt) g_targetMob = -1;
    Stack& held = heldStack();
    // касание по существу — удар
    if (g_in.placePressed && g_targetMob >= 0 && !g_in.dig) { g_in.dig = true; g_in.placePressed = false; g_in.place = false; g_attackCd = 0; }
    // удар/копание
    if (g_in.dig) {
        if (g_targetMob >= 0) {
            g_dig.active = false;
            if (g_attackCd <= 0) {
                Mob& m = g_mobs[g_targetMob];
                float dmg = 1;
                if (isTool(held.id)) dmg = toolKind(held.id) == TK_SWORD ? SWORD_DMG[toolMat(held.id)] : 2.f + toolMat(held.id) * 0.5f;
                if (g_mode == GM_CREATIVE) dmg = 100;
                mobHurt(m, dmg, P.x, P.z);
                damageTool(held, isTool(held.id) && toolKind(held.id) == TK_SWORD ? 1 : 2);
                P.exhaustion += 0.1f; P.swing = 1.f; g_attackCd = 0.45f;
            }
        } else if (g_target.hit) {
            if (!g_dig.active || g_dig.x != g_target.x || g_dig.y != g_target.y || g_dig.z != g_target.z) {
                g_dig.active = true; g_dig.x = g_target.x; g_dig.y = g_target.y; g_dig.z = g_target.z; g_dig.prog = 0; g_dig.sndT = 0;
            }
            u8 b = getBlock(g_dig.x, g_dig.y, g_dig.z);
            if (P.swing < 0.3f) P.swing = 1.f;
            if (g_mode == GM_CREATIVE) {
                if (g_attackCd <= 0 && BD[b].hardness >= 0) { breakBlockAt(g_dig.x, g_dig.y, g_dig.z, false); g_attackCd = 0.22f; g_dig.active = false; }
            } else {
                bool harvest; float bt = breakTime(b, held, &harvest);
                g_dig.prog += dt / bt;
                g_dig.sndT -= dt;
                if (g_dig.sndT <= 0) {
                    g_dig.sndT = 0.25f; playSound(SND_DIG0 + BD[b].sound, 0.5f, 0.85f + (SDL_GetTicks() % 30) / 100.f);
                    spawnBreakParticles(g_dig.x, g_dig.y, g_dig.z, BD[b].tex[2], 3);
                }
                if (g_dig.prog >= 1.f) {
                    breakBlockAt(g_dig.x, g_dig.y, g_dig.z, harvest);
                    if (BD[b].hardness > 0) damageTool(held, (isTool(held.id) && toolKind(held.id) == TK_SWORD) ? 2 : 1);
                    P.exhaustion += 0.005f;
                    g_dig.active = false;
                }
            }
        } else {
            g_dig.active = false;
            if (g_in.digPressed) { P.swing = 1.f; playSound(SND_SWING, 0.4f); }
        }
    } else g_dig.active = false;
    // лук: удерживать «поставить» — натянуть, отпустить — выстрел
    if (held.id == I_BOW) {
        bool hasArrow = g_mode == GM_CREATIVE || countItem(I_ARROW) > 0;
        u8 tb = g_target.hit ? getBlock(g_target.x, g_target.y, g_target.z) : 0;
        bool usable = tb == B_CRAFT || tb == B_CHEST || tb == B_FURNACE || tb == B_FURNACE_LIT || tb == B_DOOR || tb == B_BED;
        if (g_in.placePressed && usable && P.bowCharge <= 0) { tryInteract(g_target); g_placeCd = 0.28f; return; }
        if (g_in.place && hasArrow) {
            if (P.bowCharge <= 0) playSound(SND_SWING, 0.3f, 0.6f);
            P.bowCharge = std::min(1.f, P.bowCharge + dt * 1.1f);
        } else if (!g_in.place && P.bowCharge > 0) {
            if (P.bowCharge > 0.12f && hasArrow) {
                float ch = P.bowCharge, sp = 8.f + 26.f * ch;
                const float* f = g_cam.f;
                spawnArrow(P.x + f[0] * 0.4, P.y + PL_EYE - 0.1 + f[1] * 0.4, P.z + f[2] * 0.4, f[0] * sp, f[1] * sp, f[2] * sp, 1.5f + 7.5f * ch * ch, true);
                if (g_mode == GM_SURVIVAL) removeItem(I_ARROW, 1);
                playSound(SND_BOW, 0.8f, 0.9f + ch * 0.3f);
                P.exhaustion += 0.05f;
            }
            P.bowCharge = 0;
        } else if (g_in.placePressed && !hasArrow) showMsg("НЕТ СТРЕЛ");
        return;
    }
    P.bowCharge = 0;
    // установка / использование
    bool isFood = foodValue(held.id) > 0;
    if (g_in.place && isFood && !(g_target.hit && (getBlock(g_target.x, g_target.y, g_target.z) == B_CRAFT || getBlock(g_target.x, g_target.y, g_target.z) == B_CHEST))) {
        if (g_mode == GM_SURVIVAL && P.hunger >= 20) { if (g_in.placePressed) showMsg("ВЫ НЕ ГОЛОДНЫ"); }
        else {
            P.eatT += dt; P.swing = 0.5f + 0.3f * sinf(P.eatT * 20.f);
            if (fmodf(P.eatT, 0.3f) < dt) playSound(SND_EAT, 0.3f, 1.2f);
            if (P.eatT >= 1.0f) { eatHeld(); P.eatT = 0; }
        }
        return;
    }
    P.eatT = 0;
    if (g_in.placePressed || (g_in.place && g_placeCd <= 0)) {
        if (g_target.hit) {
            bool done = false;
            if (g_in.placePressed) done = tryInteract(g_target);
            if (!done) done = tryPlace(g_target);
            if (done) { P.swing = 1.f; g_placeCd = 0.28f; }
            else if (g_in.placePressed) g_placeCd = 0.28f;
        }
        if (g_in.placePressed && g_placeCd <= 0) g_placeCd = 0.28f;
    }
}

// ---------------------------------------------------------------- выпавшие предметы и частицы
static void updateDrops(float dt) {
    for (size_t i = 0; i < g_drops.size();) {
        Drop& d = g_drops[i];
        d.age += dt;
        bool inW = getBlock(ifloor(d.x), ifloor(d.y), ifloor(d.z)) == B_WATER;
        d.vy -= (inW ? 4.f : 20.f) * dt;
        if (inW) d.vy = std::max(d.vy, -1.f);
        float fr = expf(-(d.onGround ? 8.f : 1.f) * dt); d.vx *= fr; d.vz *= fr;
        int res = moveBody(d.x, d.y, d.z, d.vx, d.vy, d.vz, 0.12f, 0.24f, dt);
        d.onGround = (res & 2) != 0;
        // подбор
        if (!P.dead && d.age > 0.6f) {
            double dx = P.x - d.x, dy = (P.y + 0.8) - d.y, dz = P.z - d.z;
            double dist = sqrt(dx * dx + dy * dy + dz * dz);
            // зона подбора — тело игрока, расширенное на блок в стороны и вниз (подбирается и то, что упало в ямку у ног)
            bool inBox = fabs(P.x - d.x) < PL_HW + 1.0 && fabs(P.z - d.z) < PL_HW + 1.0 && d.y > P.y - 1.05 && d.y < P.y + PL_H + 0.5;
            if (inBox) {
                int left = addItem(d.s);
                if (left < d.s.cnt) { playSound(SND_PICKUP, 0.5f, 0.9f + (SDL_GetTicks() % 30) / 100.f); notePickup(d.s.id, d.s.cnt - left); }
                d.s.cnt = (u16)left;
            } else if (dist < 3.0 && d.age > 0.6f) { d.vx += (float)(dx / dist) * dt * 10.f; d.vz += (float)(dz / dist) * dt * 10.f; }
        }
        if (d.s.cnt == 0 || d.age > 300.f || d.y < -20) { g_drops[i] = g_drops.back(); g_drops.pop_back(); continue; }
        i++;
    }
}
static void updateParticles(float dt) {
    for (size_t i = 0; i < g_parts.size();) {
        Particle& p = g_parts[i];
        p.life -= dt;
        if (p.glow) { p.vy += 1.f * dt; } else p.vy -= 14.f * dt;
        float nx = p.x + p.vx * dt, ny = p.y + p.vy * dt, nz = p.z + p.vz * dt;
        if (BD[getBlock(ifloor(nx), ifloor(ny), ifloor(nz))].solid) { p.vx *= 0.3f; p.vz *= 0.3f; p.vy = 0; }
        else { p.x = nx; p.y = ny; p.z = nz; }
        if (p.life <= 0) { g_parts[i] = g_parts.back(); g_parts.pop_back(); continue; }
        i++;
    }
    if (g_parts.size() > 600) g_parts.erase(g_parts.begin(), g_parts.begin() + (g_parts.size() - 600));
}

// ---------------------------------------------------------------- отрисовка сущностей
static void lightColorAt(double x, double y, double z, float* out) {
    int sky, bl; getLightAt(ifloor(x), ifloor(y), ifloor(z), sky, bl);
    litColor((float)sky, (float)bl, 1.f, out);
}
// коробка с углами в мировых координатах относительно камеры (camSpace=false) или в пространстве камеры
static void emitBox8(const float (*corner)[3], const int tiles[6], const float col[3], float alpha, bool camSpace, float keyBias = 0.f) {
    static const int FC[6][4] = {{3, 7, 5, 1}, {6, 2, 0, 4}, {2, 3, 7, 6}, {4, 5, 1, 0}, {7, 6, 4, 5}, {2, 3, 1, 0}};
    static const float U[4] = {0, 16, 16, 0}, V[4] = {0, 0, 16, 16};
    for (int d = 0; d < 6; d++) {
        if (tiles[d] < 0) continue;
        const float* a = corner[FC[d][0]]; const float* b = corner[FC[d][1]]; const float* c = corner[FC[d][3]];
        float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        if (n[0] * a[0] + n[1] * a[1] + n[2] * a[2] >= 0) continue;   // нормаль наружу, камера в начале координат
        float W[4][3], UV[4][2], C[4][4];
        float sh = FACE_SHADE[d];
        for (int k = 0; k < 4; k++) {
            memcpy(W[k], corner[FC[d][k]], 12);
            tileUV(tiles[d], U[k], V[k], UV[k]);
            C[k][0] = col[0] * sh; C[k][1] = col[1] * sh; C[k][2] = col[2] * sh; C[k][3] = alpha;
        }
        if (camSpace) emitQuadCam(W, UV, C, keyBias); else emitQuadRel(W, UV, C, keyBias);
    }
}

struct MPart { float x, y, z, sx, sy, sz; int tf, ts, tt; int swing; };
static int mobParts(int type, MPart* p) {
    int n = 0;
    auto add = [&](float x, float y, float z, float sx, float sy, float sz, int tf, int ts, int tt, int sw) {
        MPart m = {x, y, z, sx, sy, sz, tf, ts, tt, sw}; p[n++] = m; };
    switch (type) {
        case MOB_TOPTUN:
            add(0, 0.85f, 0, 0.8f, 0.6f, 1.2f, T_TOPTUN_SIDE, T_TOPTUN_SIDE, T_TOPTUN_SIDE, 0);
            add(0, 1.05f, 0.78f, 0.56f, 0.5f, 0.42f, T_TOPTUN_FACE, T_TOPTUN_SIDE, T_TOPTUN_SIDE, 0);
            add(0, 1.4f, 0.86f, 0.1f, 0.22f, 0.1f, T_HORN, T_HORN, T_HORN, 0);
            add(0.25f, 0.28f, 0.4f, 0.22f, 0.56f, 0.22f, T_TOPTUN_LEG, T_TOPTUN_LEG, T_TOPTUN_LEG, 1);
            add(-0.25f, 0.28f, 0.4f, 0.22f, 0.56f, 0.22f, T_TOPTUN_LEG, T_TOPTUN_LEG, T_TOPTUN_LEG, 2);
            add(0.25f, 0.28f, -0.4f, 0.22f, 0.56f, 0.22f, T_TOPTUN_LEG, T_TOPTUN_LEG, T_TOPTUN_LEG, 2);
            add(-0.25f, 0.28f, -0.4f, 0.22f, 0.56f, 0.22f, T_TOPTUN_LEG, T_TOPTUN_LEG, T_TOPTUN_LEG, 1);
            break;
        case MOB_PUKH:
            add(0, 0.44f, 0, 0.56f, 0.5f, 0.56f, T_PUKH_FACE, T_PUKH_BODY, T_PUKH_BODY, 0);
            add(0.16f, 0.74f, -0.05f, 0.1f, 0.14f, 0.1f, T_PUKH_BODY, T_PUKH_BODY, T_PUKH_BODY, 0);
            add(-0.16f, 0.74f, -0.05f, 0.1f, 0.14f, 0.1f, T_PUKH_BODY, T_PUKH_BODY, T_PUKH_BODY, 0);
            add(0.13f, 0.1f, 0, 0.1f, 0.2f, 0.1f, T_PUKH_LEG, T_PUKH_LEG, T_PUKH_LEG, 1);
            add(-0.13f, 0.1f, 0, 0.1f, 0.2f, 0.1f, T_PUKH_LEG, T_PUKH_LEG, T_PUKH_LEG, 2);
            break;
        case MOB_GNIL:   // пень на корнях
            add(0, 0.78f, 0, 0.8f, 0.9f, 0.8f, T_GNIL_FACE, T_GNIL_BODY, T_GNIL_TOP, 0);
            add(0.2f, 1.30f, 0.08f, 0.34f, 0.12f, 0.34f, T_MUSHCAP, T_MUSHCAP, T_MUSHCAP, 0);
            add(-0.18f, 1.27f, -0.15f, 0.24f, 0.1f, 0.24f, T_MUSHCAP, T_MUSHCAP, T_MUSHCAP, 0);
            add(0.2f, 1.25f, 0.08f, 0.08f, 0.1f, 0.08f, T_HORN, T_HORN, T_HORN, 0);
            add(0.22f, 0.17f, 0.02f, 0.28f, 0.34f, 0.3f, T_GNIL_LEG, T_GNIL_LEG, T_GNIL_LEG, 1);
            add(-0.22f, 0.17f, 0.02f, 0.28f, 0.34f, 0.3f, T_GNIL_LEG, T_GNIL_LEG, T_GNIL_LEG, 2);
            add(0.52f, 0.72f, 0.05f, 0.2f, 0.62f, 0.2f, T_GNIL_LEG, T_GNIL_LEG, T_GNIL_LEG, 2);
            add(-0.52f, 0.72f, 0.05f, 0.2f, 0.62f, 0.2f, T_GNIL_LEG, T_GNIL_LEG, T_GNIL_LEG, 1);
            break;
        case MOB_TEN:    // зверь тени
            add(0, 0.68f, -0.05f, 0.56f, 0.46f, 0.95f, T_TEN_BODY, T_TEN_BODY, T_TEN_BODY, 0);
            add(0, 0.84f, 0.6f, 0.54f, 0.5f, 0.42f, T_TEN_FACE, T_TEN_BODY, T_TEN_BODY, 0);
            add(0, 1.0f, 0.2f, 0.1f, 0.22f, 0.1f, T_TEN_SPIKE, T_TEN_SPIKE, T_TEN_SPIKE, 9);
            add(0, 1.0f, -0.12f, 0.12f, 0.28f, 0.12f, T_TEN_SPIKE, T_TEN_SPIKE, T_TEN_SPIKE, 9);
            add(0, 0.98f, -0.42f, 0.1f, 0.2f, 0.1f, T_TEN_SPIKE, T_TEN_SPIKE, T_TEN_SPIKE, 9);
            add(0, 0.76f, -0.72f, 0.1f, 0.1f, 0.42f, T_TEN_BODY, T_TEN_BODY, T_TEN_BODY, 0);
            add(0.18f, 0.23f, 0.3f, 0.16f, 0.46f, 0.16f, T_TEN_LEG, T_TEN_LEG, T_TEN_LEG, 1);
            add(-0.18f, 0.23f, 0.3f, 0.16f, 0.46f, 0.16f, T_TEN_LEG, T_TEN_LEG, T_TEN_LEG, 2);
            add(0.18f, 0.23f, -0.35f, 0.16f, 0.46f, 0.16f, T_TEN_LEG, T_TEN_LEG, T_TEN_LEG, 2);
            add(-0.18f, 0.23f, -0.35f, 0.16f, 0.46f, 0.16f, T_TEN_LEG, T_TEN_LEG, T_TEN_LEG, 1);
            break;
        case MOB_ZOMBIE:   // сутулый, руки вытянуты вперёд
            add(0.12f, 0.38f, 0, 0.22f, 0.76f, 0.24f, T_ZOMB_PANTS, T_ZOMB_PANTS, T_ZOMB_PANTS, 1);
            add(-0.12f, 0.38f, 0, 0.22f, 0.76f, 0.24f, T_ZOMB_PANTS, T_ZOMB_PANTS, T_ZOMB_PANTS, 2);
            add(0, 1.1f, 0.03f, 0.48f, 0.7f, 0.26f, T_ZOMB_SHIRT, T_ZOMB_SHIRT, T_ZOMB_SHIRT, 0);
            add(0, 1.66f, 0.08f, 0.44f, 0.44f, 0.44f, T_ZOMB_FACE, T_ZOMB_SKIN, T_ZOMB_SKIN, 0);
            add(0.34f, 1.34f, 0.32f, 0.2f, 0.2f, 0.66f, T_ZOMB_SKIN, T_ZOMB_SHIRT, T_ZOMB_SHIRT, 0);
            add(-0.34f, 1.30f, 0.30f, 0.2f, 0.2f, 0.62f, T_ZOMB_SKIN, T_ZOMB_SHIRT, T_ZOMB_SHIRT, 0);
            break;
        case MOB_SKELETON:   // костяк в багряном капюшоне, с луком
            add(0.1f, 0.39f, 0, 0.12f, 0.78f, 0.12f, T_SKEL_BONE, T_SKEL_BONE, T_SKEL_BONE, 1);
            add(-0.1f, 0.39f, 0, 0.12f, 0.78f, 0.12f, T_SKEL_BONE, T_SKEL_BONE, T_SKEL_BONE, 2);
            add(0, 0.82f, 0, 0.36f, 0.1f, 0.18f, T_SKEL_BONE, T_SKEL_BONE, T_SKEL_BONE, 0);
            add(0, 1.18f, 0, 0.44f, 0.62f, 0.2f, T_SKEL_RIBS, T_SKEL_BONE, T_SKEL_BONE, 0);
            add(0, 1.50f, 0, 0.5f, 0.08f, 0.28f, T_SKEL_HOOD, T_SKEL_HOOD, T_SKEL_HOOD, 0);
            add(0, 1.73f, 0, 0.44f, 0.42f, 0.44f, T_SKEL_FACE, T_SKEL_BONE, T_SKEL_BONE, 0);
            add(0, 1.88f, -0.05f, 0.52f, 0.2f, 0.44f, T_SKEL_HOOD, T_SKEL_HOOD, T_SKEL_HOOD, 0);
            add(0, 1.72f, -0.24f, 0.52f, 0.4f, 0.08f, T_SKEL_HOOD, T_SKEL_HOOD, T_SKEL_HOOD, 0);
            add(0.3f, 1.3f, 0.22f, 0.1f, 0.1f, 0.56f, T_SKEL_BONE, T_SKEL_BONE, T_SKEL_BONE, 0);
            add(-0.3f, 1.3f, 0.22f, 0.1f, 0.1f, 0.56f, T_SKEL_BONE, T_SKEL_BONE, T_SKEL_BONE, 0);
            add(0.3f, 1.3f, 0.52f, 0.05f, 0.8f, 0.06f, T_BOWWOOD, T_BOWWOOD, T_BOWWOOD, 0);
            break;
        case MOB_SPIDER: {   // восемь суставчатых лап
            add(0, 0.48f, 0.22f, 0.66f, 0.42f, 0.56f, T_SPID_BODY, T_SPID_BODY, T_SPID_BODY, 0);
            add(0, 0.58f, -0.46f, 0.92f, 0.7f, 0.86f, T_SPID_BODY, T_SPID_BODY, T_SPID_ABDO, 0);
            add(0, 0.48f, 0.66f, 0.5f, 0.38f, 0.34f, T_SPID_FACE, T_SPID_BODY, T_SPID_BODY, 0);
            const float lz[4] = {0.44f, 0.2f, -0.04f, -0.28f};
            for (int i = 0; i < 4; i++) for (int sd = -1; sd <= 1; sd += 2) {
                int sw = ((i + (sd > 0 ? 0 : 1)) % 2) ? 3 : 4;
                add(sd * 0.62f, 0.66f, lz[i], 0.62f, 0.09f, 0.09f, T_SPID_LEG, T_SPID_LEG, T_SPID_LEG, sw);
                add(sd * 0.95f, 0.36f, lz[i], 0.09f, 0.62f, 0.09f, T_SPID_LEG, T_SPID_LEG, T_SPID_LEG, sw);
            }
            break;
        }
    }
    return n;
}
static void emitMobs() {
    float maxD = (float)(g_renderDist * 16);
    for (const Mob& m : g_mobs) {
        if (m.dead) continue;
        float rx = (float)(m.x - g_cam.x), ry = (float)(m.y - g_cam.y), rz = (float)(m.z - g_cam.z);
        float d2 = rx * rx + ry * ry + rz * rz;
        if (d2 > maxD * maxD) continue;
        if (!sphereVisible(rx, ry + 1, rz, 2.f)) continue;
        float col[3]; lightColorAt(m.x, m.y + MD[m.type].h * 0.6, m.z, col);
        if (m.hurtT > 0) { col[0] = std::min(1.f, col[0] + 0.5f); col[1] *= 0.45f; col[2] *= 0.45f; }
        float fa = fogAlpha(d2);
        MPart parts[24]; int n = mobParts(m.type, parts);
        float cy = cosf(m.yaw), sy = sinf(m.yaw);
        float swingA = sinf(m.walk) * 0.7f;
        for (int i = 0; i < n; i++) {
            const MPart& p = parts[i];
            float ang = p.swing == 1 ? swingA : (p.swing == 2 ? -swingA : 0.f);
            float yawA = p.swing == 3 ? swingA * 0.6f : (p.swing == 4 ? -swingA * 0.6f : 0.f);   // лапы паука ходят вперёд-назад
            float pivY = p.y + p.sy * 0.5f, pivZ = p.z, pivX = p.x > 0 ? 0.3f : -0.3f;
            float corner[8][3];
            for (int c = 0; c < 8; c++) {
                float lx = p.x + ((c & 1) ? 0.5f : -0.5f) * p.sx;
                float ly = p.y + ((c & 2) ? 0.5f : -0.5f) * p.sy;
                float lz = p.z + ((c & 4) ? 0.5f : -0.5f) * p.sz;
                if (ang != 0) {
                    float yy = ly - pivY, zz = lz - pivZ;
                    ly = pivY + yy * cosf(ang) - zz * sinf(ang);
                    lz = pivZ + yy * sinf(ang) + zz * cosf(ang);
                }
                if (yawA != 0) {
                    float xx = lx - pivX, zz = lz - pivZ;
                    lx = pivX + xx * cosf(yawA) - zz * sinf(yawA);
                    lz = pivZ + xx * sinf(yawA) + zz * cosf(yawA);
                }
                corner[c][0] = rx + lx * cy + lz * sy;
                corner[c][1] = ry + ly;
                corner[c][2] = rz - lx * sy + lz * cy;
            }
            int tiles[6] = {p.ts, p.ts, p.tt, p.ts, p.tf, p.ts};
            if (p.swing == 9) { float glow[3] = {1, 1, 1}; emitBox8(corner, tiles, glow, fa, false); }
            else emitBox8(corner, tiles, col, fa, false);
        }
    }
}
static void emitArrows() {
    for (const Arrow& a : g_arrows) {
        float rx = (float)(a.x - g_cam.x), ry = (float)(a.y - g_cam.y), rz = (float)(a.z - g_cam.z);
        if (rx * rx + ry * ry + rz * rz > 48 * 48) continue;
        const float* d = a.dir;
        float up[3] = {0, 1, 0}; if (fabsf(d[1]) > 0.95f) { up[0] = 1; up[1] = 0; }
        float p[3] = {d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0]};
        float pl = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) + 1e-5f; for (int i = 0; i < 3; i++) p[i] /= pl;
        float q[3] = {d[1] * p[2] - d[2] * p[1], d[2] * p[0] - d[0] * p[2], d[0] * p[1] - d[1] * p[0]};   // правая тройка (p, q, d)
        float lc[3]; lightColorAt(a.x, a.y, a.z, lc);
        auto box = [&](float off, float hd, float hp, float hq, const float* tint, int tile) {
            float c[3] = {rx + d[0] * off, ry + d[1] * off, rz + d[2] * off}, corner[8][3];
            for (int k = 0; k < 8; k++) {
                float sp = (k & 1) ? hp : -hp, sq = (k & 2) ? hq : -hq, sd = (k & 4) ? hd : -hd;
                for (int i = 0; i < 3; i++) corner[k][i] = c[i] + p[i] * sp + q[i] * sq + d[i] * sd;
            }
            int tiles[6] = {tile, tile, tile, tile, tile, tile};
            float col[3] = {lc[0] * tint[0], lc[1] * tint[1], lc[2] * tint[2]};
            emitBox8(corner, tiles, col, 1.f, false);
        };
        const float shaft[3] = {0.62f, 0.45f, 0.28f}, tip[3] = {0.95f, 0.93f, 0.85f}, fl[3] = {0.98f, 0.82f, 0.88f};
        box(-0.28f, 0.27f, 0.022f, 0.022f, shaft, T_WHITE);
        box(0.0f, 0.04f, 0.04f, 0.04f, tip, T_WHITE);
        box(-0.5f, 0.07f, 0.065f, 0.006f, fl, T_WHITE);
        box(-0.5f, 0.07f, 0.006f, 0.065f, fl, T_WHITE);
    }
}
static void emitDrops(float time) {
    for (const Drop& d : g_drops) {
        float rx = (float)(d.x - g_cam.x), ry = (float)(d.y - g_cam.y) + 0.12f + sinf(time * 3.f + d.age) * 0.05f, rz = (float)(d.z - g_cam.z);
        float d2 = rx * rx + ry * ry + rz * rz;
        if (d2 > 48 * 48) continue;
        float col[3]; lightColorAt(d.x, d.y + 0.2, d.z, col);
        float a = time * 1.8f + d.age;
        float ca = cosf(a), sa = sinf(a);
        int id = d.s.id;
        if (itemIsCubeIcon(id)) {
            float s = 0.13f;
            float corner[8][3];
            for (int c = 0; c < 8; c++) {
                float lx = (c & 1) ? s : -s, ly = (c & 2) ? s : -s, lz = (c & 4) ? s : -s;
                corner[c][0] = rx + lx * ca + lz * sa; corner[c][1] = ry + ly; corner[c][2] = rz - lx * sa + lz * ca;
            }
            int tiles[6]; for (int k = 0; k < 6; k++) tiles[k] = BD[id].tex[k];
            emitBox8(corner, tiles, col, 1.f, false);
        } else {
            float s = 0.2f;
            float W[4][3] = {{rx - s * ca, ry + s, rz + s * sa}, {rx + s * ca, ry + s, rz - s * sa}, {rx + s * ca, ry - s, rz - s * sa}, {rx - s * ca, ry - s, rz + s * sa}};
            float UV[4][2], C[4][4]; int t = itemTile(id);
            tileUV(t, 0, 0, UV[0]); tileUV(t, 16, 0, UV[1]); tileUV(t, 16, 16, UV[2]); tileUV(t, 0, 16, UV[3]);
            for (int k = 0; k < 4; k++) { C[k][0] = col[0]; C[k][1] = col[1]; C[k][2] = col[2]; C[k][3] = 1; }
            emitQuadRel(W, UV, C);
        }
    }
}
static void emitParticles() {
    const Camera& c = g_cam;
    for (const Particle& p : g_parts) {
        float rx = p.x - (float)c.x, ry = p.y - (float)c.y, rz = p.z - (float)c.z;
        float s = p.size;
        float W[4][3];
        for (int k = 0; k < 4; k++) {
            float a = (k == 0 || k == 3) ? -s : s, b = (k < 2) ? s : -s;
            for (int j = 0; j < 3; j++) W[k][j] = (j == 0 ? rx : (j == 1 ? ry : rz)) + c.r[j] * a + c.u[j] * b;
        }
        float col[3];
        if (p.glow) { col[0] = p.r; col[1] = p.g; col[2] = p.b; }
        else { lightColorAt(p.x, p.y, p.z, col); col[0] *= p.r; col[1] *= p.g; col[2] *= p.b; }
        float al = std::min(1.f, p.life / p.maxLife * 2.f);
        float UV[4][2], C[4][4];
        tileUV(p.tile, p.u, p.v, UV[0]); tileUV(p.tile, p.u + 3, p.v, UV[1]); tileUV(p.tile, p.u + 3, p.v + 3, UV[2]); tileUV(p.tile, p.u, p.v + 3, UV[3]);
        for (int k = 0; k < 4; k++) { C[k][0] = col[0]; C[k][1] = col[1]; C[k][2] = col[2]; C[k][3] = al; }
        emitQuadRel(W, UV, C);
    }
}
static void emitCrack() {
    if (!g_dig.active || g_mode == GM_CREATIVE) return;
    int stage = clampi((int)(g_dig.prog * 5.f), 0, 4);
    float e = 0.004f;
    float corner[8][3];
    for (int c = 0; c < 8; c++) {
        corner[c][0] = (float)(g_dig.x - g_cam.x) + ((c & 1) ? 1 + e : -e);
        corner[c][1] = (float)(g_dig.y - g_cam.y) + ((c & 2) ? 1 + e : -e);
        corner[c][2] = (float)(g_dig.z - g_cam.z) + ((c & 4) ? 1 + e : -e);
    }
    int tiles[6]; for (int k = 0; k < 6; k++) tiles[k] = T_CRACK0 + stage;
    float col[3] = {1, 1, 1};
    emitBox8(corner, tiles, col, 0.9f, false, -0.05f);
}
// рука и предмет в руке (в пространстве камеры)
static void emitHand() {
    if (P.dead) return;
    float col[3]; lightColorAt(P.x, P.y + PL_EYE, P.z, col);
    float bx = sinf(P.bobPhase) * 0.035f * P.bobAmt, by = -fabsf(cosf(P.bobPhase)) * 0.03f * P.bobAmt;
    float sw = sinf(P.swing * (float)M_PI);
    const Stack& s = P.inv[P.sel];
    auto xform = [&](float lx, float ly, float lz, float ox, float oy, float oz, float yaw, float pitch, float* o) {
        float cp = cosf(pitch), sp = sinf(pitch);
        float y1 = ly * cp - lz * sp, z1 = ly * sp + lz * cp;
        float cy = cosf(yaw), sy = sinf(yaw);
        float x2 = lx * cy + z1 * sy, z2 = -lx * sy + z1 * cy;
        o[0] = x2 + ox; o[1] = y1 + oy; o[2] = z2 + oz;
    };
    if (s.empty()) {
        float corner[8][3];
        for (int c = 0; c < 8; c++) {
            float lx = (c & 1) ? 0.12f : -0.12f, ly = (c & 2) ? 0.12f : -0.12f, lz = (c & 4) ? 0.45f : -0.45f;
            xform(lx, ly, lz, 0.62f + bx - sw * 0.15f, -0.58f + by + sw * 0.12f, 0.75f + sw * 0.1f, -0.3f - sw * 0.3f, 0.35f - sw * 0.9f, corner[c]);
        }
        int tiles[6] = {T_SKIN, T_SKIN, T_SKIN, T_SKIN, T_SKIN, T_SLEEVE};
        emitBox8(corner, tiles, col, 1.f, true);
    } else if (itemIsCubeIcon(s.id)) {
        float corner[8][3]; float h = 0.17f;
        for (int c = 0; c < 8; c++) {
            float lx = (c & 1) ? h : -h, ly = (c & 2) ? h : -h, lz = (c & 4) ? h : -h;
            xform(lx, ly, lz, 0.52f + bx - sw * 0.12f, -0.42f + by + sw * 0.08f, 0.82f + sw * 0.08f, 0.75f, -0.2f - sw * 0.7f, corner[c]);
        }
        int tiles[6]; for (int k = 0; k < 6; k++) tiles[k] = BD[s.id].tex[k];
        if (s.id == B_FURNACE) tiles[4] = T_FURN_FRONT;
        if (s.id == B_CHEST) tiles[4] = T_CHEST_FRONT;
        emitBox8(corner, tiles, col, 1.f, true);
    } else {
        float W[4][3]; float h = 0.24f;
        const float L[4][2] = {{-h, h}, {h, h}, {h, -h}, {-h, -h}};
        if (s.id == I_BOW) {   // лук держится прямо перед собой и отводится при натяжении
            float pull = P.bowCharge;
            for (int k = 0; k < 4; k++) xform(L[k][0] * 0.95f, L[k][1] * 0.95f, 0, 0.44f + bx * 0.5f - pull * 0.12f, -0.27f + by * 0.5f + pull * 0.05f, 0.85f - pull * 0.1f, -0.5f, 0.f, W[k]);
        } else
        for (int k = 0; k < 4; k++) xform(L[k][0], L[k][1], 0, 0.5f + bx - sw * 0.1f, -0.36f + by + sw * 0.1f, 0.8f, -0.35f, -0.25f - sw * 1.0f, W[k]);
        float UV[4][2], C[4][4]; int t = itemTile(s.id);
        tileUV(t, 0, 0, UV[0]); tileUV(t, 16, 0, UV[1]); tileUV(t, 16, 16, UV[2]); tileUV(t, 0, 16, UV[3]);
        for (int k = 0; k < 4; k++) { C[k][0] = col[0]; C[k][1] = col[1]; C[k][2] = col[2]; C[k][3] = 1; }
        emitQuadCam(W, UV, C);
    }
}

// ---------------------------------------------------------------- сохранение
static std::string g_basePath;
static int g_worldSlot = 1;
static std::string g_worldName = "МИР";
static float g_playTime = 0;
static const int MAX_WORLDS = 6;

static std::string worldPath(int slot) { char b[64]; snprintf(b, sizeof(b), "buildworld_world_%d.sav", slot); return g_basePath + b; }

struct WBuf {
    std::vector<u8> d; size_t p = 0; bool bad = false;
    void put(const void* s, size_t n) { const u8* b = (const u8*)s; d.insert(d.end(), b, b + n); }
    template <class T> void w(T v) { put(&v, sizeof(T)); }
    void get(void* o, size_t n) { if (p + n > d.size()) { bad = true; memset(o, 0, n); return; } memcpy(o, d.data() + p, n); p += n; }
    template <class T> T r() { T v; get(&v, sizeof(T)); return v; }
    void ws(const Stack& s) { w<u16>(s.id); w<u16>(s.cnt); w<u16>(s.dmg); }
    Stack rs() { Stack s; s.id = r<u16>(); s.cnt = r<u16>(); s.dmg = r<u16>(); if (s.id >= I_END || (s.id >= B_COUNT && s.id < I_STICK)) s.clear(); return s; }
    void wstr(const std::string& s) { w<u8>((u8)std::min<size_t>(s.size(), 63)); put(s.data(), std::min<size_t>(s.size(), 63)); }
    std::string rstr() { int n = r<u8>(); std::string s; for (int i = 0; i < n && !bad; i++) s += (char)r<u8>(); return s; }
};
static bool readFile(const std::string& path, std::vector<u8>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}
static bool writeFileAtomic(const std::string& path, const std::vector<u8>& data) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    size_t w = fwrite(data.data(), 1, data.size(), f);
    fflush(f);
    fclose(f);
    if (w != data.size()) { remove(tmp.c_str()); return false; }
#ifdef _WIN32
    remove(path.c_str());
#endif
    return rename(tmp.c_str(), path.c_str()) == 0;
}

struct WorldInfo { bool exists = false; std::string name; u32 seed = 0; int mode = 0; float playTime = 0; };
static const u32 SAVE_MAGIC = 0x4D42554Bu;   // "KUBM"
static WorldInfo readWorldInfo(int slot) {
    WorldInfo wi; std::vector<u8> d;
    if (!readFile(worldPath(slot), d)) return wi;
    WBuf b; b.d.swap(d);
    if (b.r<u32>() != SAVE_MAGIC) return wi;
    b.r<u32>();
    wi.name = b.rstr(); wi.seed = b.r<u32>(); wi.mode = b.r<u8>(); b.r<float>(); wi.playTime = b.r<float>();
    wi.exists = !b.bad;
    return wi;
}

static bool saveWorld() {
    for (auto& kv : g_chunks) if (kv.second->modified) { std::vector<u8> v; encodeChunk(kv.second, v); g_savedChunks[kv.first] = v; }
    WBuf b;
    b.w<u32>(SAVE_MAGIC); b.w<u32>(1);
    b.wstr(g_worldName); b.w<u32>(g_seed); b.w<u8>((u8)g_mode); b.w<float>(g_dayTime); b.w<float>(g_playTime); b.w<i64>(g_tick);
    b.w<double>(P.x); b.w<double>(P.y); b.w<double>(P.z); b.w<float>(g_cam.yaw); b.w<float>(g_cam.pitch);
    b.w<float>(P.health); b.w<float>(P.hunger); b.w<float>(P.exhaustion); b.w<float>(P.air);
    b.w<i32>(P.spawnX); b.w<i32>(P.spawnY); b.w<i32>(P.spawnZ); b.w<u8>((u8)P.sel); b.w<u8>(P.flying ? 1 : 0); b.w<u8>(P.dead ? 1 : 0);
    for (int i = 0; i < 36; i++) b.ws(P.inv[i]);
    b.w<u32>((u32)g_chests.size());
    for (auto& kv : g_chests) { b.w<i64>(kv.first); for (int i = 0; i < 27; i++) b.ws(i < (int)kv.second.size() ? kv.second[i] : Stack()); }
    b.w<u32>((u32)g_furnaces.size());
    for (auto& kv : g_furnaces) { b.w<i64>(kv.first); b.ws(kv.second.in); b.ws(kv.second.fuel); b.ws(kv.second.out); b.w<float>(kv.second.burn); b.w<float>(kv.second.burnMax); b.w<float>(kv.second.cook); }
    b.w<u32>((u32)g_grow.size());
    for (auto& g : g_grow) { b.w<i32>(g.x); b.w<i32>(g.y); b.w<i32>(g.z); b.w<float>(g.t); }
    b.w<u32>((u32)g_savedChunks.size());
    for (auto& kv : g_savedChunks) {
        b.w<i64>(kv.first); b.w<u32>((u32)kv.second.size()); b.put(kv.second.data(), kv.second.size());
    }
    bool ok = writeFileAtomic(worldPath(g_worldSlot), b.d);
    return ok;
}

static void resetWorldState() {
    freeAllChunks();
    g_savedChunks.clear(); g_chests.clear(); g_furnaces.clear(); g_grow.clear(); g_sched.clear(); g_pending.clear();
    g_mobs.clear(); g_drops.clear(); g_parts.clear(); g_arrows.clear();
    g_tick = 0; g_playTime = 0; g_dayTime = 0.05f;
    P = Player();
}

static bool loadWorld(int slot) {
    std::vector<u8> d;
    if (!readFile(worldPath(slot), d)) return false;
    WBuf b; b.d.swap(d);
    if (b.r<u32>() != SAVE_MAGIC) return false;
    b.r<u32>();
    resetWorldState();
    g_worldSlot = slot;
    g_worldName = b.rstr(); u32 seed = b.r<u32>(); g_mode = b.r<u8>(); g_dayTime = b.r<float>(); g_playTime = b.r<float>(); g_tick = b.r<i64>();
    worldInitNoise(seed);
    P.x = b.r<double>(); P.y = b.r<double>(); P.z = b.r<double>(); g_cam.yaw = b.r<float>(); g_cam.pitch = b.r<float>();
    P.health = b.r<float>(); P.hunger = b.r<float>(); P.exhaustion = b.r<float>(); P.air = b.r<float>();
    P.spawnX = b.r<i32>(); P.spawnY = b.r<i32>(); P.spawnZ = b.r<i32>(); P.sel = clampi(b.r<u8>(), 0, 8); P.flying = b.r<u8>() != 0; P.dead = b.r<u8>() != 0;
    for (int i = 0; i < 36; i++) P.inv[i] = b.rs();
    u32 nc = b.r<u32>();
    for (u32 i = 0; i < nc && !b.bad; i++) { i64 k = b.r<i64>(); std::vector<Stack> v(27); for (int j = 0; j < 27; j++) v[j] = b.rs(); g_chests[k] = v; }
    u32 nf = b.r<u32>();
    for (u32 i = 0; i < nf && !b.bad; i++) { i64 k = b.r<i64>(); Furnace f; f.in = b.rs(); f.fuel = b.rs(); f.out = b.rs(); f.burn = b.r<float>(); f.burnMax = b.r<float>(); f.cook = b.r<float>(); g_furnaces[k] = f; }
    u32 ng = b.r<u32>();
    for (u32 i = 0; i < ng && !b.bad; i++) { Grow g; g.x = b.r<i32>(); g.y = b.r<i32>(); g.z = b.r<i32>(); g.t = b.r<float>(); g_grow.push_back(g); }
    u32 nch = b.r<u32>();
    for (u32 i = 0; i < nch && !b.bad; i++) {
        i64 k = b.r<i64>(); u32 len = b.r<u32>();
        if (b.p + len > b.d.size()) { b.bad = true; break; }
        g_savedChunks[k] = std::vector<u8>(b.d.begin() + b.p, b.d.begin() + b.p + len);
        b.p += len;
    }
    g_grow.clear();   // саженцы будут собраны при загрузке чанков
    if (g_tick < 0) g_tick = 0;
    if (!(P.y > -50 && P.y < 400)) P.y = 100;
    return true;
}
static void deleteWorld(int slot) { remove(worldPath(slot).c_str()); }

// ---------------------------------------------------------------- настройки
struct Settings { int renderDist = 6; float sens = 1.0f; float volume = 0.8f; int inset = 4; bool showFps = false; bool autoJump = true; float uiScale = 1.0f; int shaders = 2; };
static Settings g_set;
// переезд со старых названий игры: файлы kubomir_* и buildeworld_* переименовываются, чтобы миры не пропали
static void migrateOldSaves() {
    auto mv = [](const std::string& from, const std::string& to) {
        FILE* t = fopen(to.c_str(), "rb"); if (t) { fclose(t); return; }
        FILE* f = fopen(from.c_str(), "rb"); if (!f) return; fclose(f);
        rename(from.c_str(), to.c_str());
    };
    const char* old[2] = {"kubomir", "buildeworld"};
    for (int k = 0; k < 2; k++) {
        for (int i = 1; i <= MAX_WORLDS; i++) { char b[64]; snprintf(b, sizeof(b), "%s_world_%d.sav", old[k], i); mv(g_basePath + b, worldPath(i)); }
        mv(g_basePath + old[k] + "_settings.cfg", g_basePath + "buildworld_settings.cfg");
    }
}
static void saveSettings() {
    FILE* f = fopen((g_basePath + "buildworld_settings.cfg").c_str(), "w");
    if (!f) return;
    fprintf(f, "%d %f %f %d %d %d %f %d\n", g_set.renderDist, g_set.sens, g_set.volume, g_set.inset, g_set.showFps ? 1 : 0, g_set.autoJump ? 1 : 0, g_set.uiScale, g_set.shaders);
    fclose(f);
}
static void loadSettings() {
    FILE* f = fopen((g_basePath + "buildworld_settings.cfg").c_str(), "r");
    if (!f) return;
    int rd = 6, inset = 4, fps = 0, aj = 1, sh = 2; float sens = 1, vol = 0.8f, ui = 1.f;
    if (fscanf(f, "%d %f %f %d %d %d %f %d", &rd, &sens, &vol, &inset, &fps, &aj, &ui, &sh) >= 4) {
        g_set.shaders = clampi(sh, 0, 2);
        g_set.renderDist = clampi(rd, 4, 10); g_set.sens = clampf(sens, 0.2f, 3.f); g_set.volume = clampf(vol, 0, 1);
        g_set.inset = clampi(inset, 0, 12); g_set.showFps = fps != 0; g_set.autoJump = aj != 0; g_set.uiScale = clampf(ui, 0.7f, 1.4f);
    }
    fclose(f);
}

// ============================================================================
//  ИНТЕРФЕЙС: текст, значки, кнопки (касание + геймпад), экраны
// ============================================================================
enum Screen { SCR_INTRO = -1, SCR_MAIN = 0, SCR_WORLDS, SCR_NEWWORLD, SCR_LOADING, SCR_GAME, SCR_PAUSE, SCR_SETTINGS, SCR_INV, SCR_DEAD, SCR_CONFIRM_DEL };
static int g_screen = SCR_INTRO, g_settingsBack = SCR_MAIN;
static bool g_introSkip = false;   // пропустить заставку (кнопка «назад», клавиша)
static void screenMain();
enum InvMode { INV_PLAYER = 0, INV_CRAFT, INV_FURNACE, INV_CHEST };
static int g_invMode = INV_PLAYER;
static int g_bookPage = 0, g_creativePage = 0;
// книга рецептов: панель слева от сетки создания
static bool g_bookOpen = false, g_bookOnlyCan = false;
static int g_bookTab = 0, g_bookSel = -1;           // вкладка и выбранный результат (для подсказки)
static int g_ghostRecipe = -1;                      // рецепт-подсказка в сетке, когда не хватает материалов
static bool g_altMode = false;           // «по одному»
static Stack g_cursor;
static Stack g_craft[9];
static int g_contX = 0, g_contY = 0, g_contZ = 0;
static float g_heldNameT = 0;

static float uiS() { return g_H / 100.f; }                            // 1% высоты экрана
static float fontPx() { return std::max(2.f, g_H / 240.f) * g_set.uiScale; }
static int insetX() { return (int)(g_W * g_set.inset / 100.f); }
static int insetTop() { return (int)(g_H * 0.045f); }

// ---------------------------------------------------------------- текст
static float textWidth(const std::string& s, float px) {
    const char* p = s.c_str(); int n = 0;
    while (*p) { utf8Next(p); n++; }
    return n * 6 * px - px;
}
static void drawText(const std::string& s, float x, float y, float px, SDL_Color col, int align = 0, bool shadow = true) {
    if (align == 1) x -= textWidth(s, px) / 2; else if (align == 2) x -= textWidth(s, px);
    for (int pass = shadow ? 0 : 1; pass < 2; pass++) {
        if (pass == 0) { SDL_SetTextureColorMod(g_fontTex, 20, 20, 25); SDL_SetTextureAlphaMod(g_fontTex, (Uint8)(col.a * 0.8f)); }
        else { SDL_SetTextureColorMod(g_fontTex, col.r, col.g, col.b); SDL_SetTextureAlphaMod(g_fontTex, col.a); }
        float off = pass == 0 ? std::max(1.f, px * 0.6f) : 0;
        const char* p = s.c_str(); float cx = x;
        while (*p) {
            u32 c = utf8Next(p);
            if (c != ' ') {
                int g = glyphFor(c);
                if (g >= 0) {
                    SDL_Rect src = {(g % 21) * GLYPH_CW, (g / 21) * GLYPH_CH, 5, 7};
                    SDL_Rect dst = {(int)(cx + off), (int)(y + off), (int)ceilf(5 * px), (int)ceilf(7 * px)};
                    SDL_RenderCopy(g_ren, g_fontTex, &src, &dst);
                }
            }
            cx += 6 * px;
        }
    }
}
static SDL_Color C(int r, int g, int b, int a = 255) { SDL_Color c; c.r = (Uint8)r; c.g = (Uint8)g; c.b = (Uint8)b; c.a = (Uint8)a; return c; }
static void fillRect(float x, float y, float w, float h, SDL_Color c) {
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = {(int)x, (int)y, (int)w, (int)h};
    SDL_RenderFillRect(g_ren, &r);
}
static void frameRect(float x, float y, float w, float h, float t, SDL_Color c) {
    fillRect(x, y, w, t, c); fillRect(x, y + h - t, w, t, c); fillRect(x, y, t, h, c); fillRect(x + w - t, y, t, h, c);
}
static void fillCircle(float cx, float cy, float r, SDL_Color c) {
    const int N = 28; SDL_Vertex v[N + 1]; int idx[N * 3];
    v[0].position.x = cx; v[0].position.y = cy; v[0].color = c; v[0].tex_coord.x = v[0].tex_coord.y = 0;
    for (int i = 0; i < N; i++) {
        float a = i * 6.2831853f / N;
        v[i + 1].position.x = cx + cosf(a) * r; v[i + 1].position.y = cy + sinf(a) * r; v[i + 1].color = c; v[i + 1].tex_coord.x = v[i + 1].tex_coord.y = 0;
        idx[i * 3] = 0; idx[i * 3 + 1] = i + 1; idx[i * 3 + 2] = (i + 1) % N + 1;
    }
    SDL_RenderGeometry(g_ren, NULL, v, N + 1, idx, N * 3);
}
static void ringCircle(float cx, float cy, float r, float t, SDL_Color c) {
    const int N = 32; SDL_Vertex v[N * 2]; int idx[N * 6];
    for (int i = 0; i < N; i++) {
        float a = i * 6.2831853f / N;
        for (int k = 0; k < 2; k++) {
            float rr = k ? r : r - t;
            v[i * 2 + k].position.x = cx + cosf(a) * rr; v[i * 2 + k].position.y = cy + sinf(a) * rr; v[i * 2 + k].color = c; v[i * 2 + k].tex_coord.x = v[i * 2 + k].tex_coord.y = 0;
        }
        int a0 = i * 2, a1 = ((i + 1) % N) * 2;
        int* p = idx + i * 6; p[0] = a0; p[1] = a0 + 1; p[2] = a1; p[3] = a0 + 1; p[4] = a1 + 1; p[5] = a1;
    }
    SDL_RenderGeometry(g_ren, NULL, v, N * 2, idx, N * 6);
}

// ---------------------------------------------------------------- значки предметов
static void drawTile(int tile, float x, float y, float w, float h, Uint8 a = 255) {
    SDL_Rect src = {(tile & 15) * 16, (tile >> 4) * 16, 16, 16};
    SDL_Rect dst = {(int)x, (int)y, (int)w, (int)h};
    SDL_SetTextureColorMod(g_atlasTex, 255, 255, 255); SDL_SetTextureAlphaMod(g_atlasTex, a);
    SDL_RenderCopy(g_ren, g_atlasTex, &src, &dst);
}
static void drawItemIcon(int id, float x, float y, float s) {
    if (id <= 0) return;
    if (!itemIsCubeIcon(id)) { drawTile(itemTile(id), x + s * 0.1f, y + s * 0.1f, s * 0.8f, s * 0.8f); return; }
    // изометрический кубик: верх, левая и правая грани
    float cx = x + s * 0.5f, cy = y + s * 0.5f, r = s * 0.40f;
    float h = r * 0.5f;
    float top[4][2] = {{cx, cy - r}, {cx + r * 0.87f, cy - h}, {cx, cy}, {cx - r * 0.87f, cy - h}};
    float left[4][2] = {{cx - r * 0.87f, cy - h}, {cx, cy}, {cx, cy + r}, {cx - r * 0.87f, cy + h}};
    float right[4][2] = {{cx, cy}, {cx + r * 0.87f, cy - h}, {cx + r * 0.87f, cy + h}, {cx, cy + r}};
    const BlockDef& d = BD[id];
    int tTop = d.tex[2], tL = d.tex[4], tR = d.tex[0];
    if (id == B_FURNACE) tL = T_FURN_FRONT;
    if (id == B_CHEST) tL = T_CHEST_FRONT;
    if (id == B_FURNACE_LIT) tL = T_FURN_FRONT_LIT;
    const float (*faces[3])[2] = {top, left, right};
    int tiles[3] = {tTop, tL, tR}; float shade[3] = {1.0f, 0.78f, 0.6f};
    SDL_Vertex v[12]; int idx[18];
    for (int f = 0; f < 3; f++) {
        const float UVp[4][2] = {{0, 0}, {16, 0}, {16, 16}, {0, 16}};
        for (int k = 0; k < 4; k++) {
            SDL_Vertex& vv = v[f * 4 + k];
            vv.position.x = faces[f][k][0]; vv.position.y = faces[f][k][1];
            float uv[2]; tileUV(tiles[f], UVp[k][0], UVp[k][1], uv);
            vv.tex_coord.x = uv[0]; vv.tex_coord.y = uv[1];
            Uint8 c = (Uint8)(255 * shade[f]); vv.color.r = c; vv.color.g = c; vv.color.b = c; vv.color.a = 255;
        }
        int* p = idx + f * 6; int b = f * 4;
        p[0] = b; p[1] = b + 1; p[2] = b + 2; p[3] = b; p[4] = b + 2; p[5] = b + 3;
    }
    SDL_SetTextureColorMod(g_atlasTex, 255, 255, 255); SDL_SetTextureAlphaMod(g_atlasTex, 255);
    SDL_RenderGeometry(g_ren, g_atlasTex, v, 12, idx, 18);
}
static void drawStack(const Stack& s, float x, float y, float sz) {
    if (s.empty()) return;
    drawItemIcon(s.id, x, y, sz);
    float px = std::max(1.5f, sz / 22.f);
    if (s.cnt > 1) drawText(std::to_string(s.cnt), x + sz - px * 1.5f, y + sz - 8.5f * px, px, C(255, 255, 255), 2);
    if (isTool(s.id) && s.dmg > 0) {
        float f = 1.f - s.dmg / (float)MAT_DUR[toolMat(s.id)];
        fillRect(x + sz * 0.12f, y + sz * 0.84f, sz * 0.76f, sz * 0.08f, C(0, 0, 0, 220));
        fillRect(x + sz * 0.12f, y + sz * 0.84f, sz * 0.76f * f, sz * 0.08f, C((int)(255 * (1 - f)), (int)(255 * f), 40));
    }
}

// ---------------------------------------------------------------- система кнопок
struct Widget { int id; SDL_Rect r; };
struct UIState {
    std::vector<Widget> cur, prev;
    int focus = -1; bool pad = false; int navDir = -1; bool activate = false, alt = false;
    std::vector<SDL_Point> taps;
};
static UIState g_ui;
static void uiBegin() {
    g_ui.prev.swap(g_ui.cur); g_ui.cur.clear();
    bool found = false;
    for (auto& w : g_ui.prev) if (w.id == g_ui.focus) found = true;
    if (!found && !g_ui.prev.empty()) g_ui.focus = g_ui.prev[0].id;
    if (g_ui.navDir >= 0 && !g_ui.prev.empty()) {
        SDL_Rect fr = g_ui.prev[0].r;
        for (auto& w : g_ui.prev) if (w.id == g_ui.focus) fr = w.r;
        float fx = fr.x + fr.w / 2.f, fy = fr.y + fr.h / 2.f;
        const float DIRS[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};   // вверх, вниз, влево, вправо
        float best = 1e18f; int bid = g_ui.focus;
        for (auto& w : g_ui.prev) {
            if (w.id == g_ui.focus) continue;
            float wx = w.r.x + w.r.w / 2.f - fx, wy = w.r.y + w.r.h / 2.f - fy;
            float along = wx * DIRS[g_ui.navDir][0] + wy * DIRS[g_ui.navDir][1];
            float across = fabsf(wx * DIRS[g_ui.navDir][1] - wy * DIRS[g_ui.navDir][0]);
            if (along <= 2) continue;
            float score = along + across * 2.5f;
            if (score < best) { best = score; bid = w.id; }
        }
        g_ui.focus = bid;
        playSound(SND_CLICK, 0.3f, 1.4f);
    }
    g_ui.navDir = -1;
}
static void uiEnd() { g_ui.taps.clear(); g_ui.activate = false; g_ui.alt = false; }
static bool uiHit(int id, SDL_Rect r, bool* alt = nullptr) {
    Widget w; w.id = id; w.r = r; g_ui.cur.push_back(w);
    if (alt) *alt = false;
    for (auto& t : g_ui.taps) if (t.x >= r.x && t.y >= r.y && t.x < r.x + r.w && t.y < r.y + r.h) { g_ui.focus = id; g_ui.pad = false; return true; }
    if ((g_ui.activate || g_ui.alt) && g_ui.focus == id) { if (alt) *alt = g_ui.alt; return true; }
    return false;
}
static bool uiFocused(int id) { return g_ui.pad && g_ui.focus == id; }
static SDL_Rect R(float x, float y, float w, float h) { SDL_Rect r = {(int)x, (int)y, (int)w, (int)h}; return r; }
static bool button(int id, float x, float y, float w, float h, const std::string& label, bool enabled = true, float scaleMul = 1.f) {
    SDL_Rect r = R(x, y, w, h);
    bool hit = uiHit(id, r);
    bool foc = uiFocused(id);
    fillRect(x, y, w, h, enabled ? (foc ? C(96, 132, 70, 240) : C(70, 78, 66, 225)) : C(50, 50, 50, 200));
    fillRect(x, y, w, h * 0.08f, C(255, 255, 255, enabled ? 50 : 20));
    fillRect(x, y + h * 0.92f, w, h * 0.08f, C(0, 0, 0, 90));
    frameRect(x, y, w, h, std::max(2.f, h * 0.05f), foc ? C(255, 250, 160) : C(20, 22, 18, 255));
    float px = fontPx() * 1.25f * scaleMul;
    while (textWidth(label, px) > w * 0.92f && px > 1.5f) px *= 0.9f;
    drawText(label, x + w / 2, y + h / 2 - 3.5f * px, px, enabled ? C(250, 250, 240) : C(140, 140, 140), 1);
    if (hit && enabled) playSound(SND_CLICK, 0.5f);
    return hit && enabled;
}
static void panel(float x, float y, float w, float h) {
    fillRect(x, y, w, h, C(38, 40, 46, 235));
    frameRect(x, y, w, h, std::max(2.f, uiS() * 0.4f), C(16, 16, 20, 255));
    frameRect(x + uiS() * 0.4f, y + uiS() * 0.4f, w - uiS() * 0.8f, h - uiS() * 0.8f, std::max(1.f, uiS() * 0.2f), C(90, 94, 104, 255));
}
static void slotBg(float x, float y, float s, bool sel, bool foc) {
    fillRect(x, y, s, s, C(24, 26, 30, 220));
    frameRect(x, y, s, s, std::max(1.f, s * 0.05f), sel ? C(255, 255, 255) : C(110, 112, 120));
    if (foc) frameRect(x - 2, y - 2, s + 4, s + 4, std::max(2.f, s * 0.07f), C(255, 240, 120));
}

// ---------------------------------------------------------------- операции со слотами
static void slotClick(Stack& slot, bool alt, bool canPlace = true) {
    if (g_cursor.empty()) {
        if (slot.empty()) return;
        if (alt) { int half = (slot.cnt + 1) / 2; g_cursor = slot; g_cursor.cnt = (u16)half; slot.cnt -= half; if (!slot.cnt) slot.clear(); }
        else { g_cursor = slot; slot.clear(); }
        return;
    }
    if (!canPlace) {
        if (!slot.empty() && slot.id == g_cursor.id && g_cursor.cnt + slot.cnt <= maxStack(slot.id)) { g_cursor.cnt += slot.cnt; slot.clear(); }
        return;
    }
    if (slot.empty()) {
        if (alt) { slot = g_cursor; slot.cnt = 1; g_cursor.cnt--; if (!g_cursor.cnt) g_cursor.clear(); }
        else { slot = g_cursor; g_cursor.clear(); }
        return;
    }
    if (slot.id == g_cursor.id && maxStack(slot.id) > 1) {
        int room = maxStack(slot.id) - slot.cnt;
        int mv = alt ? std::min(1, room) : std::min(room, (int)g_cursor.cnt);
        slot.cnt += mv; g_cursor.cnt -= mv; if (!g_cursor.cnt) g_cursor.clear();
        return;
    }
    std::swap(slot, g_cursor);
}
static void returnLooseItems() {
    for (int i = 0; i < 9; i++) if (!g_craft[i].empty()) { int l = addItem(g_craft[i]); if (l) { Stack s = g_craft[i]; s.cnt = (u16)l; spawnDrop(s, P.x, P.y + 1.2, P.z); } g_craft[i].clear(); }
    if (!g_cursor.empty()) { int l = addItem(g_cursor); if (l) { Stack s = g_cursor; s.cnt = (u16)l; spawnDrop(s, P.x, P.y + 1.2, P.z); } g_cursor.clear(); }
}
static void openInventory(int mode) {
    g_invMode = mode; g_screen = SCR_INV; g_altMode = false; g_ui.focus = -1; g_ghostRecipe = -1; g_bookSel = -1;
    playSound(SND_CLICK, 0.5f, 0.8f);
}
static void closeInventory() { returnLooseItems(); g_screen = SCR_GAME; playSound(SND_CLICK, 0.5f, 0.7f); }
static void openCraftingTable() { openInventory(INV_CRAFT); }
static void openFurnace(int x, int y, int z) {
    g_contX = x; g_contY = y; g_contZ = z;
    if (!g_furnaces.count(posKey(x, y, z))) g_furnaces[posKey(x, y, z)] = Furnace();
    openInventory(INV_FURNACE);
}
static void openChest(int x, int y, int z) {
    g_contX = x; g_contY = y; g_contZ = z;
    auto& v = g_chests[posKey(x, y, z)]; if (v.size() < 27) v.resize(27);
    openInventory(INV_CHEST);
    playSound(SND_DIG0 + SM_WOOD, 0.6f, 0.7f);
}
static void sleepInBed(int x, int y, int z) {
    P.spawnX = x; P.spawnY = y + 1; P.spawnZ = z;
    if (!isNight()) { showMsg("ТОЧКА ВОЗРОЖДЕНИЯ ЗАДАНА. СПАТЬ МОЖНО ТОЛЬКО НОЧЬЮ", 3.f); return; }
    for (const Mob& m : g_mobs) if (MD[m.type].hostile && fabs(m.x - P.x) < 8 && fabs(m.z - P.z) < 8 && fabs(m.y - P.y) < 5) { showMsg("НЕЛЬЗЯ СПАТЬ: РЯДОМ ЧУДОВИЩА"); return; }
    g_dayTime = 0.01f;
    for (Mob& m : g_mobs) if (MD[m.type].hostile) m.dead = true;
    showMsg("ДОБРОЕ УТРО! ТОЧКА ВОЗРОЖДЕНИЯ ЗАДАНА", 3.f);
}

// ---------------------------------------------------------------- книга рецептов
static bool recipeNeedsTable(const Recipe& r) { return r.w > 2 || r.h > 2; }
static void recipeNeeds(const Recipe& r, std::vector<std::pair<int, int>>& need) {
    need.clear();
    for (int i = 0; i < 9; i++) {
        int id = r.cell[i]; if (!id) continue;
        bool f = false;
        for (auto& n : need) if (n.first == id) { n.second++; f = true; }
        if (!f) need.push_back(std::make_pair(id, 1));
    }
}
static int craftGridW() { return g_invMode == INV_CRAFT ? 3 : 2; }
// 0 — можно сделать, 1 — не хватает материалов, 2 — нужен верстак
static int recipeState(const Recipe& r) {
    int gw = craftGridW();
    if (r.w > gw || r.h > gw) return 2;
    if (g_mode == GM_CREATIVE) return 0;
    std::vector<std::pair<int, int>> need; recipeNeeds(r, need);
    for (auto& n : need) {
        int have = countItem(n.first);
        for (int i = 0; i < gw * gw; i++) if (g_craft[i].id == n.first) have += g_craft[i].cnt;   // уже лежит в сетке
        if (have < n.second) return 1;
    }
    return 0;
}
static int bookCategory(int out) {
    if (isTool(out) || out == I_BOW || out == I_ARROW) return 1;
    if (isBlockItem(out)) return 2;
    return 3;
}
struct BookEntry { int out; std::vector<int> rec; int state; };
static void buildBookEntries(std::vector<BookEntry>& list) {
    list.clear();
    for (int i = 0; i < (int)g_recipes.size(); i++) {
        const Recipe& r = g_recipes[i];
        if (g_bookTab != 0 && bookCategory(r.out) != g_bookTab) continue;
        BookEntry* e = nullptr;
        for (auto& x : list) if (x.out == r.out) e = &x;
        if (!e) { BookEntry ne; ne.out = r.out; ne.state = 3; list.push_back(ne); e = &list.back(); }
        e->rec.push_back(i);
        e->state = std::min(e->state, recipeState(r));
    }
    if (g_bookOnlyCan) list.erase(std::remove_if(list.begin(), list.end(), [](const BookEntry& e) { return e.state != 0; }), list.end());
    std::stable_sort(list.begin(), list.end(), [](const BookEntry& a, const BookEntry& b) { return a.state < b.state; });
}
static void returnCraftGrid() {
    for (int i = 0; i < 9; i++) if (!g_craft[i].empty()) {
        int l = addItem(g_craft[i]);
        if (l) { Stack s = g_craft[i]; s.cnt = (u16)l; spawnDrop(s, P.x, P.y + 1.2, P.z); }
        g_craft[i].clear();
    }
}
static int g_lastPlaced = -1;
// разложить ингредиенты рецепта в сетку; повторное нажатие добавляет ещё один набор
static void placeRecipe(int ri) {
    const Recipe& r = g_recipes[ri];
    int gw = craftGridW();
    if (r.w > gw || r.h > gw) { showMsg("НУЖЕН ВЕРСТАК"); return; }
    int cnt = 0;
    bool same = g_lastPlaced == ri && matchRecipe(g_craft, gw, &cnt) == r.out;
    if (!same) returnCraftGrid();
    g_ghostRecipe = -1;
    bool creative = g_mode == GM_CREATIVE;
    std::vector<std::pair<int, int>> need; recipeNeeds(r, need);
    std::string lack;
    for (auto& n : need) if (!creative && countItem(n.first) < n.second) { if (!lack.empty()) lack += ", "; lack += itemName(n.first); }
    if (!lack.empty()) {
        if (same) { showMsg("БОЛЬШЕ НЕ ХВАТАЕТ: " + lack); return; }
        g_ghostRecipe = ri; g_lastPlaced = -1;
        showMsg("НЕ ХВАТАЕТ: " + lack);
        playSound(SND_CLICK, 0.4f, 0.6f);
        return;
    }
    for (int y = 0; y < r.h; y++) for (int x = 0; x < r.w; x++) {
        int id = r.cell[y * 3 + x]; if (!id) continue;
        Stack& sl = g_craft[y * gw + x];
        if (!sl.empty() && (sl.id != id || sl.cnt >= maxStack(id))) return;   // некуда добавить
    }
    for (int y = 0; y < r.h; y++) for (int x = 0; x < r.w; x++) {
        int id = r.cell[y * 3 + x]; if (!id) continue;
        Stack& sl = g_craft[y * gw + x];
        if (sl.empty()) sl = mkStack(id, 1); else sl.cnt++;
        if (!creative) removeItem(id, 1);
    }
    g_lastPlaced = ri;
    playSound(SND_CLICK, 0.45f, 1.3f);
}
static void drawRecipeBook(float x0, float y0, float w, float h, float bs) {
    float S = uiS(), px = fontPx(), pad = S * 1.5f;
    panel(x0, y0, w, h);
    float x = x0 + pad, y = y0 + pad, iw = w - pad * 2;
    drawText("КНИГА РЕЦЕПТОВ", x, y, px, C(255, 230, 150));
    y += px * 10;
    // вкладки
    static const char* TAB_NAMES[4] = {"ВСЕ РЕЦЕПТЫ", "ИНСТРУМЕНТЫ И ОРУЖИЕ", "СТРОИТЕЛЬСТВО", "МАТЕРИАЛЫ"};
    const int tabIcon[4] = {-1, toolId(MT_IRON, TK_PICK), B_STONEBRICK, I_IRON};
    float tw = iw / 4.f, th = bs * 0.85f;
    for (int t = 0; t < 4; t++) {
        float tx = x + t * tw;
        bool selT = g_bookTab == t;
        if (uiHit(2950 + t, R(tx + 1, y, tw - 2, th))) { g_bookTab = t; g_bookPage = 0; playSound(SND_CLICK, 0.4f, 1.1f); }
        fillRect(tx + 1, y, tw - 2, th + (selT ? 3 : 0), selT ? C(70, 74, 84, 255) : C(32, 34, 40, 230));
        frameRect(tx + 1, y, tw - 2, th + (selT ? 3 : 0), 2, uiFocused(2950 + t) ? C(255, 240, 120) : (selT ? C(150, 154, 166) : C(20, 20, 24)));
        float is = th * 0.78f;
        if (tabIcon[t] < 0) drawTile(T_BOOK, tx + tw / 2 - is / 2, y + th / 2 - is / 2, is, is);
        else drawItemIcon(tabIcon[t], tx + tw / 2 - is / 2, y + th / 2 - is / 2, is);
    }
    y += th + S * 1.2f;
    drawText(TAB_NAMES[g_bookTab], x, y, px * 0.8f, C(200, 205, 215));
    y += px * 9;
    // фильтр
    if (button(2960, x, y, iw, S * 7, g_bookOnlyCan ? "ПОКАЗАНЫ: МОЖНО СДЕЛАТЬ" : "ПОКАЗАНЫ: ВСЕ", true, 0.75f)) { g_bookOnlyCan = !g_bookOnlyCan; g_bookPage = 0; }
    y += S * 8.5f;
    // сетка рецептов 5 x 4
    std::vector<BookEntry> list; buildBookEntries(list);
    const int COLS = 5, ROWS = 4, PER = COLS * ROWS;
    int pages = std::max(1, ((int)list.size() + PER - 1) / PER);
    g_bookPage = clampi(g_bookPage, 0, pages - 1);
    float gx0 = x + (iw - bs * COLS) / 2;
    int focusEntry = -1;
    for (int i = 0; i < PER; i++) {
        int li = g_bookPage * PER + i;
        float sx = gx0 + (i % COLS) * bs, sy = y + (i / COLS) * bs;
        if (li >= (int)list.size()) { fillRect(sx + 2, sy + 2, bs - 4, bs - 4, C(26, 28, 32, 160)); continue; }
        const BookEntry& e = list[li];
        int wid = 3000 + e.out;
        bool hit = uiHit(wid, R(sx + 2, sy + 2, bs - 4, bs - 4));
        bool foc = uiFocused(wid);
        SDL_Color bg = e.state == 0 ? C(44, 62, 46, 235) : (e.state == 1 ? C(78, 36, 36, 235) : C(36, 36, 40, 235));
        SDL_Color br = e.state == 0 ? C(110, 170, 100) : (e.state == 1 ? C(150, 70, 66) : C(70, 70, 76));
        fillRect(sx + 2, sy + 2, bs - 4, bs - 4, bg);
        frameRect(sx + 2, sy + 2, bs - 4, bs - 4, std::max(1.f, bs * 0.04f), foc ? C(255, 240, 120) : (g_bookSel == e.out ? C(240, 240, 240) : br));
        drawItemIcon(e.out, sx + bs * 0.12f, sy + bs * 0.12f, bs * 0.76f);
        const Recipe& r0 = g_recipes[e.rec[0]];
        if (r0.cnt > 1) drawText(std::to_string(r0.cnt), sx + bs - px * 1.5f, sy + bs - px * 9.5f, px * 0.9f, C(255, 255, 255), 2);
        if (e.state != 0) fillRect(sx + 2, sy + 2, bs - 4, bs - 4, C(0, 0, 0, 90));
        if (e.state == 2) { fillRect(sx + bs * 0.52f, sy + bs * 0.04f, bs * 0.44f, bs * 0.44f, C(20, 20, 24, 200)); drawItemIcon(B_CRAFT, sx + bs * 0.54f, sy + bs * 0.06f, bs * 0.4f); }
        if (foc) focusEntry = e.out;
        if (hit) {
            g_bookSel = e.out;
            int pick = -1;
            for (int ri : e.rec) if (recipeState(g_recipes[ri]) == 0) { pick = ri; break; }
            if (pick < 0) for (int ri : e.rec) if (recipeState(g_recipes[ri]) == 1) { pick = ri; break; }
            if (pick < 0) { showMsg("ЭТОТ РЕЦЕПТ ДЕЛАЕТСЯ НА ВЕРСТАКЕ"); playSound(SND_CLICK, 0.4f, 0.6f); }
            else placeRecipe(pick);
        }
    }
    y += bs * ROWS + S * 1.5f;
    // страницы
    float pbw = S * 10, pbh = S * 7;
    if (button(2961, x, y, pbw, pbh, "<", g_bookPage > 0)) g_bookPage--;
    drawText(std::to_string(g_bookPage + 1) + " / " + std::to_string(pages), x + iw / 2, y + pbh / 2 - 3.5f * px, px, C(230, 230, 230), 1);
    if (button(2962, x + iw - pbw, y, pbw, pbh, ">", g_bookPage < pages - 1)) g_bookPage++;
    y += pbh + S * 1.5f;
    // подсказка: название и состав
    int show = focusEntry >= 0 ? focusEntry : g_bookSel;
    if (list.empty()) {
        drawText(g_bookOnlyCan ? "ПОКА НИЧЕГО НЕЛЬЗЯ СДЕЛАТЬ" : "НЕТ РЕЦЕПТОВ", x, y, px * 0.8f, C(200, 200, 200));
    } else if (show >= 0) {
        const Recipe* rp = nullptr;
        for (const auto& e : list) if (e.out == show) rp = &g_recipes[e.rec[0]];
        if (rp) {
            float npx = px * 0.95f; std::string nm = itemName(show);
            while (textWidth(nm, npx) > iw && npx > 1.2f) npx *= 0.9f;
            drawText(nm, x, y, npx, C(255, 240, 180));
            std::vector<std::pair<int, int>> need; recipeNeeds(*rp, need);
            float lx = x, ly = y + px * 10, ipx = px * 0.75f;
            for (auto& n : need) while (textWidth(itemName(n.first) + " " + std::to_string(n.second), ipx) > iw && ipx > 1.1f) ipx *= 0.92f;
            for (auto& n : need) {
                std::string part = itemName(n.first) + " " + std::to_string(n.second);
                bool ok = g_mode == GM_CREATIVE || countItem(n.first) >= n.second;
                float tw2 = textWidth(part + ", ", ipx);
                if (lx + tw2 > x + iw && lx > x) { lx = x; ly += ipx * 9; }
                drawText(part, lx, ly, ipx, ok ? C(170, 230, 160) : C(235, 140, 130));
                lx += tw2;
            }
            if (recipeNeedsTable(*rp)) drawText(g_invMode == INV_CRAFT ? "СЕТКА 3X3" : "НУЖЕН ВЕРСТАК", x, ly + ipx * 10, ipx, g_invMode == INV_CRAFT ? C(160, 200, 235) : C(235, 175, 100));
        }
    } else {
        const char* hint[3] = {"НАЖМИ НА РЕЦЕПТ -", "ИНГРЕДИЕНТЫ САМИ", "ЛЯГУТ В СЕТКУ"};
        float hp = px * 0.75f; while (textWidth(hint[1], hp) > iw && hp > 1.1f) hp *= 0.92f;
        for (int k = 0; k < 3; k++) drawText(hint[k], x, y + k * hp * 9, hp, C(190, 190, 190));
    }
}

// ---------------------------------------------------------------- экран инвентаря
static void drawCursorStack(float sz) {
    if (g_cursor.empty()) return;
    // рисуется у последнего касания/фокуса
    static float lx = 0, ly = 0;
    if (!g_ui.taps.empty()) { lx = (float)g_ui.taps.back().x; ly = (float)g_ui.taps.back().y; }
    if (g_ui.pad) for (auto& w : g_ui.cur) if (w.id == g_ui.focus) { lx = w.r.x + w.r.w * 0.75f; ly = w.r.y + w.r.h * 0.75f; }
    drawStack(g_cursor, lx - sz * 0.3f, ly - sz * 0.3f, sz * 0.9f);
}
static int slotWidget(int id, Stack& s, float x, float y, float sz, bool sel = false, bool canPlace = true) {
    bool alt = false;
    bool hit = uiHit(id, R(x, y, sz, sz), &alt);
    slotBg(x, y, sz, sel, uiFocused(id));
    drawStack(s, x + sz * 0.04f, y + sz * 0.04f, sz * 0.92f);
    if (hit) {
        int before = s.id;
        slotClick(s, alt || g_altMode, canPlace); playSound(SND_CLICK, 0.35f, 1.2f);
        int id = !g_cursor.empty() ? g_cursor.id : (!s.empty() ? s.id : before);
        showTip(id, x + sz / 2, y);
        return 1;
    }
    return 0;
}

static std::vector<int> g_creativeItems;
static void initCreativeItems() {
    for (int b = 1; b < B_COUNT; b++) if (b != B_FURNACE_LIT) g_creativeItems.push_back(b);
    for (int i = I_STICK; i <= I_ARROW; i++) g_creativeItems.push_back(i);
    for (int i = I_TOOL0; i < I_TOOL0 + 20; i++) g_creativeItems.push_back(i);
}

static void drawInventoryScreen() {
    float S = uiS();
    bool creative = g_mode == GM_CREATIVE;
    bool bookAvail = g_invMode == INV_CRAFT || (g_invMode == INV_PLAYER && !creative);
    bool book = bookAvail && g_bookOpen;
    float pad = S * 2;
    float availW = (float)(g_W - insetX() * 2);
    float sz = std::min(S * 9.5f, availW / 15.f);
    if (book) sz = std::min(sz, (availW - S * 30 - pad * 6) / (9.f + 5.f * 0.86f + 0.6f));
    float gridW = sz * 9;
    float sideW = std::max(S * 30, sz * 3.4f);
    float mainW = gridW + pad * 3 + sideW;
    float bs = sz * 0.86f, bookW = bs * 5 + S * 3;
    float totalW = mainW + (book ? bookW + pad * 0.5f : 0);
    float y0 = std::max((float)insetTop(), (g_H - (sz * 8.4f + pad * 3)) / 2);
    float ph = g_H - y0 * 2;
    float x0 = (g_W - totalW) / 2 + (book ? bookW + pad * 0.5f : 0);
    fillRect(0, 0, (float)g_W, (float)g_H, C(0, 0, 0, 130));
    if (book) drawRecipeBook((g_W - totalW) / 2, y0, bookW, ph, bs);
    panel(x0, y0, mainW, ph);
    float px = fontPx();
    float gx = x0 + pad, gy = y0 + pad;
    // боковые кнопки
    float bx = x0 + pad * 2 + gridW, bw = sideW, bh = S * 9, by = gy;
    if (bookAvail) { if (button(2002, bx, by, bw, bh, g_bookOpen ? "СКРЫТЬ КНИГУ" : "КНИГА РЕЦЕПТОВ")) g_bookOpen = !g_bookOpen; by += bh + S; }
    if (!creative || g_invMode != INV_PLAYER) { if (button(2003, bx, by, bw, bh, g_altMode ? "ПО ОДНОМУ: ДА" : "ПО ОДНОМУ: НЕТ")) g_altMode = !g_altMode; by += bh + S; }
    if (button(2004, bx, by, bw, bh, "ВЫБРОСИТЬ", !g_cursor.empty())) {
        float fx = sinf(g_cam.yaw), fz = cosf(g_cam.yaw);
        spawnDrop(g_cursor, P.x + fx * 0.6, P.y + 1.3, P.z + fz * 0.6, fx * 4.f, 2.f, fz * 4.f); g_cursor.clear();
    }
    by += bh + S;
    if (creative && g_invMode == INV_PLAYER) {
        int pages = ((int)g_creativeItems.size() + 44) / 45;
        if (button(2005, bx, by, bw / 2 - S / 2, bh, "<", g_creativePage > 0)) g_creativePage--;
        if (button(2006, bx + bw / 2 + S / 2, by, bw / 2 - S / 2, bh, ">", g_creativePage < pages - 1)) g_creativePage++;
        by += bh + S;
        if (button(2007, bx, by, bw, bh, "ОЧИСТИТЬ")) { P.inv[P.sel].clear(); g_cursor.clear(); }
        by += bh + S;
    }
    if (button(2009, bx, y0 + ph - pad - bh, bw, bh, "ЗАКРЫТЬ")) { closeInventory(); return; }

    // верхняя часть
    float topY = gy;
    if (creative && g_invMode == INV_PLAYER) {
        drawText("ВСЕ ПРЕДМЕТЫ - НАЖМИ, ЧТОБЫ ВЗЯТЬ", gx, topY, px, C(230, 230, 230));
        float yy = topY + px * 11;
        for (int i = 0; i < 45; i++) {
            int idx = g_creativePage * 45 + i;
            if (idx >= (int)g_creativeItems.size()) break;
            int id = g_creativeItems[idx];
            float sx = gx + (i % 9) * sz, sy = yy + (i / 9) * sz;
            if (uiHit(4000 + idx, R(sx, sy, sz, sz))) {
                showTip(id, sx + sz / 2, sy);
                g_cursor.clear();
                P.inv[P.sel] = mkStack(id, maxStack(id));
                playSound(SND_PICKUP, 0.4f);
            }
            slotBg(sx, sy, sz, false, uiFocused(4000 + idx));
            drawItemIcon(id, sx + sz * 0.06f, sy + sz * 0.06f, sz * 0.88f);
        }
        float hy = yy + 5 * sz + px * 13;
        drawText("ПАНЕЛЬ: ВЫБЕРИ ЯЧЕЙКУ", gx, hy - px * 10, px, C(200, 200, 200));
        for (int i = 0; i < 9; i++) {
            float sx = gx + i * sz;
            if (uiHit(1000 + i, R(sx, hy, sz, sz))) { P.sel = i; playSound(SND_CLICK, 0.4f); showTip(P.inv[i].id, sx + sz / 2, hy); }
            slotBg(sx, hy, sz, i == P.sel, uiFocused(1000 + i));
            drawStack(P.inv[i], sx + sz * 0.04f, hy + sz * 0.04f, sz * 0.92f);
        }
        // название под курсором геймпада
        for (auto& w : g_ui.cur) if (w.id == g_ui.focus && g_ui.pad && w.id >= 4000) drawText(itemName(g_creativeItems[w.id - 4000]), gx, hy + sz + S, px, C(255, 240, 180));
        return;
    }
    if (g_invMode == INV_PLAYER || g_invMode == INV_CRAFT) {
        int n = g_invMode == INV_CRAFT ? 3 : 2;
        drawText(n == 3 ? "ВЕРСТАК" : "СОЗДАНИЕ", gx, topY, px, C(230, 230, 230));
        float cy0 = topY + px * 11;
        float cx0 = gx + sz * (n == 3 ? 1.5f : 2.0f);
        // маленькая кнопка-книжка слева от сетки
        if (bookAvail) {
            float kb = sz * 0.9f, kx = gx + sz * 0.15f, ky = cy0 + (n * sz - kb) / 2;
            if (uiHit(2010, R(kx, ky, kb, kb))) { g_bookOpen = !g_bookOpen; playSound(SND_CLICK, 0.45f, 1.1f); }
            fillRect(kx, ky, kb, kb, g_bookOpen ? C(90, 110, 80, 235) : C(40, 42, 50, 235));
            frameRect(kx, ky, kb, kb, 2, uiFocused(2010) ? C(255, 240, 120) : C(120, 124, 134));
            drawTile(T_BOOK, kx + kb * 0.1f, ky + kb * 0.1f, kb * 0.8f, kb * 0.8f);
        }
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
            float sx = cx0 + x * sz, sy = cy0 + y * sz;
            if (slotWidget(1100 + y * 3 + x, g_craft[y * n + x], sx, sy, sz)) { g_ghostRecipe = -1; g_lastPlaced = -1; }
            // рецепт-подсказка: полупрозрачные ингредиенты, красный фон — если такого предмета нет
            if (g_ghostRecipe >= 0 && g_ghostRecipe < (int)g_recipes.size() && g_craft[y * n + x].empty()) {
                const Recipe& gr = g_recipes[g_ghostRecipe];
                int id = (x < gr.w && y < gr.h) ? gr.cell[y * 3 + x] : 0;
                if (id) {
                    if (countItem(id) == 0) fillRect(sx + 2, sy + 2, sz - 4, sz - 4, C(170, 40, 40, 110));
                    drawItemIcon(id, sx + sz * 0.1f, sy + sz * 0.1f, sz * 0.8f);
                    fillRect(sx + 2, sy + 2, sz - 4, sz - 4, C(30, 30, 36, 140));
                }
            }
        }
        int cnt = 0; int res = matchRecipe(g_craft, n, &cnt);
        float ax = cx0 + n * sz + sz * 0.4f, ay = cy0 + (n * sz) / 2;
        drawText(">", ax + sz * 0.3f, ay - 3.5f * px * 2, px * 2, C(230, 230, 230));
        float ox = ax + sz * 1.3f, oy = ay - sz * 0.6f;
        bool hit = uiHit(1150, R(ox, oy, sz * 1.2f, sz * 1.2f));
        slotBg(ox, oy, sz * 1.2f, res != 0, uiFocused(1150));
        if (!res && g_ghostRecipe >= 0 && g_ghostRecipe < (int)g_recipes.size()) {
            drawItemIcon(g_recipes[g_ghostRecipe].out, ox + sz * 0.1f, oy + sz * 0.1f, sz);
            fillRect(ox + 2, oy + 2, sz * 1.2f - 4, sz * 1.2f - 4, C(30, 30, 36, 150));
        }
        if (res) {
            Stack rs = mkStack(res, cnt); drawStack(rs, ox + sz * 0.1f, oy + sz * 0.1f, sz);
            std::string nm = itemName(res); float npx = px * 0.9f, maxW = gx + gridW - (ox - sz * 1.2f);
            while (textWidth(nm, npx) > maxW && npx > 1.2f) npx *= 0.9f;
            drawText(nm, ox + sz * 0.6f, oy + sz * 1.35f, npx, C(255, 240, 180), 1);
        }
        if (hit && res) showTip(res, ox + sz * 0.6f, oy);
        if (hit && res) {
            if (g_cursor.empty() || (g_cursor.id == res && g_cursor.cnt + cnt <= maxStack(res))) {
                if (g_cursor.empty()) g_cursor = mkStack(res, cnt); else g_cursor.cnt += cnt;
                for (int i = 0; i < n * n; i++) if (!g_craft[i].empty()) { g_craft[i].cnt--; if (!g_craft[i].cnt) g_craft[i].clear(); }
                playSound(SND_PICKUP, 0.5f, 1.2f);
                g_ghostRecipe = -1;
            }
        }
    } else if (g_invMode == INV_FURNACE) {
        auto it = g_furnaces.find(posKey(g_contX, g_contY, g_contZ));
        if (it == g_furnaces.end() || (getBlock(g_contX, g_contY, g_contZ) != B_FURNACE && getBlock(g_contX, g_contY, g_contZ) != B_FURNACE_LIT)) { closeInventory(); return; }
        Furnace& f = it->second;
        drawText("ПЕЧЬ", gx, topY, px, C(230, 230, 230));
        float fx = gx + sz * 2.5f, fy = topY + px * 11;
        slotWidget(1200, f.in, fx, fy, sz);
        // пламя
        float fl = f.burnMax > 0 ? f.burn / f.burnMax : 0;
        drawTile(T_FLAME, fx + sz * 0.15f, fy + sz * 1.05f, sz * 0.7f, sz * 0.7f, (Uint8)(fl > 0 ? 255 : 60));
        slotWidget(1201, f.fuel, fx, fy + sz * 1.85f, sz);
        float ax = fx + sz * 1.4f, ay = fy + sz * 0.9f;
        fillRect(ax, ay, sz * 1.6f, sz * 0.28f, C(30, 30, 30, 220));
        fillRect(ax, ay, sz * 1.6f * (f.cook / SMELT_TIME), sz * 0.28f, C(250, 210, 90));
        slotWidget(1202, f.out, ax + sz * 1.9f, fy + sz * 0.45f, sz * 1.1f, false, false);
        float hpx = px * 0.8f; std::string h1 = "СВЕРХУ - ЧТО ПЛАВИТЬ", h2 = "СНИЗУ - ТОПЛИВО: УГОЛЬ, ДЕРЕВО";
        while (textWidth(h2, hpx) > gx + gridW - ax && hpx > 1.1f) hpx *= 0.9f;
        drawText(h1, ax, fy + sz * 1.75f, hpx, C(190, 190, 190));
        drawText(h2, ax, fy + sz * 1.75f + hpx * 10, hpx, C(190, 190, 190));
    } else if (g_invMode == INV_CHEST) {
        auto& v = g_chests[posKey(g_contX, g_contY, g_contZ)];
        if (v.size() < 27) v.resize(27);
        if (getBlock(g_contX, g_contY, g_contZ) != B_CHEST) { closeInventory(); return; }
        drawText("СУНДУК", gx, topY, px, C(230, 230, 230));
        float cy0 = topY + px * 11;
        for (int i = 0; i < 27; i++) slotWidget(1300 + i, v[i], gx + (i % 9) * sz, cy0 + (i / 9) * sz, sz);
    }
    // сумка игрока
    float iy = y0 + ph - pad - sz * 4.25f;
    drawText("СУМКА", gx, iy - px * 10, px, C(230, 230, 230));
    for (int i = 9; i < 36; i++) slotWidget(1000 + i, P.inv[i], gx + ((i - 9) % 9) * sz, iy + ((i - 9) / 9) * sz, sz);
    float hy = iy + sz * 3.25f;
    for (int i = 0; i < 9; i++) slotWidget(1000 + i, P.inv[i], gx + i * sz, hy, sz, i == P.sel);
    // подсказка: имя предмета под фокусом
    if (g_ui.pad) for (auto& w : g_ui.cur) if (w.id == g_ui.focus && w.id >= 1000 && w.id < 1036 && !P.inv[w.id - 1000].empty())
        drawText(itemName(P.inv[w.id - 1000].id), bx, y0 + ph - pad - S * 22, px * 0.9f, C(255, 240, 180));
    drawCursorStack(sz);
    // название предмета: под фокусом геймпада или после нажатия
    if (g_ui.pad) {
        for (auto& w : g_ui.cur) if (w.id == g_ui.focus) {
            int id = 0;
            if (w.id >= 1000 && w.id < 1036) id = P.inv[w.id - 1000].id;
            else if (w.id >= 1100 && w.id < 1109) id = g_craft[w.id - 1100].id;
            if (id > 0 && (g_tipText != itemName(id) || SDL_GetTicks() > g_tipUntil)) { g_tipText = itemName(id); g_tipX = w.r.x + w.r.w / 2.f; g_tipY = (float)w.r.y; g_tipUntil = SDL_GetTicks() + 400; }
        }
    }
    if (!g_tipText.empty() && SDL_GetTicks() < g_tipUntil) {
        float tp = px * 1.0f, tw = textWidth(g_tipText, tp), bh = tp * 11;
        float tx = clampf(g_tipX - tw / 2 - tp * 3, 4.f, g_W - tw - tp * 6 - 4.f), ty = std::max(4.f, g_tipY - bh - tp * 2);
        Uint32 left = g_tipUntil - SDL_GetTicks(); int a = left < 300 ? (int)(left * 255 / 300) : 255;
        fillRect(tx, ty, tw + tp * 6, bh, C(16, 10, 30, a * 235 / 255));
        frameRect(tx, ty, tw + tp * 6, bh, std::max(2.f, tp * 0.6f), C(90, 50, 160, a));
        drawText(g_tipText, tx + tp * 3, ty + (bh - tp * 7) / 2, tp, C(255, 255, 255, a));
    }
}

// ---------------------------------------------------------------- HUD и сенсорное управление
struct TouchLayout { float jx, jy, jr; float bJump[3], bDown[3]; SDL_Rect pause, inv, fly; float hotX, hotY, hotS; };
static TouchLayout g_tl;
#ifdef __ANDROID__
static bool g_touchUsed = true;
#else
static bool g_touchUsed = false;
#endif
static void computeTouchLayout() {
    float H = (float)g_H, W = (float)g_W, ix = (float)insetX();
    TouchLayout& t = g_tl;
    t.jr = H * 0.13f; t.jx = ix + H * 0.24f; t.jy = H - H * 0.28f;
    t.bJump[0] = W - ix - H * 0.16f; t.bJump[1] = H - H * 0.17f; t.bJump[2] = H * 0.11f;
    t.bDown[0] = W - ix - H * 0.16f; t.bDown[1] = H - H * 0.42f; t.bDown[2] = H * 0.085f;
    float bs = H * 0.095f;
    t.pause = R(W - ix - bs - H * 0.02f, (float)insetTop(), bs, bs);
    t.fly = R(W - ix - bs * 2.2f - H * 0.02f, (float)insetTop(), bs, bs);
    t.hotS = std::min(H * 0.095f, (W - ix * 2) / 13.f);
    t.hotX = (W - t.hotS * 9) / 2; t.hotY = H - t.hotS - H * 0.012f;
    t.inv = R(t.hotX + t.hotS * 9 + t.hotS * 0.15f, t.hotY, t.hotS, t.hotS);
}
static float g_joyBaseX = 0, g_joyBaseY = 0, g_joyKnobX = 0, g_joyKnobY = 0; static bool g_joyActive = false;
static bool g_btnJump = false, g_btnDown = false;
// касания: короткое касание экрана — поставить/использовать/ударить, удержание — ломать
enum FRole { FR_NONE = 0, FR_JOY, FR_LOOK, FR_JUMP, FR_DOWN, FR_UI };
struct Finger { SDL_FingerID id; int role; float x0, y0, x, y; Uint32 t0; bool moved, hold; };
static std::vector<Finger> g_fingers;
static const Uint32 HOLD_MS = 300, TAP_MS = 300;

static void drawRoundButton(const float* b, const std::string& label, bool pressed, int icon = -1) {
    fillCircle(b[0], b[1], b[2], pressed ? C(255, 255, 255, 110) : C(20, 20, 24, 90));
    ringCircle(b[0], b[1], b[2], std::max(2.f, b[2] * 0.06f), C(255, 255, 255, 150));
    float px = fontPx() * 0.95f;
    if (icon >= 0) drawTile(icon, b[0] - b[2] * 0.45f, b[1] - b[2] * 0.7f, b[2] * 0.9f, b[2] * 0.9f, 220);
    while (textWidth(label, px) > b[2] * 1.8f && px > 1.2f) px *= 0.9f;
    drawText(label, b[0], b[1] + (icon >= 0 ? b[2] * 0.3f : -3.5f * px), px, C(255, 255, 255, 230), 1);
}
static void drawHotbar() {
    const TouchLayout& t = g_tl;
    for (int i = 0; i < 9; i++) {
        float x = t.hotX + i * t.hotS;
        fillRect(x, t.hotY, t.hotS, t.hotS, C(20, 20, 24, 150));
        frameRect(x, t.hotY, t.hotS, t.hotS, std::max(1.f, t.hotS * 0.04f), C(150, 150, 150, 200));
        drawStack(P.inv[i], x + t.hotS * 0.08f, t.hotY + t.hotS * 0.08f, t.hotS * 0.84f);
    }
    float sx = t.hotX + P.sel * t.hotS;
    frameRect(sx - 3, t.hotY - 3, t.hotS + 6, t.hotS + 6, std::max(3.f, t.hotS * 0.07f), C(255, 255, 255, 240));
    {   // подобранные предметы
        float px = fontPx() * 0.95f;
        float baseY = t.hotY - (g_mode == GM_SURVIVAL ? t.hotS * 0.75f : 0) - px * 28;
        for (size_t i = 0; i < g_picks.size(); i++) {
            const PickNote& p = g_picks[g_picks.size() - 1 - i];
            int a = (int)(255 * std::min(1.f, p.t));
            std::string txt = "+" + std::to_string(p.cnt) + " " + itemName(p.id);
            float tw = textWidth(txt, px), is = px * 9;
            float x = t.hotX + t.hotS * 9 - tw - is - px * 4, y = baseY - i * px * 11;
            fillRect(x - px * 2, y - px * 1.5f, tw + is + px * 6, px * 10, C(0, 0, 0, a / 2));
            drawItemIcon(p.id, x, y - px, is);
            drawText(txt, x + is + px * 2, y, px, C(255, 255, 255, a));
        }
    }
    if (g_heldNameT > 0 && !P.inv[P.sel].empty()) {
        float px = fontPx();
        drawText(itemName(P.inv[P.sel].id), g_W / 2.f, t.hotY - (g_mode == GM_SURVIVAL ? t.hotS * 0.75f : 0) - px * 14, px, C(255, 255, 255, (int)(255 * std::min(1.f, g_heldNameT))), 1);
    }
}
static void drawStatusBars() {
    if (g_mode != GM_SURVIVAL) return;
    const TouchLayout& t = g_tl;
    float is = t.hotS * 0.42f;
    float y = t.hotY - is - t.hotS * 0.12f;
    bool lowFlash = P.health <= 4 && (SDL_GetTicks() / 250) % 2;
    for (int i = 0; i < 10; i++) {
        float hv = P.health - i * 2;
        int tile = hv >= 2 ? T_HEART_F : (hv >= 1 ? T_HEART_H : T_HEART_E);
        float jy = lowFlash ? ((i * 7 + SDL_GetTicks() / 100) % 3) - 1.f : 0;
        drawTile(tile, t.hotX + i * is * 0.95f, y + jy, is, is);
        float fv = P.hunger - (9 - i) * 2;
        int ft = fv >= 2 ? T_FOOD_F : (fv >= 1 ? T_FOOD_H : T_FOOD_E);
        drawTile(ft, t.hotX + t.hotS * 9 - (10 - i) * is * 0.95f, y, is, is);
    }
    if (P.headInWater || P.air < 10) {
        int n = (int)ceilf(P.air);
        for (int i = 0; i < n; i++) drawTile(T_BUBBLE, t.hotX + t.hotS * 9 - (10 - i) * is * 0.95f, y - is, is, is);
    }
}
static int g_fps = 0;
static void drawHUD() {
    computeTouchLayout();
    const TouchLayout& t = g_tl;
    // прицел
    float cx = g_W / 2.f, cy = g_H / 2.f, cs = g_H * 0.022f, ct = std::max(2.f, g_H * 0.0035f);
    fillRect(cx - cs - 1, cy - ct / 2 - 1, cs * 2 + 2, ct + 2, C(0, 0, 0, 120));
    fillRect(cx - ct / 2 - 1, cy - cs - 1, ct + 2, cs * 2 + 2, C(0, 0, 0, 120));
    fillRect(cx - cs, cy - ct / 2, cs * 2, ct, C(255, 255, 255, 220));
    fillRect(cx - ct / 2, cy - cs, ct, cs * 2, C(255, 255, 255, 220));
    if (g_targetMob >= 0) frameRect(cx - cs * 0.7f, cy - cs * 0.7f, cs * 1.4f, cs * 1.4f, ct, C(255, 80, 80, 200));
    if (P.bowCharge > 0) {   // натяжение лука
        float bw = g_H * 0.12f, bh = std::max(3.f, g_H * 0.008f), by = cy + cs * 1.6f;
        fillRect(cx - bw / 2 - 1, by - 1, bw + 2, bh + 2, C(0, 0, 0, 160));
        fillRect(cx - bw / 2, by, bw * P.bowCharge, bh, P.bowCharge >= 1.f ? C(255, 220, 90, 230) : C(230, 230, 230, 220));
    }
    drawStatusBars();
    drawHotbar();
    float px = fontPx();
    if (g_touchUsed || !g_ui.pad) {
        // кнопка инвентаря
        fillRect((float)t.inv.x, (float)t.inv.y, (float)t.inv.w, (float)t.inv.h, C(20, 20, 24, 150));
        frameRect((float)t.inv.x, (float)t.inv.y, (float)t.inv.w, (float)t.inv.h, 2, C(150, 150, 150, 200));
        drawText("...", t.inv.x + t.inv.w / 2.f, t.inv.y + t.inv.h / 2.f - 5 * px, px * 1.4f, C(255, 255, 255), 1);
    }
    if (g_touchUsed) {
        // джойстик
        float bx = g_joyActive ? g_joyBaseX : t.jx, by = g_joyActive ? g_joyBaseY : t.jy;
        fillCircle(bx, by, t.jr, C(20, 20, 24, g_joyActive ? 90 : 50));
        ringCircle(bx, by, t.jr, std::max(2.f, t.jr * 0.04f), C(255, 255, 255, g_joyActive ? 140 : 70));
        float kx = g_joyActive ? g_joyKnobX : bx, ky = g_joyActive ? g_joyKnobY : by;
        fillCircle(kx, ky, t.jr * 0.42f, C(230, 230, 230, g_joyActive ? 170 : 90));
        drawRoundButton(t.bJump, "ПРЫЖОК", g_btnJump);
        // кольцо удержания: пока палец стоит на месте, кольцо сжимается; заполнилось — начинается копание
        for (const Finger& f : g_fingers) {
            if (f.role != FR_LOOK || f.moved) continue;
            float k = clampf((SDL_GetTicks() - f.t0) / (float)HOLD_MS, 0, 1);
            if (k < 0.15f) continue;
            float r = g_H * (0.075f - 0.03f * k);
            ringCircle(f.x, f.y, r, std::max(3.f, g_H * 0.006f), C(255, 255, 255, f.hold ? 200 : (int)(70 + 130 * k)));
        }
        if (P.flying) drawRoundButton(t.bDown, "ВНИЗ", g_btnDown);
    }
    // пауза
    fillRect((float)t.pause.x, (float)t.pause.y, (float)t.pause.w, (float)t.pause.h, C(20, 20, 24, 140));
    frameRect((float)t.pause.x, (float)t.pause.y, (float)t.pause.w, (float)t.pause.h, 2, C(200, 200, 200, 200));
    fillRect(t.pause.x + t.pause.w * 0.3f, t.pause.y + t.pause.h * 0.25f, t.pause.w * 0.13f, t.pause.h * 0.5f, C(255, 255, 255));
    fillRect(t.pause.x + t.pause.w * 0.57f, t.pause.y + t.pause.h * 0.25f, t.pause.w * 0.13f, t.pause.h * 0.5f, C(255, 255, 255));
    if (g_mode == GM_CREATIVE) {
        fillRect((float)t.fly.x, (float)t.fly.y, (float)t.fly.w, (float)t.fly.h, P.flying ? C(90, 140, 220, 180) : C(20, 20, 24, 140));
        frameRect((float)t.fly.x, (float)t.fly.y, (float)t.fly.w, (float)t.fly.h, 2, C(200, 200, 200, 200));
        drawText("ЛЕТ", t.fly.x + t.fly.w / 2.f, t.fly.y + t.fly.h / 2.f - 3.5f * px * 0.9f, px * 0.9f, C(255, 255, 255), 1);
    }
    // сообщения и отладка
    if (g_msgT > 0) {
        float mp = px * 1.1f;
        float tw = textWidth(g_msg, mp);
        float my = insetTop() + g_H * 0.12f;
        fillRect(g_W / 2.f - tw / 2 - mp * 4, my - mp * 3, tw + mp * 8, mp * 13, C(0, 0, 0, (int)(150 * std::min(1.f, g_msgT))));
        drawText(g_msg, g_W / 2.f, my, mp, C(255, 255, 220, (int)(255 * std::min(1.f, g_msgT))), 1);
    }
    if (g_set.showFps) {
        char b[128];
        snprintf(b, sizeof(b), "FPS %d  X %d Y %d Z %d  ГРАНЕЙ %d", g_fps, ifloor(P.x), ifloor(P.y), ifloor(P.z), g_quadsDrawn);
        drawText(b, insetX() + g_H * 0.03f, (float)insetTop(), px * 0.8f, C(255, 255, 255));
    }
}

// ---------------------------------------------------------------- меню
static WorldInfo g_worldInfos[MAX_WORLDS + 1];
static int g_newMode = GM_SURVIVAL; static u32 g_newSeed = 1; static int g_delSlot = 0;
static void refreshWorldList() { for (int i = 1; i <= MAX_WORLDS; i++) g_worldInfos[i] = readWorldInfo(i); }
static void drawBackdrop(float t) {
    // фон меню: медленно плывущие «кубики»
    SDL_SetRenderDrawColor(g_ren, 40, 60, 90, 255); SDL_RenderClear(g_ren);
    float s = g_H / 10.f;
    int cols = (int)(g_W / s) + 2, rows = (int)(g_H / s) + 2;
    for (int y = 0; y < rows; y++) for (int x = 0; x < cols; x++) {
        float ox = fmodf(t * 8.f, s);
        int tile = -1;
        if (y > rows / 2) {
            if (y == rows / 2 + 1) tile = (int)T_GRASS_SIDE;
            else tile = (hash3i(x, y, 0, 5) % 9 == 0) ? (int)T_COAL_ORE + (int)(hash3i(x, y, 1, 5) % 4) : (int)T_STONE;
        }
        if (tile < 0) continue;
        drawTile(tile, x * s - ox, y * s, s + 1, s + 1, 255);
    }
    fillRect(0, 0, (float)g_W, (float)g_H, C(10, 14, 30, 120));
}
// логотип: BUILD — золотом, WORLD — зелёным. reveal — время появления букв (сек), <0 — всё сразу
static const char* LOGO = "BUILDWORLD";
static float logoPx() { return std::min(fontPx() * 4.5f, g_W * 0.82f / 59.f); }
static float logoY() { return g_H * 0.14f; }
static void drawLogo(float reveal, float alpha) {
    float px = logoPx(), y = logoY();
    int n = (int)strlen(LOGO);
    float tw = n * 6 * px - px, x0 = g_W / 2.f - tw / 2;
    if (alpha <= 0.01f) return;
    // мягкое сияние за логотипом
    const float warm[3] = {1.f, 0.8f, 0.35f};
    float gl = reveal < 0 ? 1.f : clampf((reveal - 0.2f) / 0.6f, 0, 1);
    drawGlow(g_W / 2.f, y + 3.5f * px, tw * 0.62f, warm, 0.22f * alpha * gl);
    for (int i = 0; i < n; i++) {
        float k = 1.f, a = 1.f;
        if (reveal >= 0) {
            float lt = (reveal - i * 0.07f) / 0.28f;
            if (lt <= 0) continue;
            lt = std::min(lt, 1.f);
            const float c1 = 1.70158f, c3 = c1 + 1;   // easeOutBack — буква «выпрыгивает»
            k = 1 + c3 * powf(lt - 1, 3) + c1 * powf(lt - 1, 2);
            a = std::min(1.f, lt * 2.f);
        }
        float lpx = px * k, cx = x0 + i * 6 * px + 2.5f * px, cy = y + 3.5f * px;
        char ch[2] = {LOGO[i], 0};
        SDL_Color col = i < 5 ? C(255, 214, 80) : C(120, 214, 96);
        col.a = (Uint8)(255 * a * alpha);
        drawText(ch, cx - 2.5f * lpx + lpx * 0.9f, cy - 3.5f * lpx + lpx * 0.9f, lpx, C(0, 0, 0, (int)(170 * a * alpha)), 0, false);
        drawText(ch, cx - 2.5f * lpx, cy - 3.5f * lpx, lpx, col, 0, true);
    }
}
static void drawTitle(const std::string& t, float y, float scale) {
    float px = fontPx() * scale;
    drawText(t, g_W / 2.f + px, y + px, px, C(0, 0, 0, 160), 1, false);
    drawText(t, g_W / 2.f, y, px, C(255, 220, 90), 1);
}

// ============================================================================
//  ГЛАВНЫЙ ЦИКЛ: ввод (касания, геймпад, клавиатура), чанки, экраны
// ============================================================================
static bool g_running = true, g_background = false;
static float g_time = 0, g_autosaveT = 0, g_tickAcc = 0;
static bool g_needSpawnY = false;
static std::vector<std::pair<int, int>> g_offsets;
static SDL_GameController* g_pad = nullptr;
static float g_padStickNavT = 0;

// Сохранение при сворачивании. На Android SDL может приостановить главный поток раньше, чем
// событие дойдёт до очереди, поэтому сохраняем прямо в обработчике (он вызывается сразу),
// а главный цикл держит мьютекс, пока меняет состояние мира.
static SDL_mutex* g_worldMutex = nullptr;
static bool worldScreenActive();
static void returnLooseItems();
static int SDLCALL appEventWatch(void*, SDL_Event* e) {
    if (e->type == SDL_APP_WILLENTERBACKGROUND || e->type == SDL_APP_TERMINATING) {
        if (g_worldMutex) SDL_LockMutex(g_worldMutex);
        if (worldScreenActive()) { if (g_screen == SCR_INV) returnLooseItems(); saveWorld(); }
        saveSettings();
        if (g_worldMutex) SDL_UnlockMutex(g_worldMutex);
    }
    return 1;
}

static void createTextures() {
    if (g_atlasTex) SDL_DestroyTexture(g_atlasTex);
    if (g_fontTex) SDL_DestroyTexture(g_fontTex);
    g_atlasTex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, ATLAS, ATLAS);
    SDL_UpdateTexture(g_atlasTex, NULL, g_atlasPx, ATLAS * 4);
    SDL_SetTextureBlendMode(g_atlasTex, SDL_BLENDMODE_BLEND);
    g_fontTex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, FONT_TEX_W, FONT_TEX_H);
    SDL_UpdateTexture(g_fontTex, NULL, g_fontPx, FONT_TEX_W * 4);
    SDL_SetTextureBlendMode(g_fontTex, SDL_BLENDMODE_BLEND);
    createGlowTexture();
}

static void initOffsets() {
    for (int dz = -13; dz <= 13; dz++) for (int dx = -13; dx <= 13; dx++) g_offsets.push_back(std::make_pair(dx, dz));
    std::sort(g_offsets.begin(), g_offsets.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
        return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second; });
}
static double nowMs() { return SDL_GetPerformanceCounter() * 1000.0 / (double)SDL_GetPerformanceFrequency(); }

static void manageChunks(double budgetMs, int rd) {
    double t0 = nowMs();
    int pcx = ifloor(P.x) >> 4, pcz = ifloor(P.z) >> 4;
    // выгрузка далёких
    std::vector<Chunk*> far;
    for (auto& kv : g_chunks) { Chunk* c = kv.second; if (abs(c->cx - pcx) > rd + 3 || abs(c->cz - pcz) > rd + 3) far.push_back(c); }
    for (Chunk* c : far) unloadChunk(c);
    // срочно: изменённые рядом с игроком
    for (auto& o : g_offsets) {
        if (o.first * o.first + o.second * o.second > 8) break;
        Chunk* c = getChunk(pcx + o.first, pcz + o.second);
        if (c && c->dirty && neighborsLoaded(c->cx, c->cz)) buildChunkMesh(c);
    }
    int loaded = 0, meshed = 0;
    int rl = rd + 1;
    for (auto& o : g_offsets) {
        if (o.first * o.first + o.second * o.second > (rl + 1) * (rl + 1)) break;
        if (abs(o.first) > rl || abs(o.second) > rl) continue;
        int cx = pcx + o.first, cz = pcz + o.second;
        if (!getChunk(cx, cz)) { loadOrGenChunk(cx, cz); loaded++; if (nowMs() - t0 > budgetMs * 0.5) break; }
    }
    for (auto& o : g_offsets) {
        if (o.first * o.first + o.second * o.second > (rd + 1) * (rd + 1)) break;
        if (abs(o.first) > rd || abs(o.second) > rd) continue;
        Chunk* c = getChunk(pcx + o.first, pcz + o.second);
        if (!c || !c->dirty || !neighborsLoaded(c->cx, c->cz)) continue;
        buildChunkMesh(c); meshed++;
        if (nowMs() - t0 > budgetMs) break;
    }
    (void)loaded; (void)meshed;
}

static void giveCreativeKit() {
    const int kit[9] = {B_GRASS, B_STONE, B_PLANKS, B_GLASS, B_LOG, B_TORCH, B_STONEBRICK, B_SAND, B_DOOR};
    for (int i = 0; i < 9; i++) P.inv[i] = mkStack(kit[i], maxStack(kit[i]));
}
static void startNewWorld(int slot, u32 seed, int mode) {
    resetWorldState();
    g_worldSlot = slot; g_worldName = "МИР " + std::to_string(slot);
    worldInitNoise(seed); g_mode = mode;
    int sx = 0, sz = 0;
    for (int i = 0; i < 4000; i++) {
        float a = i * 0.7f, r = i * 3.f;
        int x = (int)(cosf(a) * r), z = (int)(sinf(a) * r);
        ColInfo ci = columnInfo(x, z);
        if (ci.biome != BI_OCEAN && ci.biome != BI_BEACH && ci.biome != BI_MOUNT && ci.h > SEA + 2 && ci.h < 90) { sx = x; sz = z; break; }
    }
    P.x = sx + 0.5; P.z = sz + 0.5; P.y = 100; P.spawnX = sx; P.spawnZ = sz; P.spawnY = 100;
    g_cam.yaw = 0.6f; g_cam.pitch = -0.1f;
    if (mode == GM_CREATIVE) giveCreativeKit();
    g_needSpawnY = true;
    g_screen = SCR_LOADING;
}
static void continueWorld(int slot) {
    if (!loadWorld(slot)) { showMsg("НЕ УДАЛОСЬ ЗАГРУЗИТЬ МИР"); return; }
    g_needSpawnY = false;
    g_screen = SCR_LOADING;
}
static bool worldScreenActive() {
    return g_screen == SCR_GAME || g_screen == SCR_PAUSE || g_screen == SCR_INV || g_screen == SCR_DEAD || (g_screen == SCR_SETTINGS && g_settingsBack == SCR_PAUSE);
}
static void exitToMenu() {
    saveWorld();
    resetWorldState();
    refreshWorldList();
    g_screen = SCR_WORLDS;
}

// ---------------------------------------------------------------- касания
static bool g_tapPlace = false, g_lookDig = false, g_lookUse = false, g_holdStarted = false;
static bool g_placeEdge = false, g_digEdge = false, g_jumpEdge = false;
static Uint32 g_lastJumpTap = 0;

static bool inCircle(float x, float y, const float* b, float k = 1.2f) { float dx = x - b[0], dy = y - b[1]; return dx * dx + dy * dy <= b[2] * b[2] * k * k; }
static bool inRect(float x, float y, const SDL_Rect& r) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static void toggleFly() {
    if (g_mode != GM_CREATIVE) return;
    P.flying = !P.flying; if (P.flying) P.vy = 2.f;
    showMsg(P.flying ? "ПОЛЁТ ВКЛЮЧЁН" : "ПОЛЁТ ВЫКЛЮЧЕН", 1.2f);
}
static void selectSlot(int s) { P.sel = (s + 9) % 9; g_heldNameT = 2.f; g_dig.active = false; }
static void openPause() { g_screen = SCR_PAUSE; g_ui.focus = -1; saveWorld(); }

static void fingerDown(SDL_FingerID id, float x, float y) {
    Finger f; f.id = id; f.x0 = f.x = x; f.y0 = f.y = y; f.t0 = SDL_GetTicks(); f.moved = false; f.hold = false; f.role = FR_UI;
    if (g_screen == SCR_GAME && !P.dead) {
        g_touchUsed = true;
        computeTouchLayout();
        const TouchLayout& t = g_tl;
        if (inRect(x, y, t.pause)) { openPause(); f.role = FR_NONE; }
        else if (g_mode == GM_CREATIVE && inRect(x, y, t.fly)) { toggleFly(); f.role = FR_NONE; }
        else if (inRect(x, y, t.inv)) { openInventory(INV_PLAYER); f.role = FR_NONE; }
        else if (y >= t.hotY && x >= t.hotX && x < t.hotX + t.hotS * 9) { selectSlot((int)((x - t.hotX) / t.hotS)); f.role = FR_NONE; }
        else if (inCircle(x, y, t.bJump)) {
            f.role = FR_JUMP; g_jumpEdge = true;
            Uint32 now = SDL_GetTicks();
            if (g_mode == GM_CREATIVE && now - g_lastJumpTap < 300) toggleFly();
            g_lastJumpTap = now;
        }
        else if (P.flying && inCircle(x, y, t.bDown)) f.role = FR_DOWN;
        else if (x < g_W * 0.42f && y > g_H * 0.12f) {
            f.role = FR_JOY; g_joyActive = true;
            g_joyBaseX = clampf(x, t.jr * 1.05f, g_W * 0.42f); g_joyBaseY = clampf(y, t.jr * 1.05f, g_H - t.jr * 1.05f);
            g_joyKnobX = x; g_joyKnobY = y;
        } else f.role = FR_LOOK;
    }
    g_fingers.push_back(f);
}
static void fingerMove(SDL_FingerID id, float x, float y) {
    for (Finger& f : g_fingers) if (f.id == id) {
        float dx = x - f.x, dy = y - f.y;
        f.x = x; f.y = y;
        if (!f.hold && fabsf(f.x - f.x0) + fabsf(f.y - f.y0) > g_H * 0.03f) f.moved = true;   // после начала копания палец можно двигать — прицел
        if (g_screen != SCR_GAME) return;
        if (f.role == FR_LOOK) {
            float k = 2.8f * g_set.sens / g_H;
            g_cam.yaw += dx * k; g_cam.pitch = clampf(g_cam.pitch - dy * k, -1.55f, 1.55f);
        } else if (f.role == FR_JOY) {
            float r = g_tl.jr; float ox = x - g_joyBaseX, oy = y - g_joyBaseY; float l = sqrtf(ox * ox + oy * oy);
            if (l > r) { // база следует за пальцем
                g_joyBaseX += ox / l * (l - r); g_joyBaseY += oy / l * (l - r); ox = x - g_joyBaseX; oy = y - g_joyBaseY;
            }
            g_joyKnobX = x; g_joyKnobY = y;
        }
        return;
    }
}
static void fingerUp(SDL_FingerID id, float x, float y) {
    for (size_t i = 0; i < g_fingers.size(); i++) if (g_fingers[i].id == id) {
        Finger f = g_fingers[i];
        g_fingers.erase(g_fingers.begin() + i);
        if (f.role == FR_UI && !f.moved) { SDL_Point p = {(int)x, (int)y}; g_ui.taps.push_back(p); g_ui.pad = false; }
        if (f.role == FR_JOY) g_joyActive = false;
        if (f.role == FR_LOOK && !f.moved && !f.hold && SDL_GetTicks() - f.t0 < TAP_MS) g_tapPlace = true;
        return;
    }
}
static void collectTouchInput() {
    g_btnJump = g_btnDown = false; g_lookDig = g_lookUse = false;
    bool anyHold = false;
    float mx = 0, mz = 0;
    for (Finger& f : g_fingers) {
        switch (f.role) {
            case FR_JOY: {
                float r = g_tl.jr; float ox = (f.x - g_joyBaseX) / r, oy = (f.y - g_joyBaseY) / r;
                float l = sqrtf(ox * ox + oy * oy);
                if (l > 0.12f) { if (l > 1) { ox /= l; oy /= l; } mx = ox; mz = -oy; }
                if (l > 0.97f && -oy > 0.85f) g_in.sprint = true;
                break;
            }
            case FR_JUMP: g_btnJump = true; break;
            case FR_DOWN: g_btnDown = true; break;
            case FR_LOOK:
                if (!f.hold && !f.moved && SDL_GetTicks() - f.t0 >= HOLD_MS) f.hold = true;
                if (f.hold) {
                    anyHold = true;
                    int held = P.inv[P.sel].id;
                    // с луком удержание натягивает тетиву, с едой — ест; иначе — копает
                    if (held == I_BOW || foodValue(held) > 0) g_lookUse = true; else g_lookDig = true;
                }
                break;
        }
    }
    if (anyHold && !g_holdStarted) { if (g_lookDig) g_digEdge = true; else g_placeEdge = true; }
    g_holdStarted = anyHold;
    if (mx != 0 || mz != 0) { g_in.mx = mx; g_in.mz = mz; }
}

// ---------------------------------------------------------------- геймпад и клавиатура
static bool g_keys[512];
static bool g_mouseL = false, g_mouseR = false;
static bool g_padL2 = false, g_padR2 = false;
static void openPad(int idx) {
    if (g_pad || !SDL_IsGameController(idx)) return;
    g_pad = SDL_GameControllerOpen(idx);
    if (g_pad) showMsg("ГЕЙМПАД ПОДКЛЮЧЁН", 1.5f);
}
static float padAxis(SDL_GameControllerAxis a) {
    if (!g_pad) return 0;
    float v = SDL_GameControllerGetAxis(g_pad, a) / 32767.f;
    float dz = 0.16f;
    if (fabsf(v) < dz) return 0;
    return (v - (v > 0 ? dz : -dz)) / (1 - dz);
}
static void backAction() {
    switch (g_screen) {
        case SCR_GAME: openPause(); break;
        case SCR_PAUSE: g_screen = SCR_GAME; break;
        case SCR_INV: closeInventory(); break;
        case SCR_SETTINGS: saveSettings(); g_screen = g_settingsBack; break;
        case SCR_WORLDS: g_screen = SCR_MAIN; break;
        case SCR_NEWWORLD: g_screen = SCR_WORLDS; break;
        case SCR_CONFIRM_DEL: g_screen = SCR_WORLDS; break;
        case SCR_INTRO: g_introSkip = true; break;
        case SCR_MAIN: g_running = false; break;
        default: break;
    }
    g_ui.focus = -1;
}
static void padButton(int b) {
    g_ui.pad = true; g_touchUsed = false;
    bool game = g_screen == SCR_GAME && !P.dead;
    switch (b) {
        case SDL_CONTROLLER_BUTTON_START: if (game) openPause(); else if (g_screen == SCR_PAUSE) g_screen = SCR_GAME; else if (g_screen == SCR_INV) closeInventory(); break;
        case SDL_CONTROLLER_BUTTON_A:
            if (game) { g_jumpEdge = true; Uint32 now = SDL_GetTicks(); if (g_mode == GM_CREATIVE && now - g_lastJumpTap < 300) toggleFly(); g_lastJumpTap = now; }
            else g_ui.activate = true;
            break;
        case SDL_CONTROLLER_BUTTON_B: if (!game) backAction(); break;
        case SDL_CONTROLLER_BUTTON_X: if (!game) g_ui.alt = true; break;
        case SDL_CONTROLLER_BUTTON_Y: if (game) openInventory(INV_PLAYER); else if (g_screen == SCR_INV) closeInventory(); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            if (game) selectSlot(P.sel + 1);
            else if (g_screen == SCR_INV) { if (g_bookOpen && g_mode != GM_CREATIVE) g_bookPage++; else if (g_mode == GM_CREATIVE) g_creativePage = std::min(g_creativePage + 1, ((int)g_creativeItems.size() + 44) / 45 - 1); }
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            if (game) selectSlot(P.sel - 1);
            else if (g_screen == SCR_INV) { if (g_bookOpen && g_mode != GM_CREATIVE) g_bookPage = std::max(0, g_bookPage - 1); else g_creativePage = std::max(0, g_creativePage - 1); }
            break;
        case SDL_CONTROLLER_BUTTON_BACK: if (game) openInventory(INV_PLAYER); else if (g_screen == SCR_INV) g_bookOpen = !g_bookOpen; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: if (!game) g_ui.navDir = 0; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: if (!game) g_ui.navDir = 1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: if (!game) g_ui.navDir = 2; else selectSlot(P.sel - 1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if (!game) g_ui.navDir = 3; else selectSlot(P.sel + 1); break;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: if (game) g_in.sprint = !g_in.sprint; break;
    }
}
static void keyDown(SDL_Keycode k) {
    bool game = g_screen == SCR_GAME && !P.dead;
    if (k == SDLK_AC_BACK || k == SDLK_ESCAPE) { backAction(); return; }
    if (k >= SDLK_1 && k <= SDLK_9 && game) selectSlot(k - SDLK_1);
    if (k == SDLK_e || k == SDLK_TAB) { if (game) openInventory(INV_PLAYER); else if (g_screen == SCR_INV) closeInventory(); }
    if (k == SDLK_SPACE && game) { g_jumpEdge = true; Uint32 now = SDL_GetTicks(); if (g_mode == GM_CREATIVE && now - g_lastJumpTap < 300) toggleFly(); g_lastJumpTap = now; }
    if (k == SDLK_F3) g_set.showFps = !g_set.showFps;
    if (!game) {
        if (k == SDLK_UP) g_ui.navDir = 0; else if (k == SDLK_DOWN) g_ui.navDir = 1; else if (k == SDLK_LEFT) g_ui.navDir = 2; else if (k == SDLK_RIGHT) g_ui.navDir = 3;
        else if (k == SDLK_RETURN) g_ui.activate = true;
        if (k == SDLK_UP || k == SDLK_DOWN || k == SDLK_LEFT || k == SDLK_RIGHT || k == SDLK_RETURN) g_ui.pad = true;
    }
}

static void gatherGameInput(float dt) {
    g_in.mx = g_in.mz = 0;
    bool wasSprint = g_in.sprint;
    g_in.sprint = false;
    collectTouchInput();
    // клавиатура
    auto key = [](SDL_Scancode s) { return g_keys[s]; };
    if (key(SDL_SCANCODE_W)) g_in.mz += 1;
    if (key(SDL_SCANCODE_S)) g_in.mz -= 1;
    if (key(SDL_SCANCODE_D)) g_in.mx += 1;
    if (key(SDL_SCANCODE_A)) g_in.mx -= 1;
    if (key(SDL_SCANCODE_LCTRL)) g_in.sprint = true;
    // геймпад
    if (g_pad) {
        float lx = padAxis(SDL_CONTROLLER_AXIS_LEFTX), ly = padAxis(SDL_CONTROLLER_AXIS_LEFTY);
        if (lx != 0 || ly != 0) { g_in.mx = lx; g_in.mz = -ly; g_ui.pad = true; if (wasSprint && ly < -0.5f) g_in.sprint = true; }
        float rx = padAxis(SDL_CONTROLLER_AXIS_RIGHTX), ry = padAxis(SDL_CONTROLLER_AXIS_RIGHTY);
        float sp = 3.2f * g_set.sens;
        g_cam.yaw += rx * fabsf(rx) * sp * dt;
        g_cam.pitch = clampf(g_cam.pitch - ry * fabsf(ry) * sp * 0.8f * dt, -1.55f, 1.55f);
        bool r2 = padAxis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.4f, l2 = padAxis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.4f;
        if (r2 && !g_padR2) g_digEdge = true;
        if (l2 && !g_padL2) g_placeEdge = true;
        g_padR2 = r2; g_padL2 = l2;
    }
    bool padJump = g_pad && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_A);
    bool padDown = g_pad && SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_B);
    g_in.jump = g_btnJump || key(SDL_SCANCODE_SPACE) || padJump;
    g_in.down = g_btnDown || key(SDL_SCANCODE_LSHIFT) || padDown;
    g_in.jumpPressed = g_jumpEdge;
    g_in.dig = g_lookDig || g_mouseL || g_padR2;
    g_in.digPressed = g_digEdge;
    g_in.place = g_lookUse || g_mouseR || g_padL2;
    g_in.placePressed = g_placeEdge || g_tapPlace;
    if (g_tapPlace) g_in.place = true;
    g_jumpEdge = g_digEdge = g_placeEdge = g_tapPlace = false;
}

// ---------------------------------------------------------------- обновление мира
static void updateGame(float dt) {
    g_playTime += dt;
    g_dayTime += dt / DAY_LEN; if (g_dayTime >= 1) g_dayTime -= 1;
    g_msgT = std::max(0.f, g_msgT - dt);
    g_heldNameT = std::max(0.f, g_heldNameT - dt);
    for (size_t i = 0; i < g_picks.size();) { g_picks[i].t -= dt; if (g_picks[i].t <= 0) g_picks.erase(g_picks.begin() + i); else i++; }
    g_tickAcc += dt;
    int ticks = 0;
    while (g_tickAcc >= 0.05f && ticks < 4) { g_tickAcc -= 0.05f; g_tick++; runScheduledUpdates(); ticks++; }
    if (g_tickAcc > 0.2f) g_tickAcc = 0;
    tickFurnaces(dt);
    tickGrowth(dt);
    // физика игрока мелкими шагами
    float rem = dt;
    while (rem > 0) { float s = std::min(rem, 1.f / 60.f); updatePlayerPhysics(s); rem -= s; }
    updateSurvival(dt);
    updateInteraction(dt);
    if (P.dead && g_screen == SCR_GAME) { g_screen = SCR_DEAD; g_ui.focus = -1; }
    static Rng mobRng(777);
    for (Mob& m : g_mobs) if (!m.dead) updateMob(m, dt, mobRng);
    for (size_t i = 0; i < g_mobs.size();) {
        Mob& m = g_mobs[i];
        double dx = m.x - P.x, dz = m.z - P.z;
        bool far = dx * dx + dz * dz > 96.0 * 96.0 || !isLoaded(ifloor(m.x), ifloor(m.z));
        if (m.dead || far) { g_mobs[i] = g_mobs.back(); g_mobs.pop_back(); continue; }
        i++;
    }
    g_spawnT -= dt;
    if (g_spawnT <= 0) { g_spawnT = 1.5f; spawnMobs(mobRng); }
    updateArrows(dt);
    updateDrops(dt);
    updateParticles(dt);
    g_autosaveT += dt;
    if (g_autosaveT > 60.f) { g_autosaveT = 0; saveWorld(); }
}

static void renderWorld() {
    int ex = ifloor(P.x), ey = ifloor(P.y + PL_EYE), ez = ifloor(P.z);
    u8 headB = getBlock(ex, ey, ez);
    bool underwater = headB == B_WATER, inLava = headB == B_LAVA;
    updateSkyColors(underwater);
    g_renderDist = g_set.renderDist;
    g_fogEnd = g_renderDist * 16.f - 6.f; g_fogStart = g_fogEnd * 0.62f;
    if (underwater) { g_fogStart = 2.f; g_fogEnd = 18.f; }
    if (inLava) { g_fogStart = 0.f; g_fogEnd = 3.f; }
    g_cam.x = P.x; g_cam.y = P.y + PL_EYE - fabsf(sinf(P.bobPhase)) * 0.06f * P.bobAmt; g_cam.z = P.z;
    camUpdate();
    g_shaders = g_set.shaders; g_fxTime = g_time;
    computeFX(g_dayTime, g_time);
    updateAnimTiles(g_time);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_ren, (Uint8)(g_skyCol[0] * 255), (Uint8)(g_skyCol[1] * 255), (Uint8)(g_skyCol[2] * 255), 255);
    SDL_RenderClear(g_ren);
    if (!underwater) drawSky();
    findVisibleSections();
    drawSections();
    if (!underwater) emitClouds(g_time);
    emitMobs();
    emitDrops(g_time);
    emitArrows();
    emitParticles();
    emitCrack();
    flushPolys(g_atlasTex);
    if (g_target.hit && !P.dead) {
        u8 b = getBlock(g_target.x, g_target.y, g_target.z);
        float y1 = (b == B_BED) ? 9 / 16.f : 1.f;
        drawBlockOutline(g_target.x, g_target.y, g_target.z, -0.002f, -0.002f, -0.002f, 1.002f, y1 + 0.002f, 1.002f);
    }
    drawLightHalos(g_cam.x, g_cam.y, g_cam.z);
    emitHand();
    flushPolys(g_atlasTex);
    drawLensFX(g_cam.x, g_cam.y, g_cam.z, underwater);
    if (underwater) fillRect(0, 0, (float)g_W, (float)g_H, C(20, 60, 160, 90));
    if (inLava) fillRect(0, 0, (float)g_W, (float)g_H, C(220, 90, 10, 200));
    if (P.hurtFlash > 0) fillRect(0, 0, (float)g_W, (float)g_H, C(200, 0, 0, (int)(P.hurtFlash * 250)));
}

// ---------------------------------------------------------------- экраны меню
// ---------------------------------------------------------------- заставка: кирка разбивает камень, появляется название
static float g_introStart = -1.f;
struct IntroChip { float x, y, vx, vy, rot, vr, size; float u, v; int tile; };
static std::vector<IntroChip> g_introChips;
static int g_introHits = 0; static bool g_introBroken = false, g_introLettersSnd[16];
// изометрический блок с трещинами (как значок в сумке, но крупно)
static void drawIsoBlock(int id, float cx, float cy, float r, int crack, float bright) {
    float h = r * 0.5f;
    float top[4][2] = {{cx, cy - r}, {cx + r * 0.87f, cy - h}, {cx, cy}, {cx - r * 0.87f, cy - h}};
    float left[4][2] = {{cx - r * 0.87f, cy - h}, {cx, cy}, {cx, cy + r}, {cx - r * 0.87f, cy + h}};
    float right[4][2] = {{cx, cy}, {cx + r * 0.87f, cy - h}, {cx + r * 0.87f, cy + h}, {cx, cy + r}};
    const float (*faces[3])[2] = {top, left, right};
    const BlockDef& d = BD[id];
    int tiles[3] = {d.tex[2], d.tex[4], d.tex[0]}; float shade[3] = {1.0f, 0.78f, 0.6f};
    for (int pass = 0; pass < (crack >= 0 ? 2 : 1); pass++) {
        SDL_Vertex v[12]; int idx[18];
        for (int f = 0; f < 3; f++) {
            const float UVp[4][2] = {{0, 0}, {16, 0}, {16, 16}, {0, 16}};
            for (int k = 0; k < 4; k++) {
                SDL_Vertex& vv = v[f * 4 + k];
                vv.position.x = faces[f][k][0]; vv.position.y = faces[f][k][1];
                float uv[2]; tileUV(pass ? T_CRACK0 + crack : tiles[f], UVp[k][0], UVp[k][1], uv);
                vv.tex_coord.x = uv[0]; vv.tex_coord.y = uv[1];
                Uint8 c = (Uint8)clampi((int)(255 * shade[f] * bright), 0, 255);
                vv.color.r = c; vv.color.g = c; vv.color.b = c; vv.color.a = 255;
            }
            int* p = idx + f * 6; int b = f * 4;
            p[0] = b; p[1] = b + 1; p[2] = b + 2; p[3] = b; p[4] = b + 2; p[5] = b + 3;
        }
        SDL_SetTextureColorMod(g_atlasTex, 255, 255, 255); SDL_SetTextureAlphaMod(g_atlasTex, 255);
        SDL_RenderGeometry(g_ren, g_atlasTex, v, 12, idx, 18);
    }
}
// повёрнутый тайл (кирка): pivot — точка тайла (0..16), вокруг которой поворот
static void drawTileRot(int tile, float px, float py, float size, float ang, float pivU, float pivV) {
    float ca = cosf(ang), sa = sinf(ang), k = size / 16.f;
    const float U[4] = {0, 16, 16, 0}, V[4] = {0, 0, 16, 16};
    SDL_Vertex v[4]; int idx[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 4; i++) {
        float lx = (U[i] - pivU) * k, ly = (V[i] - pivV) * k;
        v[i].position.x = px + lx * ca - ly * sa; v[i].position.y = py + lx * sa + ly * ca;
        float uv[2]; tileUV(tile, U[i], V[i], uv); v[i].tex_coord.x = uv[0]; v[i].tex_coord.y = uv[1];
        v[i].color.r = v[i].color.g = v[i].color.b = v[i].color.a = 255;
    }
    SDL_SetTextureColorMod(g_atlasTex, 255, 255, 255); SDL_SetTextureAlphaMod(g_atlasTex, 255);
    SDL_RenderGeometry(g_ren, g_atlasTex, v, 4, idx, 6);
}
static void introBurst(float cx, float cy, float r, int n, float power) {
    Rng rr(SDL_GetTicks() * 7 + n);
    const int tiles[3] = {T_STONE, T_STONE, T_COBBLE};
    for (int i = 0; i < n; i++) {
        IntroChip c;
        float a = rr.f() * 6.2831853f, sp = (0.3f + rr.f()) * power;
        c.x = cx + rr.sf() * r * 0.5f; c.y = cy + rr.sf() * r * 0.5f;
        c.vx = cosf(a) * sp; c.vy = sinf(a) * sp - power * 0.6f;
        c.rot = rr.f() * 6.28f; c.vr = rr.sf() * 10.f; c.size = r * (0.07f + rr.f() * 0.12f);
        c.u = (float)rr.range(12); c.v = (float)rr.range(12); c.tile = tiles[rr.range(3)];
        g_introChips.push_back(c);
    }
}
static const float INTRO_HIT[4] = {0.75f, 1.2f, 1.65f, 2.1f};   // моменты ударов
static const float INTRO_BREAK = 2.3f, INTRO_LOGO = 2.45f, INTRO_FADE = 3.9f, INTRO_END = 4.7f;
static void screenIntro(float dt) {
    if (g_introStart < 0) { g_introStart = g_time; memset(g_introLettersSnd, 0, sizeof(g_introLettersSnd)); }
    float t = g_time - g_introStart;
    if (!g_ui.taps.empty() || g_ui.activate || g_introSkip) { t = INTRO_END; g_introSkip = false; g_ui.taps.clear(); g_ui.activate = false; playSound(SND_CLICK, 0.4f); }
    if (t >= INTRO_END) { g_screen = SCR_MAIN; g_introChips.clear(); g_ui.focus = -1; screenMain(); return; }
    float S = uiS(), cx = g_W * 0.5f, cy = g_H * 0.5f, r = g_H * 0.2f;
    // фон: меню, затемнённое; в конце затемнение уходит
    drawBackdrop(g_time);
    float dark = t < INTRO_FADE ? 1.f : 1.f - smooth01(INTRO_FADE, INTRO_END, t);
    fillRect(0, 0, (float)g_W, (float)g_H, C(6, 8, 14, (int)(235 * dark)));
    if (t < 0.45f) fillRect(0, 0, (float)g_W, (float)g_H, C(0, 0, 0, (int)(255 * (1.f - t / 0.45f))));
    // удары: звук, тряска, осколки
    while (g_introHits < 4 && t >= INTRO_HIT[g_introHits]) {
        playSound(SND_DIG0 + SM_STONE, 0.9f, 0.85f + g_introHits * 0.06f);
        introBurst(cx + r * 0.25f, cy - r * 0.35f, r * 0.4f, 6, g_H * 0.7f);
        g_introHits++;
    }
    if (!g_introBroken && t >= INTRO_BREAK) {
        g_introBroken = true;
        playSound(SND_BREAK0 + SM_STONE, 1.0f, 0.9f); playSound(SND_POOF, 0.5f, 0.7f);
        introBurst(cx, cy, r * 1.4f, 70, g_H * 1.4f);
    }
    if (!g_introBroken) {
        int stage = -1;
        for (int i = 0; i < 4; i++) if (t >= INTRO_HIT[i]) stage = std::min(4, i + 1);
        float since = 1e9f; for (int i = 0; i < 4; i++) if (t >= INTRO_HIT[i]) since = t - INTRO_HIT[i];
        float shake = since < 0.12f ? (1.f - since / 0.12f) * r * 0.05f : 0.f;
        float appear = smooth01(0.1f, 0.55f, t);
        float bob = sinf(t * 2.f) * r * 0.02f;
        // тень под блоком
        fillRect(cx - r * 0.75f, cy + r * 1.05f, r * 1.5f, r * 0.08f, C(0, 0, 0, (int)(90 * appear)));
        drawIsoBlock(B_STONE, cx + (since < 0.12f ? sinf(since * 90.f) * shake : 0.f), cy + bob + (1.f - appear) * r * 0.3f, r * (0.85f + 0.15f * appear), stage, 0.6f + 0.4f * appear);
        // кирка: замах — удар
        const float UP = -1.15f, DOWN = 0.25f, SWING = 0.28f;
        int last = -1; for (int i = 0; i < 4; i++) if (t >= INTRO_HIT[i]) last = i;
        float nextHit = last < 3 ? INTRO_HIT[last + 1] : 1e9f;
        float ang;
        if (nextHit - t < SWING) { float k = 1.f - (nextHit - t) / SWING; ang = UP + (DOWN - UP) * k * k; }   // удар с ускорением
        else if (last < 0 || last == 3) ang = last == 3 ? DOWN : UP;                                          // ждёт в замахе / застыл после последнего удара
        else ang = DOWN + (UP - DOWN) * smooth01(0.f, 0.16f, t - INTRO_HIT[last]);                            // отскок и новый замах
        float pickIn = smooth01(0.25f, 0.6f, t);
        float pivX = cx + r * 1.15f + (1.f - pickIn) * r * 1.5f, pivY = cy + r * 0.2f;
        drawTileRot(T_TOOL0 + MT_DIAMOND * 4 + TK_PICK, pivX, pivY, r * 1.35f, ang - 0.35f, 2.f, 13.5f);
        // искра в момент удара
        if (since < 0.08f) { const float wc[3] = {1.f, 0.95f, 0.8f}; drawGlow(cx + r * 0.3f, cy - r * 0.35f, r * 0.5f, wc, 0.8f * (1.f - since / 0.08f)); }
    } else {
        // вспышка при разрушении
        float ft = t - INTRO_BREAK;
        if (ft < 0.35f) { const float wc[3] = {1.f, 0.9f, 0.7f}; drawGlow(cx, cy, r * (1.2f + ft * 4.f), wc, 0.9f * (1.f - ft / 0.35f)); }
    }
    // осколки
    for (size_t i = 0; i < g_introChips.size();) {
        IntroChip& c = g_introChips[i];
        c.vy += g_H * 2.4f * dt; c.x += c.vx * dt; c.y += c.vy * dt; c.rot += c.vr * dt;
        if (c.y > g_H + c.size * 2) { g_introChips[i] = g_introChips.back(); g_introChips.pop_back(); continue; }
        float ca = cosf(c.rot), sa = sinf(c.rot), hs = c.size * 0.5f;
        SDL_Vertex v[4]; int idx[6] = {0, 1, 2, 0, 2, 3};
        const float L[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int k = 0; k < 4; k++) {
            v[k].position.x = c.x + (L[k][0] * ca - L[k][1] * sa) * hs; v[k].position.y = c.y + (L[k][0] * sa + L[k][1] * ca) * hs;
            float uv[2]; tileUV(c.tile, c.u + (L[k][0] > 0 ? 4 : 0), c.v + (L[k][1] > 0 ? 4 : 0), uv);
            v[k].tex_coord.x = uv[0]; v[k].tex_coord.y = uv[1];
            Uint8 sh = (Uint8)(150 + (int)(c.u * 7) % 100); v[k].color.r = v[k].color.g = v[k].color.b = sh; v[k].color.a = 255;
        }
        SDL_RenderGeometry(g_ren, g_atlasTex, v, 4, idx, 6);
        i++;
    }
    // название
    if (t >= INTRO_LOGO) {
        float lt = t - INTRO_LOGO;
        for (int i = 0; i < 10; i++) if (!g_introLettersSnd[i] && lt >= i * 0.07f) { g_introLettersSnd[i] = true; playSound(SND_CLICK, 0.35f, 0.9f + i * 0.06f); }
        drawLogo(lt, 1.f);
        float sa = smooth01(1.0f, 1.4f, lt);
        drawText("ПЕСОЧНИЦА ИЗ КУБИКОВ", g_W / 2.f, logoY() + logoPx() * 9, fontPx(), C(230, 230, 230, (int)(255 * sa)), 1);
    }
    if (t < INTRO_FADE) drawText("НАЖМИ, ЧТОБЫ ПРОПУСТИТЬ", g_W - insetX() - S * 2, g_H - S * 6, fontPx() * 0.7f, C(160, 160, 170, (int)(160 * smooth01(0.5f, 1.2f, t))), 2);
    // кнопки меню проявляются в конце
    if (t >= INTRO_FADE) {
        float k = smooth01(INTRO_FADE, INTRO_END, t);
        float bw = S * 70, bh = S * 11, bx = (g_W - bw) / 2, by = g_H * 0.45f;
        for (int i = 0; i < 3; i++) fillRect(bx, by + bh * 1.3f * i, bw, bh, C(70, 78, 66, (int)(225 * k)));
    }
}

static void screenMain() {
    drawBackdrop(g_time);
    float S = uiS();
    drawLogo(-1.f, 1.f);
    drawText("ПЕСОЧНИЦА ИЗ КУБИКОВ", g_W / 2.f, logoY() + logoPx() * 9, fontPx(), C(230, 230, 230), 1);
    float bw = S * 70, bh = S * 11, bx = (g_W - bw) / 2, by = g_H * 0.45f;
    if (button(1, bx, by, bw, bh, "ИГРАТЬ")) { refreshWorldList(); g_screen = SCR_WORLDS; g_ui.focus = -1; }
    if (button(2, bx, by + bh * 1.3f, bw, bh, "НАСТРОЙКИ")) { g_settingsBack = SCR_MAIN; g_screen = SCR_SETTINGS; g_ui.focus = -1; }
    if (button(3, bx, by + bh * 2.6f, bw, bh, "ВЫХОД")) g_running = false;
}
static void screenWorlds() {
    drawBackdrop(g_time);
    float S = uiS();
    drawTitle("ВЫБОР МИРА", (float)insetTop(), 2.2f);
    float bw = std::min(S * 110, g_W * 0.7f), bh = S * 9.2f, bx = (g_W - bw) / 2 - S * 6, by = insetTop() + S * 13;
    for (int i = 1; i <= MAX_WORLDS; i++) {
        const WorldInfo& wi = g_worldInfos[i];
        float y = by + (i - 1) * bh * 1.15f;
        std::string label;
        if (wi.exists) {
            int mins = (int)(wi.playTime / 60);
            label = wi.name + " - " + (wi.mode == GM_CREATIVE ? "ТВОРЧЕСКИЙ" : "ВЫЖИВАНИЕ") + " - " + std::to_string(mins) + " МИН";
        } else label = "ПУСТО: СОЗДАТЬ НОВЫЙ МИР";
        if (button(10 + i, bx, y, bw, bh, label, true, 0.9f)) {
            if (wi.exists) continueWorld(i);
            else { g_worldSlot = i; g_newSeed = (u32)(SDL_GetPerformanceCounter() ^ (SDL_GetTicks() * 2654435761u)) % 1000000000u; g_newMode = GM_SURVIVAL; g_screen = SCR_NEWWORLD; g_ui.focus = -1; }
        }
        if (wi.exists && button(20 + i, bx + bw + S * 2, y, S * 10, bh, "X")) { g_delSlot = i; g_screen = SCR_CONFIRM_DEL; g_ui.focus = -1; }
    }
    if (button(30, (g_W - S * 40) / 2, g_H - S * 11, S * 40, S * 9, "НАЗАД")) { g_screen = SCR_MAIN; g_ui.focus = -1; }
}
static void screenNewWorld() {
    drawBackdrop(g_time);
    float S = uiS();
    drawTitle("НОВЫЙ МИР " + std::to_string(g_worldSlot), (float)insetTop() + S * 4, 2.2f);
    float bw = S * 80, bh = S * 11, bx = (g_W - bw) / 2, by = g_H * 0.3f;
    if (button(40, bx, by, bw, bh, std::string("РЕЖИМ: ") + (g_newMode == GM_CREATIVE ? "ТВОРЧЕСКИЙ" : "ВЫЖИВАНИЕ"))) g_newMode ^= 1;
    drawText(g_newMode == GM_CREATIVE ? "ВСЕ БЛОКИ БЕЗ ОГРАНИЧЕНИЙ, ПОЛЁТ" : "ЗДОРОВЬЕ, ГОЛОД, ЧУДОВИЩА НОЧЬЮ", g_W / 2.f, by + bh + S * 2, fontPx() * 0.9f, C(220, 220, 220), 1);
    if (button(41, bx, by + bh * 1.7f, bw, bh, "ЗЕРНО: " + std::to_string(g_newSeed))) g_newSeed = (u32)(hash32(g_newSeed + SDL_GetTicks()) % 1000000000u);
    drawText("НАЖМИ, ЧТОБЫ СМЕНИТЬ ЗЕРНО", g_W / 2.f, by + bh * 2.7f + S * 2, fontPx() * 0.9f, C(220, 220, 220), 1);
    if (button(42, bx, by + bh * 3.5f, bw, bh, "СОЗДАТЬ МИР")) startNewWorld(g_worldSlot, g_newSeed, g_newMode);
    if (button(43, (g_W - S * 40) / 2, g_H - insetTop() - S * 10, S * 40, S * 9, "НАЗАД")) { g_screen = SCR_WORLDS; g_ui.focus = -1; }
}
static void screenConfirmDelete() {
    drawBackdrop(g_time);
    float S = uiS();
    drawTitle("УДАЛИТЬ " + g_worldInfos[g_delSlot].name + "?", g_H * 0.3f, 2.f);
    drawText("ЭТО НЕЛЬЗЯ ОТМЕНИТЬ", g_W / 2.f, g_H * 0.3f + S * 10, fontPx(), C(255, 180, 160), 1);
    float bw = S * 40, bh = S * 11;
    if (button(50, g_W / 2.f - bw - S * 2, g_H * 0.55f, bw, bh, "УДАЛИТЬ")) { deleteWorld(g_delSlot); refreshWorldList(); g_screen = SCR_WORLDS; g_ui.focus = -1; }
    if (button(51, g_W / 2.f + S * 2, g_H * 0.55f, bw, bh, "ОТМЕНА")) { g_screen = SCR_WORLDS; g_ui.focus = -1; }
}
static void screenLoading() {
    int rd = std::min(g_set.renderDist, 3);
    manageChunks(28.0, g_set.renderDist);
    int pcx = ifloor(P.x) >> 4, pcz = ifloor(P.z) >> 4;
    int need = 0, done = 0;
    for (int dz = -rd; dz <= rd; dz++) for (int dx = -rd; dx <= rd; dx++) {
        need++; Chunk* c = getChunk(pcx + dx, pcz + dz);
        if (c && c->meshed && !c->dirty) done++;
    }
    drawBackdrop(g_time);
    drawTitle("СОЗДАНИЕ МИРА...", g_H * 0.35f, 2.2f);
    float bw = g_W * 0.5f, bh = uiS() * 4;
    fillRect((g_W - bw) / 2, g_H * 0.55f, bw, bh, C(20, 20, 20, 220));
    fillRect((g_W - bw) / 2, g_H * 0.55f, bw * done / (float)need, bh, C(110, 190, 80));
    if (done >= need) {
        if (g_needSpawnY) {
            int x = ifloor(P.x), z = ifloor(P.z), y = WH - 2;
            while (y > 1 && !BD[getBlock(x, y, z)].solid && getBlock(x, y, z) != B_WATER) y--;
            P.y = y + 1.01; P.spawnY = y + 1;
            g_needSpawnY = false;
            saveWorld();
        }
        // на всякий случай вытолкнуть из блоков
        for (int i = 0; i < 80 && (BD[getBlock(ifloor(P.x), ifloor(P.y), ifloor(P.z))].solid || BD[getBlock(ifloor(P.x), ifloor(P.y + 1), ifloor(P.z))].solid); i++) P.y += 1;
        g_screen = SCR_GAME; g_autosaveT = 0; g_ui.focus = -1;
        if (P.dead) g_screen = SCR_DEAD;
    }
}
static void screenPause() {
    fillRect(0, 0, (float)g_W, (float)g_H, C(0, 0, 0, 140));
    float S = uiS();
    drawTitle("ПАУЗА", g_H * 0.16f, 3.f);
    float bw = S * 70, bh = S * 11, bx = (g_W - bw) / 2, by = g_H * 0.36f;
    if (button(60, bx, by, bw, bh, "ПРОДОЛЖИТЬ")) { g_screen = SCR_GAME; }
    if (button(61, bx, by + bh * 1.3f, bw, bh, "НАСТРОЙКИ")) { g_settingsBack = SCR_PAUSE; g_screen = SCR_SETTINGS; g_ui.focus = -1; }
    if (button(62, bx, by + bh * 2.6f, bw, bh, "СОХРАНИТЬ И ВЫЙТИ")) exitToMenu();
    drawText("МИР СОХРАНЁН", g_W / 2.f, by + bh * 4.2f, fontPx() * 0.9f, C(180, 230, 170), 1);
}
static void screenDead() {
    fillRect(0, 0, (float)g_W, (float)g_H, C(120, 0, 0, 120));
    float S = uiS();
    drawTitle("ВЫ ПОГИБЛИ", g_H * 0.25f, 3.5f);
    float bw = S * 70, bh = S * 11, bx = (g_W - bw) / 2, by = g_H * 0.5f;
    if (button(70, bx, by, bw, bh, "ВОЗРОДИТЬСЯ")) { respawnPlayer(); g_needSpawnY = false; g_screen = SCR_LOADING; }
    if (button(71, bx, by + bh * 1.3f, bw, bh, "В ГЛАВНОЕ МЕНЮ")) { respawnPlayer(); exitToMenu(); }
}
static void settingRow(int id, float y, const std::string& name, const std::string& val, int& delta) {
    float S = uiS();
    float w = std::min(S * 150, g_W * 0.8f), x = (g_W - w) / 2, h = S * 8;
    fillRect(x, y, w, h, C(30, 32, 38, 200));
    drawText(name, x + S * 3, y + h / 2 - 3.5f * fontPx(), fontPx(), C(240, 240, 240));
    float bw = S * 12;
    if (button(id, x + w - bw * 3.6f, y, bw, h, "<")) delta = -1;
    drawText(val, x + w - bw * 2.05f, y + h / 2 - 3.5f * fontPx(), fontPx(), C(255, 230, 140), 1);
    if (button(id + 1, x + w - bw, y, bw, h, ">")) delta = 1;
}
static void screenSettings() {
    if (g_settingsBack == SCR_MAIN) drawBackdrop(g_time); else fillRect(0, 0, (float)g_W, (float)g_H, C(0, 0, 0, 170));
    float S = uiS();
    drawTitle("НАСТРОЙКИ", (float)insetTop(), 2.2f);
    float y = insetTop() + S * 11, step = S * 9.f;
    int d;
    char b[32];
    d = 0; settingRow(80, y, "ДАЛЬНОСТЬ ПРОРИСОВКИ", std::to_string(g_set.renderDist) + " ЧАНК.", d); g_set.renderDist = clampi(g_set.renderDist + d, 4, 10); y += step;
    d = 0; snprintf(b, sizeof(b), "%d%%", (int)lroundf(g_set.sens * 100)); settingRow(82, y, "ЧУВСТВИТЕЛЬНОСТЬ", b, d); g_set.sens = clampf(g_set.sens + d * 0.1f, 0.2f, 3.f); y += step;
    d = 0; snprintf(b, sizeof(b), "%d%%", (int)lroundf(g_set.volume * 100)); settingRow(84, y, "ГРОМКОСТЬ", b, d); g_set.volume = clampf(g_set.volume + d * 0.1f, 0.f, 1.f); g_masterVol = g_set.volume; y += step;
    d = 0; snprintf(b, sizeof(b), "%d%%", g_set.inset); settingRow(86, y, "ОТСТУП ОТ КРАЁВ (ВЫРЕЗ)", b, d); g_set.inset = clampi(g_set.inset + d, 0, 12); y += step;
    d = 0; snprintf(b, sizeof(b), "%d%%", (int)lroundf(g_set.uiScale * 100)); settingRow(88, y, "РАЗМЕР ТЕКСТА", b, d); g_set.uiScale = clampf(g_set.uiScale + d * 0.1f, 0.7f, 1.4f); y += step;
    d = 0; settingRow(90, y, "АВТОПРЫЖОК", g_set.autoJump ? "ВКЛ" : "ВЫКЛ", d); if (d) g_set.autoJump = !g_set.autoJump; g_autoJump = g_set.autoJump; y += step;
    { static const char* SH[3] = {"ВЫКЛ", "ЛЁГКИЕ", "ПОЛНЫЕ"};
      d = 0; settingRow(94, y, "ШЕЙДЕРЫ", SH[g_set.shaders], d); g_set.shaders = (g_set.shaders + d + 3) % 3; g_shaders = g_set.shaders; y += step; }
    d = 0; settingRow(92, y, "ПОКАЗ FPS", g_set.showFps ? "ВКЛ" : "ВЫКЛ", d); if (d) g_set.showFps = !g_set.showFps; y += step;
    if (button(99, (g_W - S * 40) / 2, std::min(y + S * 2, g_H - S * 11), S * 40, S * 9, "НАЗАД")) { saveSettings(); g_screen = g_settingsBack; g_ui.focus = -1; }
}

// ---------------------------------------------------------------- точка входа
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
#ifdef __ANDROID__
    const char* ip = SDL_AndroidGetInternalStoragePath();
    g_basePath = ip ? std::string(ip) + "/" : std::string("./");
    Uint32 wflags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;
    int ww = 0, wh = 0;
#else
    char* bp = SDL_GetBasePath();
    g_basePath = bp ? std::string(bp) : std::string("./");
    if (bp) SDL_free(bp);
    Uint32 wflags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    int ww = 1280, wh = 720;
#endif
    g_win = SDL_CreateWindow("BuildWorld", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh, wflags);
    if (!g_win) { SDL_Log("Window: %s", SDL_GetError()); return 1; }
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, 0);
    if (!g_ren) { SDL_Log("Renderer: %s", SDL_GetError()); return 1; }
    SDL_GetRendererOutputSize(g_ren, &g_W, &g_H);

    initBlocks(); genAtlas(); initAnimTiles(); genFont(); initLightLUT(); initStars(); initRecipes(); initCreativeItems(); initOffsets();
    createTextures();
    migrateOldSaves();
    loadSettings();
    g_masterVol = g_set.volume; g_autoJump = g_set.autoJump;
    initAudio();
    nDetail.init(99);
    g_worldMutex = SDL_CreateMutex();
    SDL_AddEventWatch(appEventWatch, nullptr);
    for (int i = 0; i < SDL_NumJoysticks(); i++) openPad(i);
    memset(g_keys, 0, sizeof(g_keys));

    double last = nowMs(), fpsT = 0; int fpsN = 0;
    while (g_running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            SDL_LockMutex(g_worldMutex);
            switch (e.type) {
                case SDL_QUIT: g_running = false; break;
                case SDL_APP_TERMINATING:
                case SDL_APP_WILLENTERBACKGROUND:   // мир уже сохранён в appEventWatch
                    if (g_screen == SCR_INV) closeInventory();
                    if (g_screen == SCR_GAME) g_screen = SCR_PAUSE;
                    g_fingers.clear(); g_joyActive = false;
                    g_background = true;
                    if (g_audioDev) SDL_PauseAudioDevice(g_audioDev, 1);
                    break;
                case SDL_APP_DIDENTERBACKGROUND: g_background = true; break;
                case SDL_APP_WILLENTERFOREGROUND: break;
                case SDL_APP_DIDENTERFOREGROUND:
                    g_background = false; last = nowMs();
                    if (g_audioDev) SDL_PauseAudioDevice(g_audioDev, 0);
                    g_fingers.clear(); g_joyActive = false;
                    break;
                case SDL_RENDER_DEVICE_RESET: case SDL_RENDER_TARGETS_RESET: createTextures(); break;
                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) SDL_GetRendererOutputSize(g_ren, &g_W, &g_H);
                    break;
                case SDL_FINGERDOWN: fingerDown(e.tfinger.fingerId, e.tfinger.x * g_W, e.tfinger.y * g_H); break;
                case SDL_FINGERMOTION: fingerMove(e.tfinger.fingerId, e.tfinger.x * g_W, e.tfinger.y * g_H); break;
                case SDL_FINGERUP: fingerUp(e.tfinger.fingerId, e.tfinger.x * g_W, e.tfinger.y * g_H); break;
                case SDL_KEYDOWN:
                    if (e.key.keysym.scancode < 512) g_keys[e.key.keysym.scancode] = true;
                    if (!e.key.repeat) keyDown(e.key.keysym.sym);
                    break;
                case SDL_KEYUP: if (e.key.keysym.scancode < 512) g_keys[e.key.keysym.scancode] = false; break;
                case SDL_MOUSEBUTTONDOWN:
                    if (e.button.which == SDL_TOUCH_MOUSEID) break;
                    if (g_screen == SCR_GAME) { if (e.button.button == SDL_BUTTON_LEFT) { g_mouseL = true; g_digEdge = true; } if (e.button.button == SDL_BUTTON_RIGHT) { g_mouseR = true; g_placeEdge = true; } }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (e.button.which == SDL_TOUCH_MOUSEID) break;
                    if (e.button.button == SDL_BUTTON_LEFT) g_mouseL = false;
                    if (e.button.button == SDL_BUTTON_RIGHT) g_mouseR = false;
                    if (g_screen != SCR_GAME && e.button.button == SDL_BUTTON_LEFT) {
                        int mx = e.button.x, my = e.button.y, winW, winH; SDL_GetWindowSize(g_win, &winW, &winH);
                        if (winW > 0) { mx = mx * g_W / winW; my = my * g_H / winH; }
                        SDL_Point p = {mx, my}; g_ui.taps.push_back(p); g_ui.pad = false;
                    }
                    break;
                case SDL_MOUSEMOTION:
                    if (e.motion.which == SDL_TOUCH_MOUSEID) break;
                    if (g_screen == SCR_GAME && SDL_GetRelativeMouseMode()) {
                        g_cam.yaw += e.motion.xrel * 0.0025f * g_set.sens;
                        g_cam.pitch = clampf(g_cam.pitch - e.motion.yrel * 0.0025f * g_set.sens, -1.55f, 1.55f);
                    }
                    break;
                case SDL_MOUSEWHEEL: if (g_screen == SCR_GAME) selectSlot(P.sel + (e.wheel.y > 0 ? -1 : 1)); break;
                case SDL_CONTROLLERDEVICEADDED: openPad(e.cdevice.which); break;
                case SDL_CONTROLLERDEVICEREMOVED:
                    if (g_pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad)) == e.cdevice.which) { SDL_GameControllerClose(g_pad); g_pad = nullptr; g_ui.pad = false; }
                    break;
                case SDL_CONTROLLERBUTTONDOWN: padButton(e.cbutton.button); break;
            }
            SDL_UnlockMutex(g_worldMutex);
        }
        if (!g_running) break;
        if (g_background) { SDL_Delay(50); continue; }
        SDL_LockMutex(g_worldMutex);
        double now = nowMs();
        float dt = (float)((now - last) / 1000.0);
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        if (dt < 0) dt = 0;
        g_time += dt;
        fpsT += dt; fpsN++;
        if (fpsT >= 1.0) { g_fps = fpsN; fpsN = 0; fpsT = 0; }
        SDL_GetRendererOutputSize(g_ren, &g_W, &g_H);
#ifndef __ANDROID__
        bool wantRel = g_screen == SCR_GAME && !g_touchUsed;
        if ((bool)SDL_GetRelativeMouseMode() != wantRel) SDL_SetRelativeMouseMode(wantRel ? SDL_TRUE : SDL_FALSE);
#endif
        // навигация левым стиком в меню
        if (g_pad && g_screen != SCR_GAME) {
            float lx = padAxis(SDL_CONTROLLER_AXIS_LEFTX), ly = padAxis(SDL_CONTROLLER_AXIS_LEFTY);
            g_padStickNavT -= dt;
            if ((fabsf(lx) > 0.6f || fabsf(ly) > 0.6f)) {
                if (g_padStickNavT <= 0) {
                    g_ui.pad = true;
                    g_ui.navDir = fabsf(lx) > fabsf(ly) ? (lx < 0 ? 2 : 3) : (ly < 0 ? 0 : 1);
                    g_padStickNavT = 0.22f;
                }
            } else g_padStickNavT = 0;
        }
        bool inWorld = g_screen == SCR_GAME || g_screen == SCR_INV || g_screen == SCR_PAUSE || g_screen == SCR_DEAD || (g_screen == SCR_SETTINGS && g_settingsBack == SCR_PAUSE);
        if (g_screen == SCR_GAME) gatherGameInput(dt);
        else { g_in = Input(); g_mouseL = g_mouseR = false; }
        if (g_screen == SCR_GAME || g_screen == SCR_INV || g_screen == SCR_DEAD) updateGame(dt);
        if (inWorld) manageChunks(7.0, g_set.renderDist);
        uiBegin();
        if (inWorld) {
            renderWorld();
            if (g_screen == SCR_GAME) drawHUD();
        }
        switch (g_screen) {
            case SCR_INTRO: screenIntro(dt); break;
            case SCR_MAIN: screenMain(); break;
            case SCR_WORLDS: screenWorlds(); break;
            case SCR_NEWWORLD: screenNewWorld(); break;
            case SCR_CONFIRM_DEL: screenConfirmDelete(); break;
            case SCR_LOADING: screenLoading(); break;
            case SCR_PAUSE: screenPause(); break;
            case SCR_SETTINGS: screenSettings(); break;
            case SCR_INV: drawInventoryScreen(); break;
            case SCR_DEAD: screenDead(); break;
            default: break;
        }
        if (g_screen != SCR_GAME && g_msgT > 0 && g_screen != SCR_LOADING) {
            g_msgT -= dt;
            drawText(g_msg, g_W / 2.f, g_H - uiS() * 8, fontPx(), C(255, 230, 160), 1);
        }
        uiEnd();
        SDL_UnlockMutex(g_worldMutex);
        SDL_RenderPresent(g_ren);
    }
    if (worldScreenActive()) { if (g_screen == SCR_INV) returnLooseItems(); saveWorld(); }
    saveSettings();
    freeAllChunks();
    if (g_audioDev) SDL_CloseAudioDevice(g_audioDev);
    if (g_pad) SDL_GameControllerClose(g_pad);
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 0;
}
