// ============================================================================
//  BUILDWORLD — мобильная 3D-песочница из кубиков.
//  Один файл, C++14 + SDL2. Всё (текстуры, шрифт, звуки) создаётся кодом.
//  Рендер: SDL_RenderGeometry + сортировка треугольников по глубине.
// ============================================================================
#include <SDL.h>
#include <functional>
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
#include <tuple>
#include <thread>
#include <atomic>
#if defined(_WIN32)
#define BW_NO_NET 1   // сетевая игра — только Android/Linux/macOS (сокеты POSIX)
#if defined(__GNUC__) || defined(__clang__)   // без сети часть сетевого кода не вызывается — это нормально
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#endif
#ifdef __ANDROID__
#include <jni.h>
#endif

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

static void padRumble(float lo, float hi, int ms);   // вибрация геймпада (низкая/высокая частота 0..1)
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
    float ox = 0, oy = 0, oz = 0;   // сдвиг по зерну: иначе в точке (0,0) шум всегда равен нулю и место появления у всех миров похоже
    void init(u32 seed, bool offset = false) {
        Rng r(seed);
        for (int i = 0; i < 256; i++) p[i] = (u8)i;
        for (int i = 255; i > 0; i--) { int j = r.range(i + 1); std::swap(p[i], p[j]); }
        for (int i = 0; i < 256; i++) p[256 + i] = p[i];
        if (offset) { ox = r.f() * 256.f; oy = r.f() * 256.f; oz = r.f() * 256.f; } else ox = oy = oz = 0;
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
        x += ox; y += oy;
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
        x += ox; y += oy; z += oz;
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
enum { CW = 16, WH = 256, SECN = WH / 16, CHUNK_VOL = CW * CW * WH, OLD_WH = 128 };
static int g_sea = 64;   // уровень моря: 48 в старых мирах (генератор 1–2), 64 в новых
enum { NET_NONE = 0, NET_HOST, NET_CLIENT };
static int g_netRole = NET_NONE;   // сетевая игра: нет, хозяин мира, гость
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
    B_DIAMOND_BLOCK, B_FURNACE_LIT, B_ANVIL,
    // новые биомы
    B_BIRCH_LOG, B_BIRCH_LEAVES, B_ACACIA_LOG, B_ACACIA_LEAVES, B_JUNGLE_LOG, B_JUNGLE_LEAVES, B_DRYGRASS, B_PODZOL, B_MUD,
    B_REDSAND, B_TERRA_R, B_TERRA_O, B_TERRA_W, B_TERRA_B, B_FERN, B_DEADBUSH, B_REED, B_LILY, B_FLOWER_B, B_FLOWER_W,
    B_MOSSY, B_WOOL_W, B_WOOL_L, B_WOOL_G, B_WOOL_K, B_WOOL_N,
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
    T_HEART_F = 80, T_HEART_H, T_HEART_E, T_FOOD_F, T_FOOD_H, T_FOOD_E, T_BUBBLE, T_BOOK, T_WATER_FX, T_STRING, T_BOW, T_ARROW, T_EBOOK, T_PBOOK, T_ANVIL_ITEM,
    T_TOOL0 = 96,                  // 96..115: материал*4 + вид
    T_ARMOR0 = 116,                // 116..127: броня, материал*4 + часть
    T_TOPTUN_SIDE = 128, T_TOPTUN_FACE, T_TOPTUN_LEG, T_PUKH_BODY, T_PUKH_FACE, T_PUKH_LEG,
    T_GNIL_BODY, T_GNIL_FACE, T_GNIL_LEG, T_TEN_BODY, T_TEN_FACE, T_TEN_LEG, T_SKIN, T_SLEEVE, T_HORN,
    T_MUSHCAP, T_GNIL_TOP, T_TEN_SPIKE,
    T_ZOMB_FACE, T_ZOMB_SKIN, T_ZOMB_SHIRT, T_ZOMB_PANTS, T_SKEL_FACE, T_SKEL_BONE, T_SKEL_RIBS, T_SKEL_HOOD,
    T_SPID_FACE, T_SPID_BODY, T_SPID_ABDO, T_SPID_LEG, T_BOWWOOD,
    T_YAK_BODY, T_YAK_TOP, T_YAK_FRINGE, T_YAK_FACE, T_YAK_HEADSIDE, T_YAK_MUZZLE, T_YAK_LEG, T_YAK_HORN, T_YAK_TAIL,
    T_DUCK_SIDE, T_DUCK_TOP, T_DUCK_CHEST, T_DUCK_HEAD, T_DUCK_HEADSIDE, T_DUCK_NECK, T_DUCK_BILL, T_DUCK_FOOT, T_DUCK_TAIL,
    T_ARMHUD_F, T_ARMHUD_H, T_ARMHUD_E, T_ANVIL_TOP, T_ANVIL_SIDE,
    T_FISH0, T_FFRY0 = T_FISH0 + 4, T_FSKIN0 = T_FFRY0 + 4, T_FFIN0 = T_FSKIN0 + 4, T_FTAIL0 = T_FFIN0 + 4, T_ROD = T_FTAIL0 + 4, T_ROD_HAND, T_FEYE0, T_CRACKX0 = T_FEYE0 + 4,   // ещё 5 стадий трещин (всего 10)
    // новые биомы (номера блочных тайлов должны оставаться < 256)
    T_BIRCH_LOG = T_CRACKX0 + 5, T_BIRCH_TOP, T_BIRCH_LEAVES, T_ACACIA_LOG, T_ACACIA_TOP, T_ACACIA_LEAVES, T_JUNGLE_LOG, T_JUNGLE_TOP, T_JUNGLE_LEAVES,
    T_DRYGRASS_TOP, T_DRYGRASS_SIDE, T_PODZOL_TOP, T_PODZOL_SIDE, T_MUD, T_REDSAND, T_TERRA_R, T_TERRA_O, T_TERRA_W, T_TERRA_B,
    T_FERN, T_DEADBUSH, T_REED, T_LILY, T_FLOWER_B, T_FLOWER_W, T_MOSSY, T_WOOL0, T_WOOL_END = T_WOOL0 + 5,
    // животные (по 16 пикселей шкуры, раскрашиваются цветом вершин)
    T_PIG_SKIN = T_WOOL_END, T_PIG_FACE, T_PIG_SNOUT, T_PIG_LEG, T_SHEEP_WOOL, T_SHEEP_FACE, T_SHEEP_LEG, T_SHEEP_SKIN,
    T_CAT_FUR, T_CAT_FACE, T_CAT_BELLY, T_DOG_FUR, T_DOG_FACE, T_DOG_MUZZLE, T_COLLAR,
    // предметы и эффекты
    T_PORK, T_PORK_C, T_MUTTON, T_MUTTON_C, T_SHEARS, T_XP0, T_RAIN = T_XP0 + 4, T_CRIT, T_SPLASH,
    // игроки в мультиплеере
    T_PL_FACE, T_PL_HEAD, T_PL_HAIR, T_PL_SHIRT, T_PL_SHIRT_F, T_PL_ARM, T_PL_PANTS, T_PL_SHOE,
    T_CAUST0, T_COUNT = T_CAUST0 + 4   // 2x2 тайла подводных бликов (рисуются на лету)
};

enum Shape { SH_NONE = 0, SH_CUBE, SH_CROSS, SH_TORCH, SH_DOOR, SH_LADDER, SH_BED, SH_FLUID, SH_ANVIL, SH_FLAT };
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
static inline bool isLog(int b) { return b == B_LOG || b == B_PINE_LOG || b == B_BIRCH_LOG || b == B_ACACIA_LOG || b == B_JUNGLE_LOG; }
static inline bool isLeaves(int b) { return b == B_LEAVES || b == B_PINE_LEAVES || b == B_BIRCH_LEAVES || b == B_ACACIA_LEAVES || b == B_JUNGLE_LEAVES; }
static inline bool isTerra(int b) { return b == B_TERRA_R || b == B_TERRA_O || b == B_TERRA_W || b == B_TERRA_B; }
static inline bool isWool(int b) { return b >= B_WOOL_W && b <= B_WOOL_N; }
// мелкие растения: ломаются без опоры, сносятся водой
static inline bool isSmallPlant(int b) {
    return b == B_TALLGRASS || b == B_FLOWER_R || b == B_FLOWER_Y || b == B_FLOWER_B || b == B_FLOWER_W || b == B_SAPLING ||
           b == B_FERN || b == B_DEADBUSH || b == B_REED || b == B_LILY;
}
static inline bool occludesSun(int b) { return (BD[b].solid && b != B_GLASS && b != B_ICE) || isLeaves(b); }
struct Chunk;
static void computeTops(Chunk* c);
static inline bool isSoil(int b) { return b == B_GRASS || b == B_DIRT || b == B_SNOWGRASS || b == B_PODZOL || b == B_DRYGRASS || b == B_MUD; }

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
    defBlock(B_ANVIL, "НАКОВАЛЬНЯ", T_ANVIL_SIDE, T_ANVIL_SIDE, T_ANVIL_TOP, T_ANVIL_SIDE, T_ANVIL_SIDE, T_ANVIL_SIDE, SH_ANVIL, false, true, 0, 5.0f, TK_PICK, 1, 0, SM_STONE, false, true);
    // новые биомы
    defBlock(B_BIRCH_LOG, "БЕРЁЗОВОЕ БРЕВНО", T_BIRCH_LOG, T_BIRCH_LOG, T_BIRCH_TOP, T_BIRCH_TOP, T_BIRCH_LOG, T_BIRCH_LOG, SH_CUBE, true, true, 0, 2.0f, TK_AXE, 0, 0, SM_WOOD);
    defBlock(B_BIRCH_LEAVES, "БЕРЁЗОВАЯ ЛИСТВА", T_BIRCH_LEAVES, T_BIRCH_LEAVES, T_BIRCH_LEAVES, T_BIRCH_LEAVES, T_BIRCH_LEAVES, T_BIRCH_LEAVES, SH_CUBE, false, true, 7, 0.2f, TK_NONE, 0, 0, SM_GRASS);
    defBlock(B_ACACIA_LOG, "БРЕВНО АКАЦИИ", T_ACACIA_LOG, T_ACACIA_LOG, T_ACACIA_TOP, T_ACACIA_TOP, T_ACACIA_LOG, T_ACACIA_LOG, SH_CUBE, true, true, 0, 2.0f, TK_AXE, 0, 0, SM_WOOD);
    defBlock(B_ACACIA_LEAVES, "ЛИСТВА АКАЦИИ", T_ACACIA_LEAVES, T_ACACIA_LEAVES, T_ACACIA_LEAVES, T_ACACIA_LEAVES, T_ACACIA_LEAVES, T_ACACIA_LEAVES, SH_CUBE, false, true, 8, 0.2f, TK_NONE, 0, 0, SM_GRASS);
    defBlock(B_JUNGLE_LOG, "ТРОПИЧЕСКОЕ БРЕВНО", T_JUNGLE_LOG, T_JUNGLE_LOG, T_JUNGLE_TOP, T_JUNGLE_TOP, T_JUNGLE_LOG, T_JUNGLE_LOG, SH_CUBE, true, true, 0, 2.0f, TK_AXE, 0, 0, SM_WOOD);
    defBlock(B_JUNGLE_LEAVES, "ТРОПИЧЕСКАЯ ЛИСТВА", T_JUNGLE_LEAVES, T_JUNGLE_LEAVES, T_JUNGLE_LEAVES, T_JUNGLE_LEAVES, T_JUNGLE_LEAVES, T_JUNGLE_LEAVES, SH_CUBE, false, true, 9, 0.2f, TK_NONE, 0, 0, SM_GRASS);
    defBlock(B_DRYGRASS, "СУХОЙ ДЁРН", T_DRYGRASS_SIDE, T_DRYGRASS_SIDE, T_DRYGRASS_TOP, T_DIRT, T_DRYGRASS_SIDE, T_DRYGRASS_SIDE, SH_CUBE, true, true, 0, 0.6f, TK_SHOVEL, 0, 0, SM_GRASS);
    defBlock(B_PODZOL, "ХВОЙНАЯ ПОДСТИЛКА", T_PODZOL_SIDE, T_PODZOL_SIDE, T_PODZOL_TOP, T_DIRT, T_PODZOL_SIDE, T_PODZOL_SIDE, SH_CUBE, true, true, 0, 0.6f, TK_SHOVEL, 0, 0, SM_GRASS);
    defCube(B_MUD, "ИЛ", T_MUD, 0.5f, TK_SHOVEL, SM_SAND);
    defCube(B_REDSAND, "КРАСНЫЙ ПЕСОК", T_REDSAND, 0.5f, TK_SHOVEL, SM_SAND);
    defCube(B_TERRA_R, "КРАСНАЯ ГЛИНА", T_TERRA_R, 1.25f, TK_PICK, SM_STONE, true);
    defCube(B_TERRA_O, "ОХРИСТАЯ ГЛИНА", T_TERRA_O, 1.25f, TK_PICK, SM_STONE, true);
    defCube(B_TERRA_W, "БЕЛАЯ ГЛИНА", T_TERRA_W, 1.25f, TK_PICK, SM_STONE, true);
    defCube(B_TERRA_B, "БУРАЯ ГЛИНА", T_TERRA_B, 1.25f, TK_PICK, SM_STONE, true);
    defBlock(B_FERN, "ПАПОРОТНИК", T_FERN, 0, 0, 0, 0, 0, SH_CROSS, false, false, 0, 0, TK_NONE, 0, 0, SM_GRASS, true);
    defBlock(B_DEADBUSH, "СУХОЙ КУСТ", T_DEADBUSH, 0, 0, 0, 0, 0, SH_CROSS, false, false, 0, 0, TK_NONE, 0, 0, SM_GRASS, true);
    defBlock(B_REED, "КАМЫШ", T_REED, 0, 0, 0, 0, 0, SH_CROSS, false, false, 0, 0, TK_NONE, 0, 0, SM_GRASS);
    defBlock(B_LILY, "КУВШИНКА", T_LILY, 0, 0, 0, 0, 0, SH_FLAT, false, false, 0, 0, TK_NONE, 0, 0, SM_GRASS);
    defBlock(B_FLOWER_B, "ВАСИЛЁК", T_FLOWER_B, 0, 0, 0, 0, 0, SH_CROSS, false, false, 0, 0, TK_NONE, 0, 0, SM_GRASS);
    defBlock(B_FLOWER_W, "РОМАШКА", T_FLOWER_W, 0, 0, 0, 0, 0, SH_CROSS, false, false, 0, 0, TK_NONE, 0, 0, SM_GRASS);
    defCube(B_MOSSY, "ЗАМШЕЛЫЙ БУЛЫЖНИК", T_MOSSY, 2.0f, TK_PICK, SM_STONE, true);
    {
        static const char* woolNames[5] = {"БЕЛАЯ ШЕРСТЬ", "СВЕТЛО-СЕРАЯ ШЕРСТЬ", "СЕРАЯ ШЕРСТЬ", "ЧЁРНАЯ ШЕРСТЬ", "БУРАЯ ШЕРСТЬ"};
        for (int i = 0; i < 5; i++) {
            defCube(B_WOOL_W + i, woolNames[i], (u8)(T_WOOL0 + i), 0.8f, TK_NONE, SM_SNOW);
        }
    }
}

// ---------------------------------------------------------------- предметы
enum Item : u16 {
    I_STICK = 128, I_COAL, I_IRON, I_GOLD, I_DIAMOND, I_APPLE, I_RAWMEAT, I_COOKED, I_BONE, I_BONEMEAL, I_ROTTEN, I_FLUFF,
    I_STRING, I_BOW, I_ARROW,
    I_TOOL0 = 144,  // 144..163: материал*4 + вид
    I_BOOK = 164, I_EBOOK,
    I_ARMOR0 = 166, // 166..177: броня (железо, золото, алмаз) * 4 части
    I_FISH0 = 178,  // 178..181: сырая рыба (окунь, карась, форель, щука)
    I_FFRY0 = 182,  // 182..185: жареная рыба
    I_ROD = 186,    // удочка
    I_PORK, I_PORK_C, I_MUTTON, I_MUTTON_C, I_SHEARS,
    I_END
};
static const int OLD_ITEM_SHIFT = 64;   // в сохранениях до версии 4 предметы начинались с 64
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
// броня: материал 0 — железо, 1 — золото, 2 — алмаз; часть 0 — шлем, 1 — нагрудник, 2 — поножи, 3 — ботинки
static inline bool isArmor(int id) { return id >= I_ARMOR0 && id < I_ARMOR0 + 12; }
static inline int armorMat(int id) { return (id - I_ARMOR0) / 4; }
static inline int armorPiece(int id) { return (id - I_ARMOR0) % 4; }
static inline int armorId(int mat, int piece) { return I_ARMOR0 + mat * 4 + piece; }
static const int ARMOR_PTS[3][4] = {{2, 6, 5, 2}, {2, 5, 3, 1}, {3, 8, 6, 3}};
static const int ARMOR_DUR[3][4] = {{165, 240, 225, 195}, {77, 112, 105, 91}, {363, 528, 495, 429}};
static const char* ARMOR_PIECE[4] = {"ШЛЕМ", "НАГРУДНИК", "ПОНОЖИ", "БОТИНКИ"};
static const char* ARMOR_MAT_M[3] = {"ЖЕЛЕЗНЫЙ", "ЗОЛОТОЙ", "АЛМАЗНЫЙ"};
static const char* ARMOR_MAT_PL[3] = {"ЖЕЛЕЗНЫЕ", "ЗОЛОТЫЕ", "АЛМАЗНЫЕ"};
static inline bool isFishRaw(int id) { return id >= I_FISH0 && id < I_FISH0 + 4; }
static inline bool isFishFry(int id) { return id >= I_FFRY0 && id < I_FFRY0 + 4; }
static const int ROD_DUR = 64;
static const int SHEARS_DUR = 238;
static inline int maxDur(int id) { if (id == I_ROD) return ROD_DUR; if (id == I_SHEARS) return SHEARS_DUR; if (isTool(id)) return MAT_DUR[toolMat(id)]; if (isArmor(id)) return ARMOR_DUR[armorMat(id)][armorPiece(id)]; return 0; }

static const char* FISH_NAME[4][2] = {{"СЫРОЙ ОКУНЬ", "ЖАРЕНЫЙ ОКУНЬ"}, {"СЫРОЙ КАРАСЬ", "ЖАРЕНЫЙ КАРАСЬ"}, {"СЫРАЯ ФОРЕЛЬ", "ЖАРЕНАЯ ФОРЕЛЬ"}, {"СЫРАЯ ЩУКА", "ЖАРЕНАЯ ЩУКА"}};
static const char* FISH_SPECIES[4] = {"ОКУНЬ", "КАРАСЬ", "ФОРЕЛЬ", "ЩУКА"};
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
        case I_BOOK: return "КНИГА"; case I_EBOOK: return "ЗАЧАРОВАННАЯ КНИГА"; case I_ROD: return "УДОЧКА";
        case I_PORK: return "СЫРАЯ СВИНИНА"; case I_PORK_C: return "ЖАРЕНАЯ СВИНИНА"; case I_MUTTON: return "СЫРАЯ БАРАНИНА";
        case I_MUTTON_C: return "ЖАРЕНАЯ БАРАНИНА"; case I_SHEARS: return "НОЖНИЦЫ";
    }
    if (isFishRaw(id)) return FISH_NAME[id - I_FISH0][0];
    if (isFishFry(id)) return FISH_NAME[id - I_FFRY0][1];
    if (isArmor(id)) { int m = armorMat(id), p = armorPiece(id); return std::string(p >= 2 ? ARMOR_MAT_PL[m] : ARMOR_MAT_M[m]) + " " + ARMOR_PIECE[p]; }
    return "?";
}
static int itemTile(int id) {
    if (id == B_DOOR) return T_DOOR_ITEM;
    if (id == B_BED) return T_BED_ITEM;
    if (id == B_ANVIL) return T_ANVIL_ITEM;
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
    if (id == I_BOOK) return T_PBOOK;
    if (id == I_EBOOK) return T_EBOOK;
    if (isArmor(id)) return T_ARMOR0 + (id - I_ARMOR0);
    if (isFishRaw(id)) return T_FISH0 + (id - I_FISH0);
    if (isFishFry(id)) return T_FFRY0 + (id - I_FFRY0);
    if (id == I_ROD) return T_ROD;
    switch (id) { case I_PORK: return T_PORK; case I_PORK_C: return T_PORK_C; case I_MUTTON: return T_MUTTON; case I_MUTTON_C: return T_MUTTON_C; case I_SHEARS: return T_SHEARS; }
    return T_WHITE;
}
static inline int maxStack(int id) { return (isTool(id) || id == I_BOW || isArmor(id) || id == I_EBOOK || id == I_ROD || id == I_SHEARS) ? 1 : 64; }
static inline int foodValue(int id) {
    switch (id) { case I_APPLE: return 4; case I_RAWMEAT: return 3; case I_COOKED: return 8; case I_ROTTEN: return 2;
                  case I_PORK: return 3; case I_PORK_C: return 8; case I_MUTTON: return 2; case I_MUTTON_C: return 6; }
    static const int raw[4] = {2, 2, 2, 3}, fry[4] = {6, 5, 6, 8};
    if (isFishRaw(id)) return raw[id - I_FISH0];
    if (isFishFry(id)) return fry[id - I_FFRY0];
    return 0;
}
static inline bool itemIsCubeIcon(int id) {
    if (!isBlockItem(id)) return false;
    int s = BD[id].shape;
    return s == SH_CUBE || s == SH_FLUID;
}

struct Stack {
    u16 id = 0, cnt = 0, dmg = 0, ench = 0;   // ench: по 3 бита на зачарование
    bool empty() const { return id == 0 || cnt == 0; }
    void clear() { id = cnt = dmg = ench = 0; }
    bool operator==(const Stack& o) const { return (empty() && o.empty()) || (id == o.id && cnt == o.cnt && dmg == o.dmg && ench == o.ench); }
};
static inline Stack mkStack(int id, int cnt) { Stack s; s.id = (u16)id; s.cnt = (u16)cnt; s.dmg = 0; s.ench = 0; return s; }

// ---------------------------------------------------------------- зачарования
enum Ench { EN_EFF = 0, EN_DMG, EN_LUCK, EN_PROT, EN_COUNT };
static const char* EN_NAME[EN_COUNT] = {"ЭФФЕКТИВНОСТЬ", "УРОН", "УДАЧА", "ЗАЩИТА"};
static const int EN_MAX[EN_COUNT] = {5, 5, 4, 5};
static const char* ROMAN[8] = {"", "I", "II", "III", "IV", "V", "VI", "VII"};
static inline int enchLvl(const Stack& s, int e) { return (s.ench >> (e * 3)) & 7; }
static inline void setEnch(Stack& s, int e, int l) { s.ench = (u16)((s.ench & ~(7u << (e * 3))) | ((u32)clampi(l, 0, 7) << (e * 3))); }
// какое зачарование на что накладывается
static bool enchFits(int id, int e) {
    if (id == I_EBOOK) return true;
    if (isTool(id)) {
        int k = toolKind(id);
        if (e == EN_EFF) return k != TK_SWORD;
        if (e == EN_DMG) return k == TK_SWORD || k == TK_AXE;
        if (e == EN_LUCK) return k != TK_SWORD;
        return false;
    }
    if (isArmor(id)) return e == EN_PROT;
    if (id == I_ROD) return e == EN_LUCK;   // удача на удочке — клюёт чаще, попадается крупная рыба
    return false;
}
static std::string enchText(const Stack& s) {
    std::string t;
    for (int e = 0; e < EN_COUNT; e++) { int l = enchLvl(s, e); if (!l) continue; if (!t.empty()) t += ", "; t += std::string(EN_NAME[e]) + " " + ROMAN[l]; }
    return t;
}
static std::string stackName(const Stack& s) { std::string n = itemName(s.id); if (s.ench) n += ": " + enchText(s); return n; }

// ============================================================================
//  ТЕКСТУРЫ: атлас 256x256 (16x16 тайлов по 16x16 пикселей), рисуется кодом
// ============================================================================
enum { ATLAS = 256, ATLAS_ROWS = 32, ATLAS_H = ATLAS_ROWS * 16 };   // 16 столбцов x 32 строки тайлов
static u32 g_atlasPx[ATLAS * ATLAS_H];

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

// сеть трещин: от центра расходятся ветви; t — «время» появления сегмента (0..1). Стадия k рисует всё, что появилось до (k+1)/10
struct CrackSeg { float x0, y0, x1, y1, t, w; };
static const std::vector<CrackSeg>& crackNet() {
    static std::vector<CrackSeg> net;
    if (!net.empty()) return net;
    Rng r(4242);
    struct Br { float x, y, a, t, w; int gen; };
    std::vector<Br> st;
    int mains = 5;
    for (int i = 0; i < mains; i++) st.push_back({0.5f + r.sf() * 0.04f, 0.5f + r.sf() * 0.04f, i * 6.2832f / mains + r.sf() * 0.5f, 0.f + i * 0.03f, 1.f, 0});
    while (!st.empty() && net.size() < 400) {
        Br b = st.back(); st.pop_back();
        int steps = b.gen == 0 ? 9 : (b.gen == 1 ? 5 : 3);
        float len = b.gen == 0 ? 0.055f : 0.045f;
        for (int k = 0; k < steps; k++) {
            float nx = b.x + cosf(b.a) * len, ny = b.y + sinf(b.a) * len;
            float nt = b.t + (b.gen == 0 ? 0.075f : 0.06f);
            net.push_back({b.x, b.y, nx, ny, b.t, b.w});
            b.x = nx; b.y = ny; b.t = nt; b.a += r.sf() * 0.55f; b.w *= 0.93f;
            if (nx < 0.02f || nx > 0.98f || ny < 0.02f || ny > 0.98f) break;
            if (b.gen < 2 && r.range(100) < 30) st.push_back({nx, ny, b.a + (r.range(2) ? 1.f : -1.f) * (0.6f + r.f() * 0.6f), nt + 0.05f, b.w * 0.75f, b.gen + 1});
        }
    }
    return net;
}
static inline int crackTile(int stage) { return stage < 5 ? T_CRACK0 + stage : T_CRACKX0 + (stage - 5); }
static void genNewBlockTiles(int R, bool to16);
static void genNewTiles16();
static void genAtlas() {
    memset(g_atlasPx, 0, sizeof(g_atlasPx));
    genBlockTextures();
    genNewTiles16();
    { TP t(T_WHITE); t.fill(255, 255, 255, 0); }
    for (int s = 0; s < 10; s++) {   // трещины: 10 стадий, растут от центра
        TP t(crackTile(s)); t.clear();
        float lim = (s + 1) / 10.f;
        for (const CrackSeg& g : crackNet()) if (g.t <= lim)
            t.line((int)(g.x0 * 15.99f), (int)(g.y0 * 15.99f), (int)(g.x1 * 15.99f), (int)(g.y1 * 15.99f), RGBA(16, 16, 16, 220));
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
    // ---------- книги
    auto bookIcon = [&](int tile, u32 cover, u32 coverD, u32 rune) {
        TP t(tile); t.clear();
        for (int y = 2; y < 14; y++) for (int x = 3; x < 13; x++) t.set(x, y, (x + y) % 7 == 0 ? HEX(coverD) : HEX(cover));
        for (int y = 3; y < 13; y++) { t.set(12, y, HEX(0xf2ead6)); t.set(13, y + 1, HEX(0xd8ceb6)); }   // страницы
        for (int x = 4; x < 13; x++) t.set(x, 13, HEX(0xe8dfc8));
        for (int y = 2; y < 14; y++) { t.set(3, y, HEX(coverD)); t.set(4, y, HEX(coverD)); }             // корешок
        t.rect(6, 4, 10, 4, HEX(rune)); t.rect(6, 6, 9, 6, HEX(rune)); t.rect(6, 10, 10, 10, HEX(rune));
        t.set(8, 8, HEX(rune)); t.set(7, 8, HEX(rune)); t.set(9, 8, HEX(rune));
    };
    bookIcon(T_PBOOK, 0x8a5a2e, 0x5e3c1c, 0xc8a46a);
    bookIcon(T_EBOOK, 0x5a2e9a, 0x3a1a6a, 0xf2c84a);
    // ---------- броня: силуэты по маскам, свет сверху-слева
    static const char* armMask[4][16] = {
        {"................", "................", "................", "....########....", "...##########...", "..############..", "..############..", "..####....####..",
         "..###......###..", "..###......###..", "..##........##..", "................", "................", "................", "................", "................"},
        {"..#####..#####..", ".######..######.", ".##############.", ".##############.", ".##############.", "..############..", "...##########...", "...##########...",
         "...##########...", "...##########...", "...##########...", "...##########...", "...##########...", "....########....", "................", "................"},
        {"...##########...", "...##########...", "...##########...", "...####..####...", "...####..####...", "...###....###...", "...###....###...", "...###....###...",
         "...###....###...", "...###....###...", "...###....###...", "...###....###...", "...###....###...", "................", "................", "................"},
        {"................", "................", "................", "................", "................", "..###......###..", "..###......###..", "..###......###..",
         "..###......###..", "..###......###..", "#####......#####", "#####......#####", "################", "................", "................", "................"},
    };
    const u32 armCol[3][4] = {{0x8a8c92, 0xb8bac0, 0xd6d8dc, 0xf2f3f5}, {0xa8780e, 0xd8a82a, 0xf2cc4a, 0xfff0a0}, {0x1e8c98, 0x38bcc6, 0x72dce2, 0xc8f8fa}};
    for (int m = 0; m < 3; m++) for (int pc = 0; pc < 4; pc++) {
        TP t(T_ARMOR0 + m * 4 + pc); t.clear();
        auto in = [&](int x, int y) { return x >= 0 && y >= 0 && x < 16 && y < 16 && armMask[pc][y][x] == '#'; };
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
            if (!in(x, y)) continue;
            int lvl = 2;
            if (!in(x - 1, y) || !in(x, y - 1)) lvl = 3;
            if (!in(x + 1, y) || !in(x, y + 1)) lvl = 0;
            else if (!in(x + 2, y) || !in(x, y + 2)) lvl = 1;
            t.set(x, y, HEX(armCol[m][lvl]));
        }
        if (pc == 0) for (int x = 6; x < 10; x++) t.set(x, 4, HEX(armCol[m][3]));
        if (pc == 1) for (int y = 3; y < 12; y++) t.set(8, y, HEX(armCol[m][1]));
    }
    // ---------- значок брони над сердцами (полный, половина, пустой)
    static const char* armHud[7] = {"##...##", "#######", ".#####.", ".#####.", ".#####.", ".#####.", "..###.."};
    for (int k = 0; k < 3; k++) { TP t(T_ARMHUD_F + k); t.clear();
        for (int y = 0; y < 7; y++) for (int x = 0; x < 7; x++) if (armHud[y][x] == '#') {
            bool full = (k == 0) || (k == 1 && x < 4);
            u32 c = full ? ((x + y < 4) ? 0xf0f2f6 : 0xb8bcc4) : 0x3a3c44;
            for (int sy = 0; sy < 2; sy++) for (int sx = 0; sx < 2; sx++) t.set(1 + x * 2 + sx, 1 + y * 2 + sy, HEX(c, full ? 255 : 200));
        } }
    // ---------- наковальня
    { TP t(T_ANVIL_SIDE); TN n = makeNoise(220, {{4, 0.5f}, {1, 0.5f}}); fillNoise(t, mkPal({0x2c2d32, 0x36373d, 0x404148, 0x4a4b52}), n, 0.6f); bevel(t, 0, 0, 15, 15, 16, 20); }
    { TP t(T_ANVIL_TOP); TN n = makeNoise(221, {{8, 0.5f}, {1, 0.5f}}); fillNoise(t, mkPal({0x3a3b42, 0x45464e, 0x50515a, 0x5c5d66}), n, 0.5f);
      t.line(3, 4, 9, 6, HEX(0x6e7078)); t.line(6, 10, 12, 9, HEX(0x6e7078)); bevel(t, 0, 0, 15, 15, 22, 18); }
    { TP t(T_ANVIL_ITEM); t.clear();
      for (int y = 3; y <= 6; y++) for (int x = 1; x <= 14; x++) t.set(x, y, y == 3 ? HEX(0x6e7078) : HEX(0x45464e));
      t.set(0, 4, HEX(0x45464e)); t.set(0, 5, HEX(0x45464e));
      for (int y = 7; y <= 10; y++) for (int x = 5; x <= 10; x++) t.set(x, y, x == 5 ? HEX(0x55565e) : HEX(0x36373d));
      for (int y = 11; y <= 13; y++) for (int x = 3; x <= 12; x++) t.set(x, y, y == 11 ? HEX(0x55565e) : HEX(0x2c2d32)); }
    // ---------- ЯК: длинная бурая шерсть, светлая морда, изогнутые рога
    const Pal yakP = mkPal({0x3a2414, 0x4a2e1a, 0x5b3920, 0x6b4527, 0x7c5230, 0x8d603a});
    auto furStrands = [&](TP& t, u32 seed, float base) {   // вертикальные пряди
        TN n = makeNoise(seed, {{4, 0.5f}, {2, 0.3f}, {1, 0.2f}});
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
            float strand = ((hash32(seed + x * 13) % 100) / 100.f - 0.5f) * 0.35f;
            float v = base + strand + (n.at(x, y) - 0.5f) * 0.4f + ((x + (int)(hash32(seed + x) % 3)) % 3 == 0 ? -0.15f : 0.f);
            t.set(x, y, rampPick(yakP, clampf(v, 0, 0.999f), x, y, 0.4f));
        }
    };
    { TP t(T_YAK_BODY); furStrands(t, 301, 0.55f); for (int x = 0; x < 16; x++) for (int y = 0; y < 3; y++) t.shade(x, y, 10); }
    { TP t(T_YAK_TOP); TN n = makeNoise(302, {{4, 0.6f}, {2, 0.4f}}); fillNoise(t, yakP, n, 0.6f); for (int y = 0; y < 16; y += 3) for (int x = 0; x < 16; x++) if ((x + y) % 5 == 0) t.shade(x, y, -18); }
    { TP t(T_YAK_FRINGE); t.clear(); furStrands(t, 303, 0.4f);   // свисающая бахрома с неровным краем
      for (int x = 0; x < 16; x++) { int len = 10 + (int)(hash32(304 + x * 7) % 6); for (int y = len; y < 16; y++) t.set(x, y, 0); t.shade(x, len - 1, -20); } }
    { TP t(T_YAK_FACE); furStrands(t, 305, 0.32f);
      for (int x = 1; x < 15; x++) for (int y = 0; y < 4; y++) if ((hash32(306 + x * 5 + y) % 3) != 0) t.set(x, y, yakP[4 + (x & 1)]);   // чёлка
      t.rect(2, 5, 4, 7, HEX(0x150d08)); t.rect(11, 5, 13, 7, HEX(0x150d08)); t.set(3, 5, HEX(0xe8dcc8)); t.set(12, 5, HEX(0xe8dcc8)); t.set(2, 7, HEX(0x3a2414)); t.set(13, 7, HEX(0x3a2414)); }
    { TP t(T_YAK_HEADSIDE); furStrands(t, 307, 0.35f); t.rect(10, 5, 12, 7, HEX(0x150d08)); t.set(11, 5, HEX(0xe8dcc8)); }
    { TP t(T_YAK_MUZZLE); TN n = makeNoise(308, {{4, 0.5f}, {1, 0.5f}}); fillNoise(t, mkPal({0x8f7a68, 0x9d8876, 0xab9684, 0xb8a492}), n, 0.5f);
      t.rect(3, 6, 5, 9, HEX(0x2a1c14)); t.rect(10, 6, 12, 9, HEX(0x2a1c14)); t.set(3, 6, HEX(0x4a382c)); t.set(10, 6, HEX(0x4a382c));
      for (int x = 2; x < 14; x++) t.set(x, 13, HEX(0x6b5848)); bevel(t, 0, 0, 15, 15, 14, 18); }
    { TP t(T_YAK_LEG); furStrands(t, 309, 0.45f); for (int y = 12; y < 16; y++) for (int x = 0; x < 16; x++) t.set(x, y, y == 12 ? HEX(0x2a2420) : HEX(0x1a1614)); for (int y = 13; y < 16; y++) t.set(7, y, HEX(0x0c0a08)); }
    { TP t(T_YAK_HORN); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { float k = (x + y) / 30.f; t.set(x, y, mixc(HEX(0xe9dfc6), HEX(0x6e5f4c), k)); }
      for (int i = 2; i < 16; i += 4) for (int x = 0; x < 16; x++) t.shade(x, i, -14); }
    { TP t(T_YAK_TAIL); t.clear(); for (int y = 0; y < 16; y++) for (int x = 5; x < 11; x++) if (y < 9 ? (x == 7 || x == 8) : true) t.set(x, y, yakP[1 + ((x + y) % 3)]); }
    // ---------- УТКА-КРЯКВА: изумрудная голова, белый ошейник, рыжая грудка, серые бока, рыжие лапы
    { TP t(T_DUCK_SIDE); TN n = makeNoise(311, {{4, 0.5f}, {1, 0.5f}}); fillNoise(t, mkPal({0x9a9690, 0xa8a49e, 0xb6b2ac, 0xc4c0ba}), n, 0.5f);
      for (int y = 2; y < 9; y++) for (int x = 3; x < 16; x++) t.set(x, y, ((x + y) & 3) == 0 ? HEX(0x5d5248) : HEX(0x6e6156));   // крыло
      for (int x = 7; x < 14; x++) { t.set(x, 7, HEX(0x2e4fb0)); t.set(x, 8, HEX(0x3c66d6)); t.set(x, 6, HEX(0xf0f0f0)); }        // синее «зеркальце»
      for (int x = 0; x < 16; x += 3) for (int y = 10; y < 16; y += 2) t.shade(x, y, -10); }
    { TP t(T_DUCK_TOP); TN n = makeNoise(312, {{4, 0.5f}, {1, 0.5f}}); fillNoise(t, mkPal({0x5d5248, 0x6e6156, 0x7f7264, 0x8f8272}), n, 0.5f);
      for (int y = 0; y < 16; y++) { t.set(7, y, HEX(0x4e443b)); t.set(8, y, HEX(0x4e443b)); } }
    { TP t(T_DUCK_CHEST); TN n = makeNoise(313, {{4, 0.5f}, {1, 0.5f}}); fillNoise(t, mkPal({0x6b3a20, 0x7d4628, 0x8e5230, 0x9f5f38}), n, 0.5f); }
    { TP t(T_DUCK_HEAD); TN n = makeNoise(314, {{4, 0.5f}, {1, 0.5f}}); fillNoise(t, mkPal({0x0f3d26, 0x145233, 0x1a6a40, 0x22844f, 0x2e9e60}), n, 0.6f);
      for (int x = 3; x < 13; x++) t.set(x, 2, HEX(0x3fb873)); }
    { TP t(T_DUCK_HEADSIDE); t.copyFrom(T_DUCK_HEAD); t.rect(9, 5, 11, 7, HEX(0x0a0a0a)); t.set(10, 5, HEX(0xffffff)); t.rect(8, 8, 11, 8, HEX(0x0d2e1d)); }
    { TP t(T_DUCK_NECK); t.copyFrom(T_DUCK_HEAD); for (int y = 11; y < 14; y++) for (int x = 0; x < 16; x++) t.set(x, y, y == 13 ? HEX(0xd8d8d0) : HEX(0xf6f6f0)); for (int y = 14; y < 16; y++) for (int x = 0; x < 16; x++) t.set(x, y, HEX(0x6b3a20)); }
    { TP t(T_DUCK_BILL); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) t.set(x, y, mixc(HEX(0xf5d23a), HEX(0xc9a01c), y / 16.f));
      t.rect(6, 1, 9, 3, HEX(0x2b2a20)); t.rect(3, 8, 4, 9, HEX(0x6a5a20)); t.rect(11, 8, 12, 9, HEX(0x6a5a20)); }
    { TP t(T_DUCK_FOOT); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) t.set(x, y, (x % 5 == 2) ? HEX(0xd8701c) : HEX(0xf28a2a)); }
    { TP t(T_DUCK_TAIL); for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) t.set(x, y, y < 6 ? HEX(0x1a1a1a) : (y < 10 ? HEX(0xf0f0ec) : HEX(0x8a8680)));
      t.rect(6, 0, 9, 3, HEX(0x0f3d26)); }
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
//  ДЕТАЛЬНЫЕ ТЕКСТУРЫ БЛОКОВ (до 128x128 на грань): цвет + карта высот → объёмное освещение
// ============================================================================
static int g_tileRes = 128;                 // пикселей на грань блока: 16, 32, 64 или 128
static std::vector<u32> g_hdAtlas;          // атлас 16 x 16 тайлов по g_tileRes пикселей
static int g_hdSize = 256;                  // ширина HD-атласа (16 тайлов)
static int g_hdH = 512;                     // высота HD-атласа (32 тайла)
static std::vector<u32> g_hdAnim[3][16];    // кадры воды, «воды для шейдеров» и лавы

struct C3 { float r, g, b; };
static inline C3 c3(u32 h) { C3 c = {((h >> 16) & 255) / 255.f, ((h >> 8) & 255) / 255.f, (h & 255) / 255.f}; return c; }
static inline C3 cmix(C3 a, C3 b, float t) { C3 c = {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; return c; }
static inline C3 cmul(C3 a, float k) { C3 c = {a.r * k, a.g * k, a.b * k}; return c; }
struct Ramp {
    std::vector<C3> s;
    Ramp(std::initializer_list<u32> l) { for (u32 h : l) s.push_back(c3(h)); }
    C3 at(float t) const {
        t = clampf(t, 0.f, 1.f) * (s.size() - 1);
        int i = std::min((int)t, (int)s.size() - 2);
        return cmix(s[i], s[i + 1], t - i);
    }
};
// бесшовный шум: px, py — число ячеек на тайл по осям
static inline float hv(int x, int y, u32 s) { return (hash32((u32)x * 0x9E3779B1u ^ hash32((u32)y * 0x85EBCA77u ^ s)) & 0xFFFFFF) / 16777215.f; }
static float pn(float u, float v, int px, int py, u32 s) {
    float x = u * px, y = v * py; int xi = ifloor(x), yi = ifloor(y);
    float fx = x - xi, fy = y - yi; fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    int x0 = ((xi % px) + px) % px, x1 = (x0 + 1) % px, y0 = ((yi % py) + py) % py, y1 = (y0 + 1) % py;
    return lerpf(lerpf(hv(x0, y0, s), hv(x1, y0, s), fx), lerpf(hv(x0, y1, s), hv(x1, y1, s), fx), fy);
}
static float fbm(float u, float v, int p, int oct, u32 s, float gain = 0.5f, int py = -1) {
    if (py < 0) py = p;
    float sum = 0, amp = 1, norm = 0;
    for (int i = 0; i < oct; i++) { sum += pn(u, v, p, py, s + i * 977u) * amp; norm += amp; amp *= gain; p *= 2; py *= 2; }
    return sum / norm;
}
static float ridged(float u, float v, int p, int oct, u32 s, int py = -1) {
    if (py < 0) py = p;
    float sum = 0, amp = 1, norm = 0;
    for (int i = 0; i < oct; i++) { float n = 1.f - fabsf(pn(u, v, p, py, s + i * 613u) * 2.f - 1.f); sum += n * n * amp; norm += amp; amp *= 0.5f; p *= 2; py *= 2; }
    return sum / norm;
}
struct VR { float f1, f2; int id; float dx, dy; };
static VR vor(float u, float v, int g, u32 s, float jit = 0.9f, int gy = -1) {
    if (gy < 0) gy = g;
    float x = u * g, y = v * gy; int xi = ifloor(x), yi = ifloor(y);
    VR r = {1e9f, 1e9f, 0, 0, 0};
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        int cx = xi + dx, cy = yi + dy, wx = ((cx % g) + g) % g, wy = ((cy % gy) + gy) % gy;
        float ox = cx + 0.5f + (hv(wx, wy, s) - 0.5f) * jit, oy = cy + 0.5f + (hv(wx, wy, s + 7) - 0.5f) * jit;
        float d = sqrtf((x - ox) * (x - ox) + (y - oy) * (y - oy));
        if (d < r.f1) { r.f2 = r.f1; r.f1 = d; r.id = wx + wy * g; r.dx = x - ox; r.dy = y - oy; } else if (d < r.f2) r.f2 = d;
    }
    return r;
}
static inline float cellRnd(int id, u32 s) { return (hash32((u32)id * 2654435761u ^ s) & 0xFFFF) / 65535.f; }

// холст тайла: цвет, прозрачность, высота
struct HDT {
    int R; std::vector<float> d;   // r g b a h
    std::vector<float> gDark, gDepth;   // маска канавок: затемнение применяется один раз, а не накапливается
    explicit HDT(int r) : R(r), d((size_t)r * r * 5, 0.f) { for (size_t i = 0; i < (size_t)R * R; i++) d[i * 5 + 3] = 1.f; }
    float* at(int x, int y) { x = ((x % R) + R) % R; y = ((y % R) + R) % R; return &d[((size_t)y * R + x) * 5]; }
    template <class F> void each(F f) { for (int y = 0; y < R; y++) for (int x = 0; x < R; x++) f(x, y, (x + 0.5f) / R, (y + 0.5f) / R, at(x, y)); }
    static void put(float* p, C3 c, float h, float a = 1.f) { p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = a; p[4] = h; }
    void clearAlpha() { for (size_t i = 0; i < (size_t)R * R; i++) { d[i * 5 + 3] = 0; d[i * 5 + 4] = 0; } }
    // круг с мягким краем (в пикселях), dome — выпуклая высота
    void disc(float cx, float cy, float rad, C3 c, float h, float a = 1.f, bool dome = true, bool wrap = true) {
        int x0 = ifloor(cx - rad - 1), x1 = ifloor(cx + rad + 1), y0 = ifloor(cy - rad - 1), y1 = ifloor(cy + rad + 1);
        for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
            if (!wrap && (x < 0 || y < 0 || x >= R || y >= R)) continue;
            float dd = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float k = clampf(rad + 0.5f - dd, 0.f, 1.f) * a;
            if (k <= 0) continue;
            float* p = at(x, y);
            float hh = dome ? h * sqrtf(std::max(0.f, 1.f - (dd / rad) * (dd / rad))) : h;
            if (p[3] < 0.01f) { p[0] = c.r; p[1] = c.g; p[2] = c.b; }
            else { p[0] += (c.r - p[0]) * k; p[1] += (c.g - p[1]) * k; p[2] += (c.b - p[2]) * k; }
            p[3] = std::max(p[3], k); p[4] = std::max(p[4], hh * k + p[4] * (1 - k));
        }
    }
    void line(float x0, float y0, float x1, float y1, float w, C3 c, float h, float a = 1.f, bool wrap = true) {
        float L = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)); int n = std::max(1, (int)(L * 2.f));
        for (int i = 0; i <= n; i++) { float t = i / (float)n; disc(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, w * 0.5f, c, h, a, false, wrap); }
    }
    // «вырезать» канавку: темнит и опускает высоту (через маску — один раз на пиксель)
    void groove(float x0, float y0, float x1, float y1, float w, float dark, float depth, bool wrap = true) {
        if (gDark.empty()) { gDark.assign((size_t)R * R, 0.f); gDepth.assign((size_t)R * R, 0.f); }
        float L = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)); int n = std::max(1, (int)(L * 2.f));
        for (int i = 0; i <= n; i++) {
            float t = i / (float)n, cx = x0 + (x1 - x0) * t, cy = y0 + (y1 - y0) * t;
            for (int y = ifloor(cy - w); y <= ifloor(cy + w); y++) for (int x = ifloor(cx - w); x <= ifloor(cx + w); x++) {
                if (!wrap && (x < 0 || y < 0 || x >= R || y >= R)) continue;
                float dd = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
                float k = clampf(w * 0.5f + 0.5f - dd, 0.f, 1.f); if (k <= 0) continue;
                int xx = ((x % R) + R) % R, yy = ((y % R) + R) % R; size_t i2 = (size_t)yy * R + xx;
                gDark[i2] = std::max(gDark[i2], dark * k); gDepth[i2] = std::max(gDepth[i2], depth * k);
            }
        }
    }
    void flushGrooves() {
        if (gDark.empty()) return;
        for (size_t i = 0; i < (size_t)R * R; i++) { float* p = &d[i * 5]; float m = 1.f - gDark[i]; p[0] *= m; p[1] *= m; p[2] *= m; p[4] -= gDepth[i]; }
        gDark.clear(); gDepth.clear();
    }
    // объём: нормаль из высоты, свет сверху-слева, затенение во впадинах
    void light(float bump, float aoK, bool wrap = true, float hi = 1.f) {
        flushGrooves();
        std::vector<float> h(R * R), hb(R * R), tmp(R * R);
        for (int i = 0; i < R * R; i++) h[i] = d[i * 5 + 4];
        auto H = [&](const std::vector<float>& a, int x, int y) {
            if (wrap) { x = ((x % R) + R) % R; y = ((y % R) + R) % R; } else { x = clampi(x, 0, R - 1); y = clampi(y, 0, R - 1); }
            return a[y * R + x];
        };
        int rad = std::max(1, R / 20);
        for (int y = 0; y < R; y++) for (int x = 0; x < R; x++) { float s = 0; for (int k = -rad; k <= rad; k++) s += H(h, x + k, y); tmp[y * R + x] = s / (2 * rad + 1); }
        for (int y = 0; y < R; y++) for (int x = 0; x < R; x++) { float s = 0; for (int k = -rad; k <= rad; k++) s += H(tmp, x, y + k); hb[y * R + x] = s / (2 * rad + 1); }
        const float L[3] = {-0.48f, -0.56f, 0.67f};
        float sc = bump * R / 16.f;
        for (int y = 0; y < R; y++) for (int x = 0; x < R; x++) {
            float* p = &d[((size_t)y * R + x) * 5];
            if (p[3] < 0.01f) continue;
            float gx = (H(h, x + 1, y) - H(h, x - 1, y)) * 0.5f * sc, gy = (H(h, x, y + 1) - H(h, x, y - 1)) * 0.5f * sc;
            float nx = -gx, ny = -gy, nz = 1.f, nl = sqrtf(nx * nx + ny * ny + nz * nz);
            float diff = (nx * L[0] + ny * L[1] + nz * L[2]) / nl;
            float shade = clampf(0.25f + 0.75f * diff / L[2], 0.35f, 1.55f);
            if (shade > 1.f) shade = 1.f + (shade - 1.f) * hi;
            float cav = clampf((h[y * R + x] - hb[y * R + x]) * 4.f, -1.f, 0.4f);
            shade *= 1.f + aoK * cav;
            p[0] *= shade; p[1] *= shade; p[2] *= shade;
        }
    }
    void write(int tile) {
        flushGrooves();
        int tx = (tile & 15) * R, ty = (tile >> 4) * R;
        for (int y = 0; y < R; y++) for (int x = 0; x < R; x++) {
            float* p = &d[((size_t)y * R + x) * 5];
            g_hdAtlas[(size_t)(ty + y) * g_hdSize + tx + x] = RGBA((int)(p[0] * 255.f + 0.5f), (int)(p[1] * 255.f + 0.5f), (int)(p[2] * 255.f + 0.5f), (int)(clampf(p[3], 0, 1) * 255.f + 0.5f));
        }
    }
    void toVec(std::vector<u32>& v) {
        v.resize((size_t)R * R);
        for (int i = 0; i < R * R; i++) { float* p = &d[(size_t)i * 5]; v[i] = RGBA((int)(p[0] * 255.f), (int)(p[1] * 255.f), (int)(p[2] * 255.f), (int)(clampf(p[3], 0, 1) * 255.f)); }
    }
};

// ---------------------------------------------------------------- материалы
static void hdDirt(HDT& t, u32 s) {
    Ramp rp({0x3f2a1c, 0x553823, 0x6a472d, 0x7d5636, 0x8f6640});
    t.each([&](int, int, float u, float v, float* p) {
        float n = fbm(u, v, 4, 5, s), m = fbm(u, v, 16, 3, s + 9);
        HDT::put(p, rp.at(n * 0.8f + m * 0.35f - 0.1f), n * 0.5f + m * 0.3f);
    });
    Rng r(s); int R = t.R; float k = R / 16.f;
    for (int i = 0; i < 26; i++) {   // камешки
        float cx = r.f() * R, cy = r.f() * R, rad = k * (0.35f + r.f() * 0.6f);
        C3 c = cmix(c3(0x8a7e70), c3(0x5d4a3a), r.f());
        t.disc(cx, cy, rad, c, 1.2f);
    }
    for (int i = 0; i < 3; i++) {    // редкие тонкие корешки
        float x = r.f() * R, y = r.f() * R, a = r.f() * 6.28f;
        for (int j = 0; j < 4; j++) { float nx = x + cosf(a) * k * 1.f, ny = y + sinf(a) * k * 1.f; t.line(x, y, nx, ny, k * 0.18f, c3(0x7d5c3c), 0.7f, 0.8f); x = nx; y = ny; a += r.sf() * 0.8f; }
    }
}
static void hdGrassTop(HDT& t, u32 s) {
    Ramp rp({0x2c5a1c, 0x3b7326, 0x4a8a30, 0x5aa13a, 0x6fb84a, 0x86cc5c});
    t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 4, 4, s); HDT::put(p, rp.at(n * 0.7f), n * 0.3f); });
    Rng r(s + 1); int R = t.R; float k = R / 16.f;
    int blades = R * R / 7;
    for (int i = 0; i < blades; i++) {
        float x = r.f() * R, y = r.f() * R, a = r.f() * 6.28f, len = k * (0.6f + r.f() * 1.1f);
        float tone = clampf(fbm(x / R, y / R, 4, 3, s) * 0.8f + r.sf() * 0.25f + 0.1f, 0, 1);
        C3 c = rp.at(tone);
        t.line(x, y, x + cosf(a) * len, y + sinf(a) * len, k * 0.32f, c, 0.6f + r.f() * 0.6f);
        t.disc(x + cosf(a) * len, y + sinf(a) * len, k * 0.18f, cmul(c, 1.18f), 1.f, 1.f, false);
    }
    for (int i = 0; i < 4; i++) { float x = r.f() * R, y = r.f() * R; t.disc(x, y, k * 0.3f, c3(0xe8d860), 1.2f); }   // крохотные цветочки
    for (int i = 0; i < 5; i++) {   // клевер: тройные листочки
        float x = r.f() * R, y = r.f() * R, a0 = r.f() * 6.28f;
        for (int l = 0; l < 3; l++) { float a = a0 + l * 2.094f; t.disc(x + cosf(a) * k * 0.45f, y + sinf(a) * k * 0.45f, k * 0.42f, c3(0x4e9a36), 1.3f); }
        t.disc(x, y, k * 0.15f, c3(0x7ec85a), 1.5f);
    }
    for (int i = 0; i < R * R / 90; i++) {   // редкие сухие травинки
        float x = r.f() * R, y = r.f() * R, a = r.f() * 6.28f, len = k * (0.7f + r.f() * 0.8f);
        t.line(x, y, x + cosf(a) * len, y + sinf(a) * len, k * 0.22f, c3(0xa8a456), 0.9f, 0.85f);
    }
}
static void hdOverhang(HDT& t, const Ramp& rp, u32 s, float minD, float maxD, C3 edge, bool blades) {
    int R = t.R; float k = R / 16.f;
    std::vector<float> depth(R);
    for (int x = 0; x < R; x++) {
        float u = (x + 0.5f) / R;
        float dd = minD + (maxD - minD) * fbm(u, 0.3f, 4, 3, s);
        float drip = pn(u, 0.7f, 12, 1, s + 3); if (drip > 0.72f) dd += (drip - 0.72f) * 0.9f;
        depth[x] = dd * R;
    }
    for (int y = 0; y < R; y++) for (int x = 0; x < R; x++) {
        float* p = t.at(x, y); float D = depth[x];
        if (y < D) {
            float u = (x + 0.5f) / R, v = (y + 0.5f) / R;
            float n = fbm(u, v, 8, 3, s + 5);
            float vert = blades ? pn(u, v, R / 2, 2, s + 8) : 0.5f;
            HDT::put(p, rp.at(0.85f - y / D * 0.45f + (n - 0.5f) * 0.4f + (vert - 0.5f) * 0.25f), 0.9f + vert * 0.3f);
        } else if (y < D + k * 0.6f) { p[0] *= 0.55f; p[1] *= 0.55f; p[2] *= 0.55f; p[4] -= 0.4f; }   // тень под кромкой
    }
    (void)edge;
}
static void hdStone(HDT& t, u32 s, float tint = 0) {
    Ramp rp({0x55575c, 0x65676c, 0x76787d, 0x87898e, 0x989a9f, 0xa9abaf});
    t.each([&](int x, int y, float u, float v, float* p) {
        float n = fbm(u, v, 3, 6, s), rd = ridged(u, v, 4, 4, s + 2), sp = pn(u, v, 64, 64, s + 4);
        float strata = pn(u * 0.5f + n * 0.08f, v + n * 0.06f, 1, 7, s + 6);       // пологие слои породы
        float patch = fbm(u, v, 2, 2, s + 8);                                       // крупные пятна оттенка
        C3 c = rp.at(n * 0.8f + rd * 0.25f - 0.05f + (strata - 0.5f) * 0.12f);
        c = cmix(c, cmul(c, patch > 0.5f ? 1.06f : 0.93f), fabsf(patch - 0.5f) * 1.2f);
        if (sp > 0.93f) c = cmul(c, 1.15f); if (sp < 0.05f) c = cmul(c, 0.8f);
        float fl = hv(x, y, s + 9);                                                 // искорки кварца и тёмные вкрапления
        if (fl > 0.9965f) c = cmix(c, c3(0xe8e6e0), 0.7f); else if (fl < 0.003f) c = cmul(c, 0.55f);
        c.r += tint * 0.03f; c.b -= tint * 0.02f;
        HDT::put(p, c, n * 0.7f + rd * 0.6f + (strata - 0.5f) * 0.25f);
    });
    Rng r(s + 1); int R = t.R; float k = R / 16.f;
    // без трещин: на больших стенах любой повторяющийся рисунок бросается в глаза
    (void)r; (void)k; (void)R;
}
// камни по Вороному: выпуклые, со швами раствора
static void hdStones(HDT& t, u32 s, int grid, const Ramp& rp, C3 mortar, float mortarW, float jit, const Ramp* alt = nullptr, float bulge = 1.f) {
    t.each([&](int, int, float u, float v, float* p) {
        VR c = vor(u, v, grid, s, jit);
        float edge = c.f2 - c.f1;
        float n = fbm(u, v, 8, 4, s + 3);
        if (edge < mortarW) { HDT::put(p, cmix(mortar, cmul(mortar, 0.7f), n), -0.2f + n * 0.2f); return; }
        float cr = cellRnd(c.id, s);
        const Ramp& rr = (alt && cr > 0.68f) ? *alt : rp;
        float dome = sqrtf(clampf((edge - mortarW) * 3.2f, 0, 1));
        C3 col = rr.at(cr * 0.7f + n * 0.4f - 0.05f);
        HDT::put(p, col, dome * bulge + n * 0.35f);
    });
}
static void hdBark(HDT& t, u32 s, const Ramp& rp) {
    t.each([&](int, int, float u, float v, float* p) {
        float fis = ridged(u + fbm(u, v, 2, 2, s + 9) * 0.15f, v, 6, 3, s, 1);   // вертикальные борозды
        float n = fbm(u, v, 8, 4, s + 5, 0.5f, 3);
        float plate = 1.f - fis;
        HDT::put(p, rp.at(plate * 0.75f + n * 0.35f - 0.1f), plate * 1.2f + n * 0.3f);
    });
}
static void hdLogTop(HDT& t, u32 s, const Ramp& wood, const Ramp& bark) {
    int R = t.R;
    t.each([&](int x, int y, float u, float v, float* p) {
        float dx = u - 0.5f, dy = v - 0.5f;
        float sq = std::max(fabsf(dx), fabsf(dy)), eu = sqrtf(dx * dx + dy * dy), rr = sq * 0.6f + eu * 0.4f;
        float wob = (fbm(u, v, 4, 3, s) - 0.5f) * 0.03f;
        if (sq > 0.44f + wob) { float n = fbm(u, v, 12, 3, s + 1); HDT::put(p, bark.at(n * 0.9f), 0.6f + n * 0.5f); return; }
        float ring = 0.5f + 0.5f * sinf((rr + wob) * 70.f);
        float n = fbm(u, v, 16, 3, s + 2);
        C3 c = wood.at(0.25f + ring * 0.55f + (n - 0.5f) * 0.3f - rr * 0.4f);
        if (rr < 0.025f) c = cmul(c, 0.7f);
        HDT::put(p, c, ring * 0.25f + n * 0.15f);
        (void)x; (void)y;
    });
    Rng r(s); float k = R / 16.f;
    for (int i = 0; i < 3; i++) {   // радиальные трещины
        float a = r.f() * 6.28f, r0 = 0.08f + r.f() * 0.1f, r1 = r0 + 0.12f + r.f() * 0.15f;
        t.groove(R * (0.5f + cosf(a) * r0), R * (0.5f + sinf(a) * r0), R * (0.5f + cosf(a) * r1), R * (0.5f + sinf(a) * r1), k * 0.22f, 0.3f, 0.4f, false);
    }
}
static void hdPlanks(HDT& t, u32 s, const Ramp& rp, C3 seam) {
    int R = t.R; float k = R / 16.f;
    const float joint[4] = {0.69f, 0.19f, 0.88f, 0.44f};
    t.each([&](int x, int y, float u, float v, float* p) {
        int b = std::min(3, (int)(v * 4)); float bv = v * 4 - b;
        float tone = 0.3f + cellRnd(b, s) * 0.35f;
        float grain = fbm(u * 1.f + b * 0.37f, v, 2, 4, s + b * 31, 0.55f, 24);
        float fine = pn(u, v, 6, 96, s + 7);
        float knot = 0;
        { float kx = cellRnd(b, s + 11), ky = 0.5f; float dx = (u - kx) * 4.f, dy = (bv - ky); float dd = sqrtf(dx * dx + dy * dy * 1.6f);
          if (cellRnd(b, s + 13) > 0.55f && dd < 0.35f) knot = 1.f - dd / 0.35f; }
        float g2 = sinf((grain * 9.f + knot * 6.f) * 3.1416f) * 0.5f + 0.5f;
        C3 c = rp.at(tone + (g2 - 0.5f) * 0.28f + (fine - 0.5f) * 0.12f - knot * 0.4f);
        float h = 0.6f + g2 * 0.15f;
        float edge = std::min(bv, 1.f - bv) * 4.f;
        h -= (1.f - clampf(edge * 2.5f, 0, 1)) * 0.5f;
        bool gap = bv > 1.f - 0.07f;
        float jd = fabsf(u - joint[b]);
        if (jd < 0.012f) gap = true;
        if (gap) { HDT::put(p, seam, -0.4f); return; }
        if (jd < 0.03f) h -= (0.03f - jd) * 12.f;
        HDT::put(p, c, h);
        (void)x; (void)y;
    });
    for (int b = 0; b < 4; b++) {   // гвоздики у стыков
        float jx = joint[b] * R, cy = (b + 0.45f) * R / 4;
        t.disc(jx - k * 0.9f, cy, k * 0.32f, c3(0x5a5650), 1.2f); t.disc(jx + k * 0.9f, cy, k * 0.32f, c3(0x5a5650), 1.2f);
    }
}
static void hdLeaves(HDT& t, u32 s, std::initializer_list<u32> cols, bool needles) {
    t.clearAlpha();
    Ramp rp(cols);
    Rng r(s); int R = t.R; float k = R / 16.f;
    if (!needles) {
        int n = R * R / 26;
        for (int i = 0; i < n; i++) {
            float cx = r.f() * R, cy = r.f() * R, a = r.f() * 6.28f, len = k * (1.1f + r.f() * 0.9f), wd = len * 0.45f;
            float tone = clampf(0.15f + r.f() * 0.75f + (fbm(cx / R, cy / R, 4, 2, s) - 0.5f) * 0.5f, 0, 1);
            C3 c = rp.at(tone);
            float ca = cosf(a), sa = sinf(a);
            float layer = r.f();
            for (int y = ifloor(-len - 1); y <= (int)(len + 1); y++) for (int x = ifloor(-len - 1); x <= (int)(len + 1); x++) {
                float lx = x * ca + y * sa, ly = -x * sa + y * ca;
                float e = (lx / len) * (lx / len) + (ly / wd) * (ly / wd);
                if (e > 1.f) continue;
                float* p = t.at(ifloor(cx) + x, ifloor(cy) + y);
                float h = layer + sqrtf(1.f - e) * 0.6f;
                if (p[3] > 0.5f && p[4] > h) continue;
                C3 cc = c;
                if (fabsf(ly) < k * 0.12f && lx > -len * 0.8f) cc = cmul(c, 1.25f);     // жилка
                cc = cmul(cc, 0.82f + 0.3f * (ly / wd * 0.5f + 0.5f));
                HDT::put(p, cc, h, 1.f);
            }
        }
    } else {
        int n = R * R / 9;
        for (int i = 0; i < n; i++) {
            float cx = r.f() * R, cy = r.f() * R, a = 1.1f + r.sf() * 0.6f + (r.range(2) ? 3.14f : 0.f), len = k * (0.8f + r.f() * 0.7f);
            C3 c = rp.at(r.f());
            t.line(cx, cy, cx + cosf(a) * len, cy + sinf(a) * len, k * 0.28f, c, 0.4f + r.f() * 0.8f);
        }
    }
    // просветы
    t.each([&](int, int, float u, float v, float* p) { if (fbm(u, v, 6, 3, s + 77) < (needles ? 0.3f : 0.27f)) p[3] = 0; });
}
static void hdOre(HDT& t, u32 s, C3 dark, C3 mid, C3 light, C3 glint, int clusters, float size, bool crystal) {
    hdStone(t, s);
    Rng r(s * 3 + 1); int R = t.R; float k = R / 16.f;
    // центры скоплений разносятся по всей грани: каждое — не ближе minD к остальным (с учётом бесшовности)
    std::vector<std::pair<float, float>> ctr;
    const float minD = 0.62f / sqrtf((float)clusters);
    for (int tries = 0; tries < 4000 && (int)ctr.size() < clusters; tries++) {
        float u = r.f(), v = r.f(); bool ok = true;
        for (auto& c : ctr) {
            float du = fabsf(u - c.first), dv = fabsf(v - c.second); du = std::min(du, 1.f - du); dv = std::min(dv, 1.f - dv);
            if (du * du + dv * dv < minD * minD) { ok = false; break; }
        }
        if (ok) ctr.push_back(std::make_pair(u, v));
    }
    std::vector<std::tuple<float, float, float>> pcs;   // куски: x, y, радиус
    for (auto& c : ctr) {
        int pieces = 2 + r.range(3);
        for (int j = 0; j < pieces; j++) {
            float a = r.f() * 6.2831853f, dd = (j == 0 ? 0.f : (1.2f + r.f() * 1.3f)) * k;
            pcs.push_back(std::make_tuple(c.first * R + cosf(a) * dd, c.second * R + sinf(a) * dd, k * size * (0.6f + r.f() * 0.6f)));
        }
    }
    for (int i = 0; i < clusters + 2; i++) pcs.push_back(std::make_tuple(r.f() * R, r.f() * R, k * size * (0.35f + r.f() * 0.25f)));   // одиночные крупинки
    for (auto& pc : pcs) {
        {
            float px = std::get<0>(pc), py = std::get<1>(pc), rad = std::get<2>(pc);
            int sides = crystal ? 5 + r.range(3) : 9;
            float rot = r.f() * 6.28f;
            for (int y = ifloor(py - rad - 2); y <= (int)(py + rad + 2); y++) for (int x = ifloor(px - rad - 2); x <= (int)(px + rad + 2); x++) {
                float dx = x + 0.5f - px, dy = y + 0.5f - py, a = atan2f(dy, dx) - rot, dd = sqrtf(dx * dx + dy * dy);
                float seg = 6.2831853f / sides, ang = fmodf(a + 62.8318f, seg) - seg / 2;
                float lim = rad * cosf(seg / 2) / cosf(ang) * (crystal ? 1.f : (0.85f + 0.15f * sinf(a * 3.f + rot)));
                float* p = t.at(x, y);
                if (dd > lim + k * 0.4f) continue;
                if (dd > lim) { p[0] *= 0.6f; p[1] *= 0.6f; p[2] *= 0.6f; p[4] -= 0.2f; continue; }   // тень вокруг
                int facet = (int)((a + 62.8318f) / seg) % sides;
                float fl = crystal ? (0.55f + 0.45f * cosf(facet * seg + rot - 2.3f)) : (1.f - dd / lim * 0.6f);
                C3 col = fl > 0.75f ? cmix(mid, light, (fl - 0.75f) * 4.f) : cmix(dark, mid, fl / 0.75f);
                HDT::put(p, col, 1.5f + (1.f - dd / lim) * (crystal ? 0.6f : 1.f));
            }
            t.disc(px - rad * 0.3f, py - rad * 0.35f, k * 0.18f, glint, 2.2f, 0.7f, false);
        }
    }
}
static void hdBrick(HDT& t, u32 s, int rows, int cols, const Ramp& rp, C3 mortar, float mw, float chamfer) {
    t.each([&](int, int, float u, float v, float* p) {
        int row = std::min(rows - 1, (int)(v * rows)); float rv = v * rows - row;
        float off = (row % 2) * 0.5f / cols;
        float cu = u + off; cu -= floorf(cu);
        int col = (int)(cu * cols); float ru = cu * cols - col;
        float ex = std::min(ru, 1.f - ru) / cols, ey = std::min(rv, 1.f - rv) / rows, e = std::min(ex, ey);
        float n = fbm(u, v, 8, 4, s);
        if (e < mw) { HDT::put(p, cmix(mortar, cmul(mortar, 0.8f), n), -0.3f); return; }
        float cr = cellRnd(row * 31 + col, s);
        float bev = clampf((e - mw) / chamfer, 0, 1);
        HDT::put(p, rp.at(cr * 0.5f + n * 0.5f), sqrtf(bev) * 1.f + n * 0.25f);
    });
}
static void hdMetal(HDT& t, u32 s, const Ramp& rp, int kind) {
    int R = t.R; float k = R / 16.f;
    t.each([&](int, int, float u, float v, float* p) {
        float br = pn(u, v, 4, 128, s), sh = 0.5f + 0.35f * sinf((u + v) * 3.f);
        float e = std::min(std::min(u, 1 - u), std::min(v, 1 - v));
        float panel = clampf(e * 14.f, 0, 1);
        float inner = fabsf(e - 0.14f) < 0.012f ? 1.f : 0.f;
        HDT::put(p, rp.at(sh * 0.7f + br * 0.25f + 0.05f - inner * 0.3f), panel * 0.8f - inner * 0.5f + br * 0.05f);
    });
    const float rv[4][2] = {{0.08f, 0.08f}, {0.92f, 0.08f}, {0.08f, 0.92f}, {0.92f, 0.92f}};
    for (auto& q : rv) t.disc(q[0] * R, q[1] * R, k * 0.5f, rp.at(0.85f), 1.6f, 1.f, true, false);
    if (kind == 2) {   // алмазный: огранка в центре
        t.each([&](int x, int y, float u, float v, float* p) {
            float dx = u - 0.5f, dy = v - 0.5f, md = fabsf(dx) + fabsf(dy);
            if (md < 0.3f) { int f = (dx > 0) + 2 * (dy > 0); float fl = 0.6f + 0.15f * f + (md < 0.12f ? 0.25f : 0); HDT::put(p, rp.at(fl), 1.4f - md * 2.f); }
            (void)x; (void)y;
        });
    } else if (kind == 1) {   // золотой: выпуклый ромб
        t.each([&](int, int, float u, float v, float* p) { float md = fabsf(u - 0.5f) + fabsf(v - 0.5f); if (md < 0.28f) { p[4] = 0.8f + (0.28f - md) * 3.f; } });
    }
}
static void hdWool(HDT& t, u32 s, C3 base, C3 dark) {
    t.each([&](int x, int y, float u, float v, float* p) {
        float weave = ((x / std::max(1, t.R / 32) + y / std::max(1, t.R / 32)) & 1) ? 0.08f : -0.08f;
        float n = fbm(u, v, 8, 4, s);
        HDT::put(p, cmix(dark, base, 0.7f + n * 0.3f + weave), 0.5f + weave * 2.f + n * 0.3f);
    });
}

// ---------------------------------------------------------------- новые блоки биомов (любое разрешение)
// to16: рисуем крупно и уменьшаем в 16-пиксельный атлас с усреднением
static void writeTile16(HDT& t, int tile) {
    t.flushGrooves();
    int f = t.R / 16, tx = (tile & 15) * 16, ty = (tile >> 4) * 16;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        float r = 0, g = 0, b = 0, a = 0;
        for (int yy = 0; yy < f; yy++) for (int xx = 0; xx < f; xx++) {
            float* p = t.at(x * f + xx, y * f + yy); float al = clampf(p[3], 0, 1);
            r += p[0] * al; g += p[1] * al; b += p[2] * al; a += al;
        }
        float n = (float)(f * f);
        u32 c = 0;
        if (a > 0.001f) c = RGBA((int)clampf(r / a * 255.f + 0.5f, 0, 255), (int)clampf(g / a * 255.f + 0.5f, 0, 255), (int)clampf(b / a * 255.f + 0.5f, 0, 255),
                                 (a / n) > 0.42f ? 255 : 0);
        g_atlasPx[(ty + y) * ATLAS + tx + x] = c;
    }
}
static void genNewTiles16() { genNewBlockTiles(64, true); }
static void hdFelt(HDT& t, u32 s, C3 base) {   // шерсть: мягкий войлок с завитками волокон
    int R = t.R; float k = R / 16.f;
    t.each([&](int, int, float u, float v, float* p) {
        float n = fbm(u, v, 6, 4, s), cur = ridged(u, v, 10, 2, s + 5);
        C3 c = cmul(base, 0.84f + n * 0.22f - cur * 0.08f);
        HDT::put(p, c, n * 0.6f + cur * 0.3f);
    });
    Rng r(s + 9);
    for (int i = 0; i < R * R / 10; i++) {   // короткие волоски-завитки
        float x = r.f() * R, y = r.f() * R, a = r.f() * 6.28f, len = k * (0.4f + r.f() * 0.6f);
        float tone = 0.9f + r.f() * 0.22f;
        t.line(x, y, x + cosf(a) * len, y + sinf(a) * len, k * 0.16f, cmul(base, tone), 0.7f + r.f() * 0.4f, 0.55f);
    }
}
static void hdClay(HDT& t, u32 s, C3 base) {   // обожжённая глина: гладкая, с тонкими слоями и вкраплениями
    t.each([&](int x, int y, float u, float v, float* p) {
        float n = fbm(u, v, 4, 4, s), strata = pn(u * 0.2f, v, 1, 10, s + 3);
        float sp = hv(x, y, s + 7);
        C3 c = cmul(base, 0.9f + n * 0.14f + (strata - 0.5f) * 0.08f);
        if (sp > 0.992f) c = cmul(c, 1.25f); else if (sp < 0.006f) c = cmul(c, 0.7f);
        HDT::put(p, c, n * 0.25f + strata * 0.15f);
    });
}
static void genNewBlockTiles(int R, bool to16) {
    const float k = R / 16.f;
    auto mk = [&]() { return HDT(R); };
    auto out = [&](HDT& t, int tile) { if (to16) writeTile16(t, tile); else t.write(tile); };
    // берёза
    Ramp bbarkR({0xb9b4a6, 0xd2cdbf, 0xe4e0d4, 0xf2efe6, 0xfbf9f3});
    { HDT t = mk();
      t.each([&](int, int, float u, float v, float* p) {
          float n = fbm(u, v, 4, 4, 401, 0.5f, 2), peel = pn(u, v, 3, 8, 402);
          HDT::put(p, bbarkR.at(0.45f + n * 0.45f + (peel - 0.5f) * 0.25f), 0.6f + n * 0.3f); });
      Rng r(403);
      for (int i = 0; i < 18; i++) {   // тёмные чечевички-штрихи поперёк ствола
          float x = r.f() * R, y = r.f() * R, w = k * (1.0f + r.f() * 3.2f), th = k * (0.22f + r.f() * 0.3f);
          t.line(x - w * 0.5f, y, x + w * 0.5f, y + r.sf() * k * 0.3f, th, cmix(c3(0x1f1c19), c3(0x3e3a35), r.f()), 0.2f);
      }
      for (int i = 0; i < 4; i++) {    // крупные тёмные пятна
          float cx = r.f() * R, cy = r.f() * R;
          for (int j = 0; j < 6; j++) t.disc(cx + r.sf() * k * 1.4f, cy + r.sf() * k * 0.6f, k * (0.5f + r.f() * 0.5f), c3(0x2b2722), 0.3f, 0.85f, false);
      }
      for (int i = 0; i < 10; i++) {   // светлые завитки отслаивающейся коры
          float x = r.f() * R, y = r.f() * R;
          t.line(x, y, x + k * (1.f + r.f() * 2.f), y + r.sf() * k * 0.2f, k * 0.18f, c3(0xffffff), 1.1f, 0.7f);
      }
      t.light(0.06f, 0.4f); out(t, T_BIRCH_LOG); }
    { HDT t = mk(); hdLogTop(t, 404, Ramp({0xc9ae7c, 0xd8bf8e, 0xe6cfa0, 0xf0dcb0}), bbarkR); t.light(0.05f, 0.4f, false); out(t, T_BIRCH_TOP); }
    { HDT t = mk(); hdLeaves(t, 405, {0x3d6a22, 0x4f8a2c, 0x62a136, 0x78b545, 0x94c95a, 0xb0d870}, false); t.light(0.06f, 0.45f); out(t, T_BIRCH_LEAVES); }
    // акация
    Ramp abarkR({0x3e3934, 0x534c45, 0x665e55, 0x7a7065, 0x8c8174});
    { HDT t = mk(); hdBark(t, 410, abarkR);
      t.each([&](int, int, float u, float v, float* p) { float o = fbm(u, v, 5, 3, 411); if (o > 0.62f) { C3 c = cmix(c3(p[0] > 0 ? 0x7a5a3e : 0), c3(0xa0603a), (o - 0.62f) * 2.5f); p[0] = lerpf(p[0], c.r, 0.5f); p[1] = lerpf(p[1], c.g, 0.5f); p[2] = lerpf(p[2], c.b, 0.5f); } });
      t.light(0.09f, 0.6f); out(t, T_ACACIA_LOG); }
    { HDT t = mk(); hdLogTop(t, 412, Ramp({0x9c4a22, 0xb85a2a, 0xcf6d36, 0xdf8246}), abarkR); t.light(0.05f, 0.4f, false); out(t, T_ACACIA_TOP); }
    { HDT t = mk(); hdLeaves(t, 413, {0x4a5a1e, 0x5c6e24, 0x6f822c, 0x84963a, 0x9aa94a}, false);
      Rng r(414); for (int i = 0; i < R / 3; i++) t.disc(r.f() * R, r.f() * R, k * 0.35f, c3(0xd8c860), 1.5f, 1.f);   // мелкие жёлтые соцветия
      t.light(0.06f, 0.45f); out(t, T_ACACIA_LEAVES); }
    // тропическое дерево: кора с мхом
    Ramp jbarkR({0x3a2a16, 0x4f3a1e, 0x664b27, 0x7a5a30, 0x8c6a3a});
    { HDT t = mk(); hdBark(t, 420, jbarkR);
      t.each([&](int, int, float u, float v, float* p) {
          float m = fbm(u, v, 3, 4, 421) + (1.f - v) * 0.05f;
          if (m > 0.55f) { float a = clampf((m - 0.55f) * 6.f, 0, 1); C3 c = cmix(c3(0x3e6a26), c3(0x6a9a3a), fbm(u, v, 12, 2, 422));
              p[0] = lerpf(p[0], c.r, a); p[1] = lerpf(p[1], c.g, a); p[2] = lerpf(p[2], c.b, a); p[4] += a * 0.4f; } });
      t.light(0.09f, 0.6f); out(t, T_JUNGLE_LOG); }
    { HDT t = mk(); hdLogTop(t, 423, Ramp({0x8a5a2a, 0xa06a32, 0xb57c3e, 0xc78e4c}), jbarkR); t.light(0.05f, 0.4f, false); out(t, T_JUNGLE_TOP); }
    { HDT t = mk(); t.clearAlpha(); Ramp rp({0x0f3a12, 0x17501a, 0x1f6822, 0x2c802c, 0x3e9838});
      Rng r(424);
      for (int i = 0; i < R * R / 60; i++) {   // крупные глянцевые листья с жилкой
          float cx = r.f() * R, cy = r.f() * R, a = r.f() * 6.28f, len = k * (2.2f + r.f() * 1.4f), wd = len * 0.42f;
          C3 c = rp.at(r.f()); float ca = cosf(a), sa = sinf(a), layer = r.f();
          for (int y = ifloor(-len - 1); y <= (int)(len + 1); y++) for (int x = ifloor(-len - 1); x <= (int)(len + 1); x++) {
              float lx = x * ca + y * sa, ly = -x * sa + y * ca;
              float e = (lx / len) * (lx / len) + (ly / wd) * (ly / wd);
              if (e > 1.f) continue;
              float* p = t.at(ifloor(cx) + x, ifloor(cy) + y);
              float h = layer + sqrtf(1.f - e) * 0.6f;
              if (p[3] > 0.5f && p[4] > h) continue;
              C3 cc = c;
              if (fabsf(ly) < k * 0.14f) cc = cmul(c, 1.3f);
              else if (fmodf(fabsf(lx) + fabsf(ly) * 0.8f, k * 1.1f) < k * 0.12f) cc = cmul(c, 1.12f);   // боковые жилки
              if (ly < 0 && e > 0.5f) cc = cmul(cc, 1.15f);   // блик по краю
              HDT::put(p, cmul(cc, 0.85f + 0.25f * (ly / wd * 0.5f + 0.5f)), h, 1.f);
          }
      }
      t.each([&](int, int, float u, float v, float* p) { if (fbm(u, v, 5, 3, 425) < 0.22f) p[3] = 0; });
      t.light(0.06f, 0.5f); out(t, T_JUNGLE_LEAVES); }
    // сухой дёрн саванны
    Ramp dryR({0x6a6a2a, 0x80803a, 0x979446, 0xaea652, 0xc2b862, 0xd4c874});
    { HDT t = mk();
      t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 4, 4, 430); HDT::put(p, dryR.at(n * 0.6f), n * 0.3f); });
      Rng r(431);
      for (int i = 0; i < R * R / 6; i++) {
          float x = r.f() * R, y = r.f() * R, a = r.f() * 6.28f, len = k * (0.7f + r.f() * 1.3f);
          C3 c = dryR.at(clampf(fbm(x / R, y / R, 4, 3, 430) * 0.9f + r.sf() * 0.3f, 0, 1));
          t.line(x, y, x + cosf(a) * len, y + sinf(a) * len, k * 0.28f, c, 0.6f + r.f() * 0.6f);
      }
      for (int i = 0; i < R / 2; i++) { float x = r.f() * R, y = r.f() * R; t.disc(x, y, k * 0.25f, c3(0x5a4a2a), 0.3f); }   // проплешины
      t.light(0.05f, 0.45f); out(t, T_DRYGRASS_TOP); }
    { HDT t = mk(); hdDirt(t, 11); hdOverhang(t, dryR, 432, 0.14f, 0.24f, c3(0x5e5e26), true); t.light(0.06f, 0.5f); out(t, T_DRYGRASS_SIDE); }
    // хвойная подстилка: бурая земля, иголки, мелкие шишки
    Ramp podR({0x3a2614, 0x4c321a, 0x5e3f22, 0x70502c, 0x84603a});
    { HDT t = mk();
      t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 5, 4, 440); HDT::put(p, podR.at(n * 0.8f), n * 0.4f); });
      Rng r(441);
      for (int i = 0; i < R * R / 8; i++) {
          float x = r.f() * R, y = r.f() * R, a = r.f() * 6.28f, len = k * (0.6f + r.f() * 0.6f);
          C3 c = r.range(5) == 0 ? c3(0x9a6a34) : (r.range(3) ? c3(0x6e4a26) : c3(0x8a5e30));
          t.line(x, y, x + cosf(a) * len, y + sinf(a) * len, k * 0.14f, c, 0.8f + r.f() * 0.4f);
      }
      for (int i = 0; i < 3; i++) {   // шишки
          float x = r.f() * R, y = r.f() * R;
          for (int j = 0; j < 5; j++) t.disc(x, y + (j - 2) * k * 0.35f, k * (0.55f - fabsf(j - 2.f) * 0.12f), j & 1 ? c3(0x5a3418) : c3(0x7a4a22), 1.4f);
      }
      t.light(0.07f, 0.55f); out(t, T_PODZOL_TOP); }
    { HDT t = mk(); hdDirt(t, 11); hdOverhang(t, podR, 442, 0.1f, 0.18f, c3(0x2e1e10), false); t.light(0.06f, 0.5f); out(t, T_PODZOL_SIDE); }
    // ил: тёмный, влажный, с отблесками
    { HDT t = mk();
      Ramp rp({0x2a2620, 0x36302a, 0x433b33, 0x50473d});
      t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 4, 5, 450), w = fbm(u, v, 3, 3, 451);
          C3 c = rp.at(n * 0.9f);
          float h = n * 0.5f;
          if (w > 0.6f) { float a = clampf((w - 0.6f) * 6.f, 0, 1); c = cmix(c, c3(0x5a5650), a * 0.5f); h = lerpf(h, 0.2f, a); }   // лужицы
          HDT::put(p, c, h); });
      Rng r(452);
      for (int i = 0; i < R / 2; i++) { float x = r.f() * R, y = r.f() * R; t.disc(x, y, k * 0.18f, c3(0x9a958c), 0.6f, 0.8f); }   // влажные блики
      t.light(0.06f, 0.5f); out(t, T_MUD); }
    // красный песок
    { HDT t = mk(); Ramp rp({0x9a4a1c, 0xae5622, 0xbf632a, 0xcc7034, 0xd88040});
      t.each([&](int x, int y, float u, float v, float* p) {
          float rip = sinf((u * 0.35f + v + fbm(u, v, 4, 2, 460) * 0.12f) * 6.2831853f * 5.f) * 0.5f + 0.5f;
          float g = hv(x, y, 461) - 0.5f, n = fbm(u, v, 6, 3, 462);
          C3 c = rp.at(0.25f + n * 0.5f + rip * 0.15f + g * 0.25f);
          if (hv(x, y, 463) > 0.985f) c = cmul(c, 0.7f);
          if (hv(x, y, 464) > 0.994f) c = c3(0xf0d8b0);
          HDT::put(p, c, rip * 0.4f + g * 0.25f); });
      t.light(0.04f, 0.3f); out(t, T_REDSAND); }
    // обожжённая глина четырёх цветов
    { HDT t = mk(); hdClay(t, 470, c3(0x9a4632)); t.light(0.04f, 0.3f); out(t, T_TERRA_R); }
    { HDT t = mk(); hdClay(t, 471, c3(0xa9653a)); t.light(0.04f, 0.3f); out(t, T_TERRA_O); }
    { HDT t = mk(); hdClay(t, 472, c3(0xcdb6a2)); t.light(0.04f, 0.3f); out(t, T_TERRA_W); }
    { HDT t = mk(); hdClay(t, 473, c3(0x5e3e2c)); t.light(0.04f, 0.3f); out(t, T_TERRA_B); }
    // папоротник: изогнутые вайи с листочками
    { HDT t = mk(); t.clearAlpha(); Rng r(480);
      for (int f = 0; f < 5; f++) {
          float bx = R * (0.5f + r.sf() * 0.12f), ang = -1.5708f + (f - 2) * 0.42f + r.sf() * 0.12f, len = R * (0.55f + r.f() * 0.3f);
          float px = bx, py = R - 0.5f;
          for (int sgm = 1; sgm <= 14; sgm++) {
              float tt = sgm / 14.f, a = ang + (ang + 1.5708f) * tt * 1.4f;
              float nx = px + cosf(a) * len / 14.f, ny = py + sinf(a) * len / 14.f;
              C3 sc = cmix(c3(0x2e5a1e), c3(0x5a9a38), tt);
              t.line(px, py, nx, ny, k * 0.32f * (1.f - tt * 0.5f), sc, 0.8f, 1.f, false);
              if (sgm > 2) for (int sd = -1; sd <= 1; sd += 2) {   // перышки по обе стороны
                  float la = a + sd * 1.25f, ll = k * (1.6f * (1.f - tt * 0.8f) + 0.3f);
                  t.line(nx, ny, nx + cosf(la) * ll, ny + sinf(la) * ll, k * 0.42f * (1.f - tt * 0.5f), cmix(c3(0x346a24), c3(0x78b84c), tt * 0.7f + r.f() * 0.3f), 1.f, 1.f, false);
              }
              px = nx; py = ny;
          }
      }
      t.light(0.05f, 0.35f, false); out(t, T_FERN); }
    // сухой куст: ветвистые прутья
    { HDT t = mk(); t.clearAlpha(); Rng r(481);
      std::function<void(float, float, float, float, int)> branch = [&](float x, float y, float a, float len, int depth) {
          float nx = x + cosf(a) * len, ny = y + sinf(a) * len;
          t.line(x, y, nx, ny, k * (0.2f + depth * 0.12f), cmix(c3(0x5a3e22), c3(0x8a6a44), r.f()), 0.6f + depth * 0.2f, 1.f, false);
          if (depth <= 0) return;
          branch(nx, ny, a - 0.45f - r.f() * 0.3f, len * 0.68f, depth - 1);
          branch(nx, ny, a + 0.45f + r.f() * 0.3f, len * 0.68f, depth - 1);
      };
      for (int i = 0; i < 3; i++) branch(R * (0.45f + i * 0.05f), R - 0.5f, -1.5708f + (i - 1) * 0.35f, R * 0.22f, 3);
      t.light(0.05f, 0.3f, false); out(t, T_DEADBUSH); }
    // камыш: стебли с узлами и узкими листьями
    { HDT t = mk(); t.clearAlpha(); Rng r(482);
      for (int sx = 0; sx < 4; sx++) {
          float x = R * (0.2f + sx * 0.2f + r.sf() * 0.04f), w = k * (0.75f + r.f() * 0.3f);
          C3 base = cmix(c3(0x6a9a4a), c3(0x8cb862), r.f());
          for (int y = 0; y < R; y++) {
              float lean = sinf(y / (float)R * 3.f + sx) * k * 0.3f;
              for (int dx = -2 * (int)k; dx <= 2 * (int)k; dx++) {
                  float xx = x + lean + dx * 0.5f; float d = fabsf(dx * 0.5f) / w; if (d > 1.f) continue;
                  float* p = t.at(ifloor(xx), y);
                  C3 c = cmul(base, 0.75f + 0.35f * (1.f - d) + (dx < 0 ? 0.08f : -0.05f));
                  HDT::put(p, c, 1.f - d);
              }
          }
          float off = r.f() * R * 0.4f;
          for (int nd = 0; nd < 3; nd++) { float ny = off + nd * R * 0.33f; t.line(x - w, ny, x + w, ny, k * 0.35f, c3(0x4e7a34), 1.2f, 1.f); }   // узлы
          float ly = off + R * 0.15f;
          t.line(x, ly, x + (sx & 1 ? 1.f : -1.f) * k * 3.f, ly - k * 4.f, k * 0.4f, c3(0x5a8a3c), 0.9f, 1.f, false);   // лист
      }
      t.light(0.05f, 0.35f, false); out(t, T_REED); }
    // кувшинка: круглый лист с вырезом и жилками
    { HDT t = mk(); t.clearAlpha(); Rng r(483);
      float cx = R * 0.5f, cy = R * 0.5f, rad = R * 0.45f, cutA = 0.6f;
      t.each([&](int x, int y, float u, float v, float* p) {
          float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);
          float wob = (fbm(u, v, 4, 2, 484) - 0.5f) * R * 0.05f;
          if (d > rad + wob) return;
          float a = atan2f(dy, dx);
          if (fabsf(a - cutA) < 0.22f * (d / rad) + 0.02f) return;   // вырез
          float vein = fabsf(sinf((a - cutA) * 7.f)) < 0.09f + d / rad * 0.02f ? 1.f : 0.f;
          float n = fbm(u, v, 6, 3, 485);
          C3 c = cmix(c3(0x2a5a1e), c3(0x4c8a30), n * 0.6f + d / rad * 0.4f);
          if (vein > 0 && d > rad * 0.12f) c = cmul(c, 1.25f);
          if (d > rad * 0.88f + wob) c = cmul(c, 0.8f);   // край чуть загнут
          HDT::put(p, c, 0.5f + n * 0.3f - (d / rad) * 0.2f, 1.f);
      });
      t.light(0.05f, 0.4f, false); out(t, T_LILY); }
    // василёк и ромашка
    auto stem = [&](HDT& t, float x, float top, u32 seed) {
        Rng r(seed);
        t.line(x, R - 0.5f, x + r.sf() * k, top, k * 0.45f, c3(0x3d7a2c), 0.6f, 1.f, false);
        for (int i = 0; i < 2; i++) { float ly = R * (0.75f - i * 0.15f), dir = i ? -1.f : 1.f;
            t.line(x, ly, x + dir * k * 2.2f, ly - k * 1.8f, k * 0.45f, c3(0x4c9038), 0.8f, 1.f, false); }
    };
    { HDT t = mk(); t.clearAlpha(); stem(t, R * 0.5f, R * 0.38f, 490);
      Rng r(491);
      for (int i = 0; i < 12; i++) {   // рваные лепестки-трубочки
          float a = i * 0.5236f + r.sf() * 0.1f, l = k * (2.2f + r.f() * 0.6f);
          float ex = R * 0.5f + cosf(a) * l, ey = R * 0.3f + sinf(a) * l * 0.85f;
          t.line(R * 0.5f, R * 0.3f, ex, ey, k * 0.75f, i & 1 ? c3(0x2c5ad8) : c3(0x4a7af0), 1.f, 1.f, false);
          t.disc(ex, ey, k * 0.5f, c3(0x6a96ff), 1.2f, 1.f, true, false);
      }
      t.disc(R * 0.5f, R * 0.3f, k * 0.8f, c3(0x2a2a8a), 1.5f, 1.f, true, false);
      t.light(0.05f, 0.3f, false); out(t, T_FLOWER_B); }
    { HDT t = mk(); t.clearAlpha(); stem(t, R * 0.5f, R * 0.4f, 492);
      for (int i = 0; i < 16; i++) {
          float a = i * 0.3927f, l = k * 2.6f;
          t.line(R * 0.5f + cosf(a) * k * 0.6f, R * 0.3f + sinf(a) * k * 0.6f, R * 0.5f + cosf(a) * l, R * 0.3f + sinf(a) * l * 0.9f, k * 0.6f,
                 i & 1 ? c3(0xffffff) : c3(0xeef0f4), 1.f, 1.f, false);
      }
      t.disc(R * 0.5f, R * 0.3f, k * 1.05f, c3(0xf2c020), 1.7f, 1.f, true, false);
      t.light(0.05f, 0.3f, false); out(t, T_FLOWER_W); }
    // замшелый булыжник
    { HDT t = mk(); Ramp rp({0x55575c, 0x67696e, 0x7a7c81, 0x8c8e93, 0x9fa1a5}); hdStones(t, 15, 3, rp, c3(0x3e3f44), 0.035f, 0.85f);
      t.each([&](int, int, float u, float v, float* p) {
          float m = fbm(u, v, 4, 4, 495);
          if (m > 0.5f) { float a = clampf((m - 0.5f) * 5.f, 0, 1); C3 c = cmix(c3(0x3a5e22), c3(0x6a9238), fbm(u, v, 16, 2, 496));
              p[0] = lerpf(p[0], c.r, a); p[1] = lerpf(p[1], c.g, a); p[2] = lerpf(p[2], c.b, a); p[4] += a * 0.3f; } });
      t.light(0.09f, 0.6f); out(t, T_MOSSY); }
    // шерсть пяти цветов
    const u32 woolC[5] = {0xeceae4, 0xa8a8a4, 0x5e5e5c, 0x1e1e22, 0x6a4a30};
    for (int i = 0; i < 5; i++) { HDT t = mk(); hdFelt(t, 500 + i, c3(woolC[i])); t.light(0.05f, 0.45f); out(t, T_WOOL0 + i); }
    // ---------------- шкуры животных (рисуются в любом разрешении; серые шкуры раскрашиваются цветом породы)
    auto furLines = [&](HDT& t, u32 seed, float len, float ang, float jitter, float tone0, float tone1, float alpha) {   // пряди шерсти
        Rng r(seed); int n = t.R * t.R / 40;   // редкие мягкие пряди: без мелкой «ряби», которая издалека превращается в точки
        for (int i = 0; i < n; i++) {
            float x = r.f() * R, y = r.f() * R, a = ang + r.sf() * jitter, l = k * len * 1.6f * (0.6f + r.f() * 0.7f);
            float tone = 1.f + (tone0 + (tone1 - tone0) * r.f() - 1.f) * 0.35f;
            float* p0 = t.at(ifloor(x), ifloor(y));
            C3 base = {p0[0], p0[1], p0[2]};
            t.line(x, y, x + cosf(a) * l, y + sinf(a) * l, k * 0.5f, cmul(base, tone), 0.4f + r.f() * 0.3f, alpha * 0.6f);
        }
    };
    // мягкое размытие шкур: животных видно мелко, и резкие пиксели мерцали бы «точками»
    auto outA = [&](HDT& t, int tile) {
        bool face = tile == T_PIG_FACE || tile == T_PIG_SNOUT || tile == T_SHEEP_FACE || tile == T_DOG_MUZZLE;   // глаза и нос оставляем чёткими
        int rad = std::max(1, face ? R / 48 : (tile == T_SHEEP_WOOL ? R / 36 : R / 18));
        std::vector<float> tmp((size_t)R * R * 3);
        for (int pass = 0; pass < 2; pass++) {
            for (int y = 0; y < R; y++) for (int x = 0; x < R; x++) {
                float acc[3] = {0, 0, 0};
                for (int d = -rad; d <= rad; d++) { const float* q = pass == 0 ? t.at(x + d, y) : t.at(x, y + d); acc[0] += q[0]; acc[1] += q[1]; acc[2] += q[2]; }
                float* o = &tmp[((size_t)y * R + x) * 3]; float n = 2.f * rad + 1.f; o[0] = acc[0] / n; o[1] = acc[1] / n; o[2] = acc[2] / n;
            }
            for (int y = 0; y < R; y++) for (int x = 0; x < R; x++) { float* q = t.at(x, y); const float* o = &tmp[((size_t)y * R + x) * 3]; q[0] = o[0]; q[1] = o[1]; q[2] = o[2]; }
        }
        out(t, tile);
    };
    // свинья: розовая кожа со щетинкой и лёгкими пятнышками
    Ramp pigR({0xd98a8a, 0xe69a98, 0xf0aaa6, 0xf6b9b4, 0xfac8c2});
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 2, 4, 600), m = fbm(u, v, 3, 2, 601);
          HDT::put(p, pigR.at(0.35f + n * 0.5f - (m > 0.7f ? 0.2f : 0.f)), n * 0.3f); });
      furLines(t, 602, 0.5f, 1.5708f, 0.6f, 0.92f, 1.08f, 0.45f);
      t.light(0.04f, 0.3f); outA(t, T_PIG_SKIN); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 2, 4, 603); HDT::put(p, pigR.at(0.4f + n * 0.45f), n * 0.3f); });
      for (int s = -1; s <= 1; s += 2) {   // глаза с бликом и надбровье
          float ex = R * (0.5f + s * 0.27f), ey = R * 0.38f;
          t.line(ex - k * 1.6f, ey - k * 1.7f, ex + k * 1.6f, ey - k * 1.9f, k * 0.7f, c3(0xd88080), 0.9f);
          t.disc(ex, ey, k * 1.25f, c3(0xf6eee8), 1.1f, 1.f, true, false);
          t.disc(ex + s * k * 0.15f, ey + k * 0.15f, k * 0.85f, c3(0x2a1a14), 1.2f, 1.f, true, false);
          t.disc(ex - k * 0.25f, ey - k * 0.3f, k * 0.28f, c3(0xffffff), 1.4f, 1.f, true, false);
      }
      t.light(0.05f, 0.35f, false); outA(t, T_PIG_FACE); }
    { HDT t = mk(); t.each([&](int x, int y, float u, float v, float* p) {   // пятачок: светлый овал с двумя ноздрями
          float dx = (x + 0.5f) / R - 0.5f, dy = (y + 0.5f) / R - 0.5f, d = sqrtf(dx * dx * 1.1f + dy * dy * 1.8f);
          float n = fbm(u, v, 2, 3, 604);
          C3 c = d < 0.42f ? cmix(c3(0xffc4c0), c3(0xf0a0a0), d * 2.f + n * 0.2f) : pigR.at(0.3f + n * 0.4f);
          HDT::put(p, c, d < 0.42f ? 0.8f - d : 0.2f); });
      t.disc(R * 0.36f, R * 0.52f, k * 1.7f, c3(0x7a3c40), -0.6f, 1.f, false, false); t.disc(R * 0.64f, R * 0.52f, k * 1.7f, c3(0x7a3c40), -0.6f, 1.f, false, false);
      t.light(0.08f, 0.5f, false); outA(t, T_PIG_SNOUT); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 2, 3, 605);
          if (v > 0.8f) HDT::put(p, cmix(c3(0x4a3a36), c3(0x6a5650), n), 0.6f + n * 0.3f);   // копытце
          else HDT::put(p, pigR.at(0.3f + n * 0.4f - v * 0.15f), n * 0.3f); });
      t.light(0.05f, 0.35f); outA(t, T_PIG_LEG); }
    // овца: курчавое руно (серое — красится цветом шерсти), светлая морда, тёмные копыта
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 2, 3, 610); HDT::put(p, cmul(c3(0xf2f0ea), 0.82f + n * 0.12f), n * 0.4f); });
      Rng r(611);
      for (int i = 0; i < R * R / 70; i++) {   // крупные мягкие клочья руна
          float x = r.f() * R, y = r.f() * R, rad = k * (1.3f + r.f() * 1.1f);
          float tone = 0.95f + r.f() * 0.07f;
          t.disc(x, y, rad, cmul(c3(0xf6f4ee), tone), 1.f + r.f() * 0.5f);
          t.disc(x + rad * 0.25f, y + rad * 0.25f, rad * 0.5f, cmul(c3(0xe4e0d8), tone), 0.6f, 0.35f);
      }
      t.light(0.05f, 0.35f); outA(t, T_SHEEP_WOOL); }
    Ramp sheepFace({0xcdb8a0, 0xd9c6b0, 0xe4d4c0, 0xece0ce});
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 2, 3, 612); HDT::put(p, sheepFace.at(0.3f + n * 0.5f + v * 0.15f), n * 0.3f); });
      for (int s = -1; s <= 1; s += 2) {
          float ex = R * (0.5f + s * 0.28f), ey = R * 0.4f;
          t.disc(ex, ey, k * 1.1f, c3(0xe8dccc), 1.f, 1.f, true, false);
          t.line(ex - k * 0.9f, ey, ex + k * 0.9f, ey, k * 0.9f, c3(0x2a2018), 1.2f, 1.f, false);   // вытянутый «козий» зрачок
          t.disc(ex - k * 0.4f, ey - k * 0.35f, k * 0.25f, c3(0xffffff), 1.4f, 1.f, true, false);
      }
      t.disc(R * 0.5f, R * 0.78f, k * 1.6f, c3(0xd49a94), 0.8f, 1.f, true, false);   // носик
      t.line(R * 0.42f, R * 0.88f, R * 0.5f, R * 0.84f, k * 0.35f, c3(0x7a5048), 0.5f, 1.f, false);
      t.line(R * 0.58f, R * 0.88f, R * 0.5f, R * 0.84f, k * 0.35f, c3(0x7a5048), 0.5f, 1.f, false);
      t.light(0.05f, 0.35f, false); outA(t, T_SHEEP_FACE); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 2, 3, 613);
          if (v > 0.82f) HDT::put(p, cmix(c3(0x3a3230), c3(0x564a44), n), 0.6f);
          else HDT::put(p, sheepFace.at(0.2f + n * 0.5f), n * 0.3f); });
      furLines(t, 614, 0.4f, 1.5708f, 0.4f, 0.9f, 1.05f, 0.5f);
      t.light(0.05f, 0.35f); outA(t, T_SHEEP_LEG); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 3, 4, 615); HDT::put(p, cmix(c3(0xd8b8a4), c3(0xead2c0), n), n * 0.4f); });
      furLines(t, 616, 0.3f, 0.f, 3.14f, 0.92f, 1.06f, 0.4f);   // короткий подшёрсток после стрижки
      t.light(0.05f, 0.4f); outA(t, T_SHEEP_SKIN); }
    // кошка: полосатый мех (серый, красится окрасом), мордочка с «М» на лбу и светлой нижней частью
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) {
          float wob = fbm(u, v, 3, 3, 620) * 0.25f;
          float stripe = 0.5f + 0.5f * sinf((u * 3.f + wob) * 6.2832f);
          float n = fbm(u, v, 3, 3, 621);
          HDT::put(p, cmul(c3(0xd6d6d6), 0.66f + 0.36f * smooth01(0.2f, 0.85f, stripe) + (n - 0.5f) * 0.06f), stripe * 0.2f + n * 0.2f); });
      furLines(t, 622, 0.45f, 1.5708f, 0.3f, 0.9f, 1.1f, 0.5f);
      t.light(0.02f, 0.2f); outA(t, T_CAT_FUR); }
    { HDT t = mk(); t.each([&](int x, int y, float u, float v, float* p) {
          float n = fbm(u, v, 3, 3, 623);
          float dx = (x + 0.5f) / R - 0.5f, dy = (y + 0.5f) / R;
          C3 c = cmul(c3(0xd6d6d6), 0.85f + (n - 0.5f) * 0.15f);
          if (dy > 0.55f && fabsf(dx) < 0.33f - (dy - 0.55f) * 0.4f) c = cmul(c3(0xf4f2ee), 1.08f);   // светлая мордочка
          float m = fabsf(fabsf(dx) - 0.12f) + fabsf(dy - 0.12f - fabsf(dx) * 0.6f);
          if (dy < 0.32f && m < 0.05f) c = cmul(c, 0.55f);   // «М» на лбу
          if (dy > 0.18f && dy < 0.3f && fabsf(fabsf(dx) - 0.4f) < 0.06f) c = cmul(c, 0.6f);   // полоски у висков
          HDT::put(p, c, n * 0.2f); });
      furLines(t, 624, 0.35f, 1.5708f, 0.5f, 0.92f, 1.08f, 0.35f);
      t.light(0.04f, 0.3f, false); outA(t, T_CAT_FACE); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 3, 3, 625); HDT::put(p, cmul(c3(0xf2f0ec), 0.92f + n * 0.12f), n * 0.3f); });
      furLines(t, 626, 0.4f, 1.5708f, 0.5f, 0.9f, 1.05f, 0.4f);
      t.light(0.04f, 0.3f); outA(t, T_CAT_BELLY); }
    // собака: густая шерсть (серая, красится окрасом), морда со светлыми бровями, нос и пасть на мордочке
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 2, 4, 630); HDT::put(p, cmul(c3(0xd2d2d2), 0.74f + n * 0.22f + v * 0.16f), n * 0.4f); });   // спина темнее, ниже — светлее
      furLines(t, 631, 0.8f, 1.35f, 0.35f, 0.82f, 1.15f, 0.6f);
      t.light(0.025f, 0.25f); outA(t, T_DOG_FUR); }
    { HDT t = mk(); t.each([&](int x, int y, float u, float v, float* p) { float n = fbm(u, v, 2, 4, 632); HDT::put(p, cmul(c3(0xd2d2d2), 0.8f + n * 0.25f), n * 0.3f); (void)x; (void)y; });
      for (int s = -1; s <= 1; s += 2) t.disc(R * (0.5f + s * 0.24f), R * 0.3f, k * 1.4f, c3(0xf0ece4), 0.6f, 0.85f);   // светлые «брови»
      furLines(t, 633, 0.5f, 1.5708f, 0.4f, 0.88f, 1.1f, 0.45f);
      t.light(0.05f, 0.35f, false); outA(t, T_DOG_FACE); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 3, 3, 634); HDT::put(p, cmul(c3(0xece8e0), 0.88f + n * 0.15f), n * 0.3f); });
      t.disc(R * 0.5f, R * 0.25f, k * 2.6f, c3(0x1c1816), 1.4f, 1.f, true, false);   // мокрый нос
      t.disc(R * 0.44f, R * 0.18f, k * 0.6f, c3(0x6a6460), 1.8f, 1.f, true, false);
      t.line(R * 0.5f, R * 0.4f, R * 0.5f, R * 0.62f, k * 0.45f, c3(0x3a3230), 0.3f, 1.f, false);   // пасть
      t.line(R * 0.5f, R * 0.62f, R * 0.3f, R * 0.7f, k * 0.45f, c3(0x3a3230), 0.3f, 1.f, false);
      t.line(R * 0.5f, R * 0.62f, R * 0.7f, R * 0.7f, k * 0.45f, c3(0x3a3230), 0.3f, 1.f, false);
      t.light(0.05f, 0.35f, false); outA(t, T_DOG_MUZZLE); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) {   // ошейник: кожа со строчкой и латунной бляшкой
          float n = fbm(u, v, 3, 3, 640);
          C3 c = cmix(c3(0xa01c1c), c3(0xc8302a), n);
          if (fabsf(v - 0.22f) < 0.03f || fabsf(v - 0.78f) < 0.03f) c = (fmodf(u * 12.f, 1.f) < 0.5f) ? c3(0xf0d8b0) : cmul(c, 0.8f);
          HDT::put(p, c, 0.5f + n * 0.2f); });
      t.disc(R * 0.5f, R * 0.5f, k * 2.4f, c3(0xd8b040), 1.6f, 1.f, true, false);
      t.disc(R * 0.46f, R * 0.44f, k * 0.8f, c3(0xfff0a0), 2.f, 1.f, true, false);
      t.light(0.06f, 0.4f, false); out(t, T_COLLAR); }
    // ---------------- игроки в сетевой игре: лицо, волосы, рубашка (красится), штаны, ботинки
    Ramp skinR({0xc89070, 0xd8a080, 0xe4b090, 0xeec0a0});
    Ramp hairR({0x2a1a10, 0x3a2414, 0x4a2e1a, 0x5a3a22});
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) {
          float n = fbm(u, v, 4, 3, 700);
          if (v < 0.22f + 0.05f * sinf(u * 18.f) * (u > 0.15f && u < 0.85f ? 1.f : 0.f) || (v < 0.42f && (u < 0.1f || u > 0.9f))) HDT::put(p, hairR.at(n), 0.8f + n * 0.3f);   // чёлка и виски
          else HDT::put(p, skinR.at(0.4f + n * 0.4f), n * 0.2f); });
      for (int s2 = -1; s2 <= 1; s2 += 2) {
          float ex = R * (0.5f + s2 * 0.2f), ey = R * 0.5f;
          t.line(ex - k * 1.5f, ey - k * 2.f, ex + k * 1.5f, ey - k * 2.2f, k * 0.6f, hairR.at(0.3f), 0.9f, 1.f, false);   // брови
          t.disc(ex, ey, k * 1.4f, c3(0xf8f6f2), 1.f, 1.f, true, false);
          t.disc(ex + s2 * k * 0.3f, ey, k * 0.9f, c3(0x3a6aa8), 1.1f, 1.f, true, false);
          t.disc(ex + s2 * k * 0.3f, ey, k * 0.45f, c3(0x101418), 1.2f, 1.f, true, false);
          t.disc(ex - k * 0.2f, ey - k * 0.4f, k * 0.25f, c3(0xffffff), 1.4f, 1.f, true, false);
      }
      t.line(R * 0.5f, R * 0.56f, R * 0.47f, R * 0.68f, k * 0.5f, c3(0xc08060), 0.8f, 0.8f, false);   // нос
      t.line(R * 0.4f, R * 0.8f, R * 0.6f, R * 0.8f, k * 0.6f, c3(0x9a5444), 0.3f, 1.f, false);       // рот
      t.light(0.05f, 0.35f, false); out(t, T_PL_FACE); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) {
          float n = fbm(u, v, 4, 3, 701);
          if (v < 0.42f || (u > 0.55f && v < 0.85f)) HDT::put(p, hairR.at(n), 0.8f + n * 0.3f);   // волосы сверху и на затылочной стороне
          else HDT::put(p, skinR.at(0.35f + n * 0.4f), n * 0.2f); });
      t.disc(R * 0.32f, R * 0.6f, k * 1.6f, skinR.at(0.2f), 0.9f, 1.f, true, false);   // ухо
      t.light(0.05f, 0.35f, false); out(t, T_PL_HEAD); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 3, 4, 702), st = 0.5f + 0.5f * sinf((u * 0.3f + v) * 40.f + n * 6.f); HDT::put(p, hairR.at(n * 0.7f + st * 0.3f), 0.6f + st * 0.3f); });
      t.light(0.08f, 0.5f); out(t, T_PL_HAIR); }
    { HDT t = mk(); t.each([&](int x, int y, float u, float v, float* p) { float n = fbm(u, v, 8, 3, 703); float weave = ((x + y) / std::max(1, R / 32)) & 1 ? 0.03f : -0.03f;
          HDT::put(p, cmul(c3(0xe8e8e8), 0.86f + n * 0.12f + weave), n * 0.3f); });
      t.light(0.04f, 0.3f); out(t, T_PL_SHIRT); }
    { HDT t = mk(); t.each([&](int x, int y, float u, float v, float* p) { float n = fbm(u, v, 8, 3, 704); float weave = ((x + y) / std::max(1, R / 32)) & 1 ? 0.03f : -0.03f;
          C3 c = cmul(c3(0xe8e8e8), 0.86f + n * 0.12f + weave);
          float dx = fabsf(u - 0.5f);
          if (v < 0.14f && dx < 0.22f - v) c = skinR.at(0.5f);                       // вырез воротника
          else if (v < 0.2f && dx < 0.3f - v * 0.5f && dx > 0.18f - v) c = cmul(c, 0.7f);   // воротник
          if (fabsf(u - 0.5f) < 0.015f && v > 0.18f) c = cmul(c, 0.8f);              // планка
          HDT::put(p, c, n * 0.3f); });
      for (int i = 0; i < 3; i++) t.disc(R * 0.5f, R * (0.32f + i * 0.2f), k * 0.55f, c3(0xd0d0d0), 1.2f, 1.f, true, false);   // пуговицы
      t.light(0.04f, 0.3f, false); out(t, T_PL_SHIRT_F); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 6, 3, 705);
          if (v < 0.55f) HDT::put(p, cmul(c3(0xe8e8e8), 0.86f + n * 0.12f), 0.5f + n * 0.2f);     // рукав
          else HDT::put(p, skinR.at(0.35f + n * 0.4f), n * 0.2f); });                           // кисть
      t.light(0.04f, 0.3f); out(t, T_PL_ARM); }
    { HDT t = mk(); t.each([&](int x, int y, float u, float v, float* p) { float n = fbm(u, v, 8, 3, 706); float tw = ((x + y * 2) / std::max(1, R / 24)) % 3 == 0 ? 0.06f : 0.f;   // саржа джинсы
          C3 c = cmix(c3(0x2a3a5c), c3(0x3c5078), n + tw);
          if (fabsf(u - 0.5f) < 0.02f) c = c3(0xc8a050);   // шов
          HDT::put(p, c, n * 0.3f + tw); });
      t.light(0.04f, 0.3f); out(t, T_PL_PANTS); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 6, 3, 707);
          C3 c = cmix(c3(0x3a2a1e), c3(0x5a4030), n); if (v > 0.8f) c = c3(0x222020);   // подошва
          HDT::put(p, c, 0.5f + n * 0.3f); });
      t.light(0.05f, 0.4f); out(t, T_PL_SHOE); }
    (void)k;
}

// ---------------------------------------------------------------- все блоки
static void genHDBlocks() {
    const int R = g_tileRes; const float k = R / 16.f;
    auto mk = [&]() { return HDT(R); };
    // земля и трава
    { HDT t = mk(); hdDirt(t, 11); t.light(0.06f, 0.5f); t.write(T_DIRT); }
    Ramp grassR({0x2c5a1c, 0x3b7326, 0x4a8a30, 0x5aa13a, 0x6fb84a, 0x86cc5c});
    { HDT t = mk(); hdGrassTop(t, 12); t.light(0.05f, 0.45f); t.write(T_GRASS_TOP); }
    { HDT t = mk(); hdDirt(t, 11); hdOverhang(t, grassR, 13, 0.16f, 0.27f, c3(0x2f5f21), true); t.light(0.06f, 0.5f); t.write(T_GRASS_SIDE); }
    // камни
    { HDT t = mk(); hdStone(t, 14); t.light(0.05f, 0.5f); t.write(T_STONE); }
    { HDT t = mk(); Ramp rp({0x55575c, 0x67696e, 0x7a7c81, 0x8c8e93, 0x9fa1a5}); hdStones(t, 15, 3, rp, c3(0x3e3f44), 0.035f, 0.85f); t.light(0.09f, 0.6f); t.write(T_COBBLE); }
    { HDT t = mk(); Ramp rp({0x5d5853, 0x6e6862, 0x7f7871, 0x908880, 0xa09890}); Ramp alt({0x6a5444, 0x7e6654, 0x927866, 0xa68a78});
      hdStones(t, 17, 6, rp, c3(0x3e3a36), 0.08f, 1.f, &alt, 0.9f); t.light(0.1f, 0.6f); t.write(T_GRAVEL); }
    { HDT t = mk(); Ramp rp({0x0e0e10, 0x1f1f23, 0x333338, 0x48484f, 0x606068}); hdStones(t, 25, 4, rp, c3(0x09090a), 0.04f, 1.f);
      t.each([&](int, int, float u, float v, float* p) { float n = ridged(u, v, 6, 3, 26); p[4] += n * 0.6f; }); t.light(0.1f, 0.7f); t.write(T_BEDROCK); }
    // песок и песчаник
    Ramp sandR({0xc4ab72, 0xd2ba82, 0xdec892, 0xe8d4a2, 0xf1e0b4});
    { HDT t = mk(); t.each([&](int x, int y, float u, float v, float* p) {
          float rip = sinf((u * 0.35f + v + fbm(u, v, 4, 2, 16) * 0.12f) * 6.2831853f * 5.f) * 0.5f + 0.5f;
          float g = hv(x, y, 77) - 0.5f, n = fbm(u, v, 6, 3, 18);
          C3 c = sandR.at(0.25f + n * 0.5f + rip * 0.15f + g * 0.25f);
          if (hv(x, y, 79) > 0.985f) c = cmul(c, 0.72f);
          if (hv(x, y, 81) > 0.993f) c = c3(0xf8f2e6);
          HDT::put(p, c, rip * 0.4f + g * 0.25f); });
      t.light(0.04f, 0.3f); t.write(T_SAND); }
    Ramp sstR({0xb9a065, 0xc8ae73, 0xd5bc82, 0xe0c890, 0xe9d49f});
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) {
          float layer = v + (fbm(u, v, 3, 2, 31) - 0.5f) * 0.05f;
          float band = sinf(layer * 6.2831853f * 4.f) * 0.5f + 0.5f, n = fbm(u, v, 8, 4, 32);
          float h = band * 0.3f + n * 0.4f;
          C3 c = sstR.at(0.25f + band * 0.35f + (n - 0.5f) * 0.4f);
          if (v < 0.17f) { c = sstR.at(0.75f + n * 0.25f); h = 0.8f + n * 0.2f; }
          else if (v < 0.2f || fabsf(v - 0.78f) < 0.025f) { c = cmul(c, 0.72f); h = -0.2f; }
          if (fbm(u, v, 16, 2, 33) < 0.18f) { c = cmul(c, 0.85f); h -= 0.3f; }   // выветренные ямки
          HDT::put(p, c, h); });
      t.light(0.06f, 0.5f); t.write(T_SANDSTONE); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) {
          float n = fbm(u, v, 6, 4, 34), e = std::min(std::min(u, 1 - u), std::min(v, 1 - v));
          HDT::put(p, sstR.at(0.45f + n * 0.4f), clampf(e * 12.f, 0, 1) * 0.6f + n * 0.25f); });
      t.light(0.06f, 0.4f, false); t.write(T_SANDSTONE_TOP); }
    // дерево
    Ramp barkR({0x2a1b10, 0x3e2918, 0x523621, 0x66442a, 0x7a5434});
    Ramp woodR({0x8c6a3f, 0x9e7a4a, 0xb08a55, 0xc29b62, 0xd1aa70});
    Ramp pbarkR({0x1e130b, 0x2e1d11, 0x3e2818, 0x4e331f, 0x5e3e27});
    Ramp pwoodR({0x7a5530, 0x8a6238, 0x9c7443, 0xae844f, 0xbe925a});
    { HDT t = mk(); hdBark(t, 18, barkR); t.light(0.08f, 0.6f); t.write(T_LOG); }
    { HDT t = mk(); hdLogTop(t, 19, woodR, barkR); t.light(0.05f, 0.4f, false); t.write(T_LOG_TOP); }
    { HDT t = mk(); hdBark(t, 33, pbarkR); t.light(0.09f, 0.6f); t.write(T_PINE_LOG); }
    { HDT t = mk(); hdLogTop(t, 35, pwoodR, pbarkR); t.light(0.05f, 0.4f, false); t.write(T_PINE_TOP); }
    Ramp plankR({0x7d5a32, 0x906a3c, 0xa47b47, 0xb68b53, 0xc79b60});
    { HDT t = mk(); hdPlanks(t, 20, plankR, c3(0x3a2814)); t.light(0.05f, 0.5f); t.write(T_PLANKS); }
    // листва
    { HDT t = mk(); hdLeaves(t, 19, {0x1d3d15, 0x2a5a1f, 0x387329, 0x478c34, 0x5ba443, 0x74bc58}, false); t.light(0.06f, 0.5f); t.write(T_LEAVES); }
    { HDT t = mk(); hdLeaves(t, 34, {0x163322, 0x1f4730, 0x295c3e, 0x33704c, 0x40855c}, true); t.light(0.05f, 0.4f); t.write(T_PINE_LEAVES); }
    // руды
    { HDT t = mk(); hdOre(t, 21, c3(0x0a0a0c), c3(0x1c1c20), c3(0x3a3a42), c3(0x50505a), 5, 0.8f, true); t.light(0.08f, 0.55f); t.write(T_COAL_ORE); }
    { HDT t = mk(); hdOre(t, 22, c3(0x7a4a2c), c3(0xc08a62), c3(0xe2b892), c3(0xfff0de), 4, 0.7f, false); t.light(0.08f, 0.55f); t.write(T_IRON_ORE); }
    { HDT t = mk(); hdOre(t, 23, c3(0x8a5a08), c3(0xdca81c), c3(0xf8d84a), c3(0xfffbd0), 5, 0.55f, true); t.light(0.08f, 0.55f, true, 1.5f); t.write(T_GOLD_ORE); }
    { HDT t = mk(); hdOre(t, 24, c3(0x0c6a76), c3(0x2cbcca), c3(0x8cf0f4), c3(0xffffff), 4, 0.75f, true); t.light(0.08f, 0.55f, true, 1.6f); t.write(T_DIAMOND_ORE); }
    // кирпич, вулканит, металлы
    { HDT t = mk(); Ramp rp({0x6c6e73, 0x7a7c81, 0x888a8f, 0x96989c}); hdBrick(t, 43, 2, 2, rp, c3(0x3c3d41), 0.02f, 0.05f);
      Rng r(44); for (int i = 0; i < 1; i++) { float x = r.f() * R, y = r.f() * R, a = r.f() * 6.28f; for (int j = 0; j < 3; j++) { float nx = x + cosf(a) * k * 1.1f, ny = y + sinf(a) * k * 1.1f; t.groove(x, y, nx, ny, k * 0.2f, 0.25f, 0.4f); x = nx; y = ny; a += r.sf(); } }
      t.light(0.08f, 0.55f); t.write(T_STONEBRICK); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) {
          VR c = vor(u, v, 4, 44, 1.f); float f = clampf(c.f1 * 1.4f, 0, 1), n = fbm(u, v, 8, 3, 45);
          C3 col = cmix(c3(0x0a0612), c3(0x2a1a42), (1.f - f) * 0.6f + n * 0.3f);
          if (c.f2 - c.f1 < 0.04f) col = c3(0x5c3a9a);
          HDT::put(p, col, (1.f - f) * 0.8f); });
      t.light(0.08f, 0.4f, true, 1.8f); t.write(T_OBSIDIAN); }
    { HDT t = mk(); hdMetal(t, 45, Ramp({0x7e8086, 0x9fa1a7, 0xbec0c5, 0xd8dade, 0xf0f1f3}), 0); t.light(0.08f, 0.4f, false, 1.5f); t.write(T_IRON_BLOCK); }
    { HDT t = mk(); hdMetal(t, 46, Ramp({0x9a6c08, 0xc8941a, 0xe6b832, 0xf6d458, 0xfff0a8}), 1); t.light(0.08f, 0.4f, false, 1.5f); t.write(T_GOLD_BLOCK); }
    { HDT t = mk(); hdMetal(t, 47, Ramp({0x14808c, 0x2eb0bc, 0x5cd2da, 0x94e8ec, 0xd8fbfc}), 2); t.light(0.08f, 0.4f, false, 1.6f); t.write(T_DIAMOND_BLOCK); }
    // снег и лёд
    Ramp snowR({0xc9d6e4, 0xdbe4ee, 0xe9eff6, 0xf5f8fb, 0xffffff});
    { HDT t = mk(); t.each([&](int x, int y, float u, float v, float* p) {
          float n = fbm(u, v, 4, 5, 28);
          float drift = 0.5f + 0.5f * sinf((u * 0.6f + v + fbm(u, v, 3, 2, 27) * 0.35f) * 6.2831853f * 3.f);   // волны, наметённые ветром
          C3 c = snowR.at(0.4f + n * 0.45f + drift * 0.15f);
          if (drift < 0.25f) c = cmix(c, c3(0xb8cce6), (0.25f - drift) * 1.2f);   // голубые тени в ложбинках
          float g = hv(x, y, 29);
          if (g > 0.993f) c = c3(0xffffff); else if (g > 0.989f) c = c3(0xdff0ff);   // искрящиеся снежинки
          HDT::put(p, c, n * 0.6f + drift * 0.5f); });
      t.light(0.05f, 0.35f); t.write(T_SNOW); }
    { HDT t = mk(); hdDirt(t, 11); hdOverhang(t, snowR, 29, 0.22f, 0.3f, c3(0xb8c6d4), false); t.light(0.06f, 0.5f); t.write(T_SNOW_SIDE); }
    { HDT t = mk(); t.each([&](int x, int y, float u, float v, float* p) {
          float n = fbm(u, v, 3, 4, 30), cr = ridged(u, v, 3, 3, 31), cr2 = ridged(u + 0.37f, v + 0.11f, 5, 2, 33);
          float depth = fbm(u, v, 2, 3, 34);   // толща: глубже — синее
          C3 c = cmix(c3(0x5e8ed4), c3(0xcfe4fb), n * 0.55f + depth * 0.35f + v * 0.1f);
          if (cr > 0.86f) c = c3(0xeef6ff);                         // крупные трещины
          else if (cr2 > 0.9f) c = cmix(c, c3(0xe2eefc), 0.6f);     // тонкая сетка мелких
          if (hv(x / 2, y / 2, 32) > 0.996f) c = c3(0xffffff);
          HDT::put(p, c, cr > 0.86f ? -0.2f : n * 0.3f, 0.82f); });
      { Rng r(35); int R2 = t.R; float k2 = R2 / 16.f;
        for (int i = 0; i < 14; i++) { float bx = r.f() * R2, by = r.f() * R2, rr = k2 * (0.2f + r.f() * 0.45f);   // вмёрзшие пузырьки
            t.disc(bx, by, rr, c3(0xf4faff), 0.6f, 0.75f); t.disc(bx + rr * 0.3f, by + rr * 0.3f, rr * 0.55f, c3(0xa8c8ee), 0.3f, 0.6f); } }
      t.light(0.04f, 0.2f, true, 1.4f); t.write(T_ICE); }
    // кактус
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) {
          float rib = 0.5f + 0.5f * cosf(u * 6.2831853f * 4.f), n = fbm(u, v, 4, 3, 36, 0.5f, 8);
          HDT::put(p, cmix(c3(0x245a20), c3(0x5aa848), rib * 0.75f + n * 0.25f), rib); });
      Rng r(37); for (int i = 0; i < 4; i++) for (int j = 0; j < 5; j++) {
          float x = (i + 0.5f) / 4.f * R, y = (j + 0.3f + 0.4f * (i & 1)) / 5.f * R;
          t.disc(x, y, k * 0.35f, c3(0xd8d0a0), 1.4f);
          for (int sp = 0; sp < 3; sp++) { float a = -1.2f + sp * 1.2f + r.sf() * 0.3f; t.line(x, y, x + cosf(a) * k * 0.9f, y + sinf(a) * k * 0.9f, k * 0.12f, c3(0xf2ecd0), 1.6f); } }
      t.light(0.07f, 0.5f); t.write(T_CACTUS_SIDE); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) {
          float dx = u - 0.5f, dy = v - 0.5f, a = atan2f(dy, dx), rr = std::max(fabsf(dx), fabsf(dy));
          float rib = 0.5f + 0.5f * cosf(a * 8.f);
          C3 c = cmix(c3(0x245a20), c3(0x6cba56), rib * 0.6f + (0.5f - rr) * 0.8f);
          if (rr > 0.46f) c = c3(0x2a6226);
          HDT::put(p, c, rib * 0.5f + (0.5f - rr)); });
      t.disc(R * 0.5f, R * 0.5f, k * 0.6f, c3(0xe6dca8), 1.5f);
      t.light(0.06f, 0.4f, false); t.write(T_CACTUS_TOP); }
    // стекло
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) {
          float e = std::min(std::min(u, 1 - u), std::min(v, 1 - v));
          float n = fbm(u, v, 3, 3, 41);
          if (e < 0.045f) HDT::put(p, cmix(c3(0x9fc4d4), c3(0xeef8fc), (u < 0.5f && v < 0.5f) ? 0.8f : 0.3f), 1.f - e * 8.f, 0.95f);
          else HDT::put(p, cmix(c3(0xc4e2f0), c3(0xe8f6fc), n), 0.f, 0.14f + n * 0.06f); });
      for (int i = 0; i < 3; i++) { float o = 0.18f + i * 0.09f; t.line(R * o, R * (o + 0.14f), R * (o + 0.14f), R * o, k * (i == 1 ? 0.5f : 0.25f), c3(0xffffff), 0.f, 0.75f, false); }
      t.line(R * 0.62f, R * 0.84f, R * 0.84f, R * 0.62f, k * 0.3f, c3(0xffffff), 0.f, 0.6f, false);
      t.light(0.05f, 0.f, false); t.write(T_GLASS); }
    // растения (прозрачный фон)
    { HDT t = mk(); t.clearAlpha(); Rng r(50);
      for (int b = 0; b < 22; b++) {
          float x = (0.05f + r.f() * 0.9f) * R, h = (0.35f + r.f() * 0.6f) * R, lean = r.sf() * 0.35f, w = k * (0.35f + r.f() * 0.25f);
          float px = x, py = R - 0.5f;
          for (int s2 = 1; s2 <= 12; s2++) { float tt = s2 / 12.f; float nx = x + lean * h * tt * tt, ny = R - 0.5f - h * tt;
              t.line(px, py, nx, ny, w * (1.f - tt * 0.7f), cmix(c3(0x2f6a22), c3(0x8acf5c), tt), 0.5f + tt, 1.f, false); px = nx; py = ny; } }
      t.light(0.04f, 0.3f, false); t.write(T_TALLGRASS); }
    auto stem = [&](HDT& t, float x, float top, u32 seed) {
        Rng r(seed);
        t.line(x, R - 0.5f, x + r.sf() * k, top, k * 0.45f, c3(0x3d7a2c), 0.6f, 1.f, false);
        for (int i = 0; i < 2; i++) { float ly = R * (0.75f - i * 0.15f), dir = i ? -1.f : 1.f;
            t.line(x, ly, x + dir * k * 2.4f, ly - k * 1.6f, k * 0.6f, c3(0x4c9038), 0.8f, 1.f, false);
            t.line(x + dir * k * 0.5f, ly - k * 0.3f, x + dir * k * 2.0f, ly - k * 1.3f, k * 0.2f, c3(0x6cb24e), 1.f, 1.f, false); }
    };
    { HDT t = mk(); t.clearAlpha(); stem(t, R * 0.5f, R * 0.4f, 51);
      for (int i = 0; i < 6; i++) { float a = i * 1.0472f; t.disc(R * 0.5f + cosf(a) * k * 1.6f, R * 0.3f + sinf(a) * k * 1.6f, k * 1.35f, i < 3 ? c3(0xe8323c) : c3(0xc01e2a), 1.f, 1.f, true, false); }
      t.disc(R * 0.5f, R * 0.3f, k * 0.9f, c3(0xf2c230), 1.6f, 1.f, true, false);
      t.light(0.05f, 0.3f, false); t.write(T_FLOWER_R); }
    { HDT t = mk(); t.clearAlpha(); stem(t, R * 0.5f, R * 0.42f, 52);
      for (int i = 0; i < 18; i++) { float a = i * 0.349f; t.line(R * 0.5f, R * 0.3f, R * 0.5f + cosf(a) * k * 2.6f, R * 0.3f + sinf(a) * k * 2.6f, k * 0.55f, i % 2 ? c3(0xffe14a) : c3(0xf0c020), 1.f, 1.f, false); }
      t.disc(R * 0.5f, R * 0.3f, k * 1.f, c3(0xd88a1c), 1.5f, 1.f, true, false);
      t.light(0.05f, 0.3f, false); t.write(T_FLOWER_Y); }
    { HDT t = mk(); t.clearAlpha(); t.line(R * 0.48f, R - 0.5f, R * 0.5f, R * 0.45f, k * 0.6f, c3(0x6e4b2c), 0.6f, 1.f, false);
      Rng r(53); for (int i = 0; i < 14; i++) { float cx = R * (0.5f + r.sf() * 0.25f), cy = R * (0.32f + r.sf() * 0.2f); t.disc(cx, cy, k * (0.9f + r.f() * 0.6f), cmix(c3(0x2f6224), c3(0x7cc85a), r.f()), 1.f + r.f(), 1.f, true, false); }
      t.light(0.05f, 0.4f, false); t.write(T_SAPLING); }
    // верстак
    { HDT t = mk(); hdPlanks(t, 60, plankR, c3(0x3a2814));
      t.each([&](int, int, float u, float v, float* p) {
          float e = std::min(std::min(u, 1 - u), std::min(v, 1 - v));
          if (e < 0.06f) HDT::put(p, cmix(c3(0x3a2614), c3(0x5a3f22), e * 16.f), 1.f - e * 4.f);
          bool grid = (u > 0.2f && u < 0.8f && v > 0.2f && v < 0.8f) && (fabsf(u - 0.4f) < 0.008f || fabsf(u - 0.6f) < 0.008f || fabsf(v - 0.4f) < 0.008f || fabsf(v - 0.6f) < 0.008f || fabsf(u - 0.2f) < 0.008f || fabsf(u - 0.8f) < 0.008f || fabsf(v - 0.2f) < 0.008f || fabsf(v - 0.8f) < 0.008f);
          if (grid) { p[0] *= 0.55f; p[1] *= 0.55f; p[2] *= 0.55f; p[4] -= 0.3f; } });
      t.light(0.06f, 0.5f, false); t.write(T_CRAFT_TOP); }
    { HDT t = mk(); hdPlanks(t, 61, plankR, c3(0x3a2814));
      t.each([&](int, int, float u, float v, float* p) { if (v < 0.16f) { float n = fbm(u, v, 8, 3, 62); HDT::put(p, woodR.at(0.3f + n * 0.3f), 1.f - (v < 0.04f ? 0.f : 0.3f)); } });
      // молоток и пила
      t.line(R * 0.22f, R * 0.32f, R * 0.22f, R * 0.82f, k * 0.8f, c3(0x6e4b2c), 1.4f, 1.f, false);
      t.line(R * 0.12f, R * 0.32f, R * 0.34f, R * 0.32f, k * 1.6f, c3(0x8f9297), 1.8f, 1.f, false);
      t.line(R * 0.55f, R * 0.36f, R * 0.84f, R * 0.36f, k * 2.2f, c3(0xbfc3c8), 1.4f, 1.f, false);
      for (int i = 0; i < 7; i++) t.disc(R * (0.56f + i * 0.045f), R * 0.43f, k * 0.35f, c3(0x9a9ea3), 1.2f, 1.f, false, false);
      t.line(R * 0.85f, R * 0.36f, R * 0.88f, R * 0.6f, k * 1.f, c3(0x6e4b2c), 1.4f, 1.f, false);
      t.light(0.06f, 0.5f, false); t.write(T_CRAFT_SIDE); }
    // печь
    Ramp fstone({0x55575c, 0x67696e, 0x797b80, 0x8b8d92});
    { HDT t = mk(); hdBrick(t, 63, 4, 2, fstone, c3(0x2e2f33), 0.012f, 0.03f);
      t.each([&](int, int, float, float v, float* p) { if (v < 0.06f || v > 0.94f) { HDT::put(p, v < 0.5f ? c3(0x9fa1a5) : c3(0x3c3d42), 1.f); } });
      t.light(0.07f, 0.55f, false); t.write(T_FURN_SIDE); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) { float n = fbm(u, v, 6, 4, 64), e = std::min(std::min(u, 1 - u), std::min(v, 1 - v));
          float ring = fabsf(e - 0.2f) < 0.02f ? 1.f : 0.f;
          HDT::put(p, fstone.at(0.4f + n * 0.5f - ring * 0.4f), clampf(e * 14.f, 0, 1) - ring * 0.6f + n * 0.2f); });
      t.light(0.07f, 0.5f, false); t.write(T_FURN_TOP); }
    auto furnFront = [&](bool lit) {
        HDT t = mk(); hdBrick(t, 63, 4, 2, fstone, c3(0x2e2f33), 0.012f, 0.03f);
        Ramp fire({0x7a1e06, 0xc8400c, 0xf27a18, 0xffb02e, 0xffe27a, 0xfff6c8});
        t.each([&](int, int, float u, float v, float* p) {
            if (v < 0.06f || v > 0.94f) { HDT::put(p, v < 0.5f ? c3(0x9fa1a5) : c3(0x3c3d42), 1.f); return; }
            float ax = (u - 0.5f) / 0.32f, top = 0.38f;
            bool arch = fabsf(ax) < 1.f && v > top - 0.12f * sqrtf(std::max(0.f, 1.f - ax * ax)) && v < 0.86f;
            if (!arch) { float ad = fabsf(ax) - 1.f; if (fabsf(ax) < 1.12f && v > 0.24f && v < 0.88f && ad < 0.12f) { p[4] = 1.3f; p[0] *= 1.1f; p[1] *= 1.1f; p[2] *= 1.1f; } return; }
            C3 c = c3(0x0c0c0e); float h = -0.8f;
            if (lit) { float n = fbm(u, v, 6, 3, 66); float heat = clampf((v - 0.45f) * 2.2f + n * 0.5f - 0.2f, 0, 1); c = fire.at(heat); h = -0.4f; }
            bool bar = v > 0.72f && fmodf(u * 10.f, 1.f) < 0.28f;
            if (bar) { c = lit ? c3(0x5a3a2a) : c3(0x3a3b40); h = -0.2f; }
            HDT::put(p, c, h);
        });
        t.light(0.07f, 0.55f, false);
        return t;
    };
    { HDT t = furnFront(false); t.write(T_FURN_FRONT); }
    { HDT t = furnFront(true); t.write(T_FURN_FRONT_LIT); }
    // сундук
    Ramp chestR({0x6e461e, 0x7f5226, 0x90602e, 0xa06c36, 0xae783e});
    auto chestBase = [&](u32 seed, bool front, bool top) {
        HDT t = mk(); hdPlanks(t, seed, chestR, c3(0x2e1c0a));
        t.each([&](int, int, float u, float v, float* p) {
            float e = std::min(std::min(u, 1 - u), std::min(v, 1 - v));
            if (e < 0.055f) { HDT::put(p, cmix(c3(0x3a3b40), c3(0x75777d), (u < 0.5f && v < 0.5f) ? 0.7f : 0.3f), 1.4f); return; }
            if (!top && fabsf(v - 0.37f) < 0.035f) HDT::put(p, c3(0x22160a), -0.5f);
        });
        const float cn[4][2] = {{0.03f, 0.03f}, {0.97f, 0.03f}, {0.03f, 0.97f}, {0.97f, 0.97f}};
        for (auto& q : cn) t.disc(q[0] * R, q[1] * R, k * 0.45f, c3(0xb8babf), 2.f, 1.f, true, false);
        if (front) {
            t.each([&](int, int, float u, float v, float* p) { if (fabsf(u - 0.5f) < 0.09f && v > 0.27f && v < 0.52f) { float e2 = std::min(0.09f - fabsf(u - 0.5f), std::min(v - 0.27f, 0.52f - v)); HDT::put(p, cmix(c3(0x9a7414), c3(0xf2d066), clampf(e2 * 25.f, 0, 1)), 1.6f + e2 * 6.f); } });
            t.disc(R * 0.5f, R * 0.42f, k * 0.35f, c3(0x1a1206), 0.f, 1.f, false, false);
        }
        t.light(0.07f, 0.5f, false);
        return t;
    };
    { HDT t = chestBase(70, false, false); t.write(T_CHEST_SIDE); }
    { HDT t = chestBase(71, false, true); t.write(T_CHEST_TOP); }
    { HDT t = chestBase(70, true, false); t.write(T_CHEST_FRONT); }
    // дверь
    Ramp doorR({0x6e4f2c, 0x7d5a32, 0x8f683b, 0xa07645, 0xb0844f});
    auto doorPart = [&](bool upper) {
        HDT t = mk();
        t.each([&](int, int, float u, float v, float* p) {
            float g = fbm(u, v, 2, 4, 80, 0.55f, 20), n = pn(u, v, 6, 80, 81);
            float gg = sinf(g * 18.f) * 0.5f + 0.5f;
            C3 c = doorR.at(0.3f + gg * 0.3f + (n - 0.5f) * 0.2f);
            float h = 0.6f;
            bool frame = u < 0.12f || u > 0.88f || (upper ? v < 0.1f : v > 0.9f);
            float pu0 = 0.22f, pu1 = 0.78f, pv0 = upper ? 0.2f : 0.1f, pv1 = upper ? 0.85f : 0.72f;
            if (!frame && u > pu0 && u < pu1 && v > pv0 && v < pv1) {
                float e = std::min(std::min(u - pu0, pu1 - u), std::min(v - pv0, pv1 - v));
                h = 0.6f - clampf(e * 20.f, 0, 1) * 0.5f;
                if (upper) {   // окно 2x2
                    float wu = (u - 0.26f) / 0.48f, wv = (v - 0.25f) / 0.55f;
                    if (wu > 0 && wu < 1 && wv > 0 && wv < 1 && fabsf(wu - 0.5f) > 0.05f && fabsf(wv - 0.5f) > 0.05f) {
                        float shine = (fmodf(wu + wv * 0.6f, 0.5f) < 0.08f) ? 0.4f : 0.f;
                        c = cmix(c3(0x8ab8d2), c3(0xd8eef8), clampf(1.f - wv + shine, 0, 1)); h = -0.3f;
                    }
                }
            }
            if (frame) { c = cmul(c, 0.85f); h = 0.8f; }
            HDT::put(p, c, h);
        });
        if (!upper) { t.disc(R * 0.82f, R * 0.4f, k * 0.6f, c3(0xa0a2a6), 2.f, 1.f, true, false); t.disc(R * 0.82f, R * 0.4f, k * 0.25f, c3(0xe0e2e6), 2.4f, 1.f, true, false); }
        t.light(0.07f, 0.5f, false);
        return t;
    };
    { HDT t = doorPart(false); t.write(T_DOOR_LO); }
    { HDT t = doorPart(true); t.write(T_DOOR_HI); }
    // лестница
    { HDT t = mk(); t.clearAlpha();
      auto wood = [&](float x0, float y0, float x1, float y1, float w) { t.line(x0, y0, x1, y1, w, c3(0x9a7044), 1.f, 1.f, false); t.line(x0 - w * 0.2f, y0, x1 - w * 0.2f, y1, w * 0.3f, c3(0xb88a56), 1.4f, 1.f, false); };
      wood(R * 0.17f, 0, R * 0.17f, (float)R, k * 1.6f); wood(R * 0.83f, 0, R * 0.83f, (float)R, k * 1.6f);
      for (int i = 0; i < 4; i++) { float y = (i + 0.4f) * R / 4; wood(R * 0.12f, y, R * 0.88f, y, k * 1.3f);
          t.disc(R * 0.17f, y, k * 0.35f, c3(0x4a4b50), 2.f, 1.f, true, false); t.disc(R * 0.83f, y, k * 0.35f, c3(0x4a4b50), 2.f, 1.f, true, false); }
      t.light(0.06f, 0.5f, false); t.write(T_LADDER); }
    // кровать (сбоку занята нижняя часть 9/16)
    auto bedTop = [&](bool head) {
        HDT t = mk(); hdWool(t, 90, c3(0xc93139), c3(0x8a1a22));
        t.each([&](int, int, float u, float v, float* p) {
            if (u < 0.05f || u > 0.95f) { HDT::put(p, c3(0x8a1a22), 0.3f); return; }
            if (fmodf(u * 4.f, 1.f) < 0.04f || fmodf(v * 4.f, 1.f) < 0.04f) { p[0] *= 0.8f; p[1] *= 0.8f; p[2] *= 0.8f; p[4] -= 0.2f; }   // стёжка
            if (head && v < 0.38f) { float pu = (u - 0.5f) / 0.42f, pv = (v - 0.19f) / 0.15f; float e = pu * pu + pv * pv;
                if (e < 1.f) HDT::put(p, cmix(c3(0xd8d4c8), c3(0xfaf8f0), sqrtf(1.f - e)), 1.f + sqrtf(1.f - e)); else if (v < 0.4f) HDT::put(p, c3(0xe6e2d6), 0.6f); }
        });
        t.light(0.07f, 0.5f, false);
        return t;
    };
    { HDT t = bedTop(false); t.write(T_BED_FOOT); }
    { HDT t = bedTop(true); t.write(T_BED_HEAD); }
    auto bedSide = [&](bool head) {
        HDT t = mk(); t.clearAlpha();
        t.each([&](int, int, float u, float v, float* p) {
            if (v < 7.f / 16.f) return;
            float n = fbm(u, v, 8, 3, 91);
            if (v < 0.5f) HDT::put(p, head && u < 0.38f ? c3(0xf4f1e8) : c3(0xe8e4d8), 0.8f);
            else if (v < 0.72f) HDT::put(p, cmix(c3(0x9a1e28), c3(0xc8303a), n), 0.7f + n * 0.3f);
            else if (v < 0.78f) HDT::put(p, c3(0x7a1820), 0.3f);
            else if (v < 0.86f) HDT::put(p, plankR.at(0.6f + n * 0.3f), 1.f);
            else HDT::put(p, (u < 0.18f || u > 0.82f) ? plankR.at(0.3f + n * 0.2f) : c3(0x1e140c), (u < 0.18f || u > 0.82f) ? 1.f : -0.5f);
        });
        t.light(0.06f, 0.5f, false);
        return t;
    };
    { HDT t = bedSide(false); t.write(T_BED_SIDE); }
    { HDT t = bedSide(true); t.write(T_BED_SIDE_HEAD); }
    // наковальня: тёмный кованый металл с царапинами и следами ударов
    { HDT t = mk(); hdMetal(t, 230, Ramp({0x1e1f23, 0x2a2b30, 0x36373d, 0x44454c, 0x55565e}), 0);
      Rng r(231); for (int i = 0; i < 9; i++) t.disc(r.f() * R, r.f() * R, k * (0.4f + r.f() * 0.5f), c3(0x2a2b30), -0.2f, 0.6f, true, false);
      t.light(0.08f, 0.5f, false, 1.4f); t.write(T_ANVIL_SIDE); }
    { HDT t = mk(); t.each([&](int, int, float u, float v, float* p) { float br = pn(u, v, 128, 3, 232), n = fbm(u, v, 4, 3, 233);
          HDT::put(p, cmix(c3(0x34353b), c3(0x5c5d66), br * 0.5f + n * 0.4f), 0.5f + n * 0.2f); });
      Rng r(234); for (int i = 0; i < 14; i++) { float x = r.f() * R, y = r.f() * R, a = r.sf() * 0.5f, L = k * (1.f + r.f() * 3.f);
          t.line(x, y, x + cosf(a) * L, y + sinf(a) * L, k * 0.12f, c3(0x8a8c94), 0.6f, 0.8f, false); }
      t.each([&](int, int, float u, float v, float* p) { float e = std::min(std::min(u, 1 - u), std::min(v, 1 - v)); if (e < 0.05f) { p[0] *= 1.25f; p[1] *= 1.25f; p[2] *= 1.25f; p[4] -= (0.05f - e) * 10.f; } });
      t.light(0.07f, 0.4f, false, 1.5f); t.write(T_ANVIL_TOP); }
    // трещины при копании
    for (int st = 0; st < 10; st++) {
        HDT t = mk(); t.clearAlpha();
        float lim = (st + 1) / 10.f;
        for (const CrackSeg& g : crackNet()) if (g.t <= lim) {
            float age = (lim - g.t) * 4.f;   // старые трещины шире и темнее, свежие — тонкие
            float w = k * (0.25f + 0.35f * std::min(1.f, age)) * (0.6f + 0.4f * g.w);
            t.line(g.x0 * R, g.y0 * R, g.x1 * R, g.y1 * R, w, c3(0x0e0e0e), 0.f, 0.6f + 0.3f * std::min(1.f, age), false);
        }
        t.write(crackTile(st));
    }
}
// анимированные вода и лава в высоком разрешении
static void genHDAnim() {
    const int R = g_tileRes;
    Ramp wR({0x1e46b0, 0x2756c4, 0x3066d2, 0x3c78de, 0x4c8ae8, 0x7ab0f2});
    Ramp fR({0x9aa8bc, 0xaab6c8, 0xbcc6d6, 0xced6e2, 0xe2e8f0, 0xf6f8fb});
    Ramp lR({0x5a1404, 0x8f1f08, 0xc8380c, 0xf06a14, 0xfba52a, 0xffd04a, 0xfff2a8});
    for (int f = 0; f < 16; f++) {
        float sh = f / 16.f;
        HDT w(R), fx(R), lv(R);
        for (int y = 0; y < R; y++) for (int x = 0; x < R; x++) {
            float u = (x + 0.5f) / R, v = (y + 0.5f) / R;
            float n1 = fbm(u + sh, v, 3, 3, 26), n2 = fbm(u, v + sh, 3, 3, 27);
            float caust = 1.f - fabsf(n1 - n2) * 6.f; caust = clampf(caust, 0, 1); caust = caust * caust * caust;
            float base = fbm(u - sh * 0.5f, v + sh * 0.5f, 2, 3, 28);
            HDT::put(w.at(x, y), wR.at(0.25f + base * 0.4f + caust * 0.45f), 0.f);
            HDT::put(fx.at(x, y), fR.at(0.3f + base * 0.35f + caust * 0.5f), 0.f);
            VR c = vor(u + sh, v + sh * 0.5f, 4, 29, 0.9f);
            float crack = clampf(1.f - (c.f2 - c.f1) * 5.f, 0, 1);
            float heat = clampf(crack * 1.1f + fbm(u - sh, v, 4, 3, 30) * 0.35f - 0.1f, 0, 1);
            HDT::put(lv.at(x, y), lR.at(heat), 0.f);
        }
        w.toVec(g_hdAnim[0][f]); fx.toVec(g_hdAnim[1][f]); lv.toVec(g_hdAnim[2][f]);
    }
}
// прозрачные пиксели получают цвет соседей — без тёмной каймы при сглаживании
static void dilateAtlas() {
    const int R = g_tileRes, S = g_hdSize, SH = g_hdH;
    std::vector<u32> src;
    for (int pass = 0; pass < 4; pass++) {
        src = g_hdAtlas;
        for (int y = 0; y < SH; y++) for (int x = 0; x < S; x++) {
            u32 c = src[(size_t)y * S + x];
            if (pA(c) > 8) continue;
            int tx0 = x / R * R, ty0 = y / R * R, rr = 0, gg = 0, bb = 0, n = 0;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy;
                if (nx < tx0 || ny < ty0 || nx >= tx0 + R || ny >= ty0 + R) continue;
                u32 q = src[(size_t)ny * S + nx];
                if (pA(q) > 8 || (pass > 0 && (pR(q) | pG(q) | pB(q)))) { rr += pR(q); gg += pG(q); bb += pB(q); n++; }
            }
            if (n) g_hdAtlas[(size_t)y * S + x] = RGBA(rr / n, gg / n, bb / n, pA(c));
        }
    }
}
// ---------------------------------------------------------------- рыбы и удочка: процедурный рисовальщик (любое разрешение)
// вид рыбы сбоку, голова слева; координаты в долях полной длины рыбы (тело + хвост)
struct FishFin { float t0, t1, h, slant; int shape; };   // shape: 0 — мягкий, 1 — колючий, 2 — округлый (жировой), 3 — длинный низкий
struct FishSpec {
    const char* raw; const char* cooked;
    float Lb; float top[7], bot[7];
    FishFin dors[2]; int nd; FishFin anal, pelv;
    float tailH, fork; int tailRound;
    float eyeT, eyeV, eyeR; u32 iris;
    u32 dorsal, side, belly, finUp, finLow, finEdge;
    int pattern; float scaleK, scaleSz;
    float iconAng, iconLen;
    float len3d;   // длина в мире, блоков
};
static const float FISH_T[7] = {0.f, 0.05f, 0.15f, 0.35f, 0.6f, 0.85f, 1.f};
static const FishSpec FISH[4] = {
    {"СЫРОЙ ОКУНЬ", "ЖАРЕНЫЙ ОКУНЬ", 0.80f, {0, .062f, .118f, .172f, .148f, .07f, .052f}, {0, .05f, .09f, .126f, .106f, .06f, .05f},
     {{0.29f, 0.56f, 0.15f, 0.25f, 1}, {0.58f, 0.77f, 0.095f, 0.3f, 0}}, 2, {0.66f, 0.79f, 0.08f, 0.45f, 0}, {0.30f, 0.40f, 0.085f, 0.6f, 0},
     0.15f, 0.36f, 0, 0.095f, 0.36f, 0.036f, 0xf0c434,
     0x2f4a1c, 0x93a845, 0xf0e8c4, 0x7d8456, 0xe85a2a, 0x3a2a14, 0, 0.2f, 0.05f, 0.72f, 1.1f, 0.5f},
    {"СЫРОЙ КАРАСЬ", "ЖАРЕНЫЙ КАРАСЬ", 0.80f, {0, .078f, .165f, .24f, .205f, .088f, .062f}, {0, .062f, .124f, .175f, .148f, .076f, .06f},
     {{0.35f, 0.75f, 0.11f, 0.15f, 3}, {0, 0, 0, 0, 0}}, 1, {0.68f, 0.79f, 0.08f, 0.4f, 0}, {0.36f, 0.46f, 0.08f, 0.55f, 0},
     0.17f, 0.26f, 0, 0.1f, 0.3f, 0.037f, 0xe0b448,
     0x4e3c14, 0xc9952c, 0xf4de98, 0x9c5a2c, 0xc06a34, 0x40220e, 1, 0.15f, 0.05f, 0.66f, 0.98f, 0.42f},
    {"СЫРАЯ ФОРЕЛЬ", "ЖАРЕНАЯ ФОРЕЛЬ", 0.82f, {0, .046f, .082f, .11f, .1f, .053f, .044f}, {0, .041f, .075f, .1f, .09f, .05f, .044f},
     {{0.38f, 0.53f, 0.095f, 0.35f, 0}, {0.80f, 0.86f, 0.035f, 0.1f, 2}}, 2, {0.68f, 0.79f, 0.065f, 0.45f, 0}, {0.46f, 0.55f, 0.06f, 0.6f, 0},
     0.12f, 0.12f, 0, 0.08f, 0.3f, 0.031f, 0xd0c8a4,
     0x3c5a48, 0xbcc2b0, 0xf2f0e6, 0x7c7e64, 0x9a8a6c, 0x2c2c22, 2, 0.12f, 0.035f, 0.76f, 1.2f, 0.6f},
    {"СЫРАЯ ЩУКА", "ЖАРЕНАЯ ЩУКА", 0.84f, {0, .022f, .042f, .061f, .071f, .053f, .043f}, {0, .025f, .047f, .065f, .069f, .051f, .043f},
     {{0.76f, 0.905f, 0.07f, 0.3f, 0}, {0, 0, 0, 0, 0}}, 1, {0.77f, 0.905f, 0.065f, 0.3f, 0}, {0.55f, 0.62f, 0.05f, 0.6f, 0},
     0.105f, 0.3f, 0, 0.1f, 0.5f, 0.026f, 0xe0c858,
     0x22381a, 0x5d7c33, 0xeeead2, 0xa8642e, 0xb8702e, 0x2a1a0c, 3, 0.1f, 0.03f, 0.785f, 1.32f, 0.95f},
};
static float fishProf(const float* a, float t) {   // полувысота тела в точке t (0 — нос, 1 — хвостовой стебель)
    t = clampf(t, 0.f, 1.f);
    if (t < FISH_T[1]) return a[1] * sqrtf(t / FISH_T[1]);   // округлая морда
    int i = 1; while (i < 5 && t > FISH_T[i + 1]) i++;
    float k = (t - FISH_T[i]) / (FISH_T[i + 1] - FISH_T[i]); k = k * k * (3 - 2 * k);
    return lerpf(a[i], a[i + 1], k);
}
static inline float sstep(float a, float b, float x) { return smooth01(a, b, x); }
static float g_fishPx = 1.f / 16;   // размер пикселя рисуемого тайла (чтобы глаз не пропал на 16x16)
// форма плавника: u — вдоль основания 0..1, возвращает высоту 0..1
static float finShape(int shape, float u) {
    if (u < 0 || u > 1) return 0;
    switch (shape) {
        case 1: { float s = powf(sinf(3.14159f * powf(u, 0.55f)), 0.6f) * (1.f - 0.25f * u);   // колючий: зубцы между лучами
                  float sp = fabsf(cosf(u * 3.14159f * 6.5f)); return s * (0.72f + 0.28f * sp * sp); }
        case 2: return sqrtf(std::max(0.f, 1.f - (2 * u - 1) * (2 * u - 1)));
        case 3: { float s = u < 0.12f ? u / 0.12f : 1.f - 0.55f * (u - 0.12f); return s * (u > 0.93f ? (1 - u) / 0.07f : 1.f); }
        default: return powf(sinf(3.14159f * powf(u, 0.5f)), 0.8f) * (1.f - 0.3f * u);
    }
}
// цвет чешуи тела; X, Y — координаты в длинах рыбы, v — 1 спина .. -1 брюхо, t — 0..1 вдоль тела
static C3 fishBody(const FishSpec& S, int sp, float t, float v, float X, float Y, bool cooked, bool flat) {
    C3 dor = c3(S.dorsal), sid = c3(S.side), bel = c3(S.belly);
    if (cooked) { dor = c3(0x6e3812); sid = c3(0xb46c28); bel = c3(0xe2b26a); }
    C3 c = cmix(bel, sid, sstep(-0.75f, -0.05f, v));
    c = cmix(c, dor, sstep(0.15f, 0.9f, v) * (cooked ? 0.7f : 1.f));
    float pat = cooked ? 0.35f : 1.f;
    switch (S.pattern) {
        case 0: {   // окунь: тёмные поперечные полосы, шире у спины
            if (t > 0.2f && t < 0.9f) {
                float q = (t - 0.2f) / 0.7f * 6.f, f = q - floorf(q) - 0.5f;
                float w = 0.17f + 0.13f * sstep(-0.4f, 0.9f, v);
                float bar = (1.f - sstep(w * 0.6f, w, fabsf(f + 0.06f * v))) * sstep(-0.55f, -0.1f, v);
                c = cmix(c, cooked ? c3(0x4a2408) : c3(0x22301a), bar * 0.75f * pat);
            }
            break;
        }
        case 1: {   // карась: золото с бронзой, отливает
            float g = (fbm(X * 0.5f + 0.5f, Y * 0.5f + 0.5f, 16, 2, 77) - 0.5f) * 0.25f;
            c = cmix(c, cooked ? c3(0xd09040) : c3(0xe6b850), sstep(0.2f, -0.4f, v) * 0.4f + g);
            break;
        }
        case 2: {   // форель: розовая полоса, чёрные и красные пятнышки
            float bandV = 0.05f - 0.1f * t;
            float b = 1.f - sstep(0.12f, 0.3f, fabsf(v - bandV));
            if (t > 0.15f) c = cmix(c, cooked ? c3(0xc87a50) : c3(0xdc7a8c), b * 0.75f);
            float sc = 0.028f; int ix = ifloor(X / sc), iy = ifloor(Y / sc);
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                u32 h = hash3i(ix + dx, iy + dy, sp, 991);
                if ((h & 7) > 2) continue;
                float cx = (ix + dx + 0.5f + ((h >> 4) & 255) / 255.f * 0.6f - 0.3f) * sc, cy = (iy + dy + 0.5f + ((h >> 12) & 255) / 255.f * 0.6f - 0.3f) * sc;
                float d = sqrtf((X - cx) * (X - cx) + (Y - cy) * (Y - cy)), r = sc * (0.22f + ((h >> 20) & 15) / 15.f * 0.12f);
                float tv = v; bool red = ((h >> 24) & 7) < 2 && fabsf(tv - bandV) < 0.35f;
                if (tv < -0.35f || t < 0.12f) continue;
                if (red) { if (d < r * 1.7f) c = cmix(c, c3(0xf4f0e0), 0.7f * pat); if (d < r) c = cmix(c, cooked ? c3(0x8a3a1a) : c3(0xd8342a), pat); }
                else if (d < r) c = cmix(c, cooked ? c3(0x3a1a08) : c3(0x16140e), 0.9f * pat);
            }
            break;
        }
        case 3: {   // щука: светлые продолговатые пятна рядами
            if (t > 0.18f && v > -0.55f) {
                float sx = 0.045f, sy = 0.022f; int ix = ifloor(X / sx), iy = ifloor(Y / sy);
                float off = (iy & 1) ? 0.5f : 0.f; ix = ifloor(X / sx - off);
                u32 h = hash3i(ix, iy, 3, 515);
                float cx = (ix + off + 0.5f + ((h & 255) / 255.f - 0.5f) * 0.3f) * sx, cy = (iy + 0.5f) * sy;
                float dx = (X - cx) / (sx * 0.34f), dy = (Y - cy) / (sy * 0.36f);
                float d = dx * dx + dy * dy;
                if (d < 1.f && (h >> 9 & 3)) c = cmix(c, cooked ? c3(0xd8a060) : c3(0xd6d88a), (1.f - sstep(0.5f, 1.f, d)) * 0.85f * pat);
            }
            break;
        }
    }
    // чешуя: дуги, обращённые к хвосту
    if (t > 0.2f && S.scaleK > 0) {
        float s = S.scaleSz; float gy = Y / (s * 0.75f); int row = ifloor(gy);
        float gx = X / s - ((row & 1) ? 0.5f : 0.f); int col = ifloor(gx);
        float fx = gx - col, fy = gy - row;
        float d = sqrtf((fx - 0.05f) * (fx - 0.05f) * 0.8f + (fy - 0.5f) * (fy - 0.5f));
        float edge = sstep(0.42f, 0.56f, d) * sstep(0.2f, 0.6f, fx);
        float shine = (1.f - sstep(0.f, 0.35f, d)) * 0.25f;
        float k = S.scaleK * sstep(0.2f, 0.3f, t) * (cooked ? 0.5f : 1.f);
        c = cmul(c, 1.f - edge * k + shine * k * 0.6f);
    }
    // боковая линия
    {
        float lv = 0.42f - 0.25f * t;
        if (t > 0.2f && fabsf(v - lv) < 0.035f && (cooked || fmodf(X * 140.f, 1.f) < 0.6f)) c = cmix(c, cooked ? c3(0x5a2a0a) : cmix(sid, c3(0xffffff), 0.35f), 0.55f);
    }
    // жаберная крышка: дуга и чуть другой оттенок головы
    {
        float gT = S.eyeT * 2.25f + 0.02f;
        float gx = gT + 0.045f * (1.f - v * v) - 0.01f;   // дуга выгнута к хвосту
        float dx = t - gx;
        if (dx < 0) c = cmix(c, cooked ? c3(0x8a4a18) : cmix(sid, c3(0xd8b860), S.pattern == 1 ? 0.35f : 0.15f), 0.35f);
        if (fabsf(dx) < 0.014f && fabsf(v) < 0.85f) c = cmul(c, 0.55f);
        else if (dx > 0 && dx < 0.03f && fabsf(v) < 0.85f) c = cmul(c, 1.12f);
    }
    // жареная: следы решётки и масляный блеск, кое-где подгорело
    if (cooked) {
        float g = (X * 0.8f + Y) * 9.f; float f = g - floorf(g);
        if (f < 0.22f && t > 0.12f && t < 0.95f) c = cmix(c, c3(0x2e1406), 0.8f);
        float burn = fbm(X * 3.f, Y * 3.f, 4, 3, 41) ; c = cmul(c, 0.85f + burn * 0.3f);
    }
    // объём: затемнение к краям, блик выше середины
    if (!flat) {
        float rim = sqrtf(std::max(0.f, 1.f - v * v));
        c = cmul(c, 0.55f + 0.5f * rim);
        float hl = expf(-((v - 0.48f) / 0.16f) * ((v - 0.48f) / 0.16f)) * (cooked ? 0.35f : 0.25f) * sstep(0.05f, 0.25f, t);
        c = cmix(c, c3(0xffffff), hl);
        if (fabsf(v) > 0.9f) c = cmul(c, 0.72f);
    } else {
        float rim = sqrtf(std::max(0.f, 1.f - v * v));
        c = cmul(c, 0.82f + 0.2f * rim);
    }
    return c;
}
// плавник: along — вдоль основания, d — 0 у основания .. 1 на краю
static C3 fishFinCol(const FishSpec& S, bool upper, float along, float d, bool cooked, bool spotOK) {
    C3 base = c3(upper ? S.finUp : S.finLow);
    if (cooked) base = cmix(base, c3(0x4a2408), 0.6f);
    C3 c = cmix(base, c3(S.finEdge), sstep(0.55f, 1.05f, d) * 0.6f);
    float ray = fabsf(fmodf(along * 14.f + d * 2.2f, 1.f) - 0.5f);
    c = cmul(c, 0.78f + 0.3f * sstep(0.05f, 0.3f, ray));   // лучи плавника
    if (spotOK && S.pattern == 3) { u32 h = hash3i(ifloor(along * 8.f), ifloor(d * 4.f), 7, 31); if ((h & 3) == 0) c = cmul(c, 0.6f); }
    return c;
}
// хвост: s 0..1 от основания, a — -1 верх .. 1 низ; возвращает покрытие
static float fishTailCover(const FishSpec& S, float s, float a, float ped) {
    if (s < 0 || s > 1) return 0;
    float span = ped + (1.f - ped) * powf(s, 0.7f);
    if (fabsf(a) > span) return 0;
    float aa = fabsf(a);
    float smax = S.tailRound ? 1.f - 0.3f * aa * aa : 1.f - S.fork * powf(1.f - aa, 1.4f) - 0.06f * powf(aa, 6.f);
    return s <= smax ? 1.f : 0.f;
}
// kind: 0 — сырая (значок), 1 — жареная (значок), 2 — кожа для 3D, 3 — спинной плавник, 4 — хвост
static void fishSample(int kind, int sp, float x, float y, float* out) {
    const FishSpec& S = FISH[sp];
    out[0] = out[1] = out[2] = out[3] = 0;
    bool cooked = kind == 1;
    if (kind == 2) {   // развёртка кожи: x — вдоль тела, y — спина..брюхо
        float t = x, v = 1.f - 2.f * y;
        float X = t * S.Lb, Y = v > 0 ? -v * fishProf(S.top, t) : -v * fishProf(S.bot, t);
        C3 c = fishBody(S, sp, t, v, X, Y, false, true);
        if (t < 0.035f && fabsf(v + 0.1f) < 0.25f) c = cmul(c, 0.5f);   // рот
        out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = 1; return;
    }
    if (kind == 3) {   // спинные плавники в своей рамке: x — от начала первого до конца последнего, y — 0 край .. 1 основание
        float a0 = S.dors[0].t0, a1 = S.dors[S.nd - 1].t1;
        float t = a0 + x * (a1 - a0), hmax = 0; for (int i = 0; i < S.nd; i++) hmax = std::max(hmax, S.dors[i].h);
        float d = (1.f - y) * hmax * 1.05f;   // высота над основанием
        for (int i = 0; i < S.nd; i++) {
            const FishFin& F = S.dors[i];
            float u = (t - F.t0 - (d / F.h) * F.slant * (F.t1 - F.t0) * 0.5f) / (F.t1 - F.t0);
            float fh = F.h * finShape(F.shape, u);
            if (d <= fh) {
                C3 c = fishFinCol(S, true, u, d / F.h, false, true);
                if (sp == 0 && i == 0 && u > 0.62f && d / F.h > 0.25f) c = cmix(c, c3(0x14140c), 0.8f);   // чёрное пятно окуня
                out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = 0.93f; return;
            }
        }
        return;
    }
    if (kind == 4) {   // хвост: x — от стебля к краю, y — верх..низ
        float ped = (fishProf(S.top, 1.f) + fishProf(S.bot, 1.f)) * 0.5f / S.tailH;
        float a = y * 2.f - 1.f;
        if (fishTailCover(S, x, a, ped) > 0) {
            C3 c = fishFinCol(S, false, (atan2f(a, x + 0.25f) + 1.6f) * 0.9f, x, false, true);
            if (S.pattern == 0) c = cmix(c, c3(0x9a8a5a), 0.45f);
            out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = 0.95f;
        }
        return;
    }
    // значок: поворот и масштаб
    float ca = cosf(S.iconAng), sa = sinf(S.iconAng);
    float px = x - 0.5f, py = y - 0.5f;
    float qx = px * ca - py * sa, qy = px * sa + py * ca;   // в систему рыбы (голова влево-вверх)
    float L = S.iconLen;
    float X = qx / L + 0.5f, Y = qy / L;
    float yOff = (fishProf(S.bot, 0.4f) - fishProf(S.top, 0.4f)) * 0.5f;   // центрируем по высоте
    Y -= yOff;
    float t = X / S.Lb;
    float finK = cooked ? 0.6f : 1.f;
    // тело
    if (t >= 0 && t <= 1.f) {
        float ht = fishProf(S.top, t), hb = fishProf(S.bot, t);
        if (Y >= -ht && Y <= hb) {
            float v = Y < 0 ? -Y / std::max(ht, 1e-4f) : -Y / std::max(hb, 1e-4f);
            C3 c = fishBody(S, sp, t, v, X, Y, cooked, false);
            // грудной плавник поверх тела
            {
                float pt0 = S.eyeT * 2.25f + 0.05f, pl = 0.11f;
                float u = (t - pt0) / pl, w = (v + 0.25f);
                float sh = u > 0 && u < 1 ? (1 - u) * 0.35f * sinf(3.14159f * sqrtf(u)) : 0;
                if (fabsf(w + u * 0.25f) < sh) {
                    C3 f = fishFinCol(S, false, w * 2.f, u, cooked, false);
                    c = cmix(c, f, cooked ? 0.6f : 0.75f);
                }
            }
            // рот
            float mY = (S.pattern == 3 ? 0.0f : 0.15f) * hb;
            if (X < S.Lb * (S.pattern == 3 ? 0.11f : 0.045f) && fabsf(Y - mY) < 0.006f + 0.4f / (16.f * L) * 0.0f) c = cmul(c, 0.35f);
            // глаз
            float ex = S.eyeT * S.Lb, ey = -fishProf(S.top, S.eyeT) * S.eyeV;
            float er = std::max(S.eyeR, g_fishPx * 0.95f / L), d = sqrtf((X - ex) * (X - ex) + (Y - ey) * (Y - ey));
            if (d < er * 1.15f) c = cmul(c, 0.55f);   // тёмный ободок
            if (d < er) {
                C3 iris = cooked ? c3(0xe8e2d4) : c3(S.iris);
                float r = d / er;
                c = cmix(iris, cmul(iris, 0.6f), r);
                if (r < (cooked ? 0.35f : 0.58f)) c = cooked ? c3(0x8a8270) : c3(0x0a0a0c);
                float hx = X - (ex - er * 0.32f), hy = Y - (ey - er * 0.32f);
                if (!cooked && hx * hx + hy * hy < er * er * 0.09f) c = c3(0xffffff);
            }
            out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = 1; return;
        }
    }
    // плавники за телом: спинные
    for (int i = 0; i < S.nd; i++) {
        const FishFin& F = S.dors[i];
        float tt = clampf(t, 0.f, 1.f), base = -fishProf(S.top, tt), dd = base - Y;
        if (dd < -0.01f) continue;
        float u = (t - F.t0 - (dd / F.h) * F.slant * (F.t1 - F.t0) * 0.5f) / (F.t1 - F.t0);
        float fh = F.h * finK * finShape(F.shape, u);
        if (dd <= fh) {
            C3 c = fishFinCol(S, true, u, dd / (F.h * finK), cooked, true);
            if (sp == 0 && i == 0 && u > 0.62f && dd / F.h > 0.25f && !cooked) c = cmix(c, c3(0x14140c), 0.8f);
            out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = 1; return;
        }
    }
    // нижние: анальный и брюшной
    const FishFin* low[2] = {&S.anal, &S.pelv};
    for (int i = 0; i < 2; i++) {
        const FishFin& F = *low[i];
        float tt = clampf(t, 0.f, 1.f), base = fishProf(S.bot, tt), dd = Y - base;
        if (dd < -0.01f) continue;
        float u = (t - F.t0 - (dd / F.h) * F.slant * (F.t1 - F.t0)) / (F.t1 - F.t0);
        float fh = F.h * finK * finShape(F.shape, u);
        if (dd <= fh) { C3 c = fishFinCol(S, false, u, dd / (F.h * finK), cooked, true); out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = 1; return; }
    }
    // хвост
    {
        float s = (X - S.Lb + 0.01f) / (1.f - S.Lb + 0.01f);
        float cy = (fishProf(S.bot, 1.f) - fishProf(S.top, 1.f)) * 0.5f;
        float ped = (fishProf(S.top, 1.f) + fishProf(S.bot, 1.f)) * 0.5f / S.tailH;
        float a = (Y - cy) / S.tailH;
        if (fishTailCover(S, s, a, ped) > 0) {
            C3 c = fishFinCol(S, false, (atan2f(a, s + 0.25f) + 1.6f) * 0.9f, s, cooked, true);
            if (S.pattern == 0) c = cmix(c, c3(0x9a8a5a), 0.45f);
            if (cooked) c = cmix(c, c3(0x2a1206), sstep(0.5f, 1.f, s) * 0.7f);
            out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = 1;
        }
    }
}
// удочка: рукоять с пробкой внизу слева, катушка, удилище с кольцами к верхнему правому углу, леска с поплавком
static void rodSample(float x, float y, float* out, bool withLine = true) {
    out[0] = out[1] = out[2] = out[3] = 0;
    auto segDist = [](float px, float py, float ax, float ay, float bx, float by, float& tt) {
        float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
        tt = clampf(((px - ax) * dx + (py - ay) * dy) / l2, 0.f, 1.f);
        float qx = ax + dx * tt - px, qy = ay + dy * tt - py; return sqrtf(qx * qx + qy * qy);
    };
    const float ax = 0.09f, ay = 0.91f, bx = 0.9f, by = 0.08f;   // от торца рукояти к кончику
    C3 c; bool hit = false;
    // леска: с кончика вниз, с лёгким провисом, к поплавку
    if (withLine) {
        float t; float lx0 = bx, ly0 = by, lx1 = 0.84f, ly1 = 0.72f;
        float d = segDist(x, y, lx0, ly0, lx1, ly1, t);
        float bow = sinf(t * 3.14159f) * 0.035f; d = fabsf((x - (lx0 + (lx1 - lx0) * t)) - bow);
        if (fabsf(y - (ly0 + (ly1 - ly0) * t)) < 0.02f || d < 0.02f) {}
        if (d < std::max(0.017f, g_fishPx * 0.6f) && y > ly0 && y < ly1) { c = c3(0xdcdcd8); hit = true; }
        // поплавок: красный низ, белый верх, тёмная антенна
        float fx = lx1, fy = ly1 + 0.07f, dx = x - fx, dy = y - fy;
        if (dx * dx / (0.055f * 0.055f) + dy * dy / (0.075f * 0.075f) < 1.f) { c = dy < -0.005f ? c3(0xf2f0ea) : c3(0xd42a22); float r = sqrtf(dx * dx + dy * dy) / 0.07f; c = cmul(c, 1.05f - 0.35f * r); if (dx < -0.015f && dy < -0.02f) c = cmix(c, c3(0xffffff), 0.5f); hit = true; }
        if (fabsf(dx) < 0.012f && dy > -0.12f && dy < -0.07f) { c = c3(0x2a2a2a); hit = true; }
    }
    // удилище
    float t; float d = segDist(x, y, ax, ay, bx, by, t);
    float w = lerpf(0.05f, 0.016f, t);
    if (t > 0.3f) w = lerpf(0.032f, 0.016f, (t - 0.3f) / 0.7f);
    w = std::max(w, g_fishPx * 0.8f);
    if (d < w) {
        float side = ((x - ax) * (by - ay) - (y - ay) * (bx - ax)) > 0 ? 1.f : -1.f;   // для блика
        float k = d / w * side;
        if (t < 0.3f) {   // пробковая рукоять с тёмным торцом
            c = t < 0.05f ? c3(0x2a2420) : c3(0xc49a64);
            if (t >= 0.05f) { float n = hv(ifloor(x * 90), ifloor(y * 90), 5); c = cmul(c, 0.82f + n * 0.3f); if (fmodf(t * 30.f, 1.f) < 0.12f) c = cmul(c, 0.75f); }
            if (t > 0.26f) c = c3(0x8a8a90);   // металлическое кольцо
        } else {
            c = cmix(c3(0x7a4a24), c3(0x5a3418), t);   // удилище: лакированное дерево
            if (fmodf(t * 5.2f, 1.f) < 0.06f) c = c3(0x1e1e22);   // обмотка у колец
        }
        c = cmul(c, 1.f - 0.35f * fabsf(k)); if (k < -0.2f && k > -0.6f) c = cmix(c, c3(0xffffff), 0.25f);
        hit = true;
    }
    // кольца-пропускники
    for (int i = 0; i < (withLine ? 3 : 0); i++) {
        float gt = 0.45f + i * 0.19f, gx = ax + (bx - ax) * gt + 0.03f, gy = ay + (by - ay) * gt + 0.03f;
        float dd = sqrtf((x - gx) * (x - gx) + (y - gy) * (y - gy));
        if (dd < 0.026f && dd > 0.012f) { c = c3(0xb8bcc4); hit = true; }
    }
    // катушка сбоку от рукояти
    {
        float cx = ax + (bx - ax) * 0.33f + 0.075f, cy = ay + (by - ay) * 0.33f + 0.075f;
        float dd = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
        if (dd < 0.085f) {
            c = dd < 0.03f ? c3(0x50545c) : (dd < 0.065f ? c3(0xd8dbe2) : c3(0x7a7e88));
            if (dd > 0.03f && dd < 0.065f && fmodf(atan2f(y - cy, x - cx) * 3.f, 1.f) < 0.15f) c = c3(0xa8acb4);
            c = cmul(c, 1.1f - 0.4f * ((x - cx) + (y - cy)) / 0.12f * 0.5f);
            hit = true;
        }
        float hx = cx + 0.06f, hy = cy + 0.09f;   // ручка катушки
        if (fabsf(x - hx) < 0.02f && fabsf(y - hy) < 0.03f) { c = c3(0x2a2a2e); hit = true; }
    }
    if (hit) { out[0] = clampf(c.r, 0, 1); out[1] = clampf(c.g, 0, 1); out[2] = clampf(c.b, 0, 1); out[3] = 1; }
}
// пишет тайл в атлас 16x16 (R = 16) или в HD-атлас (R = g_tileRes)
template <class F> static void paintTileSS(int tile, int R, int ss, bool hard, F f) {
    for (int y = 0; y < R; y++) for (int x = 0; x < R; x++) {
        float acc[4] = {0, 0, 0, 0};
        for (int j = 0; j < ss; j++) for (int i = 0; i < ss; i++) {
            float o[4]; f((x + (i + 0.5f) / ss) / R, (y + (j + 0.5f) / ss) / R, o);
            acc[0] += o[0] * o[3]; acc[1] += o[1] * o[3]; acc[2] += o[2] * o[3]; acc[3] += o[3];
        }
        float a = acc[3] / (ss * ss);
        u32 px = 0;
        if (acc[3] > 0) {
            float r = acc[0] / acc[3], g = acc[1] / acc[3], b = acc[2] / acc[3];
            float aa = hard ? (a > 0.42f ? 1.f : 0.f) : a;
            px = RGBA((int)(clampf(r, 0, 1) * 255.f + 0.5f), (int)(clampf(g, 0, 1) * 255.f + 0.5f), (int)(clampf(b, 0, 1) * 255.f + 0.5f), (int)(aa * 255.f + 0.5f));
        }
        if (R == 16) g_atlasPx[((tile >> 4) * 16 + y) * ATLAS + (tile & 15) * 16 + x] = px;
        else g_hdAtlas[(size_t)((tile >> 4) * R + y) * g_hdSize + (tile & 15) * R + x] = px;
    }
}
static void genFishTiles(int R) {
    int ss = R <= 16 ? 6 : (R <= 32 ? 4 : 3);
    g_fishPx = 1.f / R;
    bool hard = R <= 32;   // мелкие значки — с чёткими пиксельными краями
    for (int sp = 0; sp < 4; sp++) {
        paintTileSS(T_FISH0 + sp, R, ss, hard, [&](float x, float y, float* o) { fishSample(0, sp, x, y, o); });
        paintTileSS(T_FFRY0 + sp, R, ss, hard, [&](float x, float y, float* o) { fishSample(1, sp, x, y, o); });
        paintTileSS(T_FSKIN0 + sp, R, std::max(2, ss - 1), false, [&](float x, float y, float* o) { fishSample(2, sp, x, y, o); });
        paintTileSS(T_FFIN0 + sp, R, ss, false, [&](float x, float y, float* o) { fishSample(3, sp, x, y, o); });
        paintTileSS(T_FTAIL0 + sp, R, ss, false, [&](float x, float y, float* o) { fishSample(4, sp, x, y, o); });
    }
    paintTileSS(T_ROD, R, ss, hard, [&](float x, float y, float* o) { rodSample(x, y, o); });
    for (int sp = 0; sp < 4; sp++)   // круглый глаз: радужка, зрачок, блик
        paintTileSS(T_FEYE0 + sp, R, ss, false, [&](float x, float y, float* o) {
            float dx = x - 0.5f, dy = y - 0.5f, d = sqrtf(dx * dx + dy * dy) * 2.f;
            o[3] = d < 1.f ? 1.f : 0.f;
            C3 ir = c3(FISH[sp].iris), c = d > 0.86f ? c3(0x101010) : cmix(ir, cmul(ir, 0.55f), d);
            if (d < 0.5f) c = c3(0x060608);
            float hx = dx + 0.17f, hy = dy + 0.17f; if (hx * hx + hy * hy < 0.012f) c = c3(0xffffff);
            if (d > 0.5f && d < 0.86f && fmodf(atan2f(dy, dx) * 4.f + 10.f, 1.f) < 0.12f) c = cmul(c, 0.8f);   // прожилки радужки
            o[0] = c.r; o[1] = c.g; o[2] = c.b; });
    paintTileSS(T_ROD_HAND, R, ss, hard, [&](float x, float y, float* o) { rodSample(x, y, o, false); });   // в руке — без лески: настоящая леска тянется к поплавку
}

// ---------------------------------------------------------------- инструменты: процедурный рисунок любого разрешения
// рукоять снизу слева, рабочая часть сверху справа; светит сверху слева
static const u32 TOOL_PAL[5][3] = {{0xb08048, 0x5e3c1c, 0xe0b47c}, {0x8c8c8e, 0x46464a, 0xc8c8ca}, {0xc4c8d0, 0x545c6a, 0xffffff}, {0xf0c040, 0x94640e, 0xfff6c4}, {0x4fdcd8, 0x10747a, 0xeaffff}};
// c — поперёк детали (-1 освещённая сторона .. 1 теневая), e — расстояние до края, s — вдоль детали
static C3 toolMatCol(int m, float c, float s, float /*e*/, float x, float y) {
    C3 b = c3(TOOL_PAL[m][0]), d = c3(TOOL_PAL[m][1]), l = c3(TOOL_PAL[m][2]);
    c = clampf(c, -1.f, 1.f);
    C3 col = c < 0 ? cmix(b, l, -c * 0.75f) : cmix(b, d, c * 0.8f);
    switch (m) {
        case 0: { float g = fabsf(fmodf(s * 23.f + fbm(x * 2.f, y * 2.f, 8, 2, 13) * 2.f, 1.f) - 0.5f); col = cmul(col, 0.86f + 0.2f * sstep(0.1f, 0.35f, g)); break; }   // волокна дерева
        case 1: { float n = fbm(x * 3.f, y * 3.f, 12, 3, 21); col = cmul(col, 0.8f + 0.4f * n); if (hv(ifloor(x * 64), ifloor(y * 64), 9) > 0.93f) col = cmul(col, 0.7f); break; }   // зернистый камень
        case 2: case 3: {   // металл: полированный блик и шлифовка вдоль
            float sp = expf(-((c + 0.4f) / 0.16f) * ((c + 0.4f) / 0.16f));
            col = cmix(col, l, sp * 0.75f);
            col = cmul(col, 0.94f + 0.08f * hv(ifloor(s * 90.f), ifloor(c * 3.f), 5));
            break;
        }
        case 4: {   // алмаз: грани разной яркости и искры
            int q = ifloor((c + 1.f) * 2.5f + s * 3.f); static const float lv[6] = {1.15f, 0.82f, 1.02f, 0.7f, 0.92f, 1.2f};
            col = cmul(col, lv[((q % 6) + 6) % 6]);
            if (hv(ifloor(x * 40), ifloor(y * 40), 77) > 0.94f) col = cmix(col, c3(0xffffff), 0.7f);
            break;
        }
    }
    return col;
}
static float g_toolPx = 1.f / 16;
static void toolSample(int m, int k, float x, float y, float* out) {
    out[0] = out[1] = out[2] = out[3] = 0;
    const float px = g_toolPx, edgeW = std::max(px * 0.7f, 0.018f);
    C3 col; bool hit = false;
    auto put = [&](C3 c, float e) { c = cmul(c, 0.5f + 0.5f * sstep(0.f, edgeW, e)); col = c; hit = true; };
    // отрезок: s — 0..1 вдоль, c — поперёк (со знаком), dist
    auto seg = [&](float ax, float ay, float bx, float by, float& s, float& c) {
        float dx = bx - ax, dy = by - ay, L = sqrtf(dx * dx + dy * dy);
        float ux = dx / L, uy = dy / L;
        s = ((x - ax) * ux + (y - ay) * uy) / L;
        c = (x - ax) * uy - (y - ay) * ux;   // >0 — ниже/правее линии
        return L;
    };
    const float Ax = 0.1f, Ay = 0.9f;
    // ---- рукоять (у меча — обмотанная рукоять, гарда и навершие)
    float Hx, Hy;
    switch (k) { case TK_PICK: Hx = 0.66f; Hy = 0.34f; break; case TK_AXE: Hx = 0.7f; Hy = 0.3f; break; case TK_SHOVEL: Hx = 0.64f; Hy = 0.36f; break; default: Hx = 0.3f; Hy = 0.7f; }
    {
        float s, c; seg(Ax, Ay, Hx, Hy, s, c);
        float hw = std::max(k == TK_SWORD ? 0.04f : 0.042f, px * 0.85f);
        if (s >= -0.03f && s <= 1.04f && fabsf(c) < hw) {
            float cc = c / hw;
            C3 w = cmix(cmix(c3(0x8c5c2e), c3(0xbc8a52), sstep(0.2f, -0.9f, cc)), c3(0x4c2e14), sstep(0.2f, 1.f, cc));
            float g = fabsf(fmodf(s * 9.f + fbm(x * 2.f, y * 2.f, 8, 2, 31) * 1.5f, 1.f) - 0.5f);
            w = cmul(w, 0.88f + 0.16f * sstep(0.05f, 0.3f, g));
            float wrapEnd = k == TK_SWORD ? 1.f : 0.32f;
            if (s < wrapEnd && s > 0.04f) {   // кожаная обмотка
                C3 lw = cmix(c3(0x5a3a26), c3(0x2e1c10), sstep(-0.6f, 1.f, cc));
                float band = fmodf(s * (k == TK_SWORD ? 11.f : 14.f) + cc * 0.25f, 1.f);
                if (band < 0.18f) lw = cmul(lw, 0.55f); else if (band < 0.35f) lw = cmix(lw, c3(0x8a6448), 0.4f);
                w = lw;
            }
            if (s < 0.05f) w = cmul(c3(0x3a2a1e), 1.f - 0.3f * cc);   // торец
            put(w, std::min(hw - fabsf(c), (s + 0.03f) * 0.5f));
        }
        if (k == TK_SWORD) {   // навершие
            float dx = x - (Ax - 0.005f), dy = y - (Ay + 0.005f), r = std::max(0.055f, px * 1.2f), d = sqrtf(dx * dx + dy * dy);
            if (d < r) put(toolMatCol(m, (dx + dy) / r * 0.7f, 0.f, r - d, x, y), r - d);
        }
    }
    // ---- рабочая часть
    float dxh = Hx - Ax, dyh = Hy - Ay, Lh = sqrtf(dxh * dxh + dyh * dyh);
    float fx = dxh / Lh, fy = dyh / Lh;    // вдоль рукояти (к голове)
    float nx = -fy, ny = fx;               // поперёк: (0.7, 0.7) — вниз вправо
    float rx = x - Hx, ry = y - Hy;
    float w = rx * fx + ry * fy, u = rx * nx + ry * ny;
    if (k == TK_PICK) {
        const float L = 0.44f; float uu = u / L;
        if (fabsf(uu) < 1.f) {
            float wc = 0.03f - 0.17f * uu * uu;                       // дуга, концы загнуты к рукояти
            float th = (0.072f - 0.03f * fabsf(uu)) * std::min(1.f, (1.f - fabsf(uu)) / 0.22f + 0.15f);
            th = std::max(th, px * 1.0f);
            float dd = w - wc;
            if (fabsf(dd) < th) put(toolMatCol(m, -dd / th, uu, th - fabsf(dd), x, y), std::min(th - fabsf(dd), (1.f - fabsf(uu)) * L));
        }
        if (fabsf(u) < 0.065f && w > -0.07f && w < 0.075f) {   // проушина-крепление
            C3 bc = m == 0 ? cmix(c3(0xc8b48a), c3(0x6a5a3a), sstep(-0.05f, 0.07f, w)) : cmul(toolMatCol(m, w / 0.07f, 0.f, 1.f, x, y), 0.72f);
            if (m == 0 && fmodf((w + 0.1f) * 60.f, 1.f) < 0.3f) bc = cmul(bc, 0.7f);   // верёвочная обвязка
            put(bc, std::min(0.065f - fabsf(u), 0.075f - fabsf(w)));
        }
    } else if (k == TK_AXE) {
        float b = -u;   // лезвие уходит вверх-влево
        if (b > -0.1f && b < 0.36f) {
            float hh = b < 0 ? 0.055f : 0.06f + 0.5f * powf(b, 1.45f);
            float edge = 0.35f - 1.4f * w * w;                        // выпуклая режущая кромка
            if (fabsf(w) < hh && b < edge) {
                float e = std::min(hh - fabsf(w), edge - b);
                C3 cc = toolMatCol(m, w / hh * 0.9f - 0.2f, b, e, x, y);
                if (edge - b < std::max(0.035f, px * 1.1f) && b > 0.05f) cc = cmix(cc, c3(TOOL_PAL[m][2]), 0.55f);   // заточка
                if (b < 0.02f && b > -0.1f) cc = cmul(cc, 0.8f);
                put(cc, e);
            }
        }
        if (fabsf(u) < 0.05f && fabsf(w) < 0.07f) put(cmul(toolMatCol(m, w / 0.07f, 0.f, 1.f, x, y), 0.7f), std::min(0.05f - fabsf(u), 0.07f - fabsf(w)));
    } else if (k == TK_SHOVEL) {
        float s0 = 0.02f, len = 0.36f, s = w - s0;
        if (s > -0.06f && s < 0.0f && fabsf(u) < 0.05f) put(cmul(toolMatCol(m, u / 0.05f, 0.f, 1.f, x, y), 0.72f), 0.05f - fabsf(u));   // втулка
        if (s >= 0 && s < len) {
            float sn = s / len;
            float hw = 0.12f * std::min(1.f, 0.55f + sn * 4.f) * (sn > 0.7f ? sqrtf(std::max(0.f, 1.f - ((sn - 0.7f) / 0.3f) * ((sn - 0.7f) / 0.3f))) : 1.f);
            hw = std::max(hw, px * 0.6f);
            if (fabsf(u) < hw) {
                float cu = u / hw;
                C3 cc = toolMatCol(m, cu * 0.8f, sn, hw - fabsf(u), x, y);
                if (fabsf(cu) < 0.12f && sn < 0.75f) cc = cmul(cc, 0.82f);   // рёбрышко по центру
                put(cc, std::min(hw - fabsf(u), len - s));
            }
        }
    } else {   // меч: гарда и клинок
        float gl = 0.17f, gt = std::max(0.038f, px * 0.9f);
        if (fabsf(u) < gl + gt && fabsf(w) < gt) {
            float ex = std::max(0.f, fabsf(u) - gl), d = sqrtf(ex * ex + w * w);
            if (d < gt) put(cmul(toolMatCol(m, w / gt, u, gt - d, x, y), m == 0 ? 0.85f : 0.78f), gt - d);
        }
        float s, c; float L = seg(Hx, Hy, 0.93f, 0.07f, s, c);
        if (s > 0.f && s < 1.f) {
            float bw = std::max(0.062f, px * 1.0f) * (s > 0.82f ? (1.f - s) / 0.18f : 1.f) + (s > 0.82f ? px * 0.3f : 0.f);
            if (fabsf(c) < bw) {
                float cc = c / bw;
                C3 bc = toolMatCol(m, cc, s, bw - fabsf(c), x, y);
                if (fabsf(cc) < 0.22f && s > 0.05f && s < 0.72f) bc = cmul(bc, 0.78f);        // дол
                if (fabsf(cc) > 0.7f) bc = cmix(bc, c3(TOOL_PAL[m][2]), 0.35f);               // заточенные кромки
                put(bc, std::min(bw - fabsf(c), (1.f - s) * L));
            }
        }
    }
    if (hit) { out[0] = clampf(col.r, 0, 1); out[1] = clampf(col.g, 0, 1); out[2] = clampf(col.b, 0, 1); out[3] = 1; }
}
static void genToolTiles(int R) {
    g_toolPx = 1.f / R;
    int ss = R <= 16 ? 6 : (R <= 32 ? 4 : 3);
    for (int m = 0; m < 5; m++) for (int k = 0; k < 4; k++)
        paintTileSS(T_TOOL0 + m * 4 + k, R, ss, R <= 32, [&](float x, float y, float* o) { toolSample(m, k, x, y, o); });
}

// ---------------------------------------------------------------- значки предметов и интерфейса: векторные слои с объёмом
// каждая деталь — поле расстояний (меньше нуля — внутри); свет сверху слева, тёмная обводка по краю
static inline float sdCircle(float x, float y, float cx, float cy, float r) { return sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r; }
static inline float sdEll(float x, float y, float cx, float cy, float rx, float ry) { float dx = (x - cx) / rx, dy = (y - cy) / ry; return (sqrtf(dx * dx + dy * dy) - 1.f) * std::min(rx, ry); }
static inline float sdCaps(float x, float y, float ax, float ay, float bx, float by, float r) {
    float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy, t = l2 > 0 ? clampf(((x - ax) * dx + (y - ay) * dy) / l2, 0.f, 1.f) : 0.f;
    float qx = ax + dx * t - x, qy = ay + dy * t - y; return sqrtf(qx * qx + qy * qy) - r;
}
static inline float sdCaps2(float x, float y, float ax, float ay, float bx, float by, float ra, float rb) {   // конус с разными радиусами
    float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy, t = l2 > 0 ? clampf(((x - ax) * dx + (y - ay) * dy) / l2, 0.f, 1.f) : 0.f;
    float qx = ax + dx * t - x, qy = ay + dy * t - y; return sqrtf(qx * qx + qy * qy) - (ra + (rb - ra) * t);
}
static float sdPoly(float x, float y, const float* p, int n) {   // точное расстояние до многоугольника
    float d = (x - p[0]) * (x - p[0]) + (y - p[1]) * (y - p[1]); float s = 1.f;
    for (int i = 0, j = n - 1; i < n; j = i, i++) {
        float ex = p[j * 2] - p[i * 2], ey = p[j * 2 + 1] - p[i * 2 + 1], wx = x - p[i * 2], wy = y - p[i * 2 + 1];
        float t = clampf((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.f, 1.f);
        float bx = wx - ex * t, by = wy - ey * t; d = std::min(d, bx * bx + by * by);
        bool c1 = y >= p[i * 2 + 1], c2 = y < p[j * 2 + 1], c3 = ex * wy > ey * wx;
        if ((c1 && c2 && c3) || (!c1 && !c2 && !c3)) s = -s;
    }
    return s * sqrtf(d);
}
static inline float sdRBox(float x, float y, float cx, float cy, float hw, float hh, float r, float ang = 0.f) {
    float dx = x - cx, dy = y - cy;
    if (ang != 0.f) { float c = cosf(ang), s = sinf(ang), tx = dx * c + dy * s; dy = -dx * s + dy * c; dx = tx; }
    float qx = fabsf(dx) - hw + r, qy = fabsf(dy) - hh + r;
    return sqrtf(std::max(qx, 0.f) * std::max(qx, 0.f) + std::max(qy, 0.f) * std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - r;
}
struct IconPaint {
    float x, y, px; C3 c; float a = 0;
    // слой: sdf(x, y), цвет col(x, y, t) где t — 0 у края .. 1 в глубине; bevel — ширина скругления
    template <class S, class Fc> void lay(S sdf, Fc colf, float bevel, float outl = 1.f, float alpha = 1.f) {
        float d = sdf(x, y);
        if (d > 0) return;
        float e = std::max(px * 0.35f, 0.002f);
        float gx = sdf(x + e, y) - sdf(x - e, y), gy = sdf(x, y + e) - sdf(x, y - e), gl = sqrtf(gx * gx + gy * gy) + 1e-6f;
        gx /= gl; gy /= gl;
        float t = clampf(-d / bevel, 0.f, 1.f), rim = powf(1.f - t, 1.6f);
        float lit = 0.86f + 0.55f * rim * (-0.6f * gx - 0.8f * gy) - 0.12f * rim;   // скос к свету светлее, от света — темнее
        C3 col = cmul(colf(x, y, t), lit);
        float ol = std::max(px * 0.85f, 0.014f) * outl;
        if (outl > 0 && d > -ol) col = cmix(col, cmul(col, 0.32f), 0.85f);
        c = a > 0 ? cmix(c, col, alpha) : col; a = std::max(a, alpha);
    }
    template <class S> void lay(S sdf, C3 col, float bevel, float outl = 1.f, float alpha = 1.f) { lay(sdf, [&](float, float, float) { return col; }, bevel, outl, alpha); }
};
static float g_iconPx = 1.f / 16;
static C3 grad3(C3 a, C3 b, float k) { return cmix(a, b, clampf(k, 0.f, 1.f)); }
// металлический цвет для слитков и брони: 2 — железо, 3 — золото, 4 — алмаз (палитра инструментов)
static C3 metalAt(int m, float x, float y, float t) {
    C3 b = c3(TOOL_PAL[m][0]), d = c3(TOOL_PAL[m][1]), l = c3(TOOL_PAL[m][2]);
    float k = (x + y) * 0.5f;
    C3 col = k < 0.5f ? cmix(l, b, k * 2.f) : cmix(b, d, (k - 0.5f) * 1.6f);
    float band = expf(-((x - y + 0.1f) / 0.07f) * ((x - y + 0.1f) / 0.07f));   // диагональный блик
    col = cmix(col, l, band * 0.6f * t);
    if (m == 4) { int q = ifloor((x * 3.f + y * 5.f) * 2.f); static const float lv[5] = {1.1f, 0.85f, 1.f, 0.75f, 1.18f}; col = cmul(col, lv[((q % 5) + 5) % 5]); }
    return col;
}
static void itemSample(int tile, float x, float y, float* out) {
    IconPaint P2; P2.x = x; P2.y = y; P2.px = g_iconPx; P2.c = c3(0);
    IconPaint& I = P2;
    const float px = g_iconPx;
    auto wood = [&](float xx, float yy, float t) { float g = fabsf(fmodf((xx - yy) * 1.2f + fbm(xx * 2.f, yy * 2.f, 8, 2, 3) * 0.8f, 0.1f) / 0.1f - 0.5f); return cmul(grad3(c3(0xb07a40), c3(0x6e4420), yy * 0.6f + xx * 0.4f), 0.85f + 0.25f * sstep(0.1f, 0.4f, g) + 0.1f * t); };
    switch (tile) {
    case T_STICK: {
        I.lay([&](float a, float b) { return std::min(sdCaps2(a, b, 0.2f, 0.84f, 0.52f, 0.5f, 0.06f, 0.052f), sdCaps2(a, b, 0.52f, 0.5f, 0.82f, 0.16f, 0.052f, 0.045f)); }, wood, 0.06f);
        I.lay([&](float a, float b) { return sdCaps(a, b, 0.52f, 0.5f, 0.6f, 0.36f, 0.03f); }, c3(0x7a5028), 0.03f, 0.6f);   // сучок
        break;
    }
    case T_COAL: {
        auto sd = [&](float a, float b) { float ang = atan2f(b - 0.53f, a - 0.5f); float r = 0.31f + 0.04f * sinf(ang * 5.f + 1.f) + 0.025f * sinf(ang * 9.f); return sqrtf((a - 0.5f) * (a - 0.5f) + (b - 0.53f) * (b - 0.53f)) - r; };
        I.lay(sd, [&](float a, float b, float t) {
            int f = ifloor((atan2f(b - 0.5f, a - 0.48f) + 3.2f) * 1.6f + t * 2.f);   // грани
            static const float lv[6] = {1.f, 0.7f, 1.3f, 0.85f, 1.15f, 0.75f};
            C3 c = cmul(c3(0x2c2c32), lv[f % 6]);
            if (hv(ifloor(a * 30), ifloor(b * 30), 4) > 0.86f) c = cmix(c, c3(0x9a9aa4), 0.6f);
            return c; }, 0.12f);
        break;
    }
    case T_IRON_INGOT: case T_GOLD_INGOT: {   // слиток: толстый брусок со скошенным верхом
        int m = tile == T_IRON_INGOT ? 2 : 3;
        const float L[2] = {0.05f, 0.5f}, F[2] = {0.42f, 0.72f}, Rr[2] = {0.95f, 0.44f}, B[2] = {0.58f, 0.22f}, th = 0.16f;
        const float side1[8] = {L[0], L[1], F[0], F[1], F[0], F[1] + th, L[0], L[1] + th};
        const float side2[8] = {F[0], F[1], Rr[0], Rr[1], Rr[0], Rr[1] + th, F[0], F[1] + th};
        const float base[8] = {L[0], L[1], F[0], F[1], Rr[0], Rr[1], B[0], B[1]};
        float cx = (L[0] + Rr[0]) * 0.5f, cy = (L[1] + Rr[1] + F[1] + B[1]) * 0.25f, k = 0.62f;
        const float top[8] = {cx + (L[0] - cx) * k, cy + (L[1] - cy) * k, cx + (F[0] - cx) * k, cy + (F[1] - cy) * k, cx + (Rr[0] - cx) * k, cy + (Rr[1] - cy) * k, cx + (B[0] - cx) * k, cy + (B[1] - cy) * k};
        I.lay([&](float a, float b) { return sdPoly(a, b, side1, 4); }, [&](float a, float b, float) { return cmul(metalAt(m, a, b, 0.4f), 0.72f); }, 0.02f);
        I.lay([&](float a, float b) { return sdPoly(a, b, side2, 4); }, [&](float a, float b, float) { return cmul(metalAt(m, a, b, 0.3f), 0.52f); }, 0.02f);
        I.lay([&](float a, float b) { return sdPoly(a, b, base, 4); }, [&](float a, float b, float) { return cmul(metalAt(m, a, b, 0.5f), 0.92f); }, 0.02f);   // скосы
        I.lay([&](float a, float b) { return sdPoly(a, b, top, 4); }, [&](float a, float b, float t) { return cmul(metalAt(m, a, b, t), 1.18f); }, 0.03f, 0.6f);   // верх светлее
        break;
    }
    case T_DIAMOND: {
        const float gem[12] = {0.18f, 0.38f, 0.32f, 0.2f, 0.68f, 0.2f, 0.82f, 0.38f, 0.5f, 0.86f, 0.5f, 0.86f};
        I.lay([&](float a, float b) { return sdPoly(a, b, gem, 5); }, [&](float a, float b, float) {
            C3 c = c3(0x52e2dc);
            if (b < 0.38f) c = (a < 0.4f) ? c3(0x9ef6f0) : (a < 0.6f ? c3(0xd8fffc) : c3(0x6ad8d4));       // корона
            else { float k = (a - 0.5f) / (0.86f - b + 0.01f); c = k < -0.35f ? c3(0x3cc8c4) : (k < 0.05f ? c3(0x22a6a8) : (k < 0.4f ? c3(0x168c90) : c3(0x0e6c72))); }   // павильон
            if (fabsf(b - 0.38f) < px * 0.6f) c = c3(0xe8ffff);
            return c; }, 0.02f);
        float sx = x - 0.34f, sy = y - 0.3f;
        if ((fabsf(sx) < 0.012f && fabsf(sy) < 0.07f) || (fabsf(sy) < 0.012f && fabsf(sx) < 0.07f)) { I.c = c3(0xffffff); I.a = 1; }   // искра
        break;
    }
    case T_APPLE: {
        auto body = [&](float a, float b) { return std::min(sdCircle(a, b, 0.4f, 0.58f, 0.27f), sdCircle(a, b, 0.6f, 0.58f, 0.27f)) + 0.0f * a; };
        I.lay([&](float a, float b) { return sdCaps(a, b, 0.5f, 0.32f, 0.56f, 0.12f, 0.025f); }, c3(0x5a3a1e), 0.02f);
        I.lay([&](float a, float b) { float d = body(a, b); float notch = sdCircle(a, b, 0.5f, 0.27f, 0.07f); return std::max(d, -notch); }, [&](float a, float b, float t) {
            C3 c = grad3(c3(0xe8402e), c3(0x8a1414), (b - 0.35f) * 1.6f + (a - 0.3f) * 0.5f);
            float st = fbm(a * 1.f, b * 4.f, 8, 3, 9); c = cmix(c, c3(0xf0b040), sstep(0.62f, 0.8f, st) * 0.4f);
            float hl = sdCircle(a, b, 0.36f, 0.46f, 0.07f); if (hl < 0) c = cmix(c, c3(0xffffff), 0.55f * t);
            return c; }, 0.14f);
        I.lay([&](float a, float b) { float c = cosf(-0.6f), s = sinf(-0.6f), dx = a - 0.67f, dy = b - 0.2f, u = dx * c - dy * s, v = dx * s + dy * c; return sdEll(u, v, 0, 0, 0.13f, 0.06f); },
              [&](float a, float b, float) { C3 c = c3(0x4aa838); if (fabsf((a - 0.6f) * 0.55f + (b - 0.24f)) < px * 0.8f) c = c3(0x2e7a24); return c; }, 0.04f);
        break;
    }
    case T_RAWMEAT: case T_COOKED: {
        bool ck = tile == T_COOKED;
        auto sd = [&](float a, float b) { float ang = atan2f(b - 0.52f, a - 0.5f); float r = 0.3f + 0.05f * sinf(ang * 2.f + 0.6f) + 0.02f * sinf(ang * 5.f); return sdEll(a, b, 0.5f, 0.52f, r * 1.25f, r * 0.95f); };
        I.lay(sd, [&](float a, float b, float t) {
            C3 c = ck ? grad3(c3(0xa8642c), c3(0x5a2a10), b * 0.8f) : grad3(c3(0xe8606a), c3(0xa82838), b * 0.8f);
            if (t < 0.22f) c = ck ? c3(0x7a4018) : c3(0xf2dcc8);   // жировая кромка
            float v = fbm(a * 2.f, b * 2.f, 8, 3, 17); if (!ck && fabsf(v - 0.5f) < 0.03f && t > 0.3f) c = cmix(c, c3(0xf6e0e0), 0.8f);   // мраморность
            if (ck) { float g = fmodf((a + b) * 7.f, 1.f); if (g < 0.16f && t > 0.25f) c = c3(0x3a1a08); }   // решётка
            float hl = sdEll(a, b, 0.38f, 0.42f, 0.12f, 0.06f); if (hl < 0) c = cmix(c, c3(0xffffff), ck ? 0.3f : 0.4f);
            return c; }, 0.16f);
        I.lay([&](float a, float b) { return sdCircle(a, b, 0.66f, 0.6f, 0.06f); }, c3(ck ? 0xd8c8a8 : 0xf4ecde), 0.04f, 0.8f);   // косточка в срезе
        break;
    }
    case T_PORK: case T_PORK_C: {   // свиная отбивная: каплевидный кусок с широкой полосой сала и рёберной косточкой
        bool ck = tile == T_PORK_C;
        auto sd = [&](float a, float b) {
            float d = sdEll(a, b, 0.47f, 0.55f, 0.34f, 0.27f);
            d = std::min(d, sdCircle(a, b, 0.3f, 0.42f, 0.17f));
            return std::max(d, -sdCircle(a, b, 0.86f, 0.78f, 0.12f)); };
        I.lay([&](float a, float b) { return sdCaps(a, b, 0.66f, 0.62f, 0.9f, 0.86f, 0.045f); }, c3(ck ? 0xd8c8a8 : 0xf2eadc), 0.03f);   // косточка
        I.lay([&](float a, float b) { return sdCircle(a, b, 0.91f, 0.88f, 0.06f); }, c3(ck ? 0xd0bea0 : 0xece2d0), 0.03f);
        I.lay(sd, [&](float a, float b, float t) {
            C3 c = ck ? grad3(c3(0xc8884c), c3(0x7a3e1a), b * 0.9f + a * 0.2f) : grad3(c3(0xf4a0a0), c3(0xd06a72), b * 0.9f);
            if (t < 0.28f) c = ck ? grad3(c3(0xe8b878), c3(0xb07a40), t * 3.f) : grad3(c3(0xfff4ea), c3(0xf6d8d0), t * 3.f);   // сало
            if (!ck) { float v = fbm(a * 2.f, b * 2.f, 6, 3, 31); if (fabsf(v - 0.5f) < 0.025f && t > 0.35f) c = cmix(c, c3(0xffe8e8), 0.7f); }
            if (ck) { float g = fmodf((a - b) * 6.f + 10.f, 1.f); if (g < 0.14f && t > 0.32f) c = cmul(c, 0.55f); }   // полоски от жарки
            float hl = sdEll(a, b, 0.34f, 0.4f, 0.1f, 0.05f); if (hl < 0) c = cmix(c, c3(0xffffff), 0.35f);
            return c; }, 0.15f);
        break;
    }
    case T_MUTTON: case T_MUTTON_C: {   // баранья голень: мясистый «окорочок» на длинной кости
        bool ck = tile == T_MUTTON_C;
        I.lay([&](float a, float b) { return std::min(sdCaps(a, b, 0.5f, 0.5f, 0.84f, 0.16f, 0.045f), std::min(sdCircle(a, b, 0.86f, 0.1f, 0.055f), sdCircle(a, b, 0.92f, 0.17f, 0.055f))); },
              [&](float a, float b, float t) { return cmul(grad3(c3(0xf6f0dc), c3(0xc8bc98), (a + b) * 0.6f), 0.95f + 0.08f * t); }, 0.04f);
        auto sd = [&](float a, float b) { float c = cosf(0.78f), s2 = sinf(0.78f), dx = a - 0.4f, dy = b - 0.6f, u = dx * c - dy * s2, v = dx * s2 + dy * c; return sdEll(u, v, 0, 0, 0.21f, 0.3f); };
        I.lay(sd, [&](float a, float b, float t) {
            C3 c = ck ? grad3(c3(0xb0703a), c3(0x5e2c12), b * 0.9f) : grad3(c3(0xd84c4c), c3(0x8e1e2a), b * 0.9f);
            if (t < 0.16f) c = ck ? c3(0x6a3416) : c3(0xf0d4c4);
            float st = fbm(a * 3.f, b * 3.f, 6, 3, 41); if (!ck && st > 0.62f && t > 0.3f) c = cmix(c, c3(0xf4d8d8), 0.5f);
            if (ck && fbm(a * 4.f, b * 4.f, 6, 2, 43) > 0.6f) c = cmul(c, 0.7f);   // поджаристые пятна
            float hl = sdEll(a, b, 0.32f, 0.5f, 0.06f, 0.11f); if (hl < 0) c = cmix(c, c3(0xffffff), 0.35f);
            return c; }, 0.14f);
        break;
    }
    case T_SHEARS: {   // ножницы для стрижки: два лезвия и петля
        I.lay([&](float a, float b) { return std::max(sdCircle(a, b, 0.3f, 0.72f, 0.17f), -sdCircle(a, b, 0.3f, 0.72f, 0.09f)); }, c3(0x3a3c44), 0.05f);   // петля
        auto blade = [&](float a, float b, float sx) {
            const float pts[8] = {0.42f + sx, 0.6f, 0.86f + sx * 0.4f, 0.14f, 0.92f + sx * 0.4f, 0.2f, 0.5f + sx, 0.68f};
            return sdPoly(a, b, pts, 4); };
        I.lay([&](float a, float b) { return blade(a, b, -0.05f); }, [&](float a, float b, float t) { return cmul(metalAt(2, a, b, t), 0.85f); }, 0.03f);
        I.lay([&](float a, float b) { return blade(a, b, 0.05f); }, [&](float a, float b, float t) { return metalAt(2, a, b, t); }, 0.03f);
        I.lay([&](float a, float b) { return sdCircle(a, b, 0.5f, 0.6f, 0.04f); }, c3(0x22242a), 0.02f, 0.5f);   // ось
        break;
    }
    case T_XP0: case T_XP0 + 1: case T_XP0 + 2: case T_XP0 + 3: {   // шарики опыта разного размера: светящиеся, с переливом
        int k = tile - T_XP0; float r = 0.16f + k * 0.07f;
        I.lay([&](float a, float b) { return sdCircle(a, b, 0.5f, 0.5f, r); }, [&](float a, float b, float t) {
            float ang = atan2f(b - 0.5f, a - 0.5f);
            C3 c = cmix(c3(0x5aa020), c3(0xd8f040), 0.5f + 0.5f * sinf(ang * 3.f + t * 4.f));
            c = cmix(c, c3(0xfffac0), t * t * 0.7f);
            return c; }, r * 0.8f, 0.7f);
        float hx = x - 0.5f + r * 0.4f, hy = y - 0.5f + r * 0.4f;
        if (hx * hx + hy * hy < r * r * 0.08f) { I.c = c3(0xffffff); I.a = 1; }
        break;
    }
    case T_SPLASH: {   // мягкая снежинка/капля: круг с размытым краем
        float d = sqrtf((x - 0.5f) * (x - 0.5f) + (y - 0.5f) * (y - 0.5f));
        float a = clampf(1.f - d / 0.46f, 0.f, 1.f); a = a * a * (3.f - 2.f * a);
        out[0] = out[1] = out[2] = 1.f; out[3] = a; return;
    }
    case T_CRIT: {   // искра крита: четырёхлучевая звёздочка
        float dx = fabsf(x - 0.5f), dy = fabsf(y - 0.5f);
        float star = std::max(1.f - (dx * 4.f + dy * 0.9f) / 0.5f, 1.f - (dy * 4.f + dx * 0.9f) / 0.5f);
        float core = 1.f - sqrtf(dx * dx + dy * dy) / 0.16f;
        float a = clampf(std::max(star, core), 0.f, 1.f);
        out[0] = out[1] = out[2] = 1.f; out[3] = a; return;
    }
    case T_BONE: {
        auto sd = [&](float a, float b) {
            float d = sdCaps(a, b, 0.28f, 0.72f, 0.72f, 0.28f, 0.055f);
            d = std::min(d, std::min(sdCircle(a, b, 0.2f, 0.72f, 0.075f), sdCircle(a, b, 0.28f, 0.8f, 0.075f)));
            d = std::min(d, std::min(sdCircle(a, b, 0.8f, 0.28f, 0.075f), sdCircle(a, b, 0.72f, 0.2f, 0.075f)));
            return d; };
        I.lay(sd, [&](float a, float b, float t) { return cmul(grad3(c3(0xf6f0dc), c3(0xc8bc98), (a + b) * 0.6f - 0.1f), 0.95f + 0.08f * t); }, 0.07f);
        break;
    }
    case T_BONEMEAL: {
        auto sd = [&](float a, float b) { float top = 0.52f - 0.2f * expf(-((a - 0.5f) / 0.2f) * ((a - 0.5f) / 0.2f)) + 0.03f * sinf(a * 30.f); return std::max(std::max(top - b, b - 0.82f), fabsf(a - 0.5f) - 0.38f + (b - 0.82f) * 0.3f); };
        I.lay(sd, [&](float a, float b, float t) { C3 c = grad3(c3(0xffffff), c3(0xb8b8b0), (b - 0.3f) * 1.5f); if (hv(ifloor(a * 40), ifloor(b * 40), 6) > 0.8f) c = cmul(c, 0.82f); return cmul(c, 0.95f + 0.05f * t); }, 0.1f);
        break;
    }
    case T_ROTTEN: {
        auto sd = [&](float a, float b) { float ang = atan2f(b - 0.52f, a - 0.5f); float r = 0.28f + 0.05f * sinf(ang * 3.f) + 0.03f * sinf(ang * 7.f + 2.f); return sdEll(a, b, 0.5f, 0.52f, r * 1.2f, r); };
        I.lay(sd, [&](float a, float b, float t) {
            C3 c = grad3(c3(0x8a7a44), c3(0x5a3a2a), b);
            float v = fbm(a * 2.f, b * 2.f, 8, 3, 23); c = cmix(c, c3(0x6a8a3a), sstep(0.5f, 0.7f, v) * 0.7f);
            if (sstep(0.6f, 0.75f, fbm(a * 3.f, b * 3.f, 12, 2, 44)) > 0.5f) c = cmul(c, 0.55f);
            if (t < 0.2f) c = cmul(c, 0.8f); return c; }, 0.14f);
        break;
    }
    case T_FLUFF: {
        auto sd = [&](float a, float b) {
            float d = std::min(sdCircle(a, b, 0.42f, 0.55f, 0.18f), std::min(sdCircle(a, b, 0.6f, 0.45f, 0.17f), sdCircle(a, b, 0.56f, 0.64f, 0.15f)));
            return d + 0.03f * (fbm(a * 2.f, b * 2.f, 16, 2, 51) - 0.5f) * 2.f; };
        I.lay(sd, [&](float a, float b, float t) { C3 c = grad3(c3(0xfff8fa), c3(0xe8c8d4), (b - 0.3f) * 1.4f); float f = fabsf(fmodf(atan2f(b - 0.52f, a - 0.52f) * 6.f, 1.f) - 0.5f); return cmul(c, 0.9f + 0.1f * f + 0.06f * t); }, 0.12f, 0.6f);
        break;
    }
    case T_STRING: {
        auto sd = [&](float a, float b) {
            float dx = a - 0.48f, dy = b - 0.48f, r = sqrtf(dx * dx + dy * dy), ang = atan2f(dy, dx);
            float best = 9.f;
            for (int k = 0; k < 3; k++) { float rr = 0.1f + 0.055f * ((ang + 3.14159f) / 6.2832f + k); if (rr < 0.3f) best = std::min(best, fabsf(r - rr)); }
            best -= 0.018f;
            return std::min(best, sdCaps(a, b, 0.62f, 0.66f, 0.84f, 0.88f, 0.018f)); };
        I.lay(sd, [&](float a, float b, float) { return cmul(c3(0xece8dc), 0.85f + 0.2f * hv(ifloor(a * 60), ifloor(b * 60), 2)); }, 0.02f, 0.7f);
        break;
    }
    case T_BOW: {
        auto limb = [&](float a, float b) { float dx = a - 0.1f, dy = b - 0.1f; float r = sqrtf(dx * dx + dy * dy); float ang = atan2f(dy, dx); if (ang < -0.05f || ang > 1.62f) return 9.f; return fabsf(r - 0.66f) - 0.065f * (0.55f + 0.45f * sinf(ang * 2.f)); };
        I.lay([&](float a, float b) { return sdCaps(a, b, 0.76f, 0.1f, 0.1f, 0.76f, 0.012f + px * 0.5f); }, c3(0xf4f0e4), 0.01f, 0.6f);   // тетива
        I.lay(limb, wood, 0.05f);
        I.lay([&](float a, float b) { return sdCaps(a, b, 0.5f, 0.6f, 0.6f, 0.5f, 0.06f); }, [&](float a, float b, float) { return fmodf((a - b) * 30.f + 5.f, 1.f) < 0.3f ? c3(0x3a2418) : c3(0x6a4630); }, 0.04f);
        break;
    }
    case T_ARROW: {
        I.lay([&](float a, float b) { return sdCaps(a, b, 0.22f, 0.78f, 0.74f, 0.26f, 0.022f); }, wood, 0.02f);
        const float head[6] = {0.88f, 0.12f, 0.66f, 0.24f, 0.76f, 0.34f};
        I.lay([&](float a, float b) { return sdPoly(a, b, head, 3); }, [&](float a, float b, float t) { return metalAt(2, a, b, t); }, 0.03f);
        for (int s = -1; s <= 1; s += 2) {
            float f[8] = {0.3f, 0.7f, 0.14f + (s < 0 ? -0.06f : 0.1f), 0.86f + (s < 0 ? -0.1f : 0.06f), 0.1f + (s < 0 ? -0.04f : 0.06f), 0.9f + (s < 0 ? -0.06f : 0.04f), 0.2f, 0.8f};
            f[2] = s < 0 ? 0.12f : 0.26f; f[3] = s < 0 ? 0.74f : 0.88f; f[4] = s < 0 ? 0.06f : 0.18f; f[5] = s < 0 ? 0.8f : 0.94f;
            I.lay([&](float a, float b) { return sdPoly(a, b, f, 4); }, [&](float a, float b, float) { return fmodf((a + b) * 40.f, 1.f) < 0.3f ? c3(0xb83a2a) : c3(0xe85a3e); }, 0.02f);
        }
        break;
    }
    case T_PBOOK: case T_EBOOK: case T_BOOK: {
        C3 cv = tile == T_EBOOK ? c3(0x6a2c9a) : (tile == T_BOOK ? c3(0x3a7a3a) : c3(0x8a4a24));
        const float pages[8] = {0.24f, 0.24f, 0.82f, 0.2f, 0.84f, 0.76f, 0.26f, 0.8f};
        const float cover[8] = {0.16f, 0.22f, 0.76f, 0.16f, 0.78f, 0.74f, 0.18f, 0.8f};
        I.lay([&](float a, float b) { return sdPoly(a, b, pages, 4); }, [&](float, float b, float) { return fmodf(b * 40.f, 1.f) < 0.35f ? c3(0xd8d0b8) : c3(0xf4ecd6); }, 0.02f);
        I.lay([&](float a, float b) { return sdPoly(a, b, cover, 4); }, [&](float a, float b, float t) {
            C3 c = cmul(cv, 0.85f + 0.25f * (1.f - (a + b) * 0.6f) + 0.05f * t);
            if (a < 0.26f) c = cmul(c, 0.7f);   // корешок
            if (fabsf(a - 0.26f) < px * 0.7f) c = cmul(c, 0.5f);
            float fx = a - 0.5f, fy = b - 0.48f;
            if (tile == T_EBOOK) {   // золотой узор и руна
                float r = sqrtf(fx * fx + fy * fy);
                if (fabsf(r - 0.14f) < 0.016f || (fabsf(fx) < 0.014f && fabsf(fy) < 0.1f) || (fabsf(fy) < 0.014f && fabsf(fx) < 0.1f)) c = c3(0xf0c850);
                if (r < 0.04f) c = c3(0xffe8a0);
            } else if (tile == T_BOOK) {   // сетка верстака
                if (fabsf(fx) < 0.12f && fabsf(fy) < 0.12f && (fabsf(fmodf(fx + 0.12f, 0.08f)) < 0.012f || fabsf(fmodf(fy + 0.12f, 0.08f)) < 0.012f)) c = c3(0xd8e8c8);
            } else if (fabsf(fy + 0.12f) < 0.03f && fabsf(fx) < 0.16f) c = c3(0xd8b060);   // ярлык
            return c; }, 0.05f);
        break;
    }
    case T_ANVIL_ITEM: {
        const float an[24] = {0.08f, 0.3f, 0.92f, 0.3f, 0.92f, 0.44f, 0.66f, 0.46f, 0.6f, 0.62f, 0.76f, 0.7f, 0.78f, 0.82f, 0.22f, 0.82f, 0.24f, 0.7f, 0.4f, 0.62f, 0.36f, 0.46f, 0.18f, 0.42f};
        I.lay([&](float a, float b) { return sdPoly(a, b, an, 12); }, [&](float, float b, float t) { C3 c = grad3(c3(0x6a6e78), c3(0x2e3036), b * 1.2f - 0.1f); if (b < 0.33f) c = c3(0x9aa0aa); return cmul(c, 0.9f + 0.1f * t); }, 0.04f);
        break;
    }
    case T_DOOR_ITEM: {
        I.lay([&](float a, float b) { return sdRBox(a, b, 0.5f, 0.5f, 0.27f, 0.44f, 0.03f); }, [&](float a, float b, float t) {
            C3 c = wood(a, b * 0.2f, t); if (fabsf(fmodf(a - 0.23f, 0.135f)) < px * 0.8f) c = cmul(c, 0.6f);   // доски
            if (fabsf(a - 0.5f) < 0.18f && b > 0.16f && b < 0.42f) { c = grad3(c3(0xb8e0f0), c3(0x5a90b0), (b - 0.16f) * 4.f); if (fabsf(a - 0.5f) < px * 0.8f || fabsf(b - 0.29f) < px * 0.8f) c = c3(0x4a3018); }
            return c; }, 0.04f);
        I.lay([&](float a, float b) { return sdCircle(a, b, 0.66f, 0.56f, 0.03f); }, c3(0xd8b040), 0.02f, 0.6f);
        break;
    }
    case T_BED_ITEM: {
        I.lay([&](float a, float b) { return std::min(sdRBox(a, b, 0.5f, 0.66f, 0.42f, 0.08f, 0.02f), std::min(sdRBox(a, b, 0.12f, 0.6f, 0.04f, 0.2f, 0.02f), sdRBox(a, b, 0.88f, 0.68f, 0.04f, 0.14f, 0.02f))); }, wood, 0.03f);
        I.lay([&](float a, float b) { return sdRBox(a, b, 0.54f, 0.52f, 0.33f, 0.1f, 0.06f); }, [&](float a, float b, float) { C3 c = grad3(c3(0xe04848), c3(0x8a2028), (b - 0.42f) * 5.f); if (fmodf(a * 12.f, 1.f) < 0.1f) c = cmul(c, 0.85f); return c; }, 0.06f);
        I.lay([&](float a, float b) { return sdRBox(a, b, 0.27f, 0.42f, 0.11f, 0.06f, 0.05f); }, c3(0xf4f2ec), 0.05f);
        break;
    }
    case T_HEART_F: case T_HEART_H: case T_HEART_E: {
        auto heart = [&](float a, float b) {   // точная форма сердца (поле расстояний)
            const float s = 0.74f; float x0 = fabsf((a - 0.5f) / s), y0 = (0.88f - b) / s;
            float d;
            if (y0 + x0 > 1.f) d = sqrtf((x0 - 0.25f) * (x0 - 0.25f) + (y0 - 0.75f) * (y0 - 0.75f)) - 0.35355f;
            else { float m = 0.5f * std::max(x0 + y0, 0.f); float d1 = x0 * x0 + (y0 - 1.f) * (y0 - 1.f), d2 = (x0 - m) * (x0 - m) + (y0 - m) * (y0 - m); d = sqrtf(std::min(d1, d2)) * (x0 - y0 > 0 ? 1.f : -1.f); }
            return d * s; };
        I.lay(heart, c3(0x2a1418), 0.05f);   // пустое сердце — тёмная подложка
        if (tile != T_HEART_E)
            I.lay([&](float a, float b) { float d = heart(a, b) + 0.06f; return tile == T_HEART_H ? std::max(d, a - 0.5f) : d; }, [&](float a, float b, float t) {
                C3 c = grad3(c3(0xff5a5a), c3(0xb01020), (b - 0.2f) * 1.4f); if (sdEll(a, b, 0.33f, 0.36f, 0.07f, 0.045f) < 0) c = cmix(c, c3(0xffffff), 0.65f); return cmul(c, 0.95f + 0.1f * t); }, 0.18f, 0.f);
        break;
    }
    case T_FOOD_F: case T_FOOD_H: case T_FOOD_E: {   // окорок на косточке
        auto meat = [&](float a, float b) { return sdEll(a, b, 0.4f, 0.44f, 0.27f, 0.23f) * 1.f; };
        auto bone = [&](float a, float b) { return std::min(sdCaps(a, b, 0.55f, 0.6f, 0.78f, 0.82f, 0.05f), std::min(sdCircle(a, b, 0.8f, 0.76f, 0.06f), sdCircle(a, b, 0.74f, 0.86f, 0.06f))); };
        if (tile == T_FOOD_E) { I.lay([&](float a, float b) { return std::min(meat(a, b), bone(a, b)); }, c3(0x2a2018), 0.05f); break; }
        I.lay(bone, c3(0xf0e8d0), 0.05f);
        I.lay([&](float a, float b) { float d = meat(a, b); return tile == T_FOOD_H ? std::max(d, a - 0.4f) : d; }, [&](float a, float b, float t) {
            C3 c = grad3(c3(0xd8803a), c3(0x8a3a14), (b - 0.2f) * 1.6f); if (sdEll(a, b, 0.3f, 0.34f, 0.08f, 0.05f) < 0) c = cmix(c, c3(0xffffff), 0.45f); return cmul(c, 0.95f + 0.08f * t); }, 0.1f);
        if (tile == T_FOOD_H) I.lay([&](float a, float b) { return std::max(meat(a, b), 0.4f - a); }, c3(0x2a2018), 0.05f);
        break;
    }
    case T_BUBBLE: {
        I.lay([&](float a, float b) { return sdCircle(a, b, 0.5f, 0.5f, 0.38f); }, [&](float a, float b, float t) { C3 c = grad3(c3(0x9ad8ff), c3(0x2a70c8), (b - 0.2f)); c = cmix(c, c3(0x0a2a60), (1.f - t) * 0.3f); if (sdCircle(a, b, 0.38f, 0.36f, 0.08f) < 0) c = c3(0xffffff); return c; }, 0.1f);
        break;
    }
    default: {
        // броня: значки в сумке (части) и на панели здоровья
        if (tile >= T_ARMOR0 && tile < T_ARMOR0 + 12) {
            int m = 2 + (tile - T_ARMOR0) / 4, part = (tile - T_ARMOR0) % 4;
            auto col = [&](float a, float b, float t) { return metalAt(m, a, b, t); };
            if (part == 0) {   // шлем: купол, козырёк, прорезь
                I.lay([&](float a, float b) { float d = sdEll(a, b, 0.5f, 0.52f, 0.36f, 0.32f); d = std::max(d, b - 0.74f); return std::max(d, -sdRBox(a, b, 0.5f, 0.66f, 0.2f, 0.1f, 0.02f)); }, col, 0.1f);
                I.lay([&](float a, float b) { return sdRBox(a, b, 0.5f, 0.44f, 0.37f, 0.04f, 0.02f); }, [&](float a, float b, float t) { return cmul(col(a, b, t), 0.85f); }, 0.03f, 0.7f);
            } else if (part == 1) {   // нагрудник
                const float ch[20] = {0.14f, 0.2f, 0.36f, 0.14f, 0.42f, 0.22f, 0.58f, 0.22f, 0.64f, 0.14f, 0.86f, 0.2f, 0.84f, 0.42f, 0.74f, 0.42f, 0.74f, 0.86f, 0.26f, 0.86f};
                const float ch2[4] = {0.26f, 0.42f, 0.16f, 0.42f};
                (void)ch2;
                I.lay([&](float a, float b) { return sdPoly(a, b, ch, 10) * 1.f + std::min(0.f, 0.f * a) ; }, [&](float a, float b, float t) { C3 c = col(a, b, t); if (fabsf(a - 0.5f) < px * 0.7f && b > 0.3f) c = cmul(c, 0.7f); if (fabsf(b - 0.64f) < px * 0.7f) c = cmul(c, 0.75f); return c; }, 0.08f);
                I.lay([&](float a, float b) { return std::min(sdRBox(a, b, 0.2f, 0.32f, 0.07f, 0.11f, 0.03f), sdRBox(a, b, 0.8f, 0.32f, 0.07f, 0.11f, 0.03f)); }, col, 0.04f);
            } else if (part == 2) {   // поножи
                I.lay([&](float a, float b) { return std::min(sdRBox(a, b, 0.5f, 0.2f, 0.3f, 0.07f, 0.02f), std::min(sdRBox(a, b, 0.33f, 0.52f, 0.12f, 0.33f, 0.03f), sdRBox(a, b, 0.67f, 0.52f, 0.12f, 0.33f, 0.03f))); },
                      [&](float a, float b, float t) { C3 c = col(a, b, t); if (b < 0.27f) c = cmul(c, 0.8f); return c; }, 0.06f);
            } else {   // ботинки
                for (int s = 0; s < 2; s++) {
                    float ox = s ? 0.52f : 0.1f;
                    I.lay([&](float a, float b) { return std::min(sdRBox(a, b, ox + 0.13f, 0.5f, 0.1f, 0.2f, 0.03f), sdRBox(a, b, ox + 0.18f, 0.74f, 0.17f, 0.08f, 0.04f)); }, col, 0.06f);
                }
            }
        } else if (tile == T_ARMHUD_F || tile == T_ARMHUD_H || tile == T_ARMHUD_E) {
            auto shield = [&](float a, float b) { const float sh[10] = {0.16f, 0.2f, 0.5f, 0.12f, 0.84f, 0.2f, 0.78f, 0.6f, 0.5f, 0.88f}; return sdPoly(a, b, sh, 5); };
            if (tile == T_ARMHUD_E) { I.lay(shield, c3(0x22242a), 0.05f); break; }
            I.lay(shield, c3(0x22242a), 0.05f);
            I.lay([&](float a, float b) { float d = shield(a, b) + 0.05f; return tile == T_ARMHUD_H ? std::max(d, a - 0.5f) : d; }, [&](float a, float b, float t) { return metalAt(2, a, b, t); }, 0.08f, 0.f);
        } else return;
    }
    }
    if (I.a > 0) { out[0] = clampf(I.c.r, 0, 1); out[1] = clampf(I.c.g, 0, 1); out[2] = clampf(I.c.b, 0, 1); out[3] = I.a; }
    else out[0] = out[1] = out[2] = out[3] = 0;
}
static void genItemTiles(int R) {
    g_iconPx = 1.f / R;
    int ss = R <= 16 ? 6 : (R <= 32 ? 4 : 3);
    std::vector<int> ts = {T_STICK, T_COAL, T_IRON_INGOT, T_GOLD_INGOT, T_DIAMOND, T_APPLE, T_RAWMEAT, T_COOKED, T_BONE, T_BONEMEAL, T_ROTTEN, T_FLUFF,
                           T_STRING, T_BOW, T_ARROW, T_PBOOK, T_EBOOK, T_BOOK, T_ANVIL_ITEM, T_DOOR_ITEM, T_BED_ITEM,
                           T_HEART_F, T_HEART_H, T_HEART_E, T_FOOD_F, T_FOOD_H, T_FOOD_E, T_BUBBLE, T_ARMHUD_F, T_ARMHUD_H, T_ARMHUD_E};
    for (int i = 0; i < 12; i++) ts.push_back(T_ARMOR0 + i);
    for (int t : {(int)T_PORK, (int)T_PORK_C, (int)T_MUTTON, (int)T_MUTTON_C, (int)T_SHEARS, (int)T_XP0, T_XP0 + 1, T_XP0 + 2, T_XP0 + 3, (int)T_SPLASH, (int)T_CRIT}) ts.push_back(t);
    for (int t : ts) paintTileSS(t, R, ss, R <= 32, [&](float x, float y, float* o) { itemSample(t, x, y, o); });
}

// кэш готового атласа на диске: высокая детализация генерируется долго — второй запуск берёт готовое
static std::string g_basePath;
static std::string atlasCachePath(int res) { return g_basePath + "buildworld_atlas_" + std::to_string(res) + ".cache"; }
static const char* atlasCacheKey() { return "BWAT3 " __DATE__ " " __TIME__; }   // меняется при каждой сборке игры
static bool loadAtlasCache(int res) {
    if (g_basePath.empty()) return false;
    FILE* f = fopen(atlasCachePath(res).c_str(), "rb");
    if (!f) return false;
    char key[64] = {0}; u32 klen = 0, r = 0, hs = 0; bool ok = false;
    if (fread(&klen, 4, 1, f) == 1 && klen < 64 && fread(key, 1, klen, f) == klen && std::string(key, klen) == atlasCacheKey() &&
        fread(&r, 4, 1, f) == 1 && fread(&hs, 4, 1, f) == 1 && (int)r == res && (int)hs == 16 * res) {
        g_hdAtlas.resize((size_t)hs * hs * 2);
        ok = fread(g_hdAtlas.data(), 4, g_hdAtlas.size(), f) == g_hdAtlas.size();
        for (int a = 0; a < 3 && ok; a++) for (int fr = 0; fr < 16 && ok; fr++) {
            u32 n = 0; ok = fread(&n, 4, 1, f) == 1 && n <= (u32)(res * res);
            if (ok) { g_hdAnim[a][fr].resize(n); ok = n == 0 || fread(g_hdAnim[a][fr].data(), 4, n, f) == n; }
        }
    }
    fclose(f);
    if (!ok) { g_hdAtlas.clear(); return false; }
    g_tileRes = res; g_hdSize = 16 * res; g_hdH = 32 * res;
    return true;
}
static void saveAtlasCache(int res) {
    if (g_basePath.empty()) return;
    for (int rr : {32, 64, 128, 256}) if (rr != res) remove(atlasCachePath(rr).c_str());   // старые кэши других разрешений не нужны
    std::string tmp = atlasCachePath(res) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    u32 klen = (u32)strlen(atlasCacheKey()), r = (u32)res, hs = (u32)g_hdSize;
    bool ok = fwrite(&klen, 4, 1, f) == 1 && fwrite(atlasCacheKey(), 1, klen, f) == klen && fwrite(&r, 4, 1, f) == 1 && fwrite(&hs, 4, 1, f) == 1 &&
              fwrite(g_hdAtlas.data(), 4, g_hdAtlas.size(), f) == g_hdAtlas.size();
    for (int a = 0; a < 3 && ok; a++) for (int fr = 0; fr < 16 && ok; fr++) { u32 n = (u32)g_hdAnim[a][fr].size(); ok = fwrite(&n, 4, 1, f) == 1 && (n == 0 || fwrite(g_hdAnim[a][fr].data(), 4, n, f) == n); }
    fclose(f);
    if (ok) rename(tmp.c_str(), atlasCachePath(res).c_str()); else remove(tmp.c_str());   // не оставляем битый кэш, если места не хватило
}
static void buildHDAtlas(int res) {
    genFishTiles(16);
    genToolTiles(16);
    genItemTiles(16);
    if (res > 16 && loadAtlasCache(res)) return;
    g_tileRes = res; g_hdSize = 16 * res; g_hdH = 32 * res;
    g_hdAtlas.assign((size_t)g_hdSize * g_hdH, 0);
    for (int y = 0; y < g_hdH; y++) for (int x = 0; x < g_hdSize; x++) g_hdAtlas[(size_t)y * g_hdSize + x] = g_atlasPx[(y * 16 / res) * ATLAS + x * 16 / res];
    if (res > 16) {
        genHDBlocks();
        genNewBlockTiles(res, false);
        genHDAnim();
        genFishTiles(res);
        genToolTiles(res);
        genItemTiles(res);
        dilateAtlas();
        saveAtlasCache(res);
    } else for (int a = 0; a < 3; a++) for (int f = 0; f < 16; f++) g_hdAnim[a][f].clear();
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
// наш шрифт 5x7 рисуется в крупную текстуру сглаженными скруглёнными штрихами (FONT_S текселей на пиксель глифа)
enum { FONT_S = 8, GLYPH_CW = 6, GLYPH_CH = 8, FONT_TEX_W = 1024, FONT_TEX_H = 512 };
static std::vector<u32> g_fontPx;
static std::unordered_map<u32, int> g_glyphIndex;

static void genFont() {
    g_fontPx.assign((size_t)FONT_TEX_W * FONT_TEX_H, 0);
    int n = (int)(sizeof(GLYPHS) / sizeof(GLYPHS[0]));
    for (int i = 0; i < n; i++) {
        auto on = [&](int x, int y) { return x >= 0 && y >= 0 && x < 5 && y < 7 && GLYPHS[i].rows[y][x] == '#'; };
        // скелет: центры пикселей и связи между соседями; диагонали — только если угол не закрыт
        std::vector<float> segs;
        for (int y = 0; y < 7; y++) for (int x = 0; x < 5; x++) {
            if (!on(x, y)) continue;
            segs.insert(segs.end(), {(float)x, (float)y, (float)x, (float)y});
            if (on(x + 1, y)) segs.insert(segs.end(), {(float)x, (float)y, x + 1.f, (float)y});
            if (on(x, y + 1)) segs.insert(segs.end(), {(float)x, (float)y, (float)x, y + 1.f});
            if (on(x + 1, y + 1) && !on(x + 1, y) && !on(x, y + 1)) segs.insert(segs.end(), {(float)x, (float)y, x + 1.f, y + 1.f});
            if (on(x - 1, y + 1) && !on(x - 1, y) && !on(x, y + 1)) segs.insert(segs.end(), {(float)x, (float)y, x - 1.f, y + 1.f});
        }
        int ox = (i % 21) * GLYPH_CW * FONT_S, oy = (i / 21) * GLYPH_CH * FONT_S;
        for (int Y = 0; Y < GLYPH_CH * FONT_S; Y++) for (int X = 0; X < GLYPH_CW * FONT_S; X++) {
            float acc = 0;
            for (int sy = 0; sy < 2; sy++) for (int sx = 0; sx < 2; sx++) {
                float gx = (X + 0.25f + sx * 0.5f) / FONT_S - 0.75f, gy = (Y + 0.25f + sy * 0.5f) / FONT_S - 0.75f;
                if (gx < -0.7f || gx > 5.2f || gy < -0.7f || gy > 7.2f) continue;   // поле вокруг глифа пустое — соседние буквы не просвечивают при сглаживании   // поле вокруг глифа пустое — соседние буквы не просвечивают при сглаживании
                float best = 9.f;
                for (size_t k = 0; k < segs.size(); k += 4) {
                    float ax = segs[k], ay = segs[k + 1], bx = segs[k + 2], by = segs[k + 3], dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
                    float t = l2 > 0 ? clampf(((gx - ax) * dx + (gy - ay) * dy) / l2, 0.f, 1.f) : 0.f;
                    float qx = ax + dx * t - gx, qy = ay + dy * t - gy;
                    // штрих чуть квадратный (норма между круглой и квадратной) — читается как пиксельный, но гладкий
                    float d = powf(powf(fabsf(qx), 3.f) + powf(fabsf(qy), 3.f), 1.f / 3.f);
                    best = std::min(best, d);
                }
                acc += clampf((0.56f - best) * FONT_S * 0.9f + 0.5f, 0.f, 1.f);
            }
            int a = (int)(acc * 0.25f * 255.f + 0.5f);
            if (a) g_fontPx[(size_t)(oy + Y) * FONT_TEX_W + ox + X] = RGBA(255, 255, 255, a);
        }
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
    SND_EAT, SND_POOF, SND_FIZZ, SND_SWING, SND_ZOMB, SND_SKEL, SND_SPID, SND_BOW, SND_ANVIL, SND_ENCH, SND_CAST, SND_BITE, SND_REEL, SND_FLOP, SND_BIRD, SND_BIRD2, SND_CRICKET, SND_CAVE, SND_WIND,
    SND_THUNDER, SND_THUNDER2, SND_PIG, SND_SHEEP, SND_CAT, SND_PURR, SND_DOG, SND_WHINE, SND_XP, SND_LEVEL, SND_SHEAR, SND_CRIT, SND_GROWL,
    SND_MATVAR,                     // ещё по 2 варианта каждого звука материала (шаги, копание, разрушение)
    SND_COUNT = SND_MATVAR + 36
};
static std::vector<float> g_snd[SND_COUNT];
struct Voice { int snd; float pos, rate, vol; bool on; };
static Voice g_voices[MAX_VOICES];
static SDL_AudioDeviceID g_audioDev = 0;
static float g_masterVol = 0.8f;

static void audioCallback(void*, Uint8* stream, int len);
// ---------------------------------------------------------------- музыка: спокойные процедурные пьесы (фортепиано, пэд, бас, реверберация)
// настроение: 0 — день (мажор), 1 — вечер (минор), 2 — ночь/пещеры (медленно, дорийский лад)
static std::vector<float> g_musicBuf;        // играет сейчас (читает аудиопоток)
static size_t g_musicPos = 0;
static float g_musicVol = 0.6f, g_musicFade = 1.f;
static std::vector<float> g_musicNext;       // готовится в отдельном потоке
static std::atomic<int> g_musicReady(0);     // 0 — нет, 1 — готовится, 2 — готово
static std::thread g_musicThread;
static float g_musicWait = 4.f;              // пауза до следующей пьесы, с
static int g_musicMood = 0;
static std::atomic<int> g_underwaterAudio(0);
static float g_lpState = 0.f, g_lpK = 1.f;
static void genMusic(int mood, u32 seed, std::vector<float>& out) {
    Rng r(seed);
    const float bpm = mood == 2 ? 58.f : (mood == 1 ? 66.f : 74.f);
    const float beat = 60.f / bpm;
    const int bars = 16, bpb = 4;
    const float len = bars * bpb * beat + 6.f;
    out.assign((size_t)(len * AUDIO_RATE), 0.f);
    // аккорды: ступени от тоники (полутоны) и тип (0 мажор, 1 минор)
    static const int PROG[3][4][2] = {{{0, 0}, {7, 0}, {9, 1}, {5, 0}}, {{9, 1}, {5, 0}, {0, 0}, {7, 0}}, {{2, 1}, {9, 1}, {10, 0}, {5, 0}}};
    const int root = 48 + (int)(seed % 3) * 2;   // до/ре/ми — разные пьесы в разных тональностях
    auto hz = [](float midi) { return 440.f * powf(2.f, (midi - 69.f) / 12.f); };
    auto addNote = [&](float t0, float dur, float midi, float amp, int timbre) {
        float f = hz(midi);
        int s0 = (int)(t0 * AUDIO_RATE), n = (int)((dur + (timbre == 1 ? 1.5f : 1.2f)) * AUDIO_RATE);
        float ph1 = 0, ph2 = 0, ph3 = 0, ph4 = 0;
        float detune = 1.f + (r.f() - 0.5f) * 0.004f;
        for (int i = 0; i < n && s0 + i < (int)out.size(); i++) {
            float t = i / (float)AUDIO_RATE, env;
            float v;
            if (timbre == 0) {   // фортепиано: быстрая атака, затухание, верхние гармоники гаснут быстрее
                env = (1.f - expf(-t * 300.f)) * expf(-t * 1.6f) * (t > dur ? expf(-(t - dur) * 6.f) : 1.f);
                v = sinf(ph1) + 0.45f * sinf(ph2) * expf(-t * 2.5f) + 0.18f * sinf(ph3) * expf(-t * 4.f) + 0.08f * sinf(ph4) * expf(-t * 7.f);
            } else if (timbre == 1) {   // пэд: медленная атака, мягкий тембр, лёгкое вибрато
                float a = std::min(1.f, t / 0.9f), rel = t > dur ? expf(-(t - dur) * 2.2f) : 1.f;
                env = a * a * rel;
                float vib = 1.f + 0.003f * sinf(t * 5.1f);
                v = sinf(ph1 * vib) + 0.3f * sinf(ph2 * vib * 1.002f) + 0.12f * sinf(ph3);
            } else {   // бас: тёплый, с коротким щипком
                env = (1.f - expf(-t * 120.f)) * expf(-t * 1.1f) * (t > dur ? expf(-(t - dur) * 8.f) : 1.f);
                v = sinf(ph1) + 0.25f * sinf(ph2);
            }
            out[s0 + i] += v * env * amp;
            float w = 2.f * (float)M_PI * f / AUDIO_RATE;
            ph1 += w * detune; ph2 += w * 2.f; ph3 += w * 3.f; ph4 += w * 4.01f;
        }
    };
    static const int PENTA[2][5] = {{0, 2, 4, 7, 9}, {0, 3, 5, 7, 10}};
    int mel = 2;   // индекс в пентатонике (с октавами)
    for (int bar = 0; bar < bars; bar++) {
        const int* ch = PROG[mood][bar % 4];
        float t0 = 3.f + bar * bpb * beat;
        int cr = root + ch[0];
        int third = ch[1] ? 3 : 4;
        // пэд — аккорд на весь такт
        float padAmp = mood == 2 ? 0.05f : 0.042f;
        addNote(t0, bpb * beat, (float)(cr), padAmp, 1); addNote(t0, bpb * beat, (float)(cr + third), padAmp, 1); addNote(t0, bpb * beat, (float)(cr + 7), padAmp, 1);
        // бас — на первую и третью доли
        addNote(t0, beat * 1.8f, (float)(cr - 12), 0.11f, 2);
        if (r.range(100) < 60) addNote(t0 + 2 * beat, beat * 1.5f, (float)(cr - 12 + (r.range(2) ? 7 : 0)), 0.08f, 2);
        // мелодия: случайное блуждание по пентатонике, иногда паузы и восьмые
        if (bar >= 2 && bar < bars - 1) {
            int scale = (mood == 0) ? 0 : 1;
            for (int b = 0; b < bpb; b++) {
                int roll = r.range(100);
                if (roll < (mood == 2 ? 45 : 30)) continue;   // пауза
                int steps = roll < 80 ? 1 : 2;
                for (int s = 0; s < steps; s++) {
                    mel += r.range(5) - 2; mel = clampi(mel, 0, 9);
                    int deg = PENTA[scale][mel % 5] + (mel / 5) * 12;
                    int key = mood == 1 ? root + 9 - 12 : (mood == 2 ? root + 2 - 12 : root);
                    float tt = t0 + (b + s * 0.5f) * beat + (r.f() - 0.5f) * 0.02f;
                    addNote(tt, beat * (steps == 1 ? 0.9f : 0.45f), (float)(key + 12 + deg), 0.15f * (0.8f + r.f() * 0.3f), 0);
                    if (r.range(100) < 18) addNote(tt, beat * 0.9f, (float)(key + deg), 0.06f, 0);   // октава ниже — глубже звучит
                }
            }
        }
    }
    // финальный аккорд
    { float t0 = 3.f + bars * bpb * beat; addNote(t0, 2.5f, (float)root, 0.05f, 1); addNote(t0, 2.5f, (float)root + 7, 0.05f, 1); addNote(t0, 1.5f, (float)root + 12, 0.1f, 0); }
    // реверберация: 4 гребенчатых фильтра + 2 всепропускающих (Шрёдер)
    {
        std::vector<float> wet(out.size(), 0.f);
        const int CD[4] = {1116, 1188, 1277, 1356}; const float fb = 0.82f;
        for (int c = 0; c < 4; c++) {
            int d = CD[c] * AUDIO_RATE / 44100; std::vector<float> buf(d, 0.f); int p = 0; float lp = 0;
            for (size_t i = 0; i < out.size(); i++) { float y = buf[p]; lp = y * 0.6f + lp * 0.4f; buf[p] = out[i] + lp * fb; p = (p + 1) % d; wet[i] += y * 0.25f; }
        }
        const int AD[2] = {556, 441};
        for (int a = 0; a < 2; a++) {
            int d = AD[a] * AUDIO_RATE / 44100; std::vector<float> buf(d, 0.f); int p = 0;
            for (size_t i = 0; i < wet.size(); i++) { float b = buf[p], y = -wet[i] + b; buf[p] = wet[i] + b * 0.5f; p = (p + 1) % d; wet[i] = y; }
        }
        for (size_t i = 0; i < out.size(); i++) out[i] = out[i] * 0.75f + wet[i] * 0.45f;
    }
    // нормировка и плавные края
    float mx = 0; for (float v : out) mx = std::max(mx, fabsf(v));
    float k = mx > 0 ? 0.55f / mx : 1.f;
    for (size_t i = 0; i < out.size(); i++) {
        float t = i / (float)AUDIO_RATE, fade = std::min(1.f, std::min(t / 2.f, (len - t) / 3.f));
        out[i] *= k * std::max(0.f, fade);
    }
}
static void musicStartGen(int mood) {
    if (g_musicReady.load() != 0) return;
    if (g_musicThread.joinable()) g_musicThread.join();
    g_musicReady = 1; g_musicMood = mood;
    u32 seed = (u32)SDL_GetTicks() * 2654435761u + (u32)mood;
    g_musicThread = std::thread([mood, seed]() { genMusic(mood, seed, g_musicNext); g_musicReady = 2; });
}
// вызывается каждый кадр: выбирает настроение, запускает следующую пьесу после паузы
static void updateMusic(float dt, int moodWanted, bool inMenu) {
    if (!g_audioDev) return;
    bool playing;
    SDL_LockAudioDevice(g_audioDev); playing = g_musicPos < g_musicBuf.size(); SDL_UnlockAudioDevice(g_audioDev);
    if (g_musicReady.load() == 2) {
        if (g_musicThread.joinable()) g_musicThread.join();
        SDL_LockAudioDevice(g_audioDev);
        g_musicBuf.swap(g_musicNext); g_musicPos = 0; g_musicFade = 1.f;
        SDL_UnlockAudioDevice(g_audioDev);
        g_musicNext.clear(); g_musicNext.shrink_to_fit();
        g_musicReady = 0;
        g_musicWait = (inMenu ? 15.f : 70.f) + (SDL_GetTicks() % 90);   // пауза после пьесы, как в больших играх: музыка не надоедает
        return;
    }
    if (playing || g_musicReady.load() != 0) return;
    if (!g_musicBuf.empty()) { SDL_LockAudioDevice(g_audioDev); g_musicBuf.clear(); g_musicBuf.shrink_to_fit(); g_musicPos = 0; SDL_UnlockAudioDevice(g_audioDev); }
    g_musicWait -= dt;
    if (g_musicWait <= 0 && g_musicVol > 0.01f) musicStartGen(moodWanted);
}
static void stopMusicThread() { if (g_musicThread.joinable()) g_musicThread.join(); }

static std::atomic<float> g_rainVol{0.f};   // громкость шума дождя (смешивается в звуковом потоке)
static u32 g_rainSeed = 12345; static float g_rnL = 0, g_rnH = 0, g_rnL2 = 0, g_dripE = 0, g_dripPh = 0, g_dripF = 0.2f;
static void audioCallback(void*, Uint8* stream, int len) {
    Sint16* out = (Sint16*)stream;
    int n = len / 2;
    const float rv = g_rainVol.load();
    float lpTarget = g_underwaterAudio.load() ? 0.12f : 1.f;   // под водой — глухой звук (фильтр низких частот)
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
        if (g_musicPos < g_musicBuf.size()) { s += g_musicBuf[g_musicPos++] * g_musicVol * g_musicFade; }
        if (rv > 0.0005f) {   // дождь: шуршащий шум двух полос и редкая капель
            g_rainSeed = g_rainSeed * 1664525u + 1013904223u;
            float nz = (float)((g_rainSeed >> 9) & 0x7FFF) / 16384.f - 1.f;
            g_rnL += (nz - g_rnL) * 0.32f; g_rnL2 += (nz - g_rnL2) * 0.05f;
            float hp = g_rnL - g_rnH; g_rnH += (g_rnL - g_rnH) * 0.03f;
            if (((g_rainSeed >> 4) & 2047) == 0) { g_dripE = 1.f; g_dripF = 0.12f + ((g_rainSeed >> 16) & 255) / 255.f * 0.25f; }
            g_dripE *= 0.9975f; g_dripPh += g_dripF; if (g_dripPh > 6.2831853f) g_dripPh -= 6.2831853f;
            s += (hp * 0.55f + g_rnL2 * 0.35f + g_dripE * sinf(g_dripPh) * 0.12f) * rv;
        }
        g_lpK += (lpTarget - g_lpK) * 0.0005f;
        g_lpState += (s - g_lpState) * g_lpK; s = g_lpState;
        s *= g_masterVol;
        s = s / (1.f + fabsf(s) * 0.3f);   // мягкий лимитер
        int iv = (int)(s * 30000.f);
        out[i] = (Sint16)clampi(iv, -32767, 32767);
    }
}

static u32 g_sndCounter = 0;
static void playSound(int snd, float vol = 1.f, float pitch = 1.f) {
    if (snd >= 0 && snd < 18) {   // звуки материалов: случайный из трёх вариантов, без повторов подряд
        static int last[18] = {0};
        int v = (int)(hash32(++g_sndCounter * 2654435761u) % 3);
        if (v == last[snd]) v = (v + 1) % 3;
        last[snd] = v;
        if (v > 0) snd = SND_MATVAR + snd * 2 + (v - 1);
    }
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

// ---- звуки материалов: «зёрна» отфильтрованного шума + удар + резонансы
struct MatSynth {
    std::vector<float> b; Rng r;
    MatSynth(float sec, u32 seed) : b((size_t)(sec * AUDIO_RATE), 0.f), r(seed) {}
    // короткая вспышка шума через полосовой фильтр (биквад)
    void grain(float t0, float f, float q, float decay, float amp) {
        f = clampf(f, 60.f, AUDIO_RATE * 0.45f);
        float w = 2.f * (float)M_PI * f / AUDIO_RATE, al = sinf(w) / (2.f * q), a0 = 1 + al;
        float b0 = al / a0, b2 = -al / a0, a1 = -2 * cosf(w) / a0, a2 = (1 - al) / a0;
        float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        int s0 = (int)(t0 * AUDIO_RATE), n = (int)(decay * 7.f * AUDIO_RATE);
        for (int i = 0; i < n && s0 + i < (int)b.size(); i++) {
            float t = i / (float)AUDIO_RATE;
            float env = (t < 0.0008f ? t / 0.0008f : 1.f) * expf(-t / decay);
            float x = r.sf() * env;
            float y = b0 * x + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x; y2 = y1; y1 = y;
            b[s0 + i] += y * amp * 3.f;
        }
    }
    // глухой удар/резонанс: синус с падающей частотой и затуханием
    void thump(float t0, float f0, float f1, float decay, float amp) {
        int s0 = (int)(t0 * AUDIO_RATE), n = (int)(decay * 6.f * AUDIO_RATE); float ph = 0;
        for (int i = 0; i < n && s0 + i < (int)b.size(); i++) {
            float t = i / (float)AUDIO_RATE, f = f1 + (f0 - f1) * expf(-t / (decay * 0.6f));
            ph += f / AUDIO_RATE;
            b[s0 + i] += sinf(ph * 6.2831853f) * amp * (t < 0.001f ? t / 0.001f : 1.f) * expf(-t / decay);
        }
    }
    void normalize(float peak) {
        float m = 1e-6f; for (float v : b) m = std::max(m, fabsf(v));
        for (float& v : b) v *= peak / m;
        int fade = std::min((int)b.size(), (int)(0.004f * AUDIO_RATE));   // без щелчка в конце
        for (int i = 0; i < fade; i++) b[b.size() - 1 - i] *= i / (float)fade;
    }
};
// kind: 0 — шаг, 1 — удар при копании, 2 — разрушение блока
static std::vector<float> makeMatSound(int kind, int mat, u32 seed) {
    float len = kind == 0 ? 0.2f : (kind == 1 ? 0.22f : 0.5f);
    MatSynth s(len, seed * 7919u + kind * 131u + mat * 17u);
    Rng& r = s.r;
    auto rf = [&](float a, float b) { return a + (b - a) * r.f(); };
    switch (mat) {
        case SM_GRASS: {   // шорох травы и хруст земли
            int n = kind == 0 ? 12 : (kind == 1 ? 10 : 34); float span = kind == 0 ? 0.09f : (kind == 1 ? 0.07f : 0.3f);
            for (int i = 0; i < n; i++) { float t = span * powf(r.f(), 1.4f); s.grain(t, rf(1300, 3600), 1.2f, rf(0.004f, 0.011f), rf(0.2f, 0.45f) * (1.f - t / (span * 1.3f))); }
            for (int i = 0; i < n / 2; i++) s.grain(span * r.f(), rf(350, 900), 1.5f, rf(0.008f, 0.016f), rf(0.2f, 0.35f));
            s.thump(0, 95, 55, kind == 2 ? 0.05f : 0.025f, kind == 0 ? 0.35f : 0.45f);
            break;
        }
        case SM_SAND: {    // сыпучий шелест
            int n = kind == 0 ? 22 : (kind == 1 ? 18 : 60); float span = kind == 0 ? 0.13f : (kind == 1 ? 0.1f : 0.38f);
            for (int i = 0; i < n; i++) { float t = span * r.f(); s.grain(t, rf(2200, 5000), 0.9f, rf(0.006f, 0.016f), rf(0.12f, 0.3f) * (1.f - t / (span * 1.2f))); }
            s.thump(0, 70, 45, 0.03f, 0.2f);
            break;
        }
        case SM_SNOW: {    // плотный хруст
            int n = kind == 0 ? 18 : (kind == 1 ? 14 : 45); float span = kind == 0 ? 0.11f : (kind == 1 ? 0.08f : 0.32f);
            for (int i = 0; i < n; i++) { float t = span * r.f(); s.grain(t, rf(1400, 3200), 2.2f, rf(0.003f, 0.008f), rf(0.25f, 0.5f)); }
            s.thump(0, 900, 760, 0.012f, 0.06f);   // лёгкий скрип
            s.thump(0, 80, 50, 0.025f, 0.25f);
            break;
        }
        case SM_WOOD: {    // полый деревянный стук
            float base = rf(170, 215);
            int knocks = kind == 2 ? 3 : 1;
            for (int k = 0; k < knocks; k++) {
                float t = k * rf(0.06f, 0.1f), a = (kind == 0 ? 0.35f : 0.6f) * (1.f - k * 0.25f);
                s.thump(t, base * 1.0f, base * 0.97f, 0.045f, a); s.thump(t, base * 2.4f, base * 2.35f, 0.025f, a * 0.55f);
                s.thump(t, base * 4.1f, base * 4.0f, 0.014f, a * 0.3f);
                s.grain(t, rf(1600, 2600), 2.f, 0.004f, a * 0.8f);
            }
            if (kind == 2) for (int i = 0; i < 16; i++) s.grain(rf(0.02f, 0.35f), rf(2500, 5000), 1.5f, rf(0.003f, 0.008f), rf(0.1f, 0.25f));   // щепки
            break;
        }
        case SM_GLASS: {   // звон
            if (kind == 0) { s.grain(0, rf(3500, 4500), 3.f, 0.004f, 0.5f); s.thump(0, 110, 80, 0.02f, 0.3f); s.thump(0.002f, rf(3200, 3800), 3500, 0.03f, 0.08f); }
            else {
                int n = kind == 1 ? 3 : 26; float span = kind == 1 ? 0.02f : 0.3f;
                for (int i = 0; i < n; i++) { float t = span * powf(r.f(), 1.6f), f = rf(2600, 7000); s.thump(t, f, f * 0.995f, rf(0.02f, 0.07f), rf(0.08f, 0.2f)); s.grain(t, f, 4.f, 0.003f, 0.15f); }
                s.grain(0, 2500, 0.7f, 0.012f, 0.7f);
            }
            break;
        }
        default: {         // камень: щелчок + глухой удар + каменная крошка
            float click = rf(1600, 2300);
            float a = kind == 0 ? 0.5f : 0.8f;
            s.grain(0, click, 3.f, kind == 0 ? 0.003f : 0.005f, a);
            s.grain(0.001f, click * 0.55f, 2.f, 0.006f, a * 0.6f);
            s.thump(0, kind == 0 ? 120 : 160, 70, kind == 0 ? 0.02f : 0.035f, kind == 0 ? 0.45f : 0.6f);
            int n = kind == 0 ? 3 : (kind == 1 ? 6 : 42); float span = kind == 2 ? 0.4f : 0.06f;
            for (int i = 0; i < n; i++) {   // крошка, сыплется всё реже
                float t = kind == 2 ? 0.02f + span * powf(r.f(), 1.8f) : rf(0.005f, span);
                s.grain(t, rf(900, 4200), 1.8f, rf(0.002f, 0.007f), rf(0.12f, 0.35f) * (kind == 2 ? (1.f - t / (span * 1.2f)) : 1.f));
            }
            if (kind == 2) { s.grain(0, 1400, 0.7f, 0.02f, 0.9f); s.thump(0, 90, 50, 0.08f, 0.55f); }
            break;
        }
    }
    s.normalize(kind == 0 ? 0.55f : (kind == 1 ? 0.7f : 0.85f));
    return s.b;
}

static void softenStep(std::vector<float>& b) {   // шаги: убрать резкие верха и щелчок в начале
    float a = 0, z = 0;
    for (size_t i = 0; i < b.size(); i++) { a += (b[i] - a) * 0.22f; z += (a - z) * 0.35f; b[i] = z; }
    size_t fade = std::min<size_t>(b.size(), AUDIO_RATE / 200);
    for (size_t i = 0; i < fade; i++) b[i] *= i / (float)fade;
    float mx = 0; for (float v : b) mx = std::max(mx, fabsf(v));
    if (mx > 0) for (float& v : b) v *= 0.4f / mx;
}
static void genSounds() {
    for (int kind = 0; kind < 3; kind++) for (int m = 0; m < 6; m++) {
        int base = kind * 6 + m;
        g_snd[base] = makeMatSound(kind, m, 1);
        g_snd[SND_MATVAR + base * 2] = makeMatSound(kind, m, 2);
        g_snd[SND_MATVAR + base * 2 + 1] = makeMatSound(kind, m, 3);
        if (kind == 0) { softenStep(g_snd[base]); softenStep(g_snd[SND_MATVAR + base * 2]); softenStep(g_snd[SND_MATVAR + base * 2 + 1]); }
    }
    { Synth s(0.18f, 40); s.tone(0, 0.15f, 140, 70, 0.6f, 0.002f, 0.04f); s.noise(0, 0.1f, 0.4f, 0.3f, 0.01f, 0.001f, 0.025f); g_snd[SND_PLACE] = s.b; }
    { Synth s(0.14f, 41); s.tone(0, 0.12f, 500, 1300, 0.4f, 0.003f, 0.05f); g_snd[SND_PICKUP] = s.b; }
    { Synth s(0.2f, 42); s.noise(0, 0.2f, 0.9f, 0.4f, 0.02f, 0.001f, 0.04f); s.tone(0, 0.15f, 160, 60, 0.5f, 0.001f, 0.05f); g_snd[SND_HIT] = s.b; }
    { Synth s(0.3f, 43); s.tone(0, 0.3f, 320, 180, 0.4f, 0.005f, 0.1f, 1); s.noise(0, 0.15f, 0.3f, 0.3f, 0.02f, 0.002f, 0.05f); g_snd[SND_HURT] = s.b; }
    { Synth s(0.06f, 44); s.tone(0, 0.06f, 1200, 900, 0.35f, 0.001f, 0.015f, 2); g_snd[SND_CLICK] = s.b; }
    { Synth s(0.5f, 45); s.noise(0, 0.5f, 0.8f, 0.5f, 0.05f, 0.01f, 0.12f); g_snd[SND_SPLASH] = s.b; }
    { Synth s(0.6f, 46); s.tone(0, 0.6f, 95, 70, 0.6f, 0.05f, 0.25f, 1); s.tone(0, 0.6f, 190, 140, 0.2f, 0.05f, 0.2f); g_snd[SND_TOPTUN] = s.b; }
    { MatSynth s(0.5f, 47);   // утка: два носовых «кря»
      for (int q = 0; q < 2; q++) { float t0 = q * 0.22f;
          for (int h = 1; h <= 6; h++) s.thump(t0, 330.f * h * 1.02f, 300.f * h, 0.055f, 0.32f / h * (h == 3 || h == 4 ? 2.2f : 1.f));
          s.grain(t0, 1400, 3.f, 0.03f, 0.35f); s.grain(t0, 2600, 4.f, 0.02f, 0.2f); }
      s.normalize(0.6f); g_snd[SND_PUKH] = s.b; }
    { MatSynth s(0.9f, 48);   // наковальня: звонкий металлический удар с негармоническими обертонами
      const float fr[5] = {620, 1480, 2390, 3170, 4420}, dc[5] = {0.28f, 0.2f, 0.14f, 0.1f, 0.06f};
      for (int i = 0; i < 5; i++) s.thump(0, fr[i] * 1.01f, fr[i], dc[i], 0.5f / (1 + i * 0.4f));
      s.grain(0, 3000, 2.f, 0.004f, 0.8f); s.normalize(0.7f); g_snd[SND_ANVIL] = s.b; }
    { MatSynth s(1.0f, 49);   // зачарование: мерцающий перелив
      for (int i = 0; i < 9; i++) { float f = 880.f * powf(1.12246f, (float)((i * 5) % 12)); s.thump(i * 0.07f, f, f, 0.18f, 0.25f); }
      s.normalize(0.5f); g_snd[SND_ENCH] = s.b; }
    { Synth s(0.45f, 60);   // заброс: свист удилища и шуршание лески
      s.noise(0, 0.4f, 0.55f, 0.18f, 0.03f, 0.09f, 0.12f); s.noise(0.05f, 0.45f, 0.25f, 0.6f, 0.2f, 0.02f, 0.15f);
      s.tone(0.02f, 0.4f, 1800, 700, 0.06f, 0.05f, 0.15f, 1); g_snd[SND_CAST] = s.b; }
    { MatSynth m(0.5f, 61);   // поклёвка: «бульк» — пузырь с восходящим тоном и всплеск
      m.thump(0, 260, 820, 0.09f, 0.6f); m.thump(0.06f, 420, 1100, 0.06f, 0.3f);
      for (int i = 0; i < 10; i++) m.grain(0.03f + i * 0.02f, 1800.f + 300.f * (i % 4), 2.f, 0.006f, 0.25f * (1.f - i / 10.f));
      m.normalize(0.6f); g_snd[SND_BITE] = m.b;
      Synth s2(0.5f, 62); s2.noise(0, 0.45f, 0.45f, 0.5f, 0.06f, 0.01f, 0.1f); for (size_t i = 0; i < s2.b.size() && i < g_snd[SND_BITE].size(); i++) g_snd[SND_BITE][i] += s2.b[i] * 0.5f; }
    { MatSynth m(0.45f, 63);   // катушка: трещотка
      for (int i = 0; i < 14; i++) { float t = i * 0.03f; m.grain(t, 3200.f + (i & 1) * 400.f, 3.f, 0.003f, 0.55f); m.thump(t, 900, 700, 0.01f, 0.12f); }
      m.normalize(0.45f); g_snd[SND_REEL] = m.b; }
    { Synth s(0.6f, 71);   // птица: две-три быстрые трели
      for (int i = 0; i < 3; i++) { float t = i * 0.16f; s.tone(t, t + 0.11f, 2900.f + i * 250.f, 3800.f + i * 150.f, 0.35f, 0.01f, 0.05f, 0); s.tone(t + 0.05f, t + 0.1f, 4200.f, 3300.f, 0.2f, 0.005f, 0.03f, 0); }
      g_snd[SND_BIRD] = s.b; }
    { Synth s(0.8f, 72);   // другая птица: свист вверх-вниз
      s.tone(0, 0.28f, 2200, 3400, 0.4f, 0.03f, 0.08f, 0); s.tone(0.32f, 0.7f, 3600, 2400, 0.35f, 0.03f, 0.15f, 0); g_snd[SND_BIRD2] = s.b; }
    { Synth s(1.2f, 73);   // сверчок: пачки быстрых щелчков около 4.5 кГц
      for (int b = 0; b < 3; b++) for (int i = 0; i < 6; i++) { float t = b * 0.38f + i * 0.028f; s.tone(t, t + 0.018f, 4500, 4450, 0.22f, 0.002f, 0.01f, 0); }
      g_snd[SND_CRICKET] = s.b; }
    { Synth s(3.5f, 74);   // пещера: низкий гул и далёкая капель
      s.noise(0, 3.5f, 0.35f, 0.02f, 0.f, 0.8f, 1.5f); s.tone(0, 3.5f, 55, 48, 0.25f, 0.8f, 1.5f, 0);
      s.tone(1.6f, 1.75f, 1400, 2200, 0.2f, 0.002f, 0.12f, 0); g_snd[SND_CAVE] = s.b; }
    { Synth s(3.f, 75);   // ветер: мягкий шум, медленно нарастает и стихает
      s.noise(0, 3.f, 0.45f, 0.05f, 0.01f, 1.2f, 1.4f); g_snd[SND_WIND] = s.b; }
    { MatSynth m(0.25f, 64);   // рыба бьётся: мокрый шлепок
      m.thump(0, 190, 80, 0.04f, 0.7f); m.grain(0, 1300, 1.2f, 0.02f, 0.6f); m.grain(0.01f, 2600, 2.f, 0.01f, 0.3f);
      m.normalize(0.55f); g_snd[SND_FLOP] = m.b; }
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
    // гром: близкий — резкий треск и раскатистый гул; дальний — только глухое ворчание
    { Synth s(4.5f, 80);
      s.noise(0, 0.35f, 1.1f, 0.85f, 0.25f, 0.002f, 0.06f);                         // треск разряда
      s.noise(0.02f, 0.6f, 0.8f, 0.3f, 0.02f, 0.01f, 0.18f);
      for (int k = 0; k < 6; k++) { float t0 = 0.15f + k * 0.45f + (k % 2) * 0.17f; s.noise(t0, t0 + 1.6f, 0.9f - k * 0.1f, 0.025f, 0.f, 0.12f, 0.45f + k * 0.08f); }   // раскаты
      s.tone(0.1f, 4.4f, 48, 34, 0.35f, 0.25f, 1.4f, 0);
      float mx = 0; for (float v : s.b) mx = std::max(mx, fabsf(v)); if (mx > 0) for (float& v : s.b) v *= 0.9f / mx;
      g_snd[SND_THUNDER] = s.b; }
    { Synth s(4.f, 81);
      for (int k = 0; k < 5; k++) { float t0 = k * 0.55f + (k % 2) * 0.2f; s.noise(t0, t0 + 1.8f, 0.8f - k * 0.1f, 0.015f, 0.f, 0.3f, 0.6f); }
      s.tone(0, 3.9f, 40, 30, 0.3f, 0.5f, 1.2f, 0);
      float mx = 0; for (float v : s.b) mx = std::max(mx, fabsf(v)); if (mx > 0) for (float& v : s.b) v *= 0.75f / mx;
      g_snd[SND_THUNDER2] = s.b; }
    // свинья: два носовых похрюкивания
    { MatSynth m(0.55f, 82);
      for (int q = 0; q < 2; q++) { float t0 = q * 0.21f;
          for (int h = 1; h <= 7; h++) m.thump(t0, 150.f * h * 1.05f, 120.f * h, 0.09f, 0.3f / h * (h == 3 || h == 5 ? 2.f : 1.f));
          m.grain(t0, 700, 2.f, 0.07f, 0.45f); m.grain(t0 + 0.02f, 1500, 3.f, 0.05f, 0.25f); }
      m.normalize(0.6f); g_snd[SND_PIG] = m.b; }
    // овца: дрожащее «бее»
    { Synth s(0.75f, 83);
      for (int h = 1; h <= 5; h++) {
          int a = 0, e = (int)(0.7f * AUDIO_RATE); float ph = 0;
          for (int i = a; i < e && i < (int)s.b.size(); i++) {
              float t = i / (float)AUDIO_RATE;
              float f = (420.f + 25.f * sinf(t * 2 * 3.14159f * 9.f)) * h * (1.f - t * 0.12f);   // вибрато
              ph += f / AUDIO_RATE; ph -= floorf(ph);
              float env = std::min(1.f, t / 0.05f) * expf(-t * 2.6f) * (0.75f + 0.25f * sinf(t * 2 * 3.14159f * 9.f));
              float form = (h == 2 || h == 3) ? 1.f : 0.45f;
              s.b[i] += sinf(ph * 6.2831853f) * 0.22f * form / h * env * 2.f;
          }
      }
      s.noise(0, 0.6f, 0.08f, 0.5f, 0.1f, 0.03f, 0.3f);
      g_snd[SND_SHEEP] = s.b; }
    // кошка: «мяу» — тон вверх и вниз с носовой окраской
    { Synth s(0.6f, 84);
      for (int h = 1; h <= 6; h++) {
          float ph = 0;
          for (int i = 0; i < (int)(0.55f * AUDIO_RATE); i++) {
              float t = i / (float)AUDIO_RATE, k = t / 0.55f;
              float f = (520.f + 380.f * sinf(k * 3.14159f) - 120.f * k) * h;
              ph += f / AUDIO_RATE; ph -= floorf(ph);
              float env = std::min(1.f, t / 0.04f) * std::min(1.f, (0.55f - t) / 0.12f);
              float form = h == 2 ? 1.f : (h == 3 ? 0.7f : 0.35f);
              s.b[i] += sinf(ph * 6.2831853f) * 0.2f * form * env;
          }
      }
      g_snd[SND_CAT] = s.b; }
    // мурлыканье: низкий шум с частыми толчками
    { Synth s(1.6f, 85);
      Rng r(86); float l = 0;
      for (size_t i = 0; i < s.b.size(); i++) {
          float t = i / (float)AUDIO_RATE;
          l += (r.sf() - l) * 0.04f;
          float pulse = powf(0.5f + 0.5f * sinf(t * 2 * 3.14159f * 26.f), 3.f);
          float breath = 0.6f + 0.4f * sinf(t * 2 * 3.14159f * 0.62f);
          float env = std::min(1.f, t / 0.15f) * std::min(1.f, (1.6f - t) / 0.2f);
          s.b[i] = l * pulse * breath * env * 3.f;
      }
      g_snd[SND_PURR] = s.b; }
    // собака: короткий звонкий лай
    { MatSynth m(0.3f, 87);
      for (int h = 1; h <= 6; h++) m.thump(0, 460.f * h, 330.f * h, 0.07f, 0.4f / h * (h == 2 ? 1.6f : 1.f));
      m.grain(0, 1200, 1.5f, 0.06f, 0.6f); m.grain(0.01f, 2500, 2.f, 0.04f, 0.3f);
      m.normalize(0.7f); g_snd[SND_DOG] = m.b; }
    { Synth s(0.7f, 88); s.tone(0, 0.65f, 1100, 1500, 0.18f, 0.05f, 0.25f, 0); s.tone(0, 0.65f, 2200, 2900, 0.07f, 0.05f, 0.2f, 0); g_snd[SND_WHINE] = s.b; }
    { Synth s(0.9f, 89); s.tone(0, 0.9f, 140, 110, 0.4f, 0.1f, 0.4f, 1); s.noise(0, 0.9f, 0.25f, 0.06f, 0.f, 0.1f, 0.4f); g_snd[SND_GROWL] = s.b; }
    // опыт: светлый перезвон; новый уровень — восходящее трезвучие
    { MatSynth m(0.35f, 90); m.thump(0, 1760, 1760, 0.12f, 0.35f); m.thump(0.0f, 2637, 2637, 0.08f, 0.15f); m.normalize(0.45f); g_snd[SND_XP] = m.b; }
    { MatSynth m(1.2f, 91);
      const float f[4] = {523.f, 659.f, 784.f, 1047.f};
      for (int i = 0; i < 4; i++) { m.thump(i * 0.11f, f[i], f[i], 0.5f, 0.3f); m.thump(i * 0.11f, f[i] * 2.f, f[i] * 2.f, 0.25f, 0.08f); }
      m.normalize(0.55f); g_snd[SND_LEVEL] = m.b; }
    // ножницы: два щелчка лезвий
    { MatSynth m(0.3f, 92); for (int q = 0; q < 2; q++) { m.grain(q * 0.12f, 4200, 3.f, 0.006f, 0.6f); m.thump(q * 0.12f, 2600, 2400, 0.02f, 0.2f); } m.normalize(0.5f); g_snd[SND_SHEAR] = m.b; }
    // критический удар: хлёсткий удар со звоном
    { MatSynth m(0.35f, 93); m.thump(0, 180, 60, 0.05f, 0.8f); m.grain(0, 2200, 1.5f, 0.02f, 0.6f); m.thump(0.005f, 3100, 2900, 0.12f, 0.25f); m.thump(0.005f, 4700, 4500, 0.08f, 0.12f);
      m.normalize(0.7f); g_snd[SND_CRIT] = m.b; }
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
    u8 sky[4], blk[4];   // свет 0..15 с точностью 1/16 (хранится x16) — плавные переходы без ступенек
    u8 shade[4];         // направление * затенение углов (0..255)
    u8 nrm;              // 0..5, 6 = двусторонний
    u8 flags;            // 1 = вода/полупрозрачный, 2 = светится, 4 = лава
    u8 tint = 0;         // оттенок биома: 0 — нет, иначе (биом + 1) | 0x80 для воды
    u8 hzE[4] = {0, 0, 0, 0}, hzW[4] = {0, 0, 0, 0};   // «горизонт» на восток и запад: угол, ниже которого солнце закрыто (тени)
    u8 ex[4] = {0, 0, 0, 0};                           // вода: пена у берега
};
struct Section { std::vector<MQuad> q; std::vector<u16> lights; u64 conn = ~0ULL; int vis = -1; float warm = 0.25f; };   // warm: 0 — свет факелов, 1 — лавы   // lights: x | z<<4 | (y&15)<<8
struct Chunk {
    int cx = 0, cz = 0;
    u8 blk[CHUNK_VOL];
    u8 meta[CHUNK_VOL];
    u8 light[CHUNK_VOL];   // старшие 4 бита — небо, младшие — блоки
    Section sec[SECN];
    u8 biome[256];         // биом каждого столбца (для оттенков травы, листвы и воды)
    u8 top[256];           // высота самого верхнего блока, заслоняющего солнце (для теней)
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

struct Furnace { Stack in, fuel, out; float burn = 0, burnMax = 0, cook = 0, xp = 0; };
struct Grow { int x, y, z; float t; };

enum Biome { BI_PLAINS = 0, BI_FOREST, BI_DESERT, BI_SNOW, BI_MOUNT, BI_BEACH, BI_OCEAN,
             BI_BIRCH, BI_TAIGA, BI_SAVANNA, BI_SWAMP, BI_MEADOW, BI_BADLANDS, BI_JUNGLE, BI_COUNT };
static const char* BIOME_NAME[BI_COUNT] = {"РАВНИНА", "ЛЕС", "ПУСТЫНЯ", "СНЕЖНАЯ РАВНИНА", "ГОРЫ", "ПЛЯЖ", "ОКЕАН",
    "БЕРЁЗОВАЯ РОЩА", "ТАЙГА", "САВАННА", "БОЛОТО", "ЦВЕТОЧНЫЙ ЛУГ", "КАНЬОН", "ДЖУНГЛИ"};

static u32 g_seed = 12345;
static Noise nCont, nHill, nMount, nTemp, nHum, nRiver, nCave1, nCave2, nCave3, nLake, nDetail, nVar, nBad;
static std::unordered_map<i64, Chunk*> g_chunks;
static std::unordered_map<i64, std::vector<u8>> g_savedChunks;   // RLE изменённых чанков
static std::map<i64, std::vector<Stack>> g_chests;
static std::map<i64, Furnace> g_furnaces;
static std::vector<Grow> g_grow;
static i64 g_tick = 0;

// версия генератора: 1 — старые миры (без сдвигов, чтобы их несохранённые участки не изменились), 2 — со сдвигами,
// 3 — высота 256, море 64, 14 биомов
static const int GEN_VER_NEW = 3;
static int g_genVer = GEN_VER_NEW;
static void worldInitNoise(u32 seed) {
    g_seed = seed;
    bool o = g_genVer >= 2;
    nCont.init(seed * 3 + 1, o); nHill.init(seed * 5 + 2, o); nMount.init(seed * 7 + 3, o); nTemp.init(seed * 11 + 4, o);
    nHum.init(seed * 13 + 5, o); nRiver.init(seed * 17 + 6, o); nCave1.init(seed * 19 + 7, o); nCave2.init(seed * 23 + 8, o);
    nCave3.init(seed * 29 + 9, o); nLake.init(seed * 31 + 10, o); nDetail.init(seed * 37 + 11, o);
    nVar.init(seed * 41 + 12, true); nBad.init(seed * 43 + 13, true);
    g_sea = g_genVer >= 3 ? 64 : 48;
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
struct ColInfo { int h; u8 biome; float temp, hum, bad; };
// старые миры (генератор 1–2): высота 128, море 48 — формулы не меняются, чтобы несгенерированные участки совпали
static ColInfo columnInfoV2(int x, int z) {
    ColInfo ci; const int SEA = g_sea;
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
    ci.h = clampi((int)h, 6, OLD_WH - 12);
    ci.temp = nTemp.fbm2(x * 0.0021f + 100.f, z * 0.0021f, 3) * 1.6f - std::max(0, ci.h - 70) * 0.006f;
    ci.hum = nHum.fbm2(x * 0.0019f - 300.f, z * 0.0019f, 3) * 1.6f;
    ci.bad = 0;
    if (ci.h < SEA - 3) ci.biome = BI_OCEAN;
    else if (ci.h <= SEA + 1) ci.biome = ci.temp < -0.2f ? BI_SNOW : BI_BEACH;
    else if (ci.h > 92 && m > 0.5f) ci.biome = BI_MOUNT;
    else if (ci.temp < -0.18f) ci.biome = BI_SNOW;
    else if (ci.temp > 0.16f && ci.hum < 0.05f) ci.biome = BI_DESERT;
    else if (ci.hum > 0.08f) ci.biome = BI_FOREST;
    else ci.biome = BI_PLAINS;
    return ci;
}
// новые миры (генератор 3): высота 256, море 64, 14 биомов
static ColInfo columnInfoV3(int x, int z) {
    ColInfo ci; const float S = (float)g_sea;
    float cont = nCont.fbm2(x * 0.0011f, z * 0.0011f, 5);
    float hills = nHill.fbm2(x * 0.0075f, z * 0.0075f, 4);
    float mnt = nMount.fbm2(x * 0.0023f, z * 0.0023f, 4);
    float m = smooth01(0.07f, 0.17f, mnt);
    float ridge = 1.f - fabsf(nMount.n2(x * 0.0105f + 33.f, z * 0.0105f - 71.f) * 1.4f);
    ridge = clampf(ridge, 0, 1); ridge *= ridge;
    float temp = nTemp.fbm2(x * 0.00095f + 100.f, z * 0.00095f, 3) * 1.75f;
    float hum = nHum.fbm2(x * 0.00105f - 300.f, z * 0.00105f, 3) * 1.75f;
    float bad = nBad.fbm2(x * 0.0018f, z * 0.0018f, 2);
    float h = S + 10.f + cont * 130.f + hills * (22.f + m * 40.f) + m * (28.f + ridge * 72.f);
    // болото: низина у самой воды
    float temperate = smooth01(-0.18f, -0.10f, temp) * (1.f - smooth01(0.14f, 0.22f, temp));
    float swampK = smooth01(0.20f, 0.30f, hum) * temperate * (1.f - m);
    if (swampK > 0) {
        float lowK = 1.f - smooth01(S + 10.f, S + 28.f, h);
        h = lerpf(h, S + 0.4f + hills * 14.f, swampK * lowK);
    }
    // каньон: ступенчатые террасы
    float badK = smooth01(0.06f, 0.14f, bad) * smooth01(0.14f, 0.22f, temp) * (1.f - smooth01(-0.10f, 0.02f, hum));
    if (badK > 0 && h > S + 1) {
        float hb = h + badK * (14.f + 60.f * clampf(hills + 0.1f, 0.f, 0.4f));
        const float st = 7.f;
        float f = (hb - S) / st; float fl = floorf(f), fr = f - fl;
        hb = S + (fl + smooth01(0.55f, 0.85f, fr)) * st;
        h = lerpf(h, hb, smooth01(0.f, 0.45f, badK));
    }
    // реки
    float rv = fabsf(nRiver.fbm2(x * 0.0021f, z * 0.0021f, 3));
    if (rv < 0.036f && h > S - 3) {
        float k = smooth01(0.0f, 0.036f, rv);
        h = lerpf(S - 3.5f, h, k * k);
    }
    ci.h = clampi((int)h, 6, WH - 24);
    ci.temp = temp - std::max(0.f, ci.h - (S + 34.f)) * 0.0065f;
    ci.hum = hum; ci.bad = badK;
    if (ci.h < S - 3) { ci.biome = BI_OCEAN; return ci; }
    if (ci.h <= S + 1 && swampK < 0.5f) {
        ci.biome = ci.temp < -0.31f ? BI_SNOW : (badK > 0.5f ? BI_BADLANDS : BI_BEACH);
        return ci;
    }
    if (ci.h > S + 48 && m > 0.5f) { ci.biome = BI_MOUNT; return ci; }
    float var = nVar.fbm2(x * 0.0034f, z * 0.0034f, 2);
    if (ci.temp < -0.31f) ci.biome = BI_SNOW;
    else if (ci.temp < -0.14f) ci.biome = BI_TAIGA;
    else if (ci.temp > 0.18f) {
        if (badK > 0.5f) ci.biome = BI_BADLANDS;
        else if (ci.hum < -0.06f) ci.biome = BI_DESERT;
        else if (ci.hum > 0.18f) ci.biome = BI_JUNGLE;
        else ci.biome = BI_SAVANNA;
    } else {
        if (swampK > 0.5f) ci.biome = BI_SWAMP;
        else if (ci.hum > 0.04f) ci.biome = var > 0.06f ? BI_BIRCH : BI_FOREST;
        else ci.biome = var > 0.08f ? BI_MEADOW : BI_PLAINS;
    }
    return ci;
}
static inline ColInfo columnInfo(int x, int z) { return g_genVer >= 3 ? columnInfoV3(x, z) : columnInfoV2(x, z); }

// полосы обожжённой глины в каньонах
static u8 terraBand(int x, int y, int z) {
    int yy = y + (int)floorf(nBad.n2(x * 0.021f, z * 0.021f) * 3.f);
    u32 h = hash32((u32)(yy + 1000) * 2654435761u ^ g_seed) % 13;
    if (h < 2) return B_TERRA_W;
    if (h < 4) return B_TERRA_R;
    if (h < 5) return B_TERRA_B;
    return B_TERRA_O;
}

// ---------------------------------------------------------------- деревья
// kind: 0 — лиственное, 1 — ель, 2 — берёза, 3 — акация, 4 — тропическое, 5 — большое тропическое 2x2, 6 — болотное, 7 — куст
template <class SetF, class GetF>
static bool growTree(int kind, int x, int y, int z, Rng& r, SetF set, GetF get) {
    auto freeAt = [&](int xx, int yy, int zz) { u8 b = get(xx, yy, zz); return b == B_AIR || BD[b].replaceable || isLeaves(b) || b == B_SAPLING; };
    auto leaf = [&](int xx, int yy, int zz, u8 lv) { u8 b = get(xx, yy, zz); if (b == B_AIR || BD[b].replaceable) set(xx, yy, zz, lv); };
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
    } else if (kind == 1) {
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
    } else if (kind == 2) {   // берёза: стройная, крона узкая и высокая
        int h = r.range(5, 8);
        if (y + h + 2 >= WH) return false;
        for (int i = 1; i <= h; i++) if (!freeAt(x, y + i, z)) return false;
        for (int dy = h - 3; dy <= h + 1; dy++) {
            int rad = (dy >= h || dy == h - 3) ? 1 : 2;
            for (int dz = -rad; dz <= rad; dz++) for (int dx = -rad; dx <= rad; dx++) {
                if (rad == 2 && abs(dx) == 2 && abs(dz) == 2) continue;
                if (rad == 2 && (abs(dx) == 2 || abs(dz) == 2) && r.range(4) == 0) continue;
                if (dy == h + 1 && abs(dx) + abs(dz) > 1) continue;
                leaf(x + dx, y + dy, z + dz, B_BIRCH_LEAVES);
            }
        }
        for (int i = 0; i < h; i++) set(x, y + i, z, B_BIRCH_LOG);
    } else if (kind == 3) {   // акация: ствол с изгибом и плоский зонтик
        int h = r.range(3, 4), bend = r.range(2, 3), dir = r.range(4);
        if (y + h + bend + 3 >= WH) return false;
        int tx = x, tz = z, ty = y;
        for (int i = 0; i < h; i++) { if (!freeAt(tx, ty, tz) && i > 0) return false; set(tx, ty, tz, B_ACACIA_LOG); ty++; }
        for (int i = 0; i < bend; i++) { tx += HDX[dir]; tz += HDZ[dir]; set(tx, ty, tz, B_ACACIA_LOG); ty++; }
        auto canopy = [&](int cx, int cy, int cz, int rad) {
            for (int dz = -rad; dz <= rad; dz++) for (int dx = -rad; dx <= rad; dx++) {
                if (abs(dx) == rad && abs(dz) == rad) continue;
                if (abs(dx) + abs(dz) > rad + 1 && r.range(3)) continue;
                leaf(cx + dx, cy, cz + dz, B_ACACIA_LEAVES);
            }
            for (int dz = -rad + 2; dz <= rad - 2; dz++) for (int dx = -rad + 2; dx <= rad - 2; dx++) leaf(cx + dx, cy + 1, cz + dz, B_ACACIA_LEAVES);
        };
        canopy(tx, ty, tz, 3);
        if (r.range(2)) {   // вторая ветка в другую сторону, пониже
            int d2 = (dir + 1 + r.range(2) * 2) & 3, bx = x + HDX[d2], bz = z + HDZ[d2], by = y + h - 1;
            set(bx, by, bz, B_ACACIA_LOG); bx += HDX[d2]; bz += HDZ[d2]; by++; set(bx, by, bz, B_ACACIA_LOG);
            canopy(bx, by + 1, bz, 2);
        }
    } else if (kind == 4 || kind == 7) {   // тропическое дерево или куст
        int h = kind == 7 ? 1 : r.range(8, 12);
        if (y + h + 3 >= WH) return false;
        for (int i = 1; i <= h; i++) if (!freeAt(x, y + i, z)) return false;
        int top = kind == 7 ? y : y + h;
        int r0 = kind == 7 ? 2 : 3;
        for (int dy = (kind == 7 ? 0 : -2); dy <= 1; dy++) {
            int rad = kind == 7 ? (dy == 0 ? 2 : 1) : (dy == 1 ? 1 : (dy == 0 ? r0 : 2));
            for (int dz = -rad; dz <= rad; dz++) for (int dx = -rad; dx <= rad; dx++) {
                if (dx * dx + dz * dz > rad * rad + 1) continue;
                if (dx * dx + dz * dz == rad * rad + 1 && r.range(2)) continue;
                leaf(x + dx, top + dy, z + dz, B_JUNGLE_LEAVES);
            }
        }
        for (int i = 0; i < h; i++) set(x, y + i, z, B_JUNGLE_LOG);
    } else if (kind == 5) {   // гигант 2x2 с ветками
        int h = r.range(15, 22);
        if (y + h + 4 >= WH) return false;
        for (int i = 1; i <= h; i++) for (int k = 0; k < 4; k++) if (!freeAt(x + (k & 1), y + i, z + (k >> 1))) return false;
        for (int i = 0; i < h; i++) for (int k = 0; k < 4; k++) set(x + (k & 1), y + i, z + (k >> 1), B_JUNGLE_LOG);
        auto blob = [&](int cx, int cy, int cz, int rad) {
            for (int dy = -1; dy <= 1; dy++) {
                int rr = dy == 1 ? rad - 1 : (dy == -1 ? rad - 1 : rad);
                for (int dz = -rr; dz <= rr + 1; dz++) for (int dx = -rr; dx <= rr + 1; dx++) {
                    float fx = dx - 0.5f, fz = dz - 0.5f;
                    if (fx * fx + fz * fz > rr * rr + 0.8f) continue;
                    leaf(cx + dx, cy + dy, cz + dz, B_JUNGLE_LEAVES);
                }
            }
        };
        blob(x, y + h, z, 4);
        blob(x, y + h + 1, z, 2);
        for (int b = 0; b < 2; b++) {   // боковые ветки с кронами
            int d = r.range(4), by = y + h - 5 - b * 4; if (by < y + 5) break;
            int bx = x + (HDX[d] > 0 ? 2 : (HDX[d] < 0 ? -1 : 0)), bz = z + (HDZ[d] > 0 ? 2 : (HDZ[d] < 0 ? -1 : 0));
            set(bx, by, bz, B_JUNGLE_LOG); bx += HDX[d]; bz += HDZ[d]; set(bx, by + 1, bz, B_JUNGLE_LOG);
            blob(bx, by + 2, bz, 2);
        }
    } else {   // болотное: невысокое, широкая крона со свисающими листьями
        int h = r.range(4, 6);
        if (y + h + 2 >= WH) return false;
        for (int i = 1; i <= h; i++) if (!freeAt(x, y + i, z)) return false;
        for (int dy = h - 2; dy <= h + 1; dy++) {
            int rad = dy == h + 1 ? 1 : (dy == h ? 2 : 3);
            for (int dz = -rad; dz <= rad; dz++) for (int dx = -rad; dx <= rad; dx++) {
                if (abs(dx) == rad && abs(dz) == rad) continue;
                leaf(x + dx, y + dy, z + dz, B_LEAVES);
                if (dy == h - 2 && (abs(dx) == rad || abs(dz) == rad) && r.range(3) == 0) {   // свисающие пряди
                    int len = r.range(1, 3);
                    for (int k = 1; k <= len; k++) leaf(x + dx, y + dy - k, z + dz, B_LEAVES);
                }
            }
        }
        for (int i = 0; i < h; i++) set(x, y + i, z, B_LOG);
    }
    return true;
}

// ---------------------------------------------------------------- генерация чанка
static void generateChunk(Chunk* c) {
    const int bx = c->cx * 16, bz = c->cz * 16;
    const bool v3 = g_genVer >= 3;
    const int SEA = g_sea;
    const int topH = v3 ? WH : OLD_WH;
    memset(c->blk, 0, sizeof(c->blk));
    memset(c->meta, 0, sizeof(c->meta));
    memset(c->light, 0, sizeof(c->light));
    ColInfo col[16][16];
    for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) { col[z][x] = columnInfo(bx + x, bz + z); c->biome[z * 16 + x] = col[z][x].biome; }

    // 1. слои земли
    for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
        const ColInfo& ci = col[z][x];
        int h = ci.h;
        u32 hs = hash3i(bx + x, 0, bz + z, g_seed);
        int dirtDepth = 3 + (int)(hs % 2);
        bool podzol = v3 && ci.biome == BI_TAIGA && nVar.n2((bx + x) * 0.09f, (bz + z) * 0.09f) > 0.08f;
        for (int y = 0; y <= h; y++) {
            u8 b = B_STONE;
            int d = h - y;
            switch (ci.biome) {
                case BI_DESERT: b = d < 4 ? B_SAND : (d < (v3 ? 9 : 7) ? B_SANDSTONE : B_STONE); break;
                case BI_BEACH: b = d < 4 ? B_SAND : B_STONE; break;
                case BI_OCEAN: b = d < 3 ? ((hs >> 8) % 3 == 0 ? B_GRAVEL : ((hs >> 9) % 2 ? B_SAND : B_DIRT)) : B_STONE; break;
                case BI_SNOW: b = d == 0 ? (h > SEA ? B_SNOWGRASS : B_DIRT) : (d < dirtDepth ? B_DIRT : B_STONE); break;
                case BI_MOUNT:
                    if (!v3) { b = (d == 0 && h > 104) ? B_SNOW : B_STONE; if (d < 2 && h < 98 && (hs % 3)) b = d == 0 ? B_GRASS : B_DIRT; }
                    else { b = (d < 2 && h > SEA + 92 + (int)(hs % 5)) ? B_SNOW : B_STONE; if (d < 2 && h < SEA + 64 && (hs % 3)) b = d == 0 ? B_GRASS : B_DIRT;
                           if (d < 2 && b == B_STONE && (hs >> 6) % 9 == 0) b = B_GRAVEL; }
                    break;
                case BI_TAIGA: b = d == 0 ? (podzol ? B_PODZOL : B_GRASS) : (d < dirtDepth ? B_DIRT : B_STONE); break;
                case BI_SAVANNA: b = d == 0 ? B_DRYGRASS : (d < dirtDepth ? B_DIRT : B_STONE); break;
                case BI_SWAMP: b = d == 0 ? (h <= SEA ? B_MUD : B_GRASS) : (d < dirtDepth ? (h <= SEA + 1 && d < 3 ? B_MUD : B_DIRT) : B_STONE); break;
                case BI_BADLANDS:
                    if (y < SEA - 6) b = B_STONE;
                    else if (d == 0 && h < SEA + 16) b = B_REDSAND;
                    else if (d < 3 && h <= SEA + 2) b = B_REDSAND;
                    else b = terraBand(bx + x, y, bz + z);
                    break;
                default: b = d == 0 ? B_GRASS : (d < dirtDepth ? B_DIRT : B_STONE); break;
            }
            if (ci.biome != BI_OCEAN && ci.biome != BI_SWAMP && ci.biome != BI_BADLANDS && d == 0 && h < SEA) b = B_DIRT;   // дно рек и озёр
            if (ci.biome != BI_OCEAN && ci.biome != BI_DESERT && ci.biome != BI_BEACH && ci.biome != BI_SWAMP && ci.biome != BI_BADLANDS &&
                h <= SEA && d < 3 && (hs >> 4) % 4 == 0) b = B_SAND;
            c->blk[cidx(x, y, z)] = b;
        }
        for (int y = h + 1; y <= SEA; y++) c->blk[cidx(x, y, z)] = B_WATER;
        if (h < SEA && ci.temp < (v3 ? -0.31f : -0.2f)) c->blk[cidx(x, SEA, z)] = B_ICE;
        c->blk[cidx(x, 0, z)] = B_BEDROCK;
        for (int y = 1; y <= 3; y++) if (hash3i(bx + x, y, bz + z, g_seed ^ 0xBED) % (u32)(y + 1) == 0) c->blk[cidx(x, y, z)] = B_BEDROCK;
    }

    // 2. пещеры: шум на сетке 4x4x4 с трилинейной интерполяцией
    static float gT[5][WH / 4 + 1][5], gC[5][WH / 4 + 1][5];
    const int GY = topH / 4 + 1;
    for (int gz = 0; gz < 5; gz++) for (int gy = 0; gy < GY; gy++) for (int gx = 0; gx < 5; gx++) {
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
        for (int y = 1; y < std::min(h + 1, topH - 1); y++) {
            int gy = y >> 2; float fy = (y & 3) / 4.f;
            #define TRI(G) ( lerpf(lerpf(lerpf(G[gx][gy][gz], G[gx+1][gy][gz], fx), lerpf(G[gx][gy+1][gz], G[gx+1][gy+1][gz], fx), fy), \
                                  lerpf(lerpf(G[gx][gy][gz+1], G[gx+1][gy][gz+1], fx), lerpf(G[gx][gy+1][gz+1], G[gx+1][gy+1][gz+1], fx), fy), fz) )
            float t = TRI(gT);
            float ch = TRI(gC);
            #undef TRI
            int topLimit = wet ? h - 4 : h;
            bool tunnel = t < (v3 ? 0.011f : 0.010f) && y <= topLimit;
            float chThr = 0.30f + std::max(0, y - (SEA - 8)) * 0.01f;
            if (v3 && y < 40) chThr -= (40 - y) * 0.002f;   // в глубине пещеры просторнее
            bool cave = ch > chThr && y < h - 7;
            if (!(tunnel || cave)) continue;
            u8& cell = c->blk[cidx(x, y, z)];
            if (cell == B_BEDROCK || cell == B_WATER || cell == B_ICE) continue;
            if (y <= 10) cell = B_LAVA;
            else if (cave && y <= (v3 ? 30 : 22) && lakeN > 0.22f) cell = B_WATER;
            else cell = B_AIR;
            // песок, повисший над пещерой, превращаем в песчаник
            if (y + 1 < WH && (c->blk[cidx(x, y + 1, z)] == B_SAND || c->blk[cidx(x, y + 1, z)] == B_REDSAND)) c->blk[cidx(x, y + 1, z)] = B_SANDSTONE;
            if (y + 1 < WH && c->blk[cidx(x, y + 1, z)] == B_GRAVEL) c->blk[cidx(x, y + 1, z)] = B_STONE;
        }
    }

    // 3. руды жилами
    Rng r(((u64)(u32)c->cx * 341873128712ULL) ^ ((u64)(u32)c->cz * 132897987541ULL) ^ g_seed);
    struct OreSpec { u8 id; int tries, ymin, ymax, smin, smax; int chance; };
    const OreSpec oresV2[4] = {
        {B_COAL_ORE, 30, 5, 120, 4, 12, 100},
        {B_IRON_ORE, 11, 5, 62, 3, 7, 100},
        {B_GOLD_ORE, 3, 4, 30, 3, 6, 100},
        {B_DIAMOND_ORE, 1, 2, 14, 2, 5, 60},
    };
    const OreSpec oresV3[4] = {
        {B_COAL_ORE, 40, 5, 190, 4, 12, 100},
        {B_IRON_ORE, 16, 5, 90, 3, 7, 100},
        {B_GOLD_ORE, 4, 4, 34, 3, 6, 100},
        {B_DIAMOND_ORE, 1, 2, 15, 2, 5, 70},
    };
    const OreSpec* ores = v3 ? oresV3 : oresV2;
    for (int o = 0; o < 4; o++) {
        const OreSpec& s = ores[o];
        for (int t = 0; t < s.tries; t++) {
            if (r.range(100) >= s.chance) continue;
            int x = r.range(16), z = r.range(16), y = r.range(s.ymin, s.ymax);
            int n = r.range(s.smin, s.smax);
            for (int k = 0; k < n; k++) {
                if (y > 0 && y < WH && c->blk[cidx(x, y, z)] == B_STONE) c->blk[cidx(x, y, z)] = s.id;
                int d = r.range(6);
                x = clampi(x + DX[d], 0, 15); y = clampi(y + DY[d], 1, topH - 1); z = clampi(z + DZ[d], 0, 15);
            }
        }
    }
    // руды, выходящие на стены пещер: их видно, когда идёшь по пещере
    {
        auto isAir = [&](int x, int y, int z) { if (x < 0 || x > 15 || z < 0 || z > 15 || y < 1 || y >= WH) return false; return c->blk[cidx(x, y, z)] == B_AIR; };
        int wallTries = v3 ? 90 : 70, wallTop = v3 ? 84 : 64;
        for (int t = 0; t < wallTries; t++) {
            int x = r.range(16), z = r.range(16), y = r.range(6, wallTop);
            if (c->blk[cidx(x, y, z)] != B_STONE) continue;
            bool wall = false; for (int d = 0; d < 6 && !wall; d++) wall = isAir(x + DX[d], y + DY[d], z + DZ[d]);
            if (!wall) continue;
            int roll = r.range(1000); u8 id = 0;
            if (y < 16 && roll < 45) id = B_DIAMOND_ORE;
            else if (y < 32 && roll < 110) id = B_GOLD_ORE;
            else if (roll < 330) id = B_IRON_ORE;
            else if (roll < 650) id = B_COAL_ORE;
            if (!id) continue;
            int n = id == B_DIAMOND_ORE ? r.range(2, 4) : r.range(3, 7);
            int vx = x, vy = y, vz = z;
            for (int k = 0; k < n; k++) {
                if (c->blk[cidx(vx, vy, vz)] == B_STONE) c->blk[cidx(vx, vy, vz)] = id;
                int d = r.range(6); vx = clampi(vx + DX[d], 0, 15); vy = clampi(vy + DY[d], 1, topH - 1); vz = clampi(vz + DZ[d], 0, 15);
            }
        }
    }
    // гравий и камни-пятна в толще
    for (int t = 0; t < 4; t++) {
        int x = r.range(16), z = r.range(16), y = r.range(10, v3 ? 90 : 70), n = r.range(6, 14);
        for (int k = 0; k < n; k++) {
            if (c->blk[cidx(x, y, z)] == B_STONE) c->blk[cidx(x, y, z)] = B_GRAVEL;
            int d = r.range(6); x = clampi(x + DX[d], 0, 15); y = clampi(y + DY[d], 1, topH - 1); z = clampi(z + DZ[d], 0, 15);
        }
    }

    // 4. растительность
    auto lset = [&](int x, int y, int z, u8 b) { if (x >= 0 && x < 16 && z >= 0 && z < 16 && y > 0 && y < WH) c->blk[cidx(x, y, z)] = b; };
    auto lget = [&](int x, int y, int z) -> u8 { if (x < 0 || x > 15 || z < 0 || z > 15 || y < 0 || y >= WH) return B_STONE; return c->blk[cidx(x, y, z)]; };
    u8 cb = col[8][8].biome;
    if (!v3) {
        int treeCount = 0; int treeKind = 0;
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
    } else {
        // деревья: вид зависит от биома в точке посадки
        int tries = 0;
        switch (cb) {
            case BI_FOREST: tries = r.range(6, 10); break;  case BI_BIRCH: tries = r.range(6, 9); break;
            case BI_TAIGA: tries = r.range(6, 10); break;   case BI_JUNGLE: tries = r.range(10, 15); break;
            case BI_SWAMP: tries = r.range(2, 4); break;    case BI_SAVANNA: tries = r.range(3) == 0 ? 2 : 1; break;
            case BI_PLAINS: tries = r.range(4) == 0 ? 1 : 0; break; case BI_MEADOW: tries = r.range(8) == 0 ? 1 : 0; break;
            case BI_SNOW: tries = r.range(0, 2); break;      case BI_MOUNT: tries = r.range(0, 2); break;
            default: break;
        }
        for (int t = 0; t < tries; t++) {
            int x = r.range(3, 12), z = r.range(3, 12), h = col[z][x].h;
            u8 bi = col[z][x].biome, g = lget(x, h, z);
            if (!(g == B_GRASS || g == B_SNOWGRASS || g == B_PODZOL || g == B_DRYGRASS || g == B_DIRT) || h <= SEA - (bi == BI_SWAMP ? 1 : 0) || lget(x, h + 1, z) != B_AIR) continue;
            int kind = 0;
            switch (bi) {
                case BI_FOREST: kind = r.range(5) == 0 ? 2 : 0; break;
                case BI_BIRCH: kind = r.range(6) == 0 ? 0 : 2; break;
                case BI_TAIGA: case BI_SNOW: case BI_MOUNT: kind = 1; break;
                case BI_SAVANNA: kind = 3; break;
                case BI_SWAMP: kind = 6; break;
                case BI_JUNGLE: {
                    int v = r.range(10);
                    if (v == 0 && x >= 4 && x <= 10 && z >= 4 && z <= 10 && lget(x + 1, h, z) != B_AIR && lget(x, h, z + 1) != B_AIR && lget(x + 1, h, z + 1) != B_AIR) kind = 5;
                    else kind = v < 5 ? 7 : 4;
                    break;
                }
                default: kind = 0; break;
            }
            if (growTree(kind, x, h + 1, z, r, lset, lget)) {
                lset(x, h, z, B_DIRT);
                if (kind == 5) { lset(x + 1, h, z, B_DIRT); lset(x, h, z + 1, B_DIRT); lset(x + 1, h, z + 1, B_DIRT); }
            }
        }
        // валуны из замшелого булыжника в тайге
        if (cb == BI_TAIGA && r.range(5) == 0) {
            int x = r.range(3, 12), z = r.range(3, 12), h = col[z][x].h;
            if (h > SEA) for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) for (int dy = 0; dy <= 1; dy++) {
                if (abs(dx) + abs(dz) + dy * 2 > 2 && r.range(2)) continue;
                lset(x + dx, h + 1 + dy, z + dz, r.range(3) ? B_MOSSY : B_COBBLE);
            }
        }
        // кактусы
        if (cb == BI_DESERT || cb == BI_BADLANDS) {
            int n = r.range(0, cb == BI_DESERT ? 3 : 2);
            for (int t = 0; t < n; t++) {
                int x = r.range(1, 14), z = r.range(1, 14), h = col[z][x].h;
                u8 g = lget(x, h, z);
                if ((g != B_SAND && g != B_REDSAND) || h <= SEA) continue;
                int ch = r.range(1, 3);
                for (int i = 1; i <= ch; i++) if (lget(x, h + i, z) == B_AIR && lget(x + 1, h + i, z) == B_AIR && lget(x - 1, h + i, z) == B_AIR) lset(x, h + i, z, B_CACTUS);
            }
        }
        // трава, цветы, папоротники, камыш, кувшинки
        for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
            int h = col[z][x].h; u8 bi = col[z][x].biome;
            if (h + 3 >= WH) continue;
            u32 hv = hash3i(bx + x, h, bz + z, g_seed ^ 0xF10);
            int pr = hv % 1000;
            u8 g = lget(x, h, z), above = lget(x, h + 1, z);
            if (g == B_WATER && above == B_AIR) {   // поверхность воды
                if (bi == BI_SWAMP && pr < 90 && lget(x, h - 2, z) != B_WATER) lset(x, h + 1, z, B_LILY);
                else if (bi == BI_SWAMP && pr < 120) lset(x, h + 1, z, B_LILY);
                continue;
            }
            if (above != B_AIR) continue;
            // камыш у воды
            bool nearWater = false;
            for (int d = 0; d < 4 && !nearWater; d++) nearWater = lget(x + HDX[d], h, z + HDZ[d]) == B_WATER;
            if (nearWater && (g == B_SAND || g == B_GRASS || g == B_DIRT || g == B_MUD || g == B_DRYGRASS || g == B_REDSAND) && h >= SEA &&
                pr < (bi == BI_SWAMP ? 320 : (bi == BI_DESERT || bi == BI_BADLANDS || bi == BI_SNOW ? 0 : 80))) {
                int len = 1 + (int)((hv >> 12) % 3);
                for (int i = 1; i <= len; i++) lset(x, h + i, z, B_REED);
                continue;
            }
            u8 put = 0;
            int fl = (hv >> 10) % 4;
            switch (bi) {
                case BI_PLAINS: if (pr < 20) put = fl & 1 ? B_FLOWER_R : B_FLOWER_Y; else if (pr < 240) put = B_TALLGRASS; break;
                case BI_MEADOW: if (pr < 150) put = fl == 0 ? B_FLOWER_R : (fl == 1 ? B_FLOWER_Y : (fl == 2 ? B_FLOWER_B : B_FLOWER_W)); else if (pr < 420) put = B_TALLGRASS; break;
                case BI_FOREST: if (pr < 22) put = fl & 1 ? B_FLOWER_R : B_FLOWER_Y; else if (pr < 200) put = B_TALLGRASS; else if (pr < 215) put = B_FERN; break;
                case BI_BIRCH: if (pr < 30) put = fl & 1 ? B_FLOWER_W : B_FLOWER_B; else if (pr < 210) put = B_TALLGRASS; break;
                case BI_TAIGA: if (pr < 130) put = B_FERN; else if (pr < 190) put = B_TALLGRASS; break;
                case BI_SAVANNA: if (pr < 10) put = B_DEADBUSH; else if (pr < 380) put = B_TALLGRASS; break;
                case BI_SWAMP: if (pr < 110) put = B_TALLGRASS; else if (pr < 125) put = B_FLOWER_B; break;
                case BI_JUNGLE: if (pr < 240) put = B_TALLGRASS; else if (pr < 400) put = B_FERN; else if (pr < 410) put = B_FLOWER_R; break;
                case BI_DESERT: if (pr < 14) put = B_DEADBUSH; break;
                case BI_BADLANDS: if (pr < 22) put = B_DEADBUSH; break;
                case BI_MOUNT: case BI_SNOW: if (pr < 60) put = B_TALLGRASS; break;
                default: break;
            }
            if (!put) continue;
            bool soil = (put == B_DEADBUSH) ? (g == B_SAND || g == B_REDSAND || g == B_DRYGRASS || isTerra(g)) : (g == B_GRASS || g == B_PODZOL || g == B_DRYGRASS || g == B_MUD);
            if (soil) lset(x, h + 1, z, put);
        }
    }
    c->emitters = 0;
    for (int i = 0; i < CHUNK_VOL; i++) if (BD[c->blk[i]].light) c->emitters++;
    computeTops(c);
}
static void computeTops(Chunk* c) {
    for (int i = 0; i < 256; i++) {
        int y = WH - 1;
        while (y > 0 && !occludesSun(c->blk[(y << 8) | i])) y--;
        c->top[i] = (u8)y;
    }
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
    computeTops(c);
    return true;
}

// ---------------------------------------------------------------- загрузка/выгрузка чанков
static void lodFromChunk(Chunk* c);
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
            for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) c->biome[z * 16 + x] = columnInfo(cx * 16 + x, cz * 16 + z).biome;
            for (int i = 0; i < CHUNK_VOL; i++) if (c->blk[i] == B_SAPLING) {
                int x = i & 15, z = (i >> 4) & 15, y = i >> 8;
                Grow g; g.x = cx * 16 + x; g.y = y; g.z = cz * 16 + z; g.t = 30.f + (hash32(i) % 60);
                g_grow.push_back(g);
            }
        }
    }
    if (!ok) generateChunk(c);
    g_chunks[ckey(cx, cz)] = c;
    lodFromChunk(c);
    // соседям нужно перестроить края
    for (int d = 0; d < 4; d++) { Chunk* n = getChunk(cx + HDX[d], cz + HDZ[d]); if (n) n->dirty = true; }
    for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) { Chunk* n = getChunk(cx + dx, cz + dz); if (n) n->dirty = true; }
    return c;
}
static void unloadChunk(Chunk* c) {
    if (c->modified && g_netRole == NET_CLIENT) c->modified = false;   // у гостя чанки хранит хозяин
    if (c->modified) { std::vector<u8> v; encodeChunk(c, v); g_savedChunks[ckey(c->cx, c->cz)] = v; lodFromChunk(c); }
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
    return b == B_WATER || b == B_LAVA || b == B_SAND || b == B_REDSAND || b == B_GRAVEL || b == B_TORCH || isSmallPlant(b) ||
           b == B_LADDER || b == B_DOOR || b == B_CACTUS || b == B_BED;
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
static bool g_netRoleAny = false;   // включена сетевая игра (хозяин или гость)
static void netOnBlock(int x, int y, int z, u8 b, u8 m);
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
    {   // карта высот для теней
        int ci = i & 255, ty = c->top[ci];
        if (occludesSun(b) && y > ty) c->top[ci] = (u8)y;
        else if (y == ty && !occludesSun(b)) { int yy = y; while (yy > 0 && !occludesSun(c->blk[(yy << 8) | ci])) yy--; c->top[ci] = (u8)yy; }
    }
    if (g_netRoleAny) netOnBlock(x, y, z, b, m);   // сетевая игра: изменение уходит другим
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
    if (isLeaves(b) || b == B_ICE) { extra = 1; return true; }
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
        q.sky[k] = (u8)std::min(255, (ss * 16 + n / 2) / n); q.blk[k] = (u8)std::min(255, (sb * 16 + n / 2) / n);
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
            q.sky[k] = (u8)std::min(255, sky * 16); q.blk[k] = (u8)std::min(255, blk * 16); q.shade[k] = (u8)(FACE_SHADE[d] * 255.f);
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
            q.u[k] = U[k]; q.v[k] = V[k]; q.sky[k] = (u8)std::min(255, sky * 16); q.blk[k] = (u8)std::min(255, blk * 16); q.shade[k] = 230;
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

// ---------------------------------------------------------------- данные для «шейдеров»: тени, пена, подводные блики, цвет огня
// Горизонт: для каждой вершины — угол над горизонтом на восток и на запад, ниже которого солнце закрыто
// горами, деревьями и постройками. Солнце ходит с востока на запад, поэтому этого хватает для мягких
// движущихся теней, а в кадре остаётся одно сравнение на вершину.
static const float NRM[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
static const int HZ_RANGE = 40;
static void shaderExtras(Chunk* c, int s, std::vector<MQuad>& out) {
    if (out.empty()) return;
    const int bx = c->cx * 16, bz = c->cz * 16;
    // полоса высот вдоль X: столбцы от -HZ_RANGE до 16+HZ_RANGE, ряды z от -1 до 16
    const int W = 16 + 2 * HZ_RANGE + 2, ZN = 18;
    static std::vector<short> hm; hm.assign((size_t)W * ZN, -1);
    for (int zi = 0; zi < ZN; zi++) {
        int wz = bz - 1 + zi;
        Chunk* cc = nullptr; int lastCx = 0x7fffffff;
        for (int xi = 0; xi < W; xi++) {
            int wx = bx - HZ_RANGE - 1 + xi, ccx = wx >> 4;
            if (ccx != lastCx) { cc = getChunk(ccx, wz >> 4); lastCx = ccx; }
            hm[(size_t)zi * W + xi] = cc ? (short)cc->top[((wz & 15) << 4) | (wx & 15)] : (short)-1;
        }
    }
    auto colTop = [&](int wx, int wz) -> int {
        int xi = wx - (bx - HZ_RANGE - 1), zi = wz - (bz - 1);
        if (xi < 0 || xi >= W || zi < 0 || zi >= ZN) return -1;
        return hm[(size_t)zi * W + xi];
    };
    const float Q = 255.f / 1.5707963f;
    for (MQuad& q : out) {
        bool water = q.tile == T_WATER;
        float nx = 0, ny = 0.5f, nz = 0;
        if (q.nrm < 6) { nx = NRM[q.nrm][0] * 0.5f; ny = NRM[q.nrm][1] * 0.5f; nz = NRM[q.nrm][2] * 0.5f; }
        for (int k = 0; k < 4; k++) {
            if (q.sky[k] < 6 * 16) { q.hzE[k] = q.hzW[k] = 255; continue; }   // глубоко в тени — солнце не достаёт
            float sx = bx + q.p[k][0] + nx, sy = q.p[k][1] + ny + 0.02f, sz = bz + q.p[k][2] + nz;
            int cx0 = ifloor(sx), cz0 = ifloor(sz);
            if (colTop(cx0, cz0) + 1 > sy + 0.6f) { q.hzE[k] = q.hzW[k] = 255; continue; }   // над головой крона или крыша
            float bestE = 0, bestW = 0;
            for (int d = 1; d <= HZ_RANGE; d += (d < 10 ? 1 : 2)) {
                int te = colTop(cx0 + d, cz0), tw = colTop(cx0 - d, cz0);
                if (te >= 0) { float h = te + 1 - sy; if (h > 0) { float dist = std::max(0.3f, (cx0 + d) - sx); bestE = std::max(bestE, h / dist); } }
                if (tw >= 0) { float h = tw + 1 - sy; if (h > 0) { float dist = std::max(0.3f, sx - (cx0 - d + 1)); bestW = std::max(bestW, h / dist); } }
            }
            q.hzE[k] = (u8)std::min(255.f, atanf(bestE) * Q);
            q.hzW[k] = (u8)std::min(255.f, atanf(bestW) * Q);
        }
        // пена у берега: угол водной глади касается суши
        if (water && q.nrm == 2) {
            for (int k = 0; k < 4; k++) {
                int vx = (int)lroundf(q.p[k][0]), vz = (int)lroundf(q.p[k][2]), vy = ifloor(q.p[k][1] - 0.01f);
                int solid = 0;
                for (int dz = -1; dz <= 0; dz++) for (int dx = -1; dx <= 0; dx++) { u8 b = getBlock(bx + vx + dx, vy, bz + vz + dz); if (BD[b].solid && b != B_WATER) solid++; }
                q.ex[k] = (u8)(solid ? std::min(255, 140 + solid * 40) : 0);
            }
        }
        // дно под водой: на нём играют солнечные блики
        if (!water && q.nrm == 2) {
            int x = ifloor(q.p[0][0] + 0.01f), z = ifloor(q.p[0][2] + 0.01f), y = ifloor(q.p[0][1] + 0.01f);
            if (getBlock(bx + x, y, bz + z) == B_WATER) q.flags |= 8;
        }
    }
    // цвет огня в этой секции: лава — оранжевая, факелы — жёлто-тёплые
    int lava = 0, torch = 0;
    for (int y = s * 16; y < s * 16 + 16; y++) for (int i = 0; i < 256; i++) { u8 b = c->blk[(y << 8) | i]; if (b == B_LAVA) lava++; else if (b == B_TORCH || b == B_FURNACE_LIT) torch++; }
    c->sec[s].warm = (lava + torch) ? clampf(lava / (float)(lava + torch * 6), 0.f, 1.f) : (s * 16 < 24 ? 0.7f : 0.2f);
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
                        q.sky[k] = (u8)std::min(255, ns * 16); q.blk[k] = (u8)std::min(255, nb2 * 16); q.shade[k] = (u8)(FACE_SHADE[d] * 255.f);
                    }
                    q.tile = bd.tex[d]; q.nrm = (d == 2) ? 6 : (u8)d; q.flags = fl;
                    out.push_back(q);
                }
                break;
            }
            case SH_CROSS: emitCross(out, x, y, z, bd.tex[0], sky, bl); break;
            case SH_FLAT: {   // кувшинка: лежит на воде, повёрнута случайно
                MQuad q;
                int rot = (int)(hash32((u32)(c->cx * 16 + x) * 73856093u ^ (u32)(c->cz * 16 + z) * 19349663u) & 3);
                const float cx4[4] = {0, 1, 1, 0}, cz4[4] = {0, 0, 1, 1};
                const u8 U4[4] = {0, 16, 16, 0}, V4[4] = {0, 0, 16, 16};
                for (int k = 0; k < 4; k++) {
                    q.p[k][0] = x + cx4[k]; q.p[k][1] = y + 0.015f; q.p[k][2] = z + cz4[k];
                    q.u[k] = U4[(k + rot) & 3]; q.v[k] = V4[(k + rot) & 3];
                    q.sky[k] = (u8)std::min(255, sky * 16); q.blk[k] = (u8)std::min(255, bl * 16); q.shade[k] = 240;
                }
                q.tile = bd.tex[0]; q.nrm = 6; q.flags = 0;
                out.push_back(q);
                break;
            }
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
                    q.sky[k] = (u8)std::min(255, sky * 16); q.blk[k] = (u8)std::min(255, bl * 16); q.shade[k] = 210;
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
            case SH_ANVIL: {   // основание, шейка и длинная рабочая плита; поворот по meta
                const int tl[6] = {T_ANVIL_SIDE, T_ANVIL_SIDE, T_ANVIL_TOP, T_ANVIL_SIDE, T_ANVIL_SIDE, T_ANVIL_SIDE};
                bool alongZ = (m & 1) != 0;
                auto box = [&](float a0, float y0, float b0, float a1, float y1, float b1) {
                    if (alongZ) emitBox(out, x, y, z, b0 / 16.f, y0 / 16.f, a0 / 16.f, b1 / 16.f, y1 / 16.f, a1 / 16.f, tl, sky, bl, 0);
                    else emitBox(out, x, y, z, a0 / 16.f, y0 / 16.f, b0 / 16.f, a1 / 16.f, y1 / 16.f, b1 / 16.f, tl, sky, bl, 0);
                };
                box(2, 0, 2, 14, 3, 14);      // основание
                box(4, 3, 3, 12, 5, 13);      // ступень
                box(5.5f, 5, 5, 10.5f, 10, 11); // шейка
                box(0, 10, 3, 16, 16, 13);    // плита
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
    shaderExtras(c, s, out);
    // оттенки биомов: трава, листва и вода чуть различаются от места к месту
    for (MQuad& q : out) {
        bool water = q.tile == T_WATER;
        if (!(water || q.tile == T_GRASS_TOP || q.tile == T_TALLGRASS || q.tile == T_FERN || q.tile == T_LEAVES || q.tile == T_GRASS_SIDE)) continue;
        int qx = clampi(ifloor((q.p[0][0] + q.p[2][0]) * 0.5f), 0, 15), qz = clampi(ifloor((q.p[0][2] + q.p[2][2]) * 0.5f), 0, 15);
        q.tint = (u8)((c->biome[qz * 16 + qx] + 1) | (water ? 0x80 : 0));
    }
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
    const float in = std::max(0.03f, 8.f / g_tileRes);   // полтекселя от края — без затекания соседних тайлов при сглаживании
    pu = clampf(pu, in, 16.f - in); pv = clampf(pv, in, 16.f - in);
    o[0] = ((tile & 15) * 16 + pu) / (float)ATLAS;
    o[1] = ((tile >> 4) * 16 + pv) / (float)ATLAS_H;
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
static float g_shake = 0;   // сила встряски камеры (урон)
static float g_hazeK = 0.5f;            // сила воздушной дымки вдали (0 — нет)
static float g_grade[3] = {1, 1, 1};     // цветокоррекция кадра (умножение)

// ---------------------------------------------------------------- погода (состояние)
enum { WX_CLEAR = 0, WX_RAIN, WX_THUNDER };
static int g_weather = WX_CLEAR;
static float g_weatherT = 600.f;   // секунд до смены погоды
static float g_rainK = 0.f;        // плавная сила осадков 0..1
static float g_thunderK = 0.f;     // плавная «грозовость» 0..1
static float g_flash = 0.f;        // вспышка молнии
static float g_boltT = 8.f;        // до следующей молнии
struct Bolt { float x, z, yTop, yBot; float life; u32 seed; };
static std::vector<Bolt> g_bolts;
struct DelayedSnd { float t; int snd; float vol, pitch; };
static std::vector<DelayedSnd> g_delayed;
static float g_precipOpen = 0.f;            // доля открытого неба вокруг игрока (для звука)

// вид осадков в столбце: 0 — нет (пустыни), 1 — дождь, 2 — снег
static int precipAt(int x, int y, int z) {
    Chunk* c = getChunk(x >> 4, z >> 4);
    int b = c ? (int)c->biome[(z & 15) * 16 + (x & 15)] : (int)BI_PLAINS;
    if (b == BI_DESERT || b == BI_SAVANNA || b == BI_BADLANDS) return 0;
    if (b == BI_SNOW || (g_genVer >= 3 && b == BI_TAIGA)) return 2;
    if (b == BI_MOUNT && y > g_sea + (g_genVer >= 3 ? 70 : 40)) return 2;
    return 1;
}
static bool isRaining() { return g_rainK > 0.3f; }
static void setWeather(int w, float dur) { g_weather = w; g_weatherT = dur; }

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
        g_lightLUT[i] = 0.11f + 0.89f * powf(0.84f, (1.f - l) * 15.f);   // мягче спад: тень переходит в свет плавно; в пещерах не кромешная тьма
    }
    for (int i = 0; i < 16; i++) g_blockLUT[i] = i == 0 ? 0.f : 0.05f + 1.2f * powf(0.9f, (float)(15 - i));   // факелы и лава светят дальше и ярче
}
static inline float lightCurve(float l) { return g_lightLUT[clampi((int)(l * 255.f), 0, 255)]; }

static std::vector<u32> g_caustFrames[16];
static int g_caustRes = 0, g_caustFrame = -1;
static bool g_caustReady = false;
static float g_blkCol[3] = {1.f, 0.86f, 0.66f};   // цвет огня: факелы тёплые, лава оранжевее (задаётся по секции)
// параметры «шейдеров» на кадр
static float g_shadowK = 0;          // сила теней от солнца/луны
static float g_lightElevQ = 0;       // высота светила над горизонтом в единицах горизонта (0..255)
static bool g_lightEast = true;      // светило на востоке (+X)
static float g_mistK = 0;            // утренний туман в низинах
static float g_mistCol[3] = {0.8f, 0.82f, 0.85f};
// видимость солнца из точки (для мобов, предметов, руки) по карте высот
static float sunVisAt(double x, double y, double z) {
    if (g_shadowK <= 0) return 1.f;
    int cx0 = ifloor(x), cz0 = ifloor(z);
    Chunk* c = getChunk(cx0 >> 4, cz0 >> 4);
    if (!c) return 1.f;
    if (c->top[((cz0 & 15) << 4) | (cx0 & 15)] + 1 > y + 0.6) return 0.f;
    int dir = g_lightEast ? 1 : -1;
    float best = 0;
    Chunk* cc = c; int lastCx = cx0 >> 4;
    for (int d = 1; d <= 40; d += (d < 10 ? 1 : 2)) {
        int wx = cx0 + d * dir;
        if ((wx >> 4) != lastCx) { lastCx = wx >> 4; cc = getChunk(lastCx, cz0 >> 4); }
        if (!cc) break;
        float h = cc->top[((cz0 & 15) << 4) | (wx & 15)] + 1 - (float)y;
        if (h > 0) best = std::max(best, h / std::max(0.3f, (float)d - 0.5f));
    }
    float hq = atanf(best) * (255.f / 1.5707963f);
    return smooth01(hq - 9.f, hq + 9.f, g_lightElevQ);
}
static void computeShaderParams();
static inline bool isMetalTile(int t) { return t == T_GOLD_BLOCK || t == T_IRON_BLOCK || t == T_DIAMOND_BLOCK || t == T_GOLD_ORE || t == T_IRON_ORE || t == T_DIAMOND_ORE; }
// dirMul — яркость грани по направлению к солнцу, flick — мерцание факелов
static inline void litColorFX(float sky15, float blk15, float shade, float dirMul, float flick, float* out) {
    float s = lightCurve(sky15 / 15.f) * g_dayLight;   // дневной множитель: ночью всё темнеет, но видно
    float bl = clampf(blk15, 0.f, 15.f); int bi = std::min(14, (int)bl);
    float b = lerpf(g_blockLUT[bi], g_blockLUT[bi + 1], bl - bi) * flick;   // непрерывно, без ступенек
    float m = lightCurve(0);                          // минимальная яркость в глубине пещер
    float sr, sg, sb;
    if (g_shaders > 0) { float sd = s * dirMul; sr = sd * g_fx.sunCol[0]; sg = sd * g_fx.sunCol[1]; sb = sd * g_fx.sunCol[2]; }
    else { sr = s * (0.72f + 0.28f * g_dayLight); sg = s * (0.80f + 0.20f * g_dayLight); sb = s; }   // ночью — холодный оттенок
    // свет неба и тёплый свет огня складываются мягко (а не «кто ярче»), чтобы у факела днём тоже было видно тепло
    float rr = sr + b * g_blkCol[0] * (1.f - std::min(1.f, sr) * 0.6f), gg = sg + b * g_blkCol[1] * (1.f - std::min(1.f, sg) * 0.6f), bb2 = sb + b * g_blkCol[2] * (1.f - std::min(1.f, sb) * 0.6f);
    out[0] = std::min(1.25f, std::max(rr, m * 0.9f)) * shade;
    out[1] = std::min(1.25f, std::max(gg, m * 0.9f)) * shade;
    out[2] = std::min(1.25f, std::max(bb2, m)) * shade;
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

static int g_quadsDrawn = 0;

static inline bool isPlantTile(int t) {
    return t == T_TALLGRASS || t == T_FLOWER_R || t == T_FLOWER_Y || t == T_SAPLING || t == T_FERN || t == T_DEADBUSH || t == T_REED ||
           t == T_FLOWER_B || t == T_FLOWER_W;
}
// оттенки биомов (множители цвета): растительность и вода
static const float BIOME_VEG[BI_COUNT][3] = {
    {1.f, 1.f, 1.f}, {0.9f, 0.98f, 0.88f}, {1.06f, 1.f, 0.82f}, {0.86f, 0.96f, 0.96f}, {0.88f, 0.96f, 0.92f}, {1.02f, 1.02f, 0.9f}, {0.95f, 1.f, 0.95f},
    {1.06f, 1.05f, 0.86f}, {0.8f, 0.94f, 0.92f}, {1.1f, 1.f, 0.72f}, {0.76f, 0.8f, 0.58f}, {1.03f, 1.06f, 0.9f}, {1.06f, 0.94f, 0.7f}, {0.84f, 1.08f, 0.78f}};
static const float BIOME_WATER[BI_COUNT][3] = {
    {1.f, 1.f, 1.f}, {0.95f, 1.f, 1.f}, {1.f, 1.05f, 0.95f}, {0.92f, 0.97f, 1.06f}, {0.95f, 1.f, 1.04f}, {0.95f, 1.06f, 1.f}, {0.88f, 0.94f, 1.06f},
    {1.f, 1.02f, 1.f}, {0.88f, 0.95f, 1.f}, {1.f, 1.05f, 0.92f}, {0.62f, 0.7f, 0.5f}, {1.f, 1.03f, 1.f}, {1.05f, 0.98f, 0.9f}, {0.86f, 1.06f, 0.95f}};
static inline const float* quadTint(const MQuad& q, float* tmp) {
    if (!q.tint) return nullptr;
    int b = (q.tint & 0x7F) - 1; if (b < 0 || b >= BI_COUNT) return nullptr;
    const float* t = (q.tint & 0x80) ? BIOME_WATER[b] : BIOME_VEG[b];
    if (q.tile == T_GRASS_SIDE) { for (int i = 0; i < 3; i++) tmp[i] = 1.f + (t[i] - 1.f) * 0.5f; return tmp; }
    return t;
}
static inline bool isLeafTile(int t) { return t == T_LEAVES || t == T_PINE_LEAVES || t == T_BIRCH_LEAVES || t == T_ACACIA_LEAVES || t == T_JUNGLE_LEAVES; }
static void drawSections() {
    g_quadsDrawn = 0;
    const bool fx = g_shaders > 0;
    const bool full = g_shaders >= 2;
    const float t = g_fxTime, wind = g_fx.wind;
    const Camera& cm = g_cam;
    for (const VisSec& vs : g_visible) {
        Chunk* c = vs.c;
        const float wx0 = (float)(c->cx * 16), wz0 = (float)(c->cz * 16);
        float ox = (float)(c->cx * 16 - g_cam.x), oy = (float)(-g_cam.y), oz = (float)(c->cz * 16 - g_cam.z);
        // базис камеры, сдвинутый в начало чанка
        float o[3]; toCam(ox, oy, oz, o);
        {   // цвет огня секции: факелы жёлто-тёплые, лава оранжевая
            float w = fx ? c->sec[vs.s].warm : 0.15f;
            g_blkCol[0] = 1.f; g_blkCol[1] = lerpf(0.84f, 0.52f, w); g_blkCol[2] = lerpf(0.6f, 0.22f, w);
        }
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
                else if (isLeafTile(q.tile)) kind = 2;
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
            float haze = g_hazeK > 0 ? powf(smooth01(10.f, g_fogEnd, sqrtf(d2)), 1.25f) * g_hazeK : 0.f;
            float tintTmp[3]; const float* tint = quadTint(q, tintTmp);
            if (kind == 3) {
                // вода: отражение неба по Френелю и солнечный блик
                for (int k = 0; k < 4; k++) {
                    tileUV(T_WATER_FX, q.u[k], q.v[k], UV[k]);
                    float vx = L[k][0] + ox, vy = L[k][1] + oy, vz = L[k][2] + oz;
                    float vl = sqrtf(vx * vx + vy * vy + vz * vz) + 1e-4f; vx /= vl; vy /= vl; vz /= vl;
                    float cosv = fabsf(vy);
                    float fres = 0.12f + 0.88f * powf(1.f - cosv, 4.f);
                    float base[3]; litColor(q.sky[k] / 16.f, q.blk[k] / 16.f, 1.f, base);
                    const float deep[3] = {0.13f, 0.32f, 0.66f};
                    float spec = 0, glit = 0;
                    float wxk = wx0 + L[k][0], wzk = wz0 + L[k][2];
                    if (vy < 0) {   // смотрим на воду сверху: отражённый луч
                        float rx = vx, ry = -vy, rz = vz;
                        float dp = rx * g_fx.sun[0] + ry * g_fx.sun[1] + rz * g_fx.sun[2];
                        if (dp > 0.9f) spec = powf(dp, 90.f) * g_fx.sunUp * (q.sky[k] / 240.f) * 1.4f;
                        // золотая дорожка от низкого солнца: широкий блик, рассыпанный на искры волн
                        if (full && g_fx.sunset > 0.05f && dp > 0.55f) {
                            float sp = sinf(t * 3.1f + wxk * 2.3f + wzk * 1.7f) * sinf(t * 2.3f - wxk * 1.1f + wzk * 2.9f);
                            glit = powf(dp, 14.f) * (0.45f + 0.55f * std::max(0.f, sp)) * g_fx.sunset * g_fx.sunUp * (q.sky[k] / 240.f) * 1.3f * (1.f - g_rainK);
                        }
                    }
                    float foam = 0;
                    if (q.ex[k]) foam = q.ex[k] / 255.f * (0.55f + 0.45f * sinf(t * 1.6f + wxk * 1.3f + wzk * 0.9f));   // пена у берега дышит
                    const float gold[3] = {1.f, 0.66f, 0.26f};
                    for (int i = 0; i < 3; i++) {
                        float col = deep[i] * base[i] * 1.25f;
                        col = lerpf(col, g_skyCol[i] * 0.95f, fres * 0.75f * (q.sky[k] / 240.f));
                        if (tint) col *= tint[i];
                        col = lerpf(col, base[i] * 0.95f, foam * 0.75f);
                        C[k][i] = std::min(1.f, col + spec * g_fx.sunCol[i] + glit * gold[i]);
                    }
                    C[k][3] = fa * clampf(0.58f + fres * 0.38f + spec * 0.3f + glit * 0.3f + foam * 0.35f, 0, 0.97f);
                    for (int i = 0; i < 3; i++) C[k][i] = lerpf(C[k][i], g_skyCol[i], haze);
                }
            } else {
                float alpha = fa * ((q.flags & 1) ? (q.tile == T_WATER ? 0.72f : 0.85f) : 1.f);
                float dirMul = (fx && q.nrm < 6) ? g_fx.faceMul[q.nrm] : 1.f;
                float flick = 1.f;
                if (fx && std::max(std::max(q.blk[0], q.blk[1]), std::max(q.blk[2], q.blk[3])) > 2 * 16)
                    flick = 0.95f + 0.05f * sinf(t * 7.3f + (wx0 + q.p[0][0]) * 1.7f + (wz0 + q.p[0][2]) * 2.3f) * sinf(t * 3.1f + q.p[0][1]);
                const bool metal = full && isMetalTile(q.tile);
                for (int k = 0; k < 4; k++) {
                    tileUV(q.tile, q.u[k], q.v[k], UV[k]);
                    float sh = q.shade[k] / 255.f;
                    float vis = 1.f;
                    if (g_shadowK > 0) {   // тень от солнца: светило ниже «горизонта» вершины
                        float h = g_lightEast ? q.hzE[k] : q.hzW[k];
                        vis = smooth01(h - 9.f, h + 9.f, g_lightElevQ);
                    }
                    if (q.flags & 2) { C[k][0] = C[k][1] = C[k][2] = (q.flags & 4) ? 1.f : sh * 1.05f; }
                    else litColorFX(q.sky[k] / 16.f, q.blk[k] / 16.f, sh, dirMul * (1.f - g_shadowK * (1.f - vis)), flick, C[k]);
                    if (tint) for (int i = 0; i < 3; i++) C[k][i] *= tint[i];
                    if (metal && vis > 0.05f && q.nrm < 6) {   // металл и самоцветы блестят на солнце
                        float vx = -(L[k][0] + ox), vy = -(L[k][1] + oy), vz = -(L[k][2] + oz);
                        float vl = sqrtf(vx * vx + vy * vy + vz * vz) + 1e-4f; vx /= vl; vy /= vl; vz /= vl;
                        float hx = vx + g_fx.sun[0], hy = vy + g_fx.sun[1], hz = vz + g_fx.sun[2];
                        float hl = sqrtf(hx * hx + hy * hy + hz * hz) + 1e-4f;
                        const float* nn = NRM[q.nrm];
                        float nh = std::max(0.f, (nn[0] * hx + nn[1] * hy + nn[2] * hz) / hl);
                        bool ore = q.tile == T_GOLD_ORE || q.tile == T_IRON_ORE || q.tile == T_DIAMOND_ORE;
                        float spk = 0.6f + 0.4f * sinf(t * 4.f + (wx0 + q.p[k][0]) * 7.1f + q.p[k][1] * 5.3f + (wz0 + q.p[k][2]) * 3.7f);
                        float sp = (powf(nh, ore ? 18.f : 40.f) * (ore ? 0.7f : 1.3f) + (ore ? 0.05f : 0.1f) * spk) * g_fx.sunUp * vis * (q.sky[k] / 240.f) * spk * (1.f - g_rainK);
                        static const float MC_GOLD[3] = {1.f, 0.82f, 0.4f}, MC_DIA[3] = {0.6f, 1.f, 1.f}, MC_IRON[3] = {1.f, 1.f, 1.f};
                        const float* mc = (q.tile == T_GOLD_BLOCK || q.tile == T_GOLD_ORE) ? MC_GOLD
                                        : (q.tile == T_DIAMOND_BLOCK || q.tile == T_DIAMOND_ORE) ? MC_DIA : MC_IRON;
                        for (int i = 0; i < 3; i++) C[k][i] = std::min(1.25f, C[k][i] + sp * mc[i]);
                    }
                    if (haze > 0) for (int i = 0; i < 3; i++) C[k][i] = lerpf(C[k][i], g_skyCol[i], haze);
                    if (g_mistK > 0) {   // туман стелется по низинам и над водой
                        float m = g_mistK * (1.f - smooth01((float)g_sea + 3.f, (float)g_sea + 24.f, L[k][1])) * smooth01(3.f, 36.f, sqrtf(d2));
                        for (int i = 0; i < 3; i++) C[k][i] = lerpf(C[k][i], g_mistCol[i], m);
                    }
                    C[k][3] = alpha;
                }
            }
            emitQuadCam(P, UV, C);
            g_quadsDrawn++;
            if (kind == 0 && (q.flags & 8) && full && d2 < 26.f * 26.f && q.sky[0] >= 16 && g_caustReady) {
                // подводные блики: сетка света от волн поверх дна (рисуется сразу после него)
                float a = fa * (1.f - smooth01(12.f, 26.f, sqrtf(d2))) * (0.45f + 0.55f * smooth01(1.f * 16, 8.f * 16, (float)q.sky[0])) * (0.35f + 0.65f * g_dayLight) * (1.f - 0.6f * g_rainK);
                if (a > 0.02f) {
                    float mnx = std::min(std::min(q.p[0][0], q.p[1][0]), std::min(q.p[2][0], q.p[3][0]));
                    float mnz = std::min(std::min(q.p[0][2], q.p[1][2]), std::min(q.p[2][2], q.p[3][2]));
                    int bxw = ifloor(wx0 + mnx + 0.01f), bzw = ifloor(wz0 + mnz + 0.01f);
                    int tile = T_CAUST0 + (bxw & 1) + ((bzw & 1) << 1);
                    float CU[4][2], CC[4][4];
                    for (int k = 0; k < 4; k++) {
                        tileUV(tile, (q.p[k][0] - mnx) * 16.f, (q.p[k][2] - mnz) * 16.f, CU[k]);
                        static const float CAU[3] = {0.75f, 1.f, 0.92f};   // свет сквозь воду — с бирюзовым оттенком
                        for (int i = 0; i < 3; i++) CC[k][i] = std::min(1.f, C[k][i] * 2.2f + 0.22f * g_fx.sunUp * CAU[i]);
                        CC[k][3] = std::min(0.9f, a);
                    }
                    emitQuadCam(P, CU, CC, -0.02f);
                }
            }
        }
    }
}

// ============================================================================
//  ДАЛЬНИЙ ПЛАН: упрощённый рельеф до 40 чанков (за пределами полной детализации)
// ============================================================================
// каждый чанк дальнего плана — сетка 4x4 клеток по 4x4 блока: высота, цвет поверхности, вода;
// вдали клетки укрупняются до 8 и 16 блоков. Точные данные берутся из настоящих чанков, когда они были загружены,
// остальное — оценка по генератору (рельеф, биом, кроны деревьев)
struct LodQ { float p[4][3]; float rgb[3]; u8 nrm; };
struct LodChunk {
    int cx = 0, cz = 0;
    u8 h[16];          // высота верха клетки (блок над поверхностью)
    float col[16][3];  // средний цвет верха
    u8 wat[16];        // 1 — вода
    bool exact = false;
    int level = -1;    // 0 — клетки 4, 1 — 8, 2 — 16 блоков
    bool dirty = true;
    float ymin = 0, ymax = 0;
    std::vector<LodQ> q;
};
static std::unordered_map<i64, LodChunk*> g_lod;
static std::vector<std::pair<int, int>> g_lodOffsets;   // по возрастанию расстояния, радиус 44
static float g_blockAvg[B_COUNT][3];                     // средний цвет верхней грани блока
static int g_viewDist = 16;                              // полная дальность (вместе с дальним планом)
static int g_lodDrawn = 0;

static void initLodTables() {
    for (int dz = -44; dz <= 44; dz++) for (int dx = -44; dx <= 44; dx++) if (dx * dx + dz * dz <= 44 * 44) g_lodOffsets.push_back(std::make_pair(dx, dz));
    std::sort(g_lodOffsets.begin(), g_lodOffsets.end(), [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
        return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second; });
}
// средние цвета из 16-пиксельного атласа (верхняя грань)
static void computeBlockAvg() {
    for (int b = 0; b < B_COUNT; b++) {
        int tile = BD[b].tex[2]; if (BD[b].shape == SH_CROSS || BD[b].shape == SH_FLAT) tile = BD[b].tex[0];
        float r = 0, g = 0, bl = 0, n = 0;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
            u32 c = g_atlasPx[((tile >> 4) * 16 + y) * ATLAS + (tile & 15) * 16 + x];
            if (pA(c) < 128) continue;
            r += pR(c); g += pG(c); bl += pB(c); n++;
        }
        if (n < 1) { g_blockAvg[b][0] = g_blockAvg[b][1] = g_blockAvg[b][2] = 0.5f; continue; }
        g_blockAvg[b][0] = r / n / 255.f; g_blockAvg[b][1] = g / n / 255.f; g_blockAvg[b][2] = bl / n / 255.f;
    }
    // вода издалека — глубокий синий с отражением неба
    g_blockAvg[B_WATER][0] = 0.16f; g_blockAvg[B_WATER][1] = 0.33f; g_blockAvg[B_WATER][2] = 0.62f;
}
static inline void lodTint(int block, int biome, float* c) {
    if (biome < 0 || biome >= BI_COUNT) return;
    const float* t = nullptr;
    if (block == B_WATER) t = BIOME_WATER[biome];
    else if (block == B_GRASS || block == B_LEAVES || block == B_TALLGRASS) t = BIOME_VEG[biome];
    if (t) for (int i = 0; i < 3; i++) c[i] *= t[i];
}
static LodChunk* lodGet(int cx, int cz) { auto it = g_lod.find(ckey(cx, cz)); return it == g_lod.end() ? nullptr : it->second; }
static LodChunk* lodSlot(int cx, int cz) {
    LodChunk*& l = g_lod[ckey(cx, cz)];
    if (!l) { l = new LodChunk(); l->cx = cx; l->cz = cz; }
    return l;
}
static void lodMarkNeighbors(int cx, int cz) {
    for (int d = 0; d < 4; d++) { LodChunk* n = lodGet(cx + HDX[d], cz + HDZ[d]); if (n) n->dirty = true; }
}
// точные данные из настоящего чанка
static void lodFromChunk(Chunk* c) {
    LodChunk* l = lodSlot(c->cx, c->cz);
    for (int cz = 0; cz < 4; cz++) for (int cx = 0; cx < 4; cx++) {
        float hs = 0, r = 0, g = 0, b = 0; int wn = 0;
        for (int z = cz * 4; z < cz * 4 + 4; z++) for (int x = cx * 4; x < cx * 4 + 4; x++) {
            int y = WH - 1; u8 bl = B_AIR;
            for (; y > 0; y--) { bl = c->blk[cidx(x, y, z)]; if (bl != B_AIR && !isSmallPlant(bl) && bl != B_TORCH) break; }
            float cc[3] = {g_blockAvg[bl][0], g_blockAvg[bl][1], g_blockAvg[bl][2]};
            lodTint(bl, c->biome[z * 16 + x], cc);
            hs += y + 1; r += cc[0]; g += cc[1]; b += cc[2];
            if (bl == B_WATER || bl == B_ICE) wn++;
        }
        int i = cz * 4 + cx;
        l->h[i] = (u8)clampi((int)lroundf(hs / 16.f), 1, 255);
        l->col[i][0] = r / 16.f; l->col[i][1] = g / 16.f; l->col[i][2] = b / 16.f;
        l->wat[i] = wn >= 8;
    }
    l->exact = true; l->dirty = true;
    lodMarkNeighbors(c->cx, c->cz);
}
// оценка по генератору (чанк ещё ни разу не загружался)
static void lodEstimate(int ccx, int ccz) {
    LodChunk* l = lodSlot(ccx, ccz);
    const int SEA = g_sea;
    for (int cz = 0; cz < 4; cz++) for (int cx = 0; cx < 4; cx++) {
        int wx = ccx * 16 + cx * 4 + 2, wz = ccz * 16 + cz * 4 + 2;
        ColInfo ci = columnInfo(wx, wz);
        int h = ci.h; u8 top = B_GRASS;
        switch (ci.biome) {
            case BI_DESERT: case BI_BEACH: top = B_SAND; break;
            case BI_OCEAN: top = B_SAND; break;
            case BI_SNOW: top = B_SNOW; break;
            case BI_MOUNT: top = h > SEA + 92 || (g_genVer < 3 && h > 104) ? B_SNOW : B_STONE; break;
            case BI_SAVANNA: top = B_DRYGRASS; break;
            case BI_BADLANDS: top = h < SEA + 16 ? (u8)B_REDSAND : terraBand(wx, h, wz); break;
            case BI_TAIGA: top = B_GRASS; break;
            default: top = B_GRASS; break;
        }
        bool water = h < SEA;
        float canopy = 0; int th = 5; u8 leaf = B_LEAVES;
        switch (ci.biome) {
            case BI_FOREST: canopy = 0.62f; break;     case BI_BIRCH: canopy = 0.55f; leaf = B_BIRCH_LEAVES; break;
            case BI_TAIGA: canopy = 0.6f; leaf = B_PINE_LEAVES; th = 7; break;
            case BI_JUNGLE: canopy = 0.85f; leaf = B_JUNGLE_LEAVES; th = 9; break;
            case BI_SWAMP: canopy = 0.3f; break;       case BI_SAVANNA: canopy = 0.1f; leaf = B_ACACIA_LEAVES; th = 6; break;
            case BI_SNOW: canopy = 0.12f; leaf = B_PINE_LEAVES; th = 7; break;
            case BI_PLAINS: canopy = 0.04f; break;     case BI_MOUNT: canopy = 0.06f; leaf = B_PINE_LEAVES; th = 7; break;
            default: break;
        }
        if (g_genVer < 3 && ci.biome == BI_FOREST && ci.temp < -0.05f) leaf = B_PINE_LEAVES;
        // кроны: плотность леса поднимает «полог» и окрашивает клетку листвой (без одиночных столбиков)
        float dens = water ? 0.f : clampf(canopy + ((hash3i(wx, 7, wz, g_seed ^ 0x10D) & 1023) / 1023.f - 0.5f) * 0.35f, 0.f, 1.f);
        if (canopy < 0.2f) dens *= 0.5f;
        int i = cz * 4 + cx;
        float cc[3], lc[3];
        memcpy(cc, water ? g_blockAvg[B_WATER] : g_blockAvg[top], sizeof(cc));
        memcpy(lc, g_blockAvg[leaf], sizeof(lc));
        lodTint(water ? (int)B_WATER : (int)top, ci.biome, cc);
        if (leaf == B_LEAVES) lodTint(B_LEAVES, ci.biome, lc);
        float lk = clampf(dens * 1.4f, 0.f, 1.f);
        for (int k = 0; k < 3; k++) cc[k] = lerpf(cc[k], lc[k], lk);
        if (water && h < SEA - 12) for (int k = 0; k < 3; k++) cc[k] *= 0.85f;   // глубина темнее
        memcpy(l->col[i], cc, sizeof(cc));
        l->h[i] = (u8)clampi(water ? SEA + 1 : h + 1 + (int)lroundf(dens * th), 1, 255);
        l->wat[i] = water;
    }
    l->exact = false; l->dirty = true;
    lodMarkNeighbors(ccx, ccz);
}
// высота клетки уровня L (клетки по 4<<L блоков) с индексом в мировой сетке клеток
static bool lodCellAt(int gx, int gz, int L, float& h, float* col, bool& wat) {
    int n = 4 >> L, cs = 1 << L;   // клеток уровня L в чанке; базовых клеток в одной
    int ccx = gx >= 0 ? gx / n : -((-gx + n - 1) / n), ccz = gz >= 0 ? gz / n : -((-gz + n - 1) / n);
    LodChunk* l = lodGet(ccx, ccz);
    if (!l) return false;
    int lx = gx - ccx * n, lz = gz - ccz * n;
    float hs = 0, r = 0, g = 0, b = 0; int wn = 0, cnt = 0;
    for (int z = lz * cs; z < lz * cs + cs; z++) for (int x = lx * cs; x < lx * cs + cs; x++) {
        int i = z * 4 + x; hs += l->h[i]; r += l->col[i][0]; g += l->col[i][1]; b += l->col[i][2]; wn += l->wat[i]; cnt++;
    }
    h = hs / cnt; wat = wn * 2 >= cnt;
    if (col) { col[0] = r / cnt; col[1] = g / cnt; col[2] = b / cnt; }
    return true;
}
static void lodBuildMesh(LodChunk* l, int L) {
    l->q.clear(); l->level = L; l->dirty = false;
    int n = 4 >> L; float cs = 16.f / n;
    l->ymin = 1e9f; l->ymax = -1e9f;
    for (int cz = 0; cz < n; cz++) for (int cx = 0; cx < n; cx++) {
        int gx = l->cx * n + cx, gz = l->cz * n + cz;
        float h, col[3]; bool wat;
        lodCellAt(gx, gz, L, h, col, wat);
        if (!wat) h = floorf(h + 0.5f);
        float x0 = cx * cs, z0 = cz * cs, x1 = x0 + cs, z1 = z0 + cs;
        float top = wat ? h - 0.12f : h;
        LodQ q;
        q.nrm = 2; memcpy(q.rgb, col, sizeof(col));
        const float T[4][3] = {{x0, top, z0}, {x1, top, z0}, {x1, top, z1}, {x0, top, z1}};
        memcpy(q.p, T, sizeof(T));
        l->q.push_back(q);
        l->ymax = std::max(l->ymax, top);
        // боковые стенки к более низким соседям (у края без соседа — «юбка» вниз)
        float side[3] = {col[0] * 0.82f, col[1] * 0.8f, col[2] * 0.78f};
        if (!wat) {   // стенки — землистого оттенка: под травой видна земля и камень
            float soil = h > g_sea + 60 ? 0.f : 0.45f;
            const float* sc = h > g_sea + 60 ? g_blockAvg[B_STONE] : g_blockAvg[B_DIRT];
            for (int k = 0; k < 3; k++) side[k] = lerpf(col[k], sc[k], soil) * 0.85f;
        }
        for (int d = 0; d < 4; d++) {
            float nh; bool nw;
            float lo;
            if (lodCellAt(gx + HDX[d], gz + HDZ[d], L, nh, nullptr, nw)) { if (!nw) nh = floorf(nh + 0.5f); else nh -= 0.12f; lo = nh; }
            else lo = top - 8.f;
            if (lo >= top - 0.01f) continue;
            lo -= 1.f;   // небольшой нахлёст скрывает щели между уровнями
            LodQ s; s.nrm = (u8)(d == 0 ? 0 : d == 1 ? 4 : d == 2 ? 1 : 5);
            memcpy(s.rgb, side, sizeof(side));
            float P4[4][3];
            switch (d) {
                case 0: { float P[4][3] = {{x1, top, z0}, {x1, top, z1}, {x1, lo, z1}, {x1, lo, z0}}; memcpy(P4, P, sizeof(P)); break; }   // +X
                case 1: { float P[4][3] = {{x1, top, z1}, {x0, top, z1}, {x0, lo, z1}, {x1, lo, z1}}; memcpy(P4, P, sizeof(P)); break; }   // +Z
                case 2: { float P[4][3] = {{x0, top, z1}, {x0, top, z0}, {x0, lo, z0}, {x0, lo, z1}}; memcpy(P4, P, sizeof(P)); break; }   // -X
                default: { float P[4][3] = {{x0, top, z0}, {x1, top, z0}, {x1, lo, z0}, {x0, lo, z0}}; memcpy(P4, P, sizeof(P)); break; } // -Z
            }
            memcpy(s.p, P4, sizeof(P4));
            l->q.push_back(s);
            l->ymin = std::min(l->ymin, lo);
        }
        l->ymin = std::min(l->ymin, top);
    }
}
static void lodClear() {
    for (auto& kv : g_lod) delete kv.second;
    g_lod.clear();
}
// подгрузка оценок вокруг игрока (в пределах бюджета времени)
static double nowMs();
static void manageLod(double budgetMs, double px, double pz, int rd) {
    if (g_viewDist <= rd) return;
    double t0 = nowMs();
    int pcx = ifloor(px) >> 4, pcz = ifloor(pz) >> 4;
    int R = g_viewDist + 1;
    int made = 0;
    for (auto& o : g_lodOffsets) {
        int d2 = o.first * o.first + o.second * o.second;
        if (d2 > R * R) break;
        int cx = pcx + o.first, cz = pcz + o.second;
        if (lodGet(cx, cz)) continue;
        lodEstimate(cx, cz); made++;
        if ((made & 3) == 0 && nowMs() - t0 > budgetMs) break;
    }
    // выгрузка далёких
    static double lastClean = 0;
    if (t0 - lastClean > 2000) {
        lastClean = t0;
        int K = g_viewDist + 6;
        for (auto it = g_lod.begin(); it != g_lod.end();) {
            LodChunk* l = it->second;
            if (abs(l->cx - pcx) > K || abs(l->cz - pcz) > K) { delete l; it = g_lod.erase(it); } else ++it;
        }
    }
}
static void drawLod() {
    g_lodDrawn = 0;
    if (g_viewDist <= g_renderDist || g_lod.empty()) return;
    const Camera& cm = g_cam;
    int pcx = ifloor(g_cam.x) >> 4, pcz = ifloor(g_cam.z) >> 4;
    int rd = g_renderDist;
    float dirMulTop = g_shaders > 0 ? g_fx.faceMul[2] : 1.f;
    for (auto& o : g_lodOffsets) {
        int d2 = o.first * o.first + o.second * o.second;
        if (d2 > g_viewDist * g_viewDist) break;
        int cx = pcx + o.first, cz = pcz + o.second;
        if (d2 <= rd * rd && abs(o.first) <= rd && abs(o.second) <= rd) {
            Chunk* c = getChunk(cx, cz);
            if (c && c->meshed) continue;   // здесь рисуется настоящий чанк
        }
        LodChunk* l = lodGet(cx, cz);
        if (!l) continue;
        float dist = sqrtf((float)d2);
        int L = dist < 14.f ? 0 : (dist < 26.f ? 1 : 2);
        if (l->dirty || l->level != L) lodBuildMesh(l, L);
        if (l->q.empty()) continue;
        float ox = (float)(cx * 16 - g_cam.x), oy = (float)(-g_cam.y), oz = (float)(cz * 16 - g_cam.z);
        float hy = (l->ymax - l->ymin) * 0.5f;
        if (!sphereVisible(ox + 8, oy + (l->ymin + l->ymax) * 0.5f, oz + 8, sqrtf(128.f + hy * hy) + 1.f)) continue;
        float o3[3]; toCam(ox, oy, oz, o3);
        for (const LodQ& q : l->q) {
            if (q.nrm != 2) {
                const float* nn = NRM[q.nrm];
                float dd = (q.p[0][0] + ox) * nn[0] + (q.p[0][1] + oy) * nn[1] + (q.p[0][2] + oz) * nn[2];
                if (dd >= 0) continue;
            } else if (q.p[0][1] + oy > 0) continue;   // верх выше глаз не виден
            float P[4][3], UV[4][2], C[4][4];
            float mx = 0, my = 0, mz = 0;
            for (int k = 0; k < 4; k++) {
                const float* lp = q.p[k];
                P[k][0] = o3[0] + lp[0] * cm.r[0] + lp[1] * cm.r[1] + lp[2] * cm.r[2];
                P[k][1] = o3[1] + lp[0] * cm.u[0] + lp[1] * cm.u[1] + lp[2] * cm.u[2];
                P[k][2] = o3[2] + lp[0] * cm.f[0] + lp[1] * cm.f[1] + lp[2] * cm.f[2];
                mx += P[k][0]; my += P[k][1]; mz += P[k][2];
                tileUV(T_WHITE, 8, 8, UV[k]);
            }
            mx *= 0.25f; my *= 0.25f; mz *= 0.25f;
            float dq2 = mx * mx + my * my + mz * mz;
            float fa = fogAlpha(dq2);
            if (fa <= 0.01f) continue;
            float lit[3];
            float dm = q.nrm == 2 ? dirMulTop : (g_shaders > 0 ? g_fx.faceMul[q.nrm] : 1.f);
            litColorFX(15.f, 0.f, FACE_SHADE[q.nrm], dm, 1.f, lit);
            float haze = g_hazeK > 0 ? powf(smooth01(10.f, g_fogEnd, sqrtf(dq2)), 1.25f) * g_hazeK : 0.f;
            float mist = 0;
            if (g_mistK > 0) mist = g_mistK * (1.f - smooth01((float)g_sea + 3.f, (float)g_sea + 24.f, q.p[0][1])) * 0.85f;
            for (int k = 0; k < 4; k++) {
                for (int i = 0; i < 3; i++) C[k][i] = lerpf(lerpf(q.rgb[i] * lit[i], g_skyCol[i], haze), g_mistCol[i], mist);
                C[k][3] = fa;
            }
            emitPolyCam(4, P, UV, C, dq2 + 1e6f);
            g_lodDrawn++;
        }
    }
}

// ---------------------------------------------------------------- небо
static float g_dayTime = 0.05f;       // 0..1 (0 — рассвет, 0.25 — полдень, 0.5 — закат, 0.75 — полночь)
static int g_dayCount = 0;            // прожитые сутки — от них зависит фаза луны (8 фаз)
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

static float g_waterDepth = 0;   // сколько воды над глазами (для цвета глубины)
static void updateSkyColors(bool underwater) {
    float sh = sunHeight();
    float dl = smooth01(-0.18f, 0.28f, sh);
    g_dayLight = 0.26f + 0.74f * dl;
    float day[3] = {0.53f, 0.74f, 0.98f}, night[3] = {0.03f, 0.04f, 0.09f};
    float zday[3] = {0.30f, 0.50f, 0.92f}, znight[3] = {0.01f, 0.01f, 0.04f};
    for (int i = 0; i < 3; i++) { g_skyCol[i] = lerpf(night[i], day[i], dl); g_zenCol[i] = lerpf(znight[i], zday[i], dl); }
    float sunset = clampf(1.f - fabsf(sh) * 4.f, 0, 1) * 0.65f;
    g_skyCol[0] = lerpf(g_skyCol[0], 0.98f, sunset); g_skyCol[1] = lerpf(g_skyCol[1], 0.55f, sunset); g_skyCol[2] = lerpf(g_skyCol[2], 0.30f, sunset);
    if (underwater) {
        if (g_shaders > 0) {   // у поверхности — бирюзовая, в глубине — тёмно-синяя толща
            float dk = smooth01(1.f, 22.f, g_waterDepth);
            const float shallow[3] = {0.10f, 0.42f, 0.52f}, deepc[3] = {0.02f, 0.08f, 0.22f};
            for (int i = 0; i < 3; i++) g_skyCol[i] = lerpf(shallow[i], deepc[i], dk) * (0.25f + 0.75f * g_dayLight);
        } else { g_skyCol[0] = 0.05f; g_skyCol[1] = 0.18f; g_skyCol[2] = 0.45f * g_dayLight + 0.05f; }
        for (int i = 0; i < 3; i++) g_zenCol[i] = g_skyCol[i];
    }
    g_hazeK = underwater ? 0.f : 0.32f + 0.25f * dl;
    // цветокоррекция: днём чуть тёплый, на закате — золотой, ночью — холодный лунный
    const float gDay[3] = {1.f, 0.985f, 0.95f}, gSet[3] = {1.f, 0.9f, 0.78f}, gNight[3] = {0.78f, 0.86f, 1.f};
    for (int i = 0; i < 3; i++) { float c = lerpf(gNight[i], gDay[i], dl); g_grade[i] = lerpf(c, gSet[i], sunset * 1.2f); }
    if (underwater) { g_grade[0] = 0.7f; g_grade[1] = 0.92f; g_grade[2] = 1.f; }
}

static void drawSkyGlow();
static void fillRectF(float x, float y, float w, float h, float r, float g, float b, float a) {
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_ren, (Uint8)(clampf(r, 0, 1) * 255), (Uint8)(clampf(g, 0, 1) * 255), (Uint8)(clampf(b, 0, 1) * 255), (Uint8)(clampf(a, 0, 1) * 255));
    SDL_Rect rr = {(int)floorf(x), (int)floorf(y), (int)ceilf(w), (int)ceilf(h)};
    SDL_RenderFillRect(g_ren, &rr);
}
// северное сияние: колышущиеся занавеси — зелёные внизу, сиреневые вверху. Видно ясной ночью в снегах, тайге и горах
static float g_auroraK = 0;
static bool projectDir(const float* dir, float& sx, float& sy);
static void drawAurora(float starA) {
    if (g_shaders < 2) { g_auroraK = 0; return; }
    int bx = ifloor(g_cam.x), bz = ifloor(g_cam.z);
    Chunk* ch = getChunk(bx >> 4, bz >> 4);
    int bi = ch ? ch->biome[(bz & 15) * 16 + (bx & 15)] : BI_PLAINS;
    float want = (bi == BI_SNOW || bi == BI_TAIGA || bi == BI_MOUNT) ? 1.f : 0.f;
    g_auroraK += (want - g_auroraK) * 0.02f;   // плавно появляется при входе в холодные края
    float A = g_auroraK * starA * (1.f - g_rainK);
    if (A < 0.02f) return;
    const float t = g_fxTime;
    const int SEG = 120;
    static std::vector<SDL_Vertex> vb; static std::vector<int> ib;
    for (int cur = 0; cur < 3; cur++) {
        vb.clear(); ib.clear();
        float az0 = -1.7f + cur * 1.1f, span = 2.4f;     // азимут: занавеси тянутся вдоль севера (−Z)
        float e0 = 0.22f + cur * 0.06f, hgt = 0.32f + cur * 0.05f;
        for (int i = 0; i <= SEG; i++) {
            float u = i / (float)SEG;
            float az = az0 + u * span + sinf(u * 7.f + t * 0.23f + cur * 2.f) * 0.08f;
            float fold = sinf(u * 13.f - t * 0.6f + cur) * 0.5f + 0.5f;           // складки
            float ray = sinf(u * 97.f + t * 0.7f + cur * 5.f) * sinf(u * 41.f - t * 0.45f);   // вертикальные лучи-струны
            float bright = (0.45f + 0.55f * fold) * (0.6f + 0.4f * sinf(u * 4.f + t * 0.4f + cur * 1.7f)) * (0.55f + 0.45f * ray * ray);
            float edge = smooth01(0.f, 0.12f, u) * smooth01(1.f, 0.88f, u);
            float el0 = e0 + sinf(u * 5.f + t * 0.3f + cur) * 0.03f, el1 = el0 + hgt * (0.7f + 0.3f * fold);
            float d0[3] = {sinf(az) * cosf(el0), sinf(el0), -cosf(az) * cosf(el0)}, d1[3] = {sinf(az) * cosf(el1), sinf(el1), -cosf(az) * cosf(el1)};
            float x0, y0, x1, y1;
            bool ok0 = projectDir(d0, x0, y0), ok1 = projectDir(d1, x1, y1);
            if (!ok0 || !ok1) { x0 = y0 = x1 = y1 = -99999.f; }
            float a = A * bright * edge * (ok0 && ok1 ? 1.f : 0.f);
            SDL_Vertex v0, v1;
            v0.position.x = x0; v0.position.y = y0; v0.color = mkColor(0.15f, 0.95f, 0.55f, std::min(1.f, a * 1.5f));
            v1.position.x = x1; v1.position.y = y1; v1.color = mkColor(0.55f, 0.25f, 0.85f, 0.f);
            v0.tex_coord.x = v0.tex_coord.y = v1.tex_coord.x = v1.tex_coord.y = 0;
            vb.push_back(v0); vb.push_back(v1);
            if (i > 0 && ok0 && ok1 && vb[vb.size() - 4].position.x > -9999.f) {
                int b = (int)vb.size() - 4;
                // середина полосы ярче: добавим третью точку по высоте
                ib.push_back(b); ib.push_back(b + 1); ib.push_back(b + 2);
                ib.push_back(b + 1); ib.push_back(b + 3); ib.push_back(b + 2);
            }
        }
        if (!ib.empty()) {
            SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_ADD);
            SDL_RenderGeometry(g_ren, NULL, vb.data(), (int)vb.size(), ib.data(), (int)ib.size());
            // вторая, сиреневая кайма поверх верхней части
            for (size_t k = 0; k < vb.size(); k += 2) {
                SDL_Vertex& lo = vb[k]; SDL_Vertex& hi = vb[k + 1];
                float a = lo.color.a / 255.f;
                SDL_FPoint mid = {(lo.position.x + hi.position.x) * 0.5f, (lo.position.y + hi.position.y) * 0.5f};
                lo.position = mid; lo.color = mkColor(0.5f, 0.3f, 0.95f, a * 0.5f);
                hi.color = mkColor(0.6f, 0.2f, 0.8f, 0.f);
            }
            SDL_RenderGeometry(g_ren, NULL, vb.data(), (int)vb.size(), ib.data(), (int)ib.size());
            SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
        }
    }
}
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
    float starA = clampf(-sh * 3.f, 0, 1) * (1.f - g_rainK);
    if (starA > 0.02f) {
        SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
        const bool tw = g_shaders >= 1;
        int si = 0;
        for (const Star& s : g_stars) {
            si++;
            // вращение вокруг оси Z вместе с небом
            float x = s.x * sc - s.y * sh, y = s.x * sh + s.y * sc, z = s.z;
            float p[3]; toCam(x, y, z, p);
            if (p[2] < 0.05f) continue;
            float sx = c.cx + p[0] / p[2] * c.focal, sy = c.cy - p[1] / p[2] * c.focal;
            if (sx < 0 || sy < 0 || sx > g_W || sy > g_H) continue;
            float a = starA * s.b;
            u8 cr = 255, cg = 255, cb = 240;
            if (tw) {
                // мерцание: у горизонта воздух неспокойнее — звёзды дрожат сильнее
                float hor = 1.f - clampf(y * 2.5f, 0.f, 1.f);
                float ph = (float)(hash32((u32)si * 2654435761u) % 1000) * 0.00628f;
                float sp = 1.3f + (si % 7) * 0.45f;
                float k = 0.5f + 0.5f * sinf(g_fxTime * sp + ph) * sinf(g_fxTime * sp * 1.7f + ph * 2.f);
                a *= 1.f - (0.35f + 0.4f * hor) * (1.f - k);
                int tint = si % 9;   // немного голубых и тёплых звёзд
                if (tint == 0) { cr = 190; cg = 215; cb = 255; } else if (tint == 1) { cr = 255; cg = 220; cb = 180; }
            }
            SDL_SetRenderDrawColor(g_ren, cr, cg, cb, (Uint8)(255 * clampf(a, 0, 1)));
            int sz = (g_H > 800 ? 3 : 2) + (tw && s.b > 0.93f ? 1 : 0);
            SDL_Rect rr = {(int)sx, (int)sy, sz, sz};
            SDL_RenderFillRect(g_ren, &rr);
        }
        drawAurora(starA);
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
        SDL_Rect src = {(tile & 15) * g_tileRes, (tile >> 4) * g_tileRes, g_tileRes, g_tileRes};
        SDL_Rect dst = {(int)(sx - size / 2), (int)(sy - size / 2), (int)size, (int)size};
        SDL_SetTextureColorMod(g_atlasTex, 255, 255, 255);
        SDL_SetTextureAlphaMod(g_atlasTex, (Uint8)(255 * clampf(1.f - g_rainK * 1.1f, 0.f, 1.f)));   // за тучами светил не видно
        SDL_RenderCopy(g_ren, g_atlasTex, &src, &dst);
        if (body == 1 && g_shaders >= 1) {
            // фаза луны: тень наползает с одной стороны (0 — полнолуние, 4 — новолуние)
            int ph = ((g_dayCount % 8) + 8) % 8;
            if (ph != 0) {
                float m0 = dst.x + size * 3.f / 16.f, m1 = dst.x + size * 13.f / 16.f, my0 = dst.y + size * 3.f / 16.f, my1 = dst.y + size * 13.f / 16.f;
                float w = m1 - m0;
                float off = ph <= 4 ? (1.f - ph / 4.f) * w : -(1.f - (8 - ph) / 4.f) * w;   // сначала тень справа, потом уходит влево
                float s0 = std::max(m0, m0 + off), s1 = std::min(m1, m1 + off);
                if (s1 > s0) {
                    float bg[3]; for (int i = 0; i < 3; i++) bg[i] = lerpf(g_skyCol[i], g_zenCol[i], 0.6f);
                    fillRectF(s0, my0, s1 - s0, my1 - my0, bg[0] * 1.15f + 0.015f, bg[1] * 1.15f + 0.02f, bg[2] * 1.15f + 0.035f, 0.94f);   // пепельный свет — чуть светлее неба
                }
            }
        }
    }
    SDL_SetTextureAlphaMod(g_atlasTex, 255);
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
static void uploadTile(int tile, const u32* px16, int which, int frame) {
    const int R = g_tileRes;
    const u32* px = px16;
    std::vector<u32> up;
    if (R > 16) {
        if (!g_hdAnim[which][frame].empty()) px = g_hdAnim[which][frame].data();
        else { up.resize((size_t)R * R); for (int y = 0; y < R; y++) for (int x = 0; x < R; x++) up[(size_t)y * R + x] = px16[(y * 16 / R) * 16 + x * 16 / R]; px = up.data(); }
    }
    int tx = (tile & 15) * R, ty = (tile >> 4) * R;
    if ((int)g_hdAtlas.size() == g_hdSize * g_hdH) for (int y = 0; y < R; y++) memcpy(&g_hdAtlas[(size_t)(ty + y) * g_hdSize + tx], px + (size_t)y * R, (size_t)R * 4);
    if (g_atlasTex) { SDL_Rect r = {tx, ty, R, R}; SDL_UpdateTexture(g_atlasTex, &r, px, R * 4); }
}
static void updateAnimTiles(float t) {
    int fw = g_shaders ? ((int)(t * 7.f)) & 15 : 0, fl = g_shaders ? ((int)(t * 4.f)) & 15 : 0;
    if (fw != g_animFrameW) { uploadTile(T_WATER, g_animWater[fw], 0, fw); uploadTile(T_WATER_FX, g_animWaterFX[fw], 1, fw); g_animFrameW = fw; }
    if (fl != g_animFrameL) { uploadTile(T_LAVA, g_animLava[fl], 2, fl); g_animFrameL = fl; }
}
// подводные блики: «паутина» света — края ячеек Вороного, точки которых кружат по замкнутым петлям (16 кадров зациклены);
// узор занимает 2x2 блока и бесшовно повторяется
static void buildCaustics(int R) {
    const int S = R * 2;
    const int NP = 14;
    float bx[NP], by[NP], ph[NP], rad[NP];
    Rng r(9137);
    for (int i = 0; i < NP; i++) { bx[i] = r.f(); by[i] = r.f(); ph[i] = r.f() * 6.2831853f; rad[i] = 0.025f + r.f() * 0.035f; }
    for (int f = 0; f < 16; f++) {
        float a = f / 16.f * 6.2831853f;
        float px[NP], py[NP];
        for (int i = 0; i < NP; i++) { px[i] = bx[i] + cosf(a + ph[i]) * rad[i]; py[i] = by[i] + sinf(a * (i & 1 ? 1.f : -1.f) + ph[i]) * rad[i]; }
        std::vector<u32>& out = g_caustFrames[f];
        out.assign((size_t)S * S, 0);
        for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
            float u = (x + 0.5f) / S, v = (y + 0.5f) / S;
            // лёгкое искажение — линии не прямые, а изогнутые, как от настоящих волн
            float wu = u + sinf((v + f / 16.f) * 6.2831853f * 2.f) * 0.012f, wv = v + sinf((u - f / 16.f) * 6.2831853f * 2.f) * 0.012f;
            float d1 = 9, d2 = 9;
            for (int i = 0; i < NP; i++) {
                float dx = fabsf(wu - px[i]); dx -= floorf(dx + 0.5f);   // расстояние на торе
                float dy = fabsf(wv - py[i]); dy -= floorf(dy + 0.5f);
                float d = dx * dx + dy * dy;
                if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) d2 = d;
            }
            float e = sqrtf(d2) - sqrtf(d1);
            float I = expf(-e * 38.f);
            I = I * I * 0.9f + expf(-e * 12.f) * 0.18f;
            int al = (int)(clampf(I, 0.f, 1.f) * 255.f);
            out[(size_t)y * S + x] = RGBA(255, 255, 250, al);
        }
    }
    g_caustRes = R; g_caustFrame = -1;
}
static void updateCaustics(float t) {
    if (g_shaders < 2) { g_caustReady = false; return; }
    const int R = g_tileRes;
    if (g_caustRes != R) buildCaustics(R);
    int f = ((int)(t * 6.f)) & 15;
    if (f == g_caustFrame && g_caustReady) return;
    g_caustFrame = f; g_caustReady = true;
    const int S = R * 2;
    static std::vector<u32> tile; tile.resize((size_t)R * R);
    for (int q = 0; q < 4; q++) {
        int ox = (q & 1) * R, oy = (q >> 1) * R;
        for (int y = 0; y < R; y++) memcpy(&tile[(size_t)y * R], &g_caustFrames[f][(size_t)(oy + y) * S + ox], (size_t)R * 4);
        int tt = T_CAUST0 + q, tx = (tt & 15) * R, ty = (tt >> 4) * R;
        if (g_atlasTex) { SDL_Rect rr = {tx, ty, R, R}; SDL_UpdateTexture(g_atlasTex, &rr, tile.data(), R * 4); }
    }
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
        if (isLeaves(b)) tr *= 0.55f;
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
        if (g_shaders >= 2 && g_fx.sunset > 0.02f) {
            // закатное зарево вокруг самого солнца: золотое ядро и широкий малиново-розовый ореол
            const float gold[3] = {1.f, 0.7f, 0.3f}, rose[3] = {0.95f, 0.35f, 0.4f};
            float k = g_fx.sunset * (1.f - g_rainK) * smooth01(-0.25f, 0.02f, g_fx.sun[1]);
            drawGlow(sx, sy, g_cam.focal * 1.1f, gold, 0.55f * k);
            drawGlow(sx, sy, g_cam.focal * 2.2f, rose, 0.35f * k);
        }
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
    // объёмные облака: клетки-«подушки» толщиной 4 блока; рисуются только внешние грани
    const float CY = g_genVer >= 3 ? 196.f : 118.f, CELL = 10.f, TH = 4.f;
    float drift = gameTime * 0.6f;
    int R = std::min(32, (int)((g_fogEnd + 30) / CELL));
    int bx = ifloor((g_cam.x + drift) / CELL), bz = ifloor(g_cam.z / CELL);
    float cl = 0.55f + 0.45f * g_dayLight;
    auto dens = [&](int gx, int gz) { return nDetail.n2(gx * 0.14f, gz * 0.14f) + nDetail.n2(gx * 0.45f + 7, gz * 0.45f) * 0.3f; };
    const float thr = 0.18f - 0.24f * g_rainK;   // в непогоду небо затягивает
    auto isCloud = [&](int gx, int gz) { return dens(gx, gz) >= thr; };
    float camY = (float)g_cam.y;
    for (int dz = -R; dz <= R; dz++) for (int dx = -R; dx <= R; dx++) {
        if (dx * dx + dz * dz > R * R) continue;
        int gx = bx + dx, gz = bz + dz;
        float n = dens(gx, gz);
        if (n < thr) continue;
        float x0 = gx * CELL - drift - (float)g_cam.x, z0 = gz * CELL - (float)g_cam.z, y0 = CY - camY, y1 = y0 + TH;
        float cx = x0 + CELL * 0.5f, cz = z0 + CELL * 0.5f;
        float d = sqrtf(cx * cx + cz * cz + y0 * y0);
        float fa = 1.f - smooth01(g_fogEnd * 0.8f, g_fogEnd + 40.f, d);
        if (fa <= 0.01f) continue;
        float op = clampf(0.55f + (n - thr) * 1.6f + g_rainK * 0.2f, 0.55f, 0.95f) * fa;   // плотнее в середине облака
        float base[3]; for (int i = 0; i < 3; i++) base[i] = (g_shaders ? cl * (0.62f + 0.38f * g_fx.sunCol[i]) : cl) * (1.f - 0.42f * g_rainK - 0.15f * g_thunderK) + g_flash * 0.4f;
        // серебряная кайма: облако между глазом и солнцем светится по краям, грани к солнцу — тёплые
        float lining = 0;
        if (g_shaders >= 2 && g_fx.sunUp > 0.02f) {
            float vd = sqrtf(cx * cx + (y0 + 2) * (y0 + 2) + cz * cz) + 1e-3f;
            float dp = (cx * g_fx.sun[0] + (y0 + 2) * g_fx.sun[1] + cz * g_fx.sun[2]) / vd;
            float edgeK = clampf(1.f - (n - thr) * 3.f, 0.f, 1.f);   // тонкий край облака светится сильнее
            lining = powf(std::max(0.f, dp), 6.f) * (0.35f + 0.65f * edgeK) * g_fx.sunUp * (1.f - g_rainK);
        }
        auto face = [&](float (*W)[3], float sh, float bluish, float nx, float ny, float nz) {
            float UV[4][2], C[4][4];
            float lit = 0;
            if (g_shaders >= 2) lit = std::max(0.f, nx * g_fx.sun[0] + ny * g_fx.sun[1] + nz * g_fx.sun[2]) * 0.22f * g_fx.sunUp * (1.f - g_rainK);
            for (int k = 0; k < 4; k++) {
                tileUV(T_WHITE, 8, 8, UV[k]);
                for (int i = 0; i < 3; i++) C[k][i] = std::min(1.f, base[i] * sh * (i == 2 ? 1.f + bluish : 1.f) + lit * g_fx.sunCol[i] + lining * (0.7f + 0.3f * g_fx.sunCol[i]));
                C[k][3] = std::min(1.f, op * (1.f - lining * 0.25f));
            }
            emitQuadRel(W, UV, C);
        };
        float x1 = x0 + CELL, z1 = z0 + CELL;
        if (camY < CY) { float W[4][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}}; face(W, 0.72f, 0.06f, 0, -1, 0); }        // низ — серее
        if (camY > CY + TH) { float W[4][3] = {{x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}}; face(W, 1.05f, 0.f, 0, 1, 0); }   // верх — самый светлый
        if (x0 > 0 && !isCloud(gx - 1, gz)) { float W[4][3] = {{x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}}; face(W, 0.86f, 0.03f, -1, 0, 0); }
        if (x1 < 0 && !isCloud(gx + 1, gz)) { float W[4][3] = {{x1, y0, z0}, {x1, y0, z1}, {x1, y1, z1}, {x1, y1, z0}}; face(W, 0.86f, 0.03f, 1, 0, 0); }
        if (z0 > 0 && !isCloud(gx, gz - 1)) { float W[4][3] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}}; face(W, 0.93f, 0.02f, 0, 0, -1); }
        if (z1 < 0 && !isCloud(gx, gz + 1)) { float W[4][3] = {{x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}}; face(W, 0.93f, 0.02f, 0, 0, 1); }
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
static void showTipS(const Stack& s, float x, float y) { if (s.empty()) return; g_tipText = stackName(s); g_tipX = x; g_tipY = y; g_tipUntil = SDL_GetTicks() + 2600; }
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
    Stack armor[4];   // шлем, нагрудник, поножи, ботинки
    bool dead = false;
    float swing = 0;   // анимация взмаха
    float eatT = 0;
    float bowCharge = 0;   // натяжение лука 0..1
    int xpLevel = 0, xpTotal = 0; float xpProg = 0, levelFlash = 0;   // опыт
};
static Player P;
static void computeShaderParams() {
    // тени: светило днём — солнце, ночью — луна (тени слабее)
    const float* L = g_fx.light;
    float el = atan2f(L[1], fabsf(L[0]) + 1e-4f);
    g_lightEast = L[0] >= 0;
    g_lightElevQ = clampf(el, 0.f, 1.5707963f) * (255.f / 1.5707963f);
    bool day = g_fx.sun[1] > -0.05f;
    g_shadowK = g_shaders >= 2 ? (day ? 0.5f : 0.22f) * smooth01(0.02f, 0.22f, L[1]) * (1.f - 0.85f * g_rainK) : 0.f;
    // утренний туман: появляется к рассвету и тает к полудню
    float x = fmodf(g_dayTime + 0.09f, 1.f);
    g_mistK = (g_shaders >= 1 && x < 0.3f) ? sqrtf(sinf(x / 0.3f * 3.1416f)) * 0.85f * (1.f - g_rainK) : 0.f;
    if (g_mistK > 0) {
        Chunk* pc = getChunk(ifloor(P.x) >> 4, ifloor(P.z) >> 4);
        int b = pc ? pc->biome[(ifloor(P.z) & 15) * 16 + (ifloor(P.x) & 15)] : BI_PLAINS;
        if (b == BI_DESERT || b == BI_BADLANDS || b == BI_SAVANNA) g_mistK *= 0.2f;
        if (b == BI_SWAMP) g_mistK = std::min(1.f, g_mistK * 1.5f);
        for (int i = 0; i < 3; i++) g_mistCol[i] = lerpf(g_skyCol[i], 0.93f, 0.55f) * (0.55f + 0.45f * g_dayLight);
    }
}
static const float PL_HW = 0.3f, PL_H = 1.8f, PL_EYE = 1.62f;
struct XpOrb { double x, y, z; float vx, vy, vz, age; int val; float ph; };
static std::vector<XpOrb> g_orbs;
static void spawnXp(double x, double y, double z, int total);
static bool spendLevels(int n);
static void addXp(int v);

// ---------------------------------------------------------------- инвентарь
static int addItem(Stack s) {   // возвращает остаток
    if (s.empty()) return 0;
    int ms = maxStack(s.id);
    for (int pass = 0; pass < 2 && s.cnt > 0; pass++) {
        for (int i = 0; i < 36 && s.cnt > 0; i++) {
            Stack& d = P.inv[i];
            if (pass == 0) {
                if (d.empty() || d.id != s.id || d.ench != s.ench || ms == 1) continue;
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
struct Recipe { int w, h; u16 cell[9]; u16 out; u8 cnt; u16 ench; };
static u16 g_matchEnch = 0;   // зачарование результата последнего совпавшего рецепта
static std::vector<Recipe> g_recipes;
static void addRecipe(std::initializer_list<const char*> rows, std::initializer_list<std::pair<char, int>> keys, int out, int cnt, u16 ench = 0) {
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
    r.out = (u16)out; r.cnt = (u8)cnt; r.ench = ench;
    g_recipes.push_back(r);
}
static void initRecipes() {
    addRecipe({"L"}, {{'L', B_LOG}}, B_PLANKS, 4);
    addRecipe({"L"}, {{'L', B_PINE_LOG}}, B_PLANKS, 4);
    addRecipe({"L"}, {{'L', B_BIRCH_LOG}}, B_PLANKS, 4);
    addRecipe({"L"}, {{'L', B_ACACIA_LOG}}, B_PLANKS, 4);
    addRecipe({"L"}, {{'L', B_JUNGLE_LOG}}, B_PLANKS, 4);
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
    for (int w = 0; w < 5; w++) addRecipe({"WWW", "PPP"}, {{'W', B_WOOL_W + w}, {'P', B_PLANKS}}, B_BED, 1);
    addRecipe({".I", "I."}, {{'I', I_IRON}}, I_SHEARS, 1);
    addRecipe({"SS", "SS"}, {{'S', I_STRING}}, B_WOOL_W, 1);
    addRecipe({"NNN", "PPP"}, {{'N', I_STRING}, {'P', B_PLANKS}}, B_BED, 1);
    addRecipe({".SN", "S.N", ".SN"}, {{'S', I_STICK}, {'N', I_STRING}}, I_BOW, 1);
    addRecipe({"..S", ".SN", "S.N"}, {{'S', I_STICK}, {'N', I_STRING}}, I_ROD, 1);
    addRecipe({"B", "S", "F"}, {{'B', I_BONE}, {'S', I_STICK}, {'F', I_FLUFF}}, I_ARROW, 4);
    // книги и зачарования
    // зачарованные книги не создаются — их можно только выловить удочкой (шанс 2%)
    addRecipe({"BBB", ".I.", "III"}, {{'B', B_IRON_BLOCK}, {'I', I_IRON}}, B_ANVIL, 1);
    // броня
    const int armMats[3] = {I_IRON, I_GOLD, I_DIAMOND};
    for (int m = 0; m < 3; m++) {
        addRecipe({"MMM", "M.M"}, {{'M', armMats[m]}}, armorId(m, 0), 1);
        addRecipe({"M.M", "MMM", "MMM"}, {{'M', armMats[m]}}, armorId(m, 1), 1);
        addRecipe({"MMM", "M.M", "M.M"}, {{'M', armMats[m]}}, armorId(m, 2), 1);
        addRecipe({"M.M", "M.M"}, {{'M', armMats[m]}}, armorId(m, 3), 1);
    }
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
            if (ok) { *outCnt = r.cnt; g_matchEnch = r.ench; return r.out; }
        }
    }
    return 0;
}

// ---------------------------------------------------------------- печь
static int smeltResult(int id) {
    switch (id) {
        case B_IRON_ORE: return I_IRON; case B_GOLD_ORE: return I_GOLD; case B_SAND: return B_GLASS; case B_COBBLE: return B_STONE;
        case I_RAWMEAT: return I_COOKED; case I_PORK: return I_PORK_C; case I_MUTTON: return I_MUTTON_C; case B_REDSAND: return B_GLASS;
        case B_LOG: case B_PINE_LOG: case B_BIRCH_LOG: case B_ACACIA_LOG: case B_JUNGLE_LOG: return I_COAL; case B_COAL_ORE: return I_COAL;
        case B_DIAMOND_ORE: return I_DIAMOND;
    }
    if (isFishRaw(id)) return I_FFRY0 + (id - I_FISH0);
    return 0;
}
static float fuelTime(int id) {
    if (id == I_COAL) return 40.f;
    if (isLog(id) || id == B_PLANKS || id == B_CRAFT || id == B_CHEST) return 7.5f;
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
                f.xp += res == I_GOLD ? 1.f : (res == I_IRON || res == I_DIAMOND ? 0.7f : (res == I_COAL ? 0.15f : 0.35f));
            }
        } else f.cook = std::max(0.f, f.cook - dt * 2);
        if (f.burn < 0) f.burn = 0;
        if (f.xp >= 1.f && isLoaded(fx, fz)) {   // накопленный опыт выпрыгивает из печи, когда игрок рядом
            double ddx = fx + 0.5 - P.x, ddz = fz + 0.5 - P.z;
            if (ddx * ddx + ddz * ddz < 36.0) { int v = (int)f.xp; f.xp -= v; spawnXp(fx + 0.5, fy + 1.0, fz + 0.5, v); }
        }
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
struct Particle { float x, y, z, vx, vy, vz, life, maxLife, size; int tile; float u, v; float r, g, b; bool glow; u8 kind = 0; float ph = 0; };   // kind: 0 обычная, 1 лист, 2 снежинка
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
// искры критического удара: белые звёздочки разлетаются и медленно гаснут
static void spawnCritSparks(float x, float y, float z, int n) {
    static Rng r(4711);
    for (int i = 0; i < n; i++) {
        Particle p; p.kind = 0; p.x = x + r.sf() * 0.3f; p.y = y + r.sf() * 0.3f; p.z = z + r.sf() * 0.3f;
        float a = r.f() * 6.2832f, e = r.sf() * 1.2f, sp = 2.5f + r.f() * 3.f;
        p.vx = cosf(a) * cosf(e) * sp; p.vy = sinf(e) * sp + 1.f; p.vz = sinf(a) * cosf(e) * sp;
        p.maxLife = p.life = 0.45f + r.f() * 0.4f; p.size = 0.06f + r.f() * 0.05f;
        p.tile = T_CRIT; p.u = 0; p.v = 0; p.glow = true; p.r = 1.f; p.g = 0.97f; p.b = 0.75f + r.f() * 0.25f; p.kind = 3;
        g_parts.push_back(p);
    }
}
static int g_dropSink = 0;                // >0: выпавшее отдаётся гостю с этим номером, а не лежит у хозяина
struct SinkItem { Stack s; double x, y, z; };
static std::vector<SinkItem> g_sinkItems; static int g_sinkXp = 0;
struct Drop { Stack s; double x, y, z; float vx, vy, vz; float age; bool onGround; };
static std::vector<Drop> g_drops;
static void spawnDrop(Stack s, double x, double y, double z, float vx = 1e9f, float vy = 3.f, float vz = 0) {
    if (s.empty()) return;
    if (g_dropSink > 0) { SinkItem it; it.s = s; it.x = x; it.y = y; it.z = z; g_sinkItems.push_back(it); return; }   // достанется гостю
    Drop d; d.s = s; d.x = x; d.y = y; d.z = z; d.age = 0; d.onGround = false;
    Rng r(SDL_GetTicks() + (u32)g_drops.size() * 77);
    if (vx > 1e8f) { d.vx = r.sf() * 1.5f; d.vz = r.sf() * 1.5f; } else { d.vx = vx; d.vz = vz; }
    d.vy = vy;
    g_drops.push_back(d);
}

// ---------------------------------------------------------------- урон игроку
static int armorPoints() { int p = 0; for (int i = 0; i < 4; i++) if (!P.armor[i].empty()) p += ARMOR_PTS[armorMat(P.armor[i].id)][armorPiece(P.armor[i].id)]; return p; }
static int protectionLevels() { int p = 0; for (int i = 0; i < 4; i++) p += enchLvl(P.armor[i], EN_PROT); return p; }
// armorMode: 2 — броня и защита (удары, стрелы, лава), 1 — только зачарование «Защита» (падение), 0 — ничего (голод, утопление)
static void playerHurt(float amount, double fromX = 1e9, double fromZ = 0, bool ignoreCd = false, int armorMode = 2) {
    if (P.dead || g_mode == GM_CREATIVE) return;
    if (P.hurtCd > 0 && !ignoreCd) return;
    {
        float red = 0;
        int pts = armorPoints();
        if (armorMode >= 2) red += pts * 0.04f;
        if (armorMode >= 1) red += protectionLevels() * 0.04f;
        red = std::min(red, 0.8f);
        float raw = amount;
        amount *= 1.f - red;
        if (armorMode >= 2 && pts > 0) {   // броня изнашивается
            int wear = std::max(1, (int)(raw / 4.f));
            for (int i = 0; i < 4; i++) {
                Stack& a = P.armor[i]; if (a.empty()) continue;
                a.dmg += wear;
                if (a.dmg >= maxDur(a.id)) { showMsg("СЛОМАНО: " + itemName(a.id)); a.clear(); playSound(SND_BREAK0 + SM_GLASS, 0.6f, 0.7f); }
            }
        }
    }
    P.health -= amount; P.hurtCd = 0.5f; P.hurtFlash = 0.35f;
    g_shake = std::min(1.f, g_shake + 0.35f + amount * 0.08f);   // встряска камеры
    playSound(SND_HURT, 0.8f, 0.9f + (SDL_GetTicks() % 20) / 100.f);
    if (fromX < 1e8) {
        float dx = (float)(P.x - fromX), dz = (float)(P.z - fromZ), l = sqrtf(dx * dx + dz * dz) + 0.001f;
        P.vx += dx / l * 6.f; P.vz += dz / l * 6.f; P.vy = std::max(P.vy, 5.f);
    }
    padRumble(std::min(1.f, 0.35f + amount * 0.08f), std::min(1.f, 0.25f + amount * 0.06f), 140 + (int)(amount * 20));
    if (P.health <= 0) {
        padRumble(1.f, 0.6f, 600);
        P.health = 0; P.dead = true;
        for (int i = 0; i < 36; i++) if (!P.inv[i].empty()) { spawnDrop(P.inv[i], P.x, P.y + 1, P.z); P.inv[i].clear(); }
        for (int i = 0; i < 4; i++) if (!P.armor[i].empty()) { spawnDrop(P.armor[i], P.x, P.y + 1, P.z); P.armor[i].clear(); }
        spawnXp(P.x, P.y + 0.5, P.z, std::min(100, P.xpLevel * 7));   // часть опыта остаётся на месте гибели
        P.xpLevel = 0; P.xpProg = 0; P.xpTotal = 0;
    }
}

// ============================================================================
//  СЕТЕВАЯ ИГРА: хозяин мира и гости по Wi-Fi (или по IP-адресу)
// ============================================================================
// Хозяин считает весь мир: блоки, жидкости, мобов, погоду, время. Гости получают чанки и изменения,
// присылают свои действия. Поиск игр — широковещательные UDP-маяки в локальной сети.
#ifndef BW_NO_NET
static const int NET_PORT = 41234, DISC_PORT = 41235;
#endif
static const int NET_PROTO = 4;
enum NetMsg : u8 {
    // гость -> хозяин
    M_HELLO = 1, M_POS, M_SETBLOCK, M_CHUNKREQ, M_HITMOB, M_USEMOB, M_CHESTREQ, M_CHESTSET, M_FURNREQ, M_FURNSET, M_ARROW, M_BYE,
    // хозяин -> гость
    M_WELCOME = 40, M_CHUNK, M_BLOCK, M_PLAYERS, M_JOIN, M_LEAVE, M_MOBS, M_TIME, M_HURT, M_BOLT, M_GIVE, M_CHEST, M_FURN, M_ARROWFX, M_USEOK, M_KICK
};
struct RemotePlayer {
    int id = 0; std::string name;
    double x = 0, y = 0, z = 0, tx = 0, ty = 0, tz = 0;   // показ и цель (плавное движение)
    float yaw = 0, pitch = 0, tyaw = 0, tpitch = 0;
    u16 held = 0; u8 flags = 0; float hp = 20; float walk = 0, swing = 0, speed = 0; int heldTile = -1;
    double lastPos = 0;
};
static std::vector<RemotePlayer> g_remote;
static RemotePlayer* remoteById(int id) { for (RemotePlayer& r : g_remote) if (r.id == id) return &r; return nullptr; }
static std::string g_netStatus;           // сообщение на экране сетевой игры
static std::string g_myName;              // имя игрока (хранится в настройках)
static int g_myId = 0;                    // у хозяина 0
static bool g_netApplying = false;        // применяем изменение, пришедшее по сети (не пересылать обратно)
static int g_netFrom = -1;                // от какого гостя пришло изменение

// кто из игроков ближе всего (для мобов): у хозяина — он сам и гости
struct Who { double x, y, z; float vx, vz; bool dead, flying; int held; int id; };
static Who whoLocal() { Who w; w.x = P.x; w.y = P.y; w.z = P.z; w.vx = P.vx; w.vz = P.vz; w.dead = P.dead; w.flying = P.flying; w.held = P.inv[P.sel].id; w.id = g_myId; return w; }
static Who whoRemote(const RemotePlayer& r) { Who w; w.x = r.tx; w.y = r.ty; w.z = r.tz; w.vx = 0; w.vz = 0; w.dead = (r.flags & 4) != 0; w.flying = (r.flags & 8) != 0; w.held = r.held; w.id = r.id; return w; }
static Who nearestPlayer(double x, double y, double z) {
    Who best = whoLocal(); double bd = P.dead ? 1e18 : (P.x - x) * (P.x - x) + (P.y - y) * (P.y - y) + (P.z - z) * (P.z - z);
    if (g_netRole == NET_HOST) for (const RemotePlayer& r : g_remote) {
        if (r.flags & 4) continue;
        double d = (r.tx - x) * (r.tx - x) + (r.ty - y) * (r.ty - y) + (r.tz - z) * (r.tz - z);
        if (d < bd) { bd = d; best = whoRemote(r); }
    }
    return best;
}
static Who playerById(int id) {
    if (g_netRole == NET_HOST && id != 0) { RemotePlayer* r = remoteById(id); if (r) return whoRemote(*r); Who w = whoLocal(); w.dead = true; w.id = id; return w; }
    return whoLocal();
}
static double nearestPlayerDist2(double x, double z) {
    double bd = (P.x - x) * (P.x - x) + (P.z - z) * (P.z - z);
    if (g_netRole == NET_HOST) for (const RemotePlayer& r : g_remote) bd = std::min(bd, (r.tx - x) * (r.tx - x) + (r.tz - z) * (r.tz - z));
    return bd;
}
static void netHurtPlayer(int id, float dmg, double fx, double fz);
static void hurtWho(const Who& w, float dmg, double fx, double fz) { if (w.id == g_myId) playerHurt(dmg, fx, fz); else netHurtPlayer(w.id, dmg, fx, fz); }
struct Arrow;
static void netOnBolt(float x, float z, u32 seed);
static void netOnArrow(const Arrow& a, int exceptId);
static void netSendArrow(const Arrow& a, float dmg);
static bool netClientOpenContainer(int x, int y, int z, bool chest);
static bool nearRemoteChunk(int cx, int cz, int r);
static void netRequestChunk(int cx, int cz);
static void saveMpProfile();
static void netHostStop();
static void netSendGive(int id, double x, double y, double z);
struct Mob;
static void netHitMob(Mob& m, float dmg);

// ---------------------------------------------------------------- мобы
enum MobType { MOB_TOPTUN = 0, MOB_PUKH, MOB_GNIL, MOB_TEN, MOB_ZOMBIE, MOB_SKELETON, MOB_SPIDER, MOB_FISH, MOB_PIG, MOB_SHEEP, MOB_CAT, MOB_DOG, MOB_COUNT };
struct MobDef { const char* name; float hp, hw, h, speed; bool hostile; float dmg; int snd; bool burns; };
static const MobDef MD[MOB_COUNT] = {
    {"ЯК", 10, 0.48f, 1.4f, 1.5f, false, 0, SND_TOPTUN, false},
    {"УТКА", 4, 0.24f, 0.7f, 1.9f, false, 0, SND_PUKH, false},
    {"ГНИЛЬ", 20, 0.42f, 1.45f, 2.2f, true, 3, SND_GNIL, true},
    {"ТЕНЕВИК", 14, 0.35f, 1.1f, 4.0f, true, 3, SND_TEN, true},
    {"ЗОМБИ", 20, 0.3f, 1.9f, 2.3f, true, 3, SND_ZOMB, true},       // медленный, бьёт в ближнем бою
    {"СКЕЛЕТ", 16, 0.3f, 1.95f, 2.6f, true, 0, SND_SKEL, true},     // держит дистанцию и стреляет из лука
    {"ПАУК", 14, 0.65f, 0.85f, 3.6f, true, 2, SND_SPID, false},     // быстрый, прыгает, лазает по стенам; днём мирный
    {"РЫБА", 3, 0.24f, 0.3f, 1.3f, false, 0, SND_FLOP, false},       // плавает в реках и озёрах; вид — в var
    {"СВИНЬЯ", 10, 0.42f, 0.9f, 1.6f, false, 0, SND_PIG, false},
    {"ОВЦА", 8, 0.42f, 1.3f, 1.5f, false, 0, SND_SHEEP, false},      // var — цвет шерсти
    {"КОШКА", 10, 0.25f, 0.7f, 2.4f, false, 0, SND_CAT, false},       // var — окрас; приручается рыбой
    {"СОБАКА", 8, 0.3f, 0.85f, 3.0f, false, 0, SND_DOG, false},       // var — окрас; приручается костью
};
struct Mob {
    int type; double x, y, z; float vx = 0, vy = 0, vz = 0; float yaw = 0, wishYaw = 0; float hp; float hurtT = 0;
    float walk = 0; float aiT = 0; bool moving = false; bool onGround = false; float atkCd = 0; float sndT = 5; float burnT = 0; float fleeT = 0;
    float angryT = 0, leapCd = 0, strafeT = 0;
    int var = 0; float pitch = 0, phase = 0, outT = 0, swim = 0; double tx = 0, ty = 0, tz = 0; bool bait = false;   // рыба
    bool dead = false;
    // животные
    u32 uid = 0, targetUid = 0; int owner = 0;
    bool tame = false, sit = false, sheared = false;
    float loveT = 0, breedCd = 0, age = 0, eatT = 0, headYaw = 0, lookT = 0;
};
static std::vector<Mob> g_mobs;
static inline bool isFarmAnimal(int t) { return t == MOB_TOPTUN || t == MOB_PIG || t == MOB_SHEEP; }
static inline bool isPetType(int t) { return t == MOB_CAT || t == MOB_DOG; }
static inline bool isNewAnimal(int t) { return t == MOB_PIG || t == MOB_SHEEP || t == MOB_CAT || t == MOB_DOG; }
static inline float mobSc(const Mob& m) { return m.age < 0 ? 0.55f : 1.f; }
static inline float mobMaxHp(const Mob& m) { return (m.type == MOB_DOG && m.tame) ? 20.f : MD[m.type].hp; }

// ---------------------------------------------------------------- стрелы
static const float ARROW_G = 20.f;
struct Arrow { double x, y, z; float vx, vy, vz; float life, dmg; bool fromPlayer, stuck; float dir[3]; bool crit = false; float trailT = 0; int shooter = 0; bool cosmetic = false; };
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
    if (m.age < 0) return;   // детёныши ничего не оставляют
    if (m.type != MOB_FISH) spawnXp(m.x, m.y + 0.4, m.z, MD[m.type].hostile ? 5 : r.range(1, 4));
    switch (m.type) {
        case MOB_PIG: spawnDrop(mkStack(I_PORK, r.range(1, 4)), m.x, m.y + 0.5, m.z); break;
        case MOB_SHEEP: if (!m.sheared) spawnDrop(mkStack(B_WOOL_W + m.var, 1), m.x, m.y + 0.6, m.z); spawnDrop(mkStack(I_MUTTON, r.range(1, 3)), m.x, m.y + 0.5, m.z); break;
        case MOB_CAT: { int n = r.range(0, 3); if (n) spawnDrop(mkStack(I_STRING, n), m.x, m.y + 0.4, m.z); } break;
        case MOB_DOG: break;
        case MOB_TOPTUN: spawnDrop(mkStack(I_RAWMEAT, r.range(1, 3)), m.x, m.y + 0.5, m.z); break;
        case MOB_PUKH: spawnDrop(mkStack(I_RAWMEAT, 1), m.x, m.y + 0.5, m.z); { int f = r.range(0, 2); if (f) spawnDrop(mkStack(I_FLUFF, f), m.x, m.y + 0.5, m.z); } break;
        case MOB_GNIL: { int n = r.range(0, 2); if (n) spawnDrop(mkStack(I_ROTTEN, n), m.x, m.y + 0.5, m.z); } break;
        case MOB_TEN: { int n = r.range(0, 2); if (n) spawnDrop(mkStack(I_BONE, n), m.x, m.y + 0.5, m.z); } break;
        case MOB_ZOMBIE: { int n = r.range(0, 2); if (n) spawnDrop(mkStack(I_ROTTEN, n), m.x, m.y + 0.5, m.z); if (r.range(100) < 4) spawnDrop(mkStack(I_IRON, 1), m.x, m.y + 0.5, m.z); } break;
        case MOB_SKELETON: { int n = r.range(0, 2); if (n) spawnDrop(mkStack(I_BONE, n), m.x, m.y + 0.5, m.z); int a = r.range(0, 2); if (a) spawnDrop(mkStack(I_ARROW, a), m.x, m.y + 0.5, m.z); } break;
        case MOB_SPIDER: { int n = r.range(0, 2); if (n) spawnDrop(mkStack(I_STRING, n), m.x, m.y + 0.5, m.z); } break;
        case MOB_FISH: spawnDrop(mkStack(I_FISH0 + m.var, 1), m.x, m.y + 0.3, m.z); break;
    }
}
static void mobHurt(Mob& m, float dmg, double fromX, double fromZ) {
    m.hp -= dmg; m.hurtT = 0.4f;
    float dx = (float)(m.x - fromX), dz = (float)(m.z - fromZ), l = sqrtf(dx * dx + dz * dz) + 0.001f;
    m.vx = dx / l * 7.f; m.vz = dz / l * 7.f; m.vy = 5.5f;
    if (m.type == MOB_FISH) { m.vx *= 0.45f; m.vz *= 0.45f; m.vy = 1.f; m.bait = false; }
    playSound(SND_HIT, 0.8f);
    playSound(MD[m.type].snd, 0.5f, 1.15f);
    if (!MD[m.type].hostile && !isPetType(m.type)) m.fleeT = 4.f;
    if (m.type == MOB_CAT && !m.tame) m.fleeT = 4.f;
    if (m.type == MOB_SPIDER) m.angryT = 30.f;   // обиженный паук нападает и днём
    if (m.type == MOB_DOG && !m.tame) {   // дикая стая встаёт на защиту
        m.angryT = 60.f;
        for (Mob& o : g_mobs) if (o.type == MOB_DOG && !o.tame && !o.dead) { double dx = o.x - m.x, dz = o.z - m.z; if (dx * dx + dz * dz < 16 * 16) o.angryT = 60.f; }
        playSound(SND_GROWL, 0.6f);
    }
    if (m.tame) m.sit = false;
    m.loveT = 0;
    if (m.hp <= 0) { if (m.tame) showMsg(m.type == MOB_CAT ? "ВАША КОШКА ПОГИБЛА" : "ВАША СОБАКА ПОГИБЛА", 3.f); mobDie(m); }
}

static void damageTool(Stack& s, int amount);
// ============================================================================
//  ДОМАШНИЕ И ДИКИЕ ЖИВОТНЫЕ: свинья, овца, кошка, собака
// ============================================================================
static const u32 WOOL_RGB[5] = {0xeceae4, 0xa8a8a4, 0x5e5e5c, 0x26262a, 0x6a4a30};
static const float CAT_TINT[4][3] = {{0.78f, 0.76f, 0.72f}, {1.08f, 0.66f, 0.34f}, {0.26f, 0.25f, 0.27f}, {1.06f, 1.04f, 1.f}};
static const float DOG_TINT[4][3] = {{0.7f, 0.5f, 0.32f}, {0.95f, 0.95f, 0.97f}, {1.f, 0.8f, 0.48f}, {0.34f, 0.31f, 0.3f}};
static u32 g_mobUidNext = 1;
static Mob* mobByUid(u32 uid) { if (!uid) return nullptr; for (Mob& m : g_mobs) if (m.uid == uid && !m.dead) return &m; return nullptr; }
static int g_lastHurtByUid = 0, g_lastHitUid = 0;   // кто последним ударил игрока и кого последним ударил игрок (для собак)
static void spawnHearts(float x, float y, float z, int n, bool smoke = false) {
    static Rng r(9091);
    for (int i = 0; i < n; i++) {
        Particle p; p.kind = 3; p.x = x + r.sf() * 0.4f; p.y = y + r.f() * 0.3f; p.z = z + r.sf() * 0.4f;
        p.vx = r.sf() * 0.4f; p.vy = 1.2f + r.f() * 0.8f; p.vz = r.sf() * 0.4f;
        p.maxLife = p.life = 0.9f + r.f() * 0.5f; p.size = 0.09f + r.f() * 0.03f;
        if (smoke) { p.tile = T_SPLASH; p.r = p.g = p.b = 0.55f; p.glow = false; p.kind = 0; p.vy = 0.8f; }
        else { p.tile = T_HEART_F; p.r = p.g = p.b = 1.f; p.glow = true; }
        p.u = 0; p.v = 0;
        g_parts.push_back(p);
    }
}
static Mob makeMob(int t, double x, double y, double z, Rng& r) {
    Mob m; m.type = t; m.x = x; m.y = y; m.z = z; m.hp = MD[t].hp; m.yaw = r.f() * 6.28f; m.wishYaw = m.yaw; m.uid = g_mobUidNext++;
    if (t == MOB_SHEEP) { int v = r.range(1000); m.var = v < 815 ? 0 : (v < 865 ? 1 : (v < 915 ? 2 : (v < 965 ? 3 : 4))); }
    if (t == MOB_CAT || t == MOB_DOG) m.var = r.range(4);
    if (t == MOB_PIG) m.var = r.range(5) == 0 ? 1 : 0;
    return m;
}
// свободное место рядом с точкой (для телепорта питомца к хозяину)
static bool findStandSpot(double cx, double cy, double cz, double& ox, double& oy, double& oz) {
    static Rng r(31337);
    for (int a = 0; a < 20; a++) {
        int x = ifloor(cx) + r.range(-2, 3), z = ifloor(cz) + r.range(-2, 3);
        for (int dy = 1; dy >= -2; dy--) {
            int y = ifloor(cy) + dy;
            if (BD[getBlock(x, y - 1, z)].solid && !BD[getBlock(x, y, z)].solid && !BD[getBlock(x, y + 1, z)].solid && getBlock(x, y, z) != B_LAVA && getBlock(x, y, z) != B_WATER) {
                ox = x + 0.5; oy = y + 0.02; oz = z + 0.5; return true;
            }
        }
    }
    return false;
}
static bool likesFood(int t, int id) {
    switch (t) {
        case MOB_PIG: case MOB_SHEEP: case MOB_TOPTUN: return id == I_APPLE;
        case MOB_CAT: return isFishRaw(id);
        case MOB_DOG: return id == I_BONE || id == I_RAWMEAT || id == I_COOKED || id == I_PORK || id == I_PORK_C || id == I_MUTTON || id == I_MUTTON_C || id == I_ROTTEN;
    }
    return false;
}
// мозг животного: выставляет wishYaw, moving и множитель скорости; возвращает true, если живое «занято» (не бродит)
static void animalBrain(Mob& m, float dt, Rng& r, float& spdMul) {
    spdMul = 1.f;
    Who pl = m.tame ? playerById(m.owner) : nearestPlayer(m.x, m.y, m.z);   // хозяин или ближайший игрок
    const float pdx = (float)(pl.x - m.x), pdz = (float)(pl.z - m.z);
    const float pdist = sqrtf(pdx * pdx + pdz * pdz);
    if (m.age < 0) { m.age += dt; }
    m.loveT = std::max(0.f, m.loveT - dt); m.breedCd = std::max(0.f, m.breedCd - dt); m.angryT = std::max(0.f, m.angryT - dt);
    m.lookT -= dt;
    // взгляд: иногда поворачивает голову к игроку рядом
    float wantHead = 0;
    if (pdist < 7.f && m.lookT > -3.f) { float rel = atan2f(pdx, pdz) - m.yaw; while (rel > 3.1416f) rel -= 6.2832f; while (rel < -3.1416f) rel += 6.2832f; wantHead = clampf(rel, -0.9f, 0.9f); }
    if (m.lookT < -6.f) m.lookT = r.f() * 4.f;
    m.headYaw += (wantHead - m.headYaw) * std::min(1.f, dt * 5.f);
    // любовь: ищет пару своего вида
    if (m.loveT > 0) {
        if (fmodf(m.loveT, 0.6f) < dt) spawnHearts((float)m.x, (float)m.y + MD[m.type].h * mobSc(m), (float)m.z, 1);
        Mob* best = nullptr; double bd = 64;
        for (Mob& o : g_mobs) if (&o != &m && !o.dead && o.type == m.type && o.loveT > 0 && o.age >= 0) {
            double dx = o.x - m.x, dz = o.z - m.z, d = dx * dx + dz * dz; if (d < bd) { bd = d; best = &o; } }
        if (best) {
            m.wishYaw = atan2f((float)(best->x - m.x), (float)(best->z - m.z)); m.moving = bd > 1.0;
            if (bd < 1.3 && m.uid < best->uid) {   // пара встретилась: появляется детёныш
                Mob baby = makeMob(m.type, (m.x + best->x) * 0.5, m.y + 0.1, (m.z + best->z) * 0.5, r);
                baby.age = -600.f;
                if (m.type == MOB_SHEEP && r.range(2)) baby.var = r.range(2) ? m.var : best->var;
                g_mobs.push_back(baby);
                m.loveT = best->loveT = 0; m.breedCd = best->breedCd = 300.f;
                spawnHearts((float)m.x, (float)m.y + 0.8f, (float)m.z, 7);
                spawnXp(m.x, m.y + 0.5, m.z, r.range(1, 8));
            }
            return;
        }
    }
    // приманка: идёт за едой в руке игрока
    const int heldId = pl.held;
    bool tempted = !pl.dead && pdist < 10.f && heldId > 0 && likesFood(m.type, heldId) && !(m.tame && m.sit) && !(m.type == MOB_DOG && m.angryT > 0);
    if (m.type == MOB_CAT && !m.tame) tempted = tempted && (fabsf(pl.vx) + fabsf(pl.vz) < 2.f);   // дикая кошка подходит, только если игрок не бежит
    // ---- собака
    if (m.type == MOB_DOG) {
        if (!m.tame && m.angryT > 0 && !pl.dead && g_mode == GM_SURVIVAL) {   // разозлённая дикая собака нападает
            m.wishYaw = atan2f(pdx, pdz); m.moving = pdist > 0.9f; spdMul = 1.35f;
            if (pdist < 1.4f && fabsf((float)(pl.y - m.y)) < 1.5f && m.atkCd <= 0) { m.atkCd = 1.f; hurtWho(pl, 3.f, m.x, m.z); playSound(SND_GROWL, 0.6f, 1.2f); }
            return;
        }
        if (m.tame && !m.sit) {
            Mob* tgt = mobByUid(m.targetUid);
            if (!tgt || tgt == &m || tgt->tame || (isFarmAnimal(tgt->type) && tgt->uid != (u32)g_lastHitUid)) { m.targetUid = 0; tgt = nullptr; }
            if (!tgt) {   // защищает хозяина: цель — кто ударил игрока или кого ударил игрок
                Mob* c = mobByUid((u32)g_lastHurtByUid); if (!c) c = mobByUid((u32)g_lastHitUid);
                if (c && c != &m && !c->tame && c->type != MOB_FISH) { double dx = c->x - m.x, dz = c->z - m.z; if (dx * dx + dz * dz < 24 * 24) { m.targetUid = c->uid; tgt = c; } }
            }
            if (tgt) {
                float tx = (float)(tgt->x - m.x), tz = (float)(tgt->z - m.z), td = sqrtf(tx * tx + tz * tz);
                m.wishYaw = atan2f(tx, tz); m.moving = td > 1.0f; spdMul = 1.45f;
                if (td < 1.5f + MD[tgt->type].hw && fabsf((float)(tgt->y - m.y)) < 1.6f && m.atkCd <= 0) {
                    m.atkCd = 0.9f; mobHurt(*tgt, 4.f, m.x, m.z); playSound(SND_DOG, 0.5f, 1.15f);
                    if (tgt->dead || tgt->hp <= 0) { m.targetUid = 0; if ((u32)g_lastHurtByUid == tgt->uid) g_lastHurtByUid = 0; if ((u32)g_lastHitUid == tgt->uid) g_lastHitUid = 0; }
                    else if (MD[tgt->type].hostile && tgt->atkCd <= 0 && r.range(2) == 0) { tgt->atkCd = 1.f; m.hp -= MD[tgt->type].dmg; m.hurtT = 0.3f; playSound(SND_WHINE, 0.4f, 1.3f); }   // отбивается
                }
                return;
            }
        }
    }
    // ---- питомец следует за хозяином
    if (m.tame) {
        if (pl.dead && !m.sit) { m.moving = false; return; }   // хозяина нет в мире — ждёт на месте
        if (m.sit) { m.moving = false; if (pdist < 5.f && m.type == MOB_CAT && r.range(1200) == 0) playSound(SND_PURR, 0.35f, 0.95f + r.f() * 0.1f); return; }
        if (pdist > 16.f && !pl.flying) {   // отстал — догоняет мгновенно
            double ox, oy, oz;
            if (findStandSpot(pl.x, pl.y, pl.z, ox, oy, oz)) { m.x = ox; m.y = oy; m.z = oz; m.vx = m.vy = m.vz = 0; }
            return;
        }
        if (pdist > 3.f) { m.wishYaw = atan2f(pdx, pdz); m.moving = true; spdMul = pdist > 8.f ? 1.6f : 1.15f; return; }
        m.moving = false; m.headYaw = wantHead;
        if (tempted) return;
    }
    if (tempted) { m.wishYaw = atan2f(pdx, pdz); m.moving = pdist > 2.2f; spdMul = 0.75f; return; }
    // дикая кошка держится подальше от бегущего игрока
    if (m.type == MOB_CAT && !m.tame && pdist < 6.f && (fabsf(pl.vx) + fabsf(pl.vz) > 2.5f || heldId <= 0 || !isFishRaw(heldId))) {
        m.wishYaw = atan2f(-pdx, -pdz); m.moving = true; spdMul = 1.6f; return;
    }
    if (m.fleeT > 0) { m.fleeT -= dt; m.wishYaw = atan2f(-pdx, -pdz) + sinf(m.fleeT * 3.f) * 0.5f; m.moving = true; spdMul = 1.8f; return; }
    // овца щиплет траву: голова вниз, дёрн становится землёй, шерсть отрастает
    if (m.type == MOB_SHEEP) {
        if (m.eatT > 0) {
            m.eatT -= dt; m.moving = false;
            if (m.eatT <= 0) {
                int bx = ifloor(m.x), by = ifloor(m.y - 0.1), bz = ifloor(m.z);
                if (getBlock(bx, by + 1, bz) == B_TALLGRASS) { setBlock(bx, by + 1, bz, B_AIR); m.sheared = false; if (m.age < 0) m.age += 60.f; }
                else if (getBlock(bx, by, bz) == B_GRASS) { setBlock(bx, by, bz, B_DIRT); m.sheared = false; if (m.age < 0) m.age += 60.f; }
            }
            return;
        }
        if (m.onGround && r.range(m.sheared ? 300 : 900) == 0) {
            int bx = ifloor(m.x), by = ifloor(m.y - 0.1), bz = ifloor(m.z);
            if (getBlock(bx, by, bz) == B_GRASS || getBlock(bx, by + 1, bz) == B_TALLGRASS) { m.eatT = 2.2f; m.moving = false; return; }
        }
    }
    // бродит
    m.aiT -= dt;
    if (m.aiT <= 0) {
        m.aiT = 2.f + r.f() * 5.f;
        m.moving = r.range(100) < (m.type == MOB_CAT ? 40 : 55);
        m.wishYaw = r.f() * 6.2831853f;
    }
    if (m.age < 0) spdMul = 1.15f;
}
// использование предмета на существе (касание по животному); true — действие выполнено, удара нет
// actor — кто действует: местный игрок (g_myId) или гость; held — его предмет в руке
static bool tryInteractMobAs(Mob& m, Stack& held, int actor) {
    const bool me = actor == g_myId;
    bool surv = g_mode == GM_SURVIVAL;
    auto consume = [&]() { if (surv || !me) { held.cnt--; if (!held.cnt) held.clear(); } };
    float hy = (float)m.y + MD[m.type].h * mobSc(m);
    // стрижка
    if (m.type == MOB_SHEEP && held.id == I_SHEARS && !m.sheared && m.age >= 0) {
        m.sheared = true;
        static Rng r(424242);
        int n = r.range(1, 4);
        spawnDrop(mkStack(B_WOOL_W + m.var, n), m.x, m.y + 0.9, m.z, r.sf() * 1.5f, 3.f, r.sf() * 1.5f);
        playSound(SND_SHEAR, 0.7f); playSound(SND_SHEEP, 0.4f, 1.1f);
        if (me) damageTool(held, 1); else held.dmg++;
        if (me) P.swing = 1.f;
        return true;
    }
    if (!held.empty() && likesFood(m.type, held.id)) {
        // приручение
        if (isPetType(m.type) && !m.tame) {
            if (m.type == MOB_DOG && held.id != I_BONE) return false;
            if (m.type == MOB_DOG && m.angryT > 0) return false;
            consume(); if (me) P.swing = 1.f;
            static Rng r(777123);
            if (r.range(3) == 0 || g_mode == GM_CREATIVE) {
                m.tame = true; m.sit = true; m.owner = actor; m.hp = mobMaxHp(m); m.fleeT = 0;
                spawnHearts((float)m.x, hy, (float)m.z, 7);
                playSound(m.type == MOB_CAT ? SND_PURR : SND_DOG, 0.6f, 1.1f);
                if (me) showMsg(m.type == MOB_CAT ? "КОШКА ПРИРУЧЕНА! КАСАНИЕ - СИДЕТЬ/ИДТИ" : "СОБАКА ПРИРУЧЕНА! КАСАНИЕ - СИДЕТЬ/ИДТИ", 3.f);
            } else { spawnHearts((float)m.x, hy, (float)m.z, 5, true); playSound(m.type == MOB_CAT ? SND_CAT : SND_WHINE, 0.4f, 1.2f); }
            return true;
        }
        // лечение питомца
        if (m.tame && m.hp < mobMaxHp(m)) {
            consume(); if (me) P.swing = 1.f;
            m.hp = std::min(mobMaxHp(m), m.hp + (float)std::max(2, foodValue(held.id)));
            spawnHearts((float)m.x, hy, (float)m.z, 2); playSound(SND_EAT, 0.5f, 1.2f);
            return true;
        }
        // разведение
        if (isFarmAnimal(m.type)) {
            if (m.age < 0) { consume(); m.age = std::min(0.f, m.age + 60.f); spawnHearts((float)m.x, hy, (float)m.z, 1); playSound(SND_EAT, 0.4f, 1.4f); return true; }
            if (m.loveT <= 0 && m.breedCd <= 0) { consume(); m.loveT = 30.f; spawnHearts((float)m.x, hy, (float)m.z, 4); playSound(SND_EAT, 0.5f, 1.1f); if (me) P.swing = 1.f; return true; }
        }
    }
    // свой питомец: сидеть / идти
    if (m.tame && m.owner == actor && !(isTool(held.id) && toolKind(held.id) == TK_SWORD) && held.id != I_BOW) {
        m.sit = !m.sit; m.targetUid = 0; m.moving = false; m.vx = m.vz = 0;
        playSound(m.type == MOB_CAT ? (m.sit ? SND_PURR : SND_CAT) : (m.sit ? SND_WHINE : SND_DOG), 0.45f, 1.1f);
        if (me) showMsg(m.sit ? "СИДЕТЬ" : "ЗА МНОЙ", 1.f);
        return true;
    }
    return false;
}
static bool netUseMob(Mob& m);
static bool tryInteractMob(Mob& m) {
    if (g_netRole == NET_CLIENT) return netUseMob(m);   // у гостя действие выполняет хозяин мира
    return tryInteractMobAs(m, heldStack(), g_myId);
}

static void updateFish(Mob& m, float dt, Rng& r);
static void updateMob(Mob& m, float dt, Rng& r) {
    if (m.type == MOB_FISH) { updateFish(m, dt, r); return; }
    const MobDef& d = MD[m.type];
    m.hurtT = std::max(0.f, m.hurtT - dt);
    m.atkCd = std::max(0.f, m.atkCd - dt);
    Who pl = nearestPlayer(m.x, m.y, m.z);   // цель — ближайший игрок (в сетевой игре — любой)
    float pdx = (float)(pl.x - m.x), pdz = (float)(pl.z - m.z), pdy = (float)(pl.y - m.y);
    float pdist = sqrtf(pdx * pdx + pdz * pdz);
    bool chasing = false;
    bool useMove = false; float moveYaw = 0;   // скелет смотрит на игрока, а идёт в другую сторону
    m.leapCd = std::max(0.f, m.leapCd - dt);
    bool calm = false;
    const bool brain = isNewAnimal(m.type) || m.type == MOB_TOPTUN;
    float spdMul = 1.f;
    if (brain) animalBrain(m, dt, r, spdMul);
    // теневик боится кошек
    bool scared = false;
    if (m.type == MOB_TEN) for (const Mob& c : g_mobs) if (c.type == MOB_CAT && !c.dead) { double dx = c.x - m.x, dz = c.z - m.z; if (dx * dx + dz * dz < 36) { scared = true; m.wishYaw = atan2f((float)-dx, (float)-dz); m.moving = true; break; } }
    if (brain || scared) {}
    else if (m.type == MOB_SPIDER) {   // днём на свету паук спокоен, пока его не тронуть
        m.angryT = std::max(0.f, m.angryT - dt);
        int sk, bl2; getLightAt(ifloor(m.x), ifloor(m.y + 0.5), ifloor(m.z), sk, bl2);
        calm = !isNight() && sk >= 10 && m.angryT <= 0;
    }
    if (brain || scared) {}
    else if (d.hostile && !calm && !pl.dead && g_mode == GM_SURVIVAL && pdist < 20.f && fabsf(pdy) < 10.f) {
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
                float dx = (float)(pl.x - ex), dyy = (float)(pl.y + 1.2 - ey), dz = (float)(pl.z - ez);
                float L = sqrtf(dx * dx + dyy * dyy + dz * dz) + 1e-4f;
                if (rayTransmit(ex, ey, ez, dx / L, dyy / L, dz / L, L - 0.6f) > 0.5f) {
                    const float SPD = 20.f; float tt = L / SPD;
                    float vx = dx / tt + r.sf() * 1.0f, vz = dz / tt + r.sf() * 1.0f, vy = dyy / tt + 0.5f * ARROW_G * tt + r.sf() * 0.7f;
                    spawnArrow(ex + dx / L * 0.5, ey, ez + dz / L * 0.5, vx, vy, vz, 3.f, false);
                    netOnArrow(g_arrows.back(), -1);
                    m.atkCd = 2.0f + r.f() * 0.8f;
                    playSound(SND_BOW, 0.7f * (1.f - std::min(1.f, L / 24.f)), 1.f + r.sf() * 0.1f);
                } else m.atkCd = 0.4f;
            }
        } else {
            if (pdist < 1.3f + d.hw && fabsf(pdy) < 1.6f && m.atkCd <= 0) {
                m.atkCd = 1.0f;
                hurtWho(pl, d.dmg, m.x, m.z);
                if (pl.id == g_myId) g_lastHurtByUid = (int)m.uid;
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
    float spd = brain ? d.speed * spdMul : d.speed * (m.fleeT > 0 ? 1.8f : 1.f) * (chasing ? 1.1f : 1.f);
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
    int res = moveBody(m.x, m.y, m.z, m.vx, m.vy, m.vz, d.hw * mobSc(m), d.h * mobSc(m), dt);
    m.onGround = (res & 2) != 0;
    if (m.type == MOB_SPIDER && (res & 1) && m.moving) m.vy = std::max(m.vy, 3.6f);   // лезет по стене
    else if ((res & 1) && m.onGround && m.moving) m.vy = 8.6f;
    if (m.moving && m.onGround) m.walk += dt * spd * 3.f; else m.walk *= expf(-dt * 5);
    // звуки
    m.sndT -= dt;
    if (m.sndT <= 0) {
        m.sndT = 6.f + r.f() * 10.f;
        float dist = sqrtf(pdist * pdist + pdy * pdy);
        int snd = d.snd; float pitch = 0.9f + r.f() * 0.2f;
        { float lx = (float)(P.x - m.x), lz = (float)(P.z - m.z), ly = (float)(P.y - m.y); pdist = sqrtf(lx * lx + lz * lz); pdy = ly; }   // слышит местный игрок
        if (m.type == MOB_DOG && m.tame && m.hp < mobMaxHp(m) * 0.5f) snd = SND_WHINE;
        if (m.type == MOB_DOG && !m.tame && m.angryT > 0) snd = SND_GROWL;
        if (m.type == MOB_CAT && m.tame && m.sit) snd = SND_PURR;
        if (m.age < 0) pitch *= 1.35f;   // детёныши пищат тоньше
        if (dist < 24) playSound(snd, 0.6f * (1.f - dist / 24.f), pitch);
    }
    // солнце и лава
    if (d.burns && !isNight() && !(isRaining() && precipAt(ifloor(m.x), ifloor(m.y), ifloor(m.z)) != 0)) {   // дождь гасит
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
    for (const Mob& m : g_mobs) { if (m.type == MOB_FISH || m.tame) continue; if (MD[m.type].hostile) hostile++; else peaceful++; }
    for (int attempt = 0; attempt < 6; attempt++) {
        float ang = r.f() * 6.2831853f, dist = 18.f + r.f() * 26.f;
        double cxp = P.x, czp = P.z;   // в сетевой игре мобы появляются и возле гостей
        if (g_netRole == NET_HOST && !g_remote.empty()) { int k = r.range((int)g_remote.size() + 1); if (k > 0) { cxp = g_remote[k - 1].tx; czp = g_remote[k - 1].tz; } }
        int x = ifloor(cxp + sinf(ang) * dist), z = ifloor(czp + cosf(ang) * dist);
        if (g_netRole == NET_HOST && nearestPlayerDist2(x + 0.5, z + 0.5) < 16.0 * 16.0) continue;
        Chunk* c = getChunk(x >> 4, z >> 4);
        if (!c || !c->meshed) continue;
        // поверхность
        int top = WH - 2;
        while (top > 1 && !BD[getBlock(x, top, z)].solid && getBlock(x, top, z) != B_WATER) top--;
        u8 ground = getBlock(x, top, z);
        bool night = isNight() || g_thunderK > 0.7f;   // в грозу темно — чудища выходят и днём
        if (r.range(100) < 50) {
            // на поверхности
            if (getBlock(x, top + 1, z) != B_AIR || getBlock(x, top + 2, z) != B_AIR) continue;
            if (!night && peaceful < 14 && (ground == B_GRASS || ground == B_SNOWGRASS || ground == B_PODZOL || ground == B_DRYGRASS)) {
                bool nearWater = false;
                for (int k = 0; k < 6 && !nearWater; k++) if (getBlock(x + r.range(-6, 6), top, z + r.range(-6, 6)) == B_WATER) nearWater = true;
                Chunk* bc = getChunk(x >> 4, z >> 4);
                int bio = bc ? (int)bc->biome[(z & 15) * 16 + (x & 15)] : (int)BI_PLAINS;
                // кто водится в краю: вероятности (як, утка, свинья, овца, кошка, собака)
                int w[6] = {15, 20, 30, 35, 0, 0};
                switch (bio) {
                    case BI_FOREST: case BI_BIRCH: { int t2[6] = {10, 30, 35, 15, 2, 8}; memcpy(w, t2, sizeof(w)); break; }
                    case BI_TAIGA: case BI_SNOW: case BI_MOUNT: { int t2[6] = {30, 10, 0, 32, 0, 28}; memcpy(w, t2, sizeof(w)); break; }
                    case BI_SAVANNA: { int t2[6] = {40, 5, 20, 30, 5, 0}; memcpy(w, t2, sizeof(w)); break; }
                    case BI_JUNGLE: { int t2[6] = {0, 35, 40, 0, 25, 0}; memcpy(w, t2, sizeof(w)); break; }
                    case BI_SWAMP: { int t2[6] = {0, 55, 35, 10, 0, 0}; memcpy(w, t2, sizeof(w)); break; }
                    case BI_MEADOW: case BI_PLAINS: { int t2[6] = {15, 18, 28, 33, 6, 0}; memcpy(w, t2, sizeof(w)); break; }
                    default: break;
                }
                if (nearWater) w[1] += 25;
                static const int TYPES[6] = {MOB_TOPTUN, MOB_PUKH, MOB_PIG, MOB_SHEEP, MOB_CAT, MOB_DOG};
                int sum = 0; for (int k = 0; k < 6; k++) sum += w[k];
                int pick = r.range(std::max(1, sum)), t = MOB_TOPTUN;
                for (int k = 0; k < 6; k++) { if (pick < w[k]) { t = TYPES[k]; break; } pick -= w[k]; }
                int pets = 0; for (const Mob& o : g_mobs) if (isPetType(o.type) && !o.tame) pets++;
                if (isPetType(t) && pets >= 4) continue;
                int n = t == MOB_CAT ? 1 : (t == MOB_DOG ? r.range(1, 4) : r.range(1, 4));
                for (int i = 0; i < n && peaceful < 14; i++) {
                    Mob m = makeMob(t, x + 0.5 + r.sf(), top + 1.05, z + 0.5 + r.sf(), r);
                    if (BD[getBlock(ifloor(m.x), ifloor(m.y), ifloor(m.z))].solid) continue;
                    if (isFarmAnimal(t) && r.range(10) == 0) m.age = -300.f - r.f() * 300.f;   // иногда с детёнышами
                    g_mobs.push_back(m); peaceful++;
                }
            } else if (night && hostile < 10 && BD[ground].solid && !isLeaves(ground)) {
                int t = pickHostile(r);
                if (!spawnSpaceOK(x, top + 1, z, t)) continue;
                Mob m = makeMob(t, x + 0.5, top + 1.05, z + 0.5, r);
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
            Mob m = makeMob(t, x + 0.5, y + 0.02, z + 0.5, r);
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
            if (a.fromPlayer && a.shooter == 0 && !a.cosmetic && !P.dead && g_mode == GM_SURVIVAL && dx * dx + dy * dy + dz * dz < 2.2 && addItem(mkStack(I_ARROW, 1)) == 0) {
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
                if (a.cosmetic) {   // у гостя: стрела только для вида, попадания считает хозяин
                    bool hitSomething = false;
                    for (const Mob& m : g_mobs) { const MobDef& md = MD[m.type]; if (!m.dead && nx > m.x - md.hw && nx < m.x + md.hw && nz > m.z - md.hw && nz < m.z + md.hw && ny > m.y && ny < m.y + md.h) hitSomething = true; }
                    if (hitSomething) { remove = true; break; }
                } else if (!a.fromPlayer) {
                    if (!P.dead && nx > P.x - PL_HW - 0.05 && nx < P.x + PL_HW + 0.05 && nz > P.z - PL_HW - 0.05 && nz < P.z + PL_HW + 0.05 && ny > P.y && ny < P.y + PL_H) {
                        playerHurt(a.dmg, a.x - a.vx, a.z - a.vz); remove = true; break;
                    }
                    if (g_netRole == NET_HOST) for (const RemotePlayer& rp : g_remote) {   // стрела скелета попала в гостя
                        if (!(rp.flags & 4) && nx > rp.tx - PL_HW - 0.05 && nx < rp.tx + PL_HW + 0.05 && nz > rp.tz - PL_HW - 0.05 && nz < rp.tz + PL_HW + 0.05 && ny > rp.ty && ny < rp.ty + PL_H) {
                            netHurtPlayer(rp.id, a.dmg, a.x - a.vx, a.z - a.vz); remove = true; break;
                        }
                    }
                    if (remove) break;
                } else {
                    for (Mob& m : g_mobs) {
                        if (m.dead) continue;
                        const MobDef& md = MD[m.type];
                        if (nx > m.x - md.hw - 0.1 && nx < m.x + md.hw + 0.1 && nz > m.z - md.hw - 0.1 && nz < m.z + md.hw + 0.1 && ny > m.y && ny < m.y + md.h) {
                            float dmg = a.dmg;
                            if (a.crit) { dmg += (float)(SDL_GetTicks() % 5) + 1.f; spawnCritSparks((float)m.x, (float)m.y + md.h * 0.7f, (float)m.z, 10); playSound(SND_CRIT, 0.5f, 1.1f); }
                            g_dropSink = a.shooter;   // добыча — тому, кто стрелял
                            mobHurt(m, dmg, a.x - a.vx * 0.1, a.z - a.vz * 0.1);
                            g_dropSink = 0;
                            if (a.shooter > 0) netSendGive(a.shooter, m.x, m.y + 0.5, m.z);
                            remove = true; break;
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
            if (a.crit && !a.stuck && !remove) {   // след из искр за критической стрелой
                a.trailT -= dt;
                if (a.trailT <= 0) { a.trailT = 0.03f; spawnCritSparks((float)a.x, (float)a.y, (float)a.z, 1); g_parts.back().vx *= 0.2f; g_parts.back().vy *= 0.2f; g_parts.back().vz *= 0.2f; }
            }
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
        const MobDef& d = MD[m.type]; const float sc = mobSc(m);
        double b0[3] = {m.x - d.hw * sc - 0.1, m.y, m.z - d.hw * sc - 0.1}, b1[3] = {m.x + d.hw * sc + 0.1, m.y + d.h * sc, m.z + d.hw * sc + 0.1};
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
    if (tk >= 0 && tk == d.tool) { speed = MAT_SPEED[tm]; int ef = enchLvl(tool, EN_EFF); if (ef) speed += ef * ef + 1; }   // «Эффективность»
    if (tk == TK_SWORD && (isLeaves(b) || b == B_CACTUS)) speed = 1.5f;
    if (tk == TK_SWORD && isWool(b)) speed = 3.f;
    if (tool.id == I_SHEARS && (isLeaves(b) || isWool(b))) speed = isWool(b) ? 5.f : 15.f;
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
        case B_LEAVES: case B_PINE_LEAVES: case B_BIRCH_LEAVES: case B_ACACIA_LEAVES: case B_JUNGLE_LEAVES: {
            int v = r.range(100);
            if (v < (b == B_JUNGLE_LEAVES ? 3 : 8)) return mkStack(B_SAPLING, 1);
            if (b == B_LEAVES && v < 13) return mkStack(I_APPLE, 1);
            return Stack();
        }
        case B_DRYGRASS: case B_PODZOL: return mkStack(B_DIRT, 1);
        case B_DEADBUSH: return r.range(3) ? mkStack(I_STICK, 1 + r.range(2)) : Stack();
        case B_TALLGRASS: case B_FERN: case B_GLASS: case B_ICE: case B_BEDROCK: case B_WATER: case B_LAVA: return Stack();
        case B_FURNACE_LIT: return mkStack(B_FURNACE, 1);
        default: return mkStack(b, 1);
    }
}
static void damageTool(Stack& s, int amount = 1) {
    if (!(isTool(s.id) || s.id == I_ROD || s.id == I_SHEARS) || g_mode == GM_CREATIVE) return;
    s.dmg += amount;
    if (s.dmg >= maxDur(s.id)) { s.clear(); playSound(SND_BREAK0 + SM_GLASS, 0.6f, 0.6f); showMsg("ИНСТРУМЕНТ СЛОМАЛСЯ"); }
}

static void breakBlockAt(int x, int y, int z, bool harvest) {
    u8 b = getBlock(x, y, z); u8 m = getMeta(x, y, z);
    if (b == B_AIR || BD[b].hardness < 0) return;
    playSound(SND_BREAK0 + BD[b].sound, 0.8f, 0.9f + (SDL_GetTicks() % 20) / 100.f);
    padRumble(0.25f, 0.35f, 70);
    spawnBreakParticles(x, y, z, BD[b].shape == SH_CUBE ? BD[b].tex[0] : BD[b].tex[0]);
    Rng r(SDL_GetTicks() ^ (u32)(x * 31 + z * 17 + y));
    bool surv = g_mode == GM_SURVIVAL;
    if (surv && harvest) {
        Stack s = dropFor(b, r);
        int luck = enchLvl(P.inv[P.sel], EN_LUCK);   // «Удача»: из руд выпадает больше
        if (luck && !s.empty() && (b == B_COAL_ORE || b == B_IRON_ORE || b == B_GOLD_ORE || b == B_DIAMOND_ORE)) s.cnt += r.range(0, luck);
        if (!s.empty()) spawnDrop(s, x + 0.5, y + 0.4, z + 0.5);
        if (b == B_COAL_ORE) spawnXp(x + 0.5, y + 0.4, z + 0.5, r.range(0, 3));
        else if (b == B_DIAMOND_ORE) spawnXp(x + 0.5, y + 0.4, z + 0.5, r.range(3, 8));
    }
    i64 k = posKey(x, y, z);
    if (g_netRole == NET_CLIENT) { g_chests.erase(k); g_furnaces.erase(k); }   // содержимое отдаст хозяин
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
        case B_TALLGRASS: case B_FLOWER_R: case B_FLOWER_Y: case B_FLOWER_B: case B_FLOWER_W: case B_SAPLING: case B_FERN: {
            u8 g = getBlock(x, y - 1, z); return isSoil(g);
        }
        case B_DEADBUSH: { u8 g = getBlock(x, y - 1, z); return g == B_SAND || g == B_REDSAND || isTerra(g) || isSoil(g); }
        case B_REED: { u8 g = getBlock(x, y - 1, z); return g == B_REED || isSoil(g) || g == B_SAND || g == B_REDSAND; }
        case B_LILY: return getBlock(x, y - 1, z) == B_WATER;
        case B_CACTUS: { u8 g = getBlock(x, y - 1, z); return g == B_SAND || g == B_REDSAND || g == B_CACTUS; }
        case B_DOOR: {
            if (m & 8) return getBlock(x, y - 1, z) == B_DOOR;
            return getBlock(x, y + 1, z) == B_DOOR && BD[getBlock(x, y - 1, z)].solid;
        }
        default: return true;
    }
}

// ---------------------------------------------------------------- обработка отложенных обновлений
// шипение при встрече воды и лавы: звук и клубы пара, поднимающиеся вверх
static void lavaFizz(int x, int y, int z) {
    double dx = x + 0.5 - P.x, dy = y + 0.5 - P.y, dz = z + 0.5 - P.z;
    float d2 = (float)(dx * dx + dy * dy + dz * dz);
    if (d2 < 32.f * 32.f) {
        static Uint32 lastSnd = 0; Uint32 now = SDL_GetTicks();
        if (now - lastSnd > 90) { playSound(SND_FIZZ, 0.6f * (1.f - d2 / 1024.f), 0.85f + (now % 7) * 0.05f); lastSnd = now; }   // не сливать десятки шипений в одно
        spawnPuff(x + 0.5f, y + 1.05f, z + 0.5f, 6, 0.86f, 0.86f, 0.88f, true, 2.6f);   // пар: светлый, поднимается
        spawnPuff(x + 0.5f, y + 1.0f, z + 0.5f, 3, 0.35f, 0.35f, 0.38f, true, 2.f);     // немного тёмного дыма
    }
}
static bool fluidCanEnter(u8 n) { return n == B_AIR || (BD[n].replaceable && BD[n].shape != SH_FLUID); }
static void fluidUpdate(int x, int y, int z, u8 b, u8 m) {
    bool water = b == B_WATER;
    int step = water ? 1 : 2;
    // контакт воды и лавы
    if (!water) {
        for (int d = 0; d < 6; d++) {
            if (d == 3) continue;
            if (getBlock(x + DX[d], y + DY[d], z + DZ[d]) == B_WATER) {
                // как в оригинальных песочницах: источник лавы — вулканит, текущая лава — булыжник
                setBlock(x, y, z, ((m & 7) == 0 && !(m & 8)) ? B_OBSIDIAN : B_COBBLE);
                lavaFizz(x, y, z);
                return;
            }
        }
    }
    bool source = (m & 7) == 0 && !(m & 8);
    if (!source) {
        u8 above = getBlock(x, y + 1, z);
        int nm;
        bool falling = above == b;
        if (falling) nm = 8;   // падающий поток: питается сверху
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
        if (!falling && nm > 7) { setBlock(x, y, z, B_AIR); return; }
        if (nm != m) { setBlock(x, y, z, b, (u8)nm); m = (u8)nm; }
    }
    // растекание
    u8 below = getBlock(x, y - 1, z);
    if (y > 0 && fluidCanEnter(below)) { setBlock(x, y - 1, z, b, 8); return; }
    if (y > 0 && below == B_LAVA && water) { scheduleUpdate(x, y - 1, z, 1); return; }
    if (y > 0 && below == B_WATER && !water) {   // лава стекает в воду: вода под ней застывает камнем
        setBlock(x, y - 1, z, B_STONE);
        lavaFizz(x, y - 1, z);
        return;
    }
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
    int kind = 0;
    Rng r(hash3i(x, y, z, g_seed) + SDL_GetTicks());
    switch (ci.biome) {   // саженец вырастает деревом своего края
        case BI_SNOW: case BI_MOUNT: case BI_TAIGA: kind = 1; break;
        case BI_BIRCH: kind = 2; break;
        case BI_SAVANNA: kind = 3; break;
        case BI_JUNGLE: kind = r.range(3) ? 4 : 7; break;
        case BI_SWAMP: kind = 6; break;
        case BI_FOREST: kind = r.range(5) ? 0 : 2; break;
        default: kind = 0; break;
    }
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
static void openAnvil();
static int g_contX = 0, g_contY = 0, g_contZ = 0;

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
    g_shake = std::max(0.f, g_shake - dt * 2.5f);
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
        if (h > 3.4f) { playerHurt(floorf(h - 3.f), 1e9, 0, true, 1); playSound(SND_HIT, 0.7f, 0.7f); }
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
            if (under != B_AIR) playSound(SND_STEP0 + BD[under].sound, 0.16f, 0.92f + (SDL_GetTicks() % 16) / 100.f);
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
        if (P.starveT >= 4.f) { P.starveT = 0; if (P.health > 1) playerHurt(1, 1e9, 0, true, 0); }
    }
    if (P.headInWater) {
        P.air -= dt;
        if (P.air < 0) { P.air = 0; P.drownT += dt; if (P.drownT > 1.f) { P.drownT = 0; playerHurt(2, 1e9, 0, true, 0); } }
    } else { P.air = std::min(10.f, P.air + dt * 4.f); P.drownT = 0; }
    if (P.inLava) playerHurt(4);
    if (boxTouchesBlock(P.x, P.y, P.z, PL_HW, PL_H, B_CACTUS, 0.04f)) { P.cactusT += dt; if (P.cactusT > 0.5f) { P.cactusT = 0; playerHurt(1); } }
    if (P.y < -16) playerHurt(4, 1e9, 0, true, 0);
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
    padRumble(0.2f, 0.f, 90);
    return true;
}

// кувшинку кладём на гладь воды, на которую смотрит игрок
static bool tryPlaceLily() {
    Stack& s = heldStack();
    float d[3] = {g_cam.f[0], g_cam.f[1], g_cam.f[2]};
    double ex = g_cam.x, ey = g_cam.y, ez = g_cam.z;
    float maxT = g_target.hit ? g_target.dist + 0.05f : 5.f;
    for (float t = 0.2f; t < std::min(maxT, 5.f); t += 0.05f) {
        int x = ifloor(ex + d[0] * t), y = ifloor(ey + d[1] * t), z = ifloor(ez + d[2] * t);
        if (getBlock(x, y, z) == B_WATER && getBlock(x, y + 1, z) == B_AIR && (getMeta(x, y, z) & 7) == 0) {
            if (anyMobIntersects(x, y + 1, z)) return false;
            setBlock(x, y + 1, z, B_LILY);
            playSound(SND_PLACE, 0.6f, 1.2f); playSound(SND_STEP0 + SM_GRASS, 0.4f);
            if (g_mode == GM_SURVIVAL) { s.cnt--; if (!s.cnt) s.clear(); }
            return true;
        }
    }
    return false;
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
        case B_ANVIL: meta = (u8)((pf + 1) & 1); break;
        case B_SAPLING: case B_FLOWER_R: case B_FLOWER_Y: case B_FLOWER_B: case B_FLOWER_W: case B_TALLGRASS: case B_FERN:
        case B_DEADBUSH: case B_REED: case B_LILY: case B_CACTUS:
            if (!hasSupport(tx, ty, tz, (u8)id, 0)) return false;
            break;
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
    padRumble(0.f, 0.18f, 35);
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
        case B_ANVIL: g_contX = h.x; g_contY = h.y; g_contZ = h.z; openAnvil(); return true;
    }
    return false;
}

// ---------------------------------------------------------------- рыбы: появление и плавание
static inline bool isWaterAt(double x, double y, double z) { return getBlock(ifloor(x), ifloor(y), ifloor(z)) == B_WATER; }
static const float FISH_SPEED[4] = {1.3f, 0.9f, 1.8f, 1.2f};
static int fishCount() { int n = 0; for (const Mob& m : g_mobs) if (m.type == MOB_FISH && !m.dead) n++; return n; }
static int pickFishSpecies(Rng& r, int depth, int luck) {
    int w[4] = {40, 30, 22, 8};
    if (depth >= 5) { w[0] = 32; w[1] = 22; w[2] = 22; w[3] = 24; }   // в глубокой воде чаще щука
    w[2] += luck * 3; w[3] += luck * 4;
    int sum = w[0] + w[1] + w[2] + w[3], v = r.range(sum);
    for (int i = 0; i < 4; i++) { if (v < w[i]) return i; v -= w[i]; }
    return 0;
}
static void spawnFish(Rng& r) {
    int have = fishCount();
    for (int attempt = 0; attempt < 4 && have < 14; attempt++) {
        float ang = r.f() * 6.2831853f, dist = 8.f + r.f() * 30.f;
        int x = ifloor(P.x + sinf(ang) * dist), z = ifloor(P.z + cosf(ang) * dist);
        Chunk* c = getChunk(x >> 4, z >> 4);
        if (!c || !c->meshed) continue;
        int top = WH - 2;
        while (top > 1 && getBlock(x, top, z) == B_AIR) top--;
        if (getBlock(x, top, z) != B_WATER) continue;
        int depth = 0; while (depth < 24 && getBlock(x, top - depth, z) == B_WATER) depth++;
        if (depth < 2) continue;
        int sp = pickFishSpecies(r, depth, 0);
        int n = sp == 3 ? 1 : r.range(2, 4);   // стайкой, щука — одна
        double y = top - std::min(depth - 1, 1 + r.range(0, std::max(0, depth - 2))) + 0.35;
        for (int i = 0; i < n && have < 14; i++) {
            Mob m; m.type = MOB_FISH; m.var = sp; m.hp = MD[MOB_FISH].hp + (sp == 3 ? 3 : 0);
            m.x = x + 0.5 + r.sf() * 0.8; m.y = y - 0.15 + r.sf() * 0.3; m.z = z + 0.5 + r.sf() * 0.8;
            if (!isWaterAt(m.x, m.y + 0.15, m.z)) continue;
            m.yaw = r.f() * 6.28f; m.phase = r.f() * 6.28f; m.tx = m.x; m.ty = m.y; m.tz = m.z; m.aiT = r.f() * 2.f;
            g_mobs.push_back(m); have++;
        }
    }
}
struct Bobber {
    bool active = false; int state = 0;   // 0 летит, 1 ждёт, 2 рыба подплывает, 3 клюёт, 4 лежит на земле
    double x = 0, y = 0, z = 0; float vx = 0, vy = 0, vz = 0;
    float t = 0, waitT = 0, stateT = 0, nibT = 0, dip = 0; int fishMob = -1; int species = 0;
    float trailAng = 0;
};
static Bobber g_bob;
static float g_rodTipCam[3] = {0.3f, -0.1f, 1.2f}; static bool g_rodTipOK = false;
static int g_lureLuck = 0;
static void updateFish(Mob& m, float dt, Rng& r) {
    m.hurtT = std::max(0.f, m.hurtT - dt);
    m.fleeT = std::max(0.f, m.fleeT - dt);
    double cy = m.y + 0.15;
    bool inW = isWaterAt(m.x, cy, m.z);
    float spdBase = FISH_SPEED[m.var];
    if (inW) {
        m.outT = 0;
        float pdx = (float)(m.x - P.x), pdy = (float)(cy - (P.y + 0.9)), pdz = (float)(m.z - P.z);
        float pd = sqrtf(pdx * pdx + pdy * pdy + pdz * pdz);
        float want = 0.f;
        if (!P.dead && pd < 2.8f && (fabsf(P.vx) + fabsf(P.vz) > 0.5f || pd < 1.6f)) m.fleeT = std::max(m.fleeT, 1.5f);   // пугается подошедшего игрока
        if (m.fleeT > 0) {
            m.bait = false;
            float l = std::max(0.2f, pd);
            m.tx = m.x + pdx / l * 4.0 + r.sf() * 0.5; m.ty = cy + pdy / l * 1.0; m.tz = m.z + pdz / l * 4.0;
            want = spdBase * 2.6f; m.aiT = 0.3f;
        } else if (m.bait && g_bob.active && (g_bob.state == 1 || g_bob.state == 2)) {   // идёт на наживку
            m.tx = g_bob.x; m.ty = g_bob.y - 0.25; m.tz = g_bob.z;
            want = spdBase * 0.7f;
        } else {
            m.bait = false;
            m.aiT -= dt;
            double ddx = m.tx - m.x, ddy = m.ty - cy, ddz = m.tz - m.z;
            if (m.aiT <= 0 || ddx * ddx + ddy * ddy + ddz * ddz < 0.25) {
                m.aiT = 2.5f + r.f() * 5.f;
                for (int k = 0; k < 8; k++) {
                    double nx = m.x + r.sf() * 5.0, ny = cy + r.sf() * 1.2, nz = m.z + r.sf() * 5.0;
                    if (isWaterAt(nx, ny, nz) && isWaterAt(nx, ny + 0.35, nz)) { m.tx = nx; m.ty = ny; m.tz = nz; break; }
                }
                m.swim = r.range(100) < 25 ? 0.25f : 1.f;   // иногда просто стоит на месте, шевеля плавниками
            }
            want = spdBase * m.swim;
        }
        double ddx = m.tx - m.x, ddy = m.ty - cy, ddz = m.tz - m.z;
        float dl = (float)sqrt(ddx * ddx + ddy * ddy + ddz * ddz) + 1e-4f;
        if (dl < 0.3f) want *= dl / 0.3f;
        float wx = (float)ddx / dl * want, wy = (float)ddy / dl * want * 0.6f, wz = (float)ddz / dl * want;
        float k = std::min(1.f, dt * (m.fleeT > 0 ? 5.f : 1.8f));
        m.vx += (wx - m.vx) * k; m.vy += (wy - m.vy) * k; m.vz += (wz - m.vz) * k;
        // остаться в воде: впереди не вода — разворот
        double nx = m.x + m.vx * dt * 3.f, ny = cy + m.vy * dt * 3.f, nz = m.z + m.vz * dt * 3.f;
        if (!isWaterAt(nx, ny, nz) || !isWaterAt(nx, ny + 0.3, nz)) {
            if (!isWaterAt(m.x, cy + 0.3, m.z)) m.vy = -0.4f;   // у самой поверхности — вниз
            if (!isWaterAt(nx, cy, nz)) { m.vx *= -0.3f; m.vz *= -0.3f; }
            m.aiT = 0; m.tx = m.x - m.vx; m.tz = m.z - m.vz; m.ty = cy - 0.2;
        }
        double ox = m.x, oy = m.y, oz = m.z;
        m.x += m.vx * dt; m.y += m.vy * dt; m.z += m.vz * dt;
        if (BD[getBlock(ifloor(m.x), ifloor(m.y + 0.15), ifloor(m.z))].solid) { m.x = ox; m.y = oy; m.z = oz; m.vx = m.vy = m.vz = 0; m.aiT = 0; }
        float hs = sqrtf(m.vx * m.vx + m.vz * m.vz);
        if (hs > 0.05f) {
            float wy2 = atan2f(m.vx, m.vz), dy = wy2 - m.yaw;
            while (dy > (float)M_PI) dy -= 2 * (float)M_PI; while (dy < -(float)M_PI) dy += 2 * (float)M_PI;
            m.yaw += dy * std::min(1.f, dt * 5.f);
        }
        float wp = clampf(atan2f(m.vy, std::max(hs, 0.2f)), -0.6f, 0.6f);
        m.pitch += (wp - m.pitch) * std::min(1.f, dt * 4.f);
        float sp = sqrtf(hs * hs + m.vy * m.vy);
        m.phase += dt * (3.f + sp * 7.f);
        m.walk = sp;   // для анимации: сила взмахов хвоста
        if (r.range(1000) < 4) spawnPuff((float)m.x, (float)cy + 0.1f, (float)m.z, 1, 0.75f, 0.85f, 1.f, true, 0.8f);   // пузырёк
    } else {
        // на суше: бьётся и подпрыгивает, долго не выживет
        m.vy -= 28.f * dt;
        int res = moveBody(m.x, m.y, m.z, m.vx, m.vy, m.vz, 0.2f, 0.25f, dt);
        bool ground = (res & 2) != 0;
        if (ground) {
            m.vx *= expf(-dt * 8.f); m.vz *= expf(-dt * 8.f);
            m.aiT -= dt;
            if (m.aiT <= 0) {
                m.aiT = 0.5f + r.f() * 0.7f; m.vy = 3.5f + r.f() * 1.5f; m.vx = r.sf() * 1.6f; m.vz = r.sf() * 1.6f; m.yaw += r.sf() * 2.f;
                float d2 = (float)((m.x - P.x) * (m.x - P.x) + (m.z - P.z) * (m.z - P.z));
                if (d2 < 400) playSound(SND_FLOP, 0.5f * (1.f - d2 / 400.f), 0.9f + r.f() * 0.3f);
            }
        }
        m.pitch = 0; m.phase += dt * 14.f; m.walk = 2.5f;
        m.outT += dt;
        if (m.outT > 2.f) { m.outT = 0; m.hp -= 1; m.hurtT = 0.2f; if (m.hp <= 0) mobDie(m); }
    }
    if (m.y < -20) m.dead = true;
}

// ---------------------------------------------------------------- рыбалка
static void rodCancel() { g_bob.active = false; if (g_bob.fishMob >= 0 && g_bob.fishMob < (int)g_mobs.size()) g_mobs[g_bob.fishMob].bait = false; g_bob.fishMob = -1; }
static void rodCast() {
    const float* f = g_cam.f;
    g_bob = Bobber(); g_bob.active = true; g_bob.state = 0;
    g_lureLuck = enchLvl(P.inv[P.sel], EN_LUCK);
    g_bob.x = P.x + f[0] * 0.6; g_bob.y = P.y + PL_EYE - 0.1 + f[1] * 0.6; g_bob.z = P.z + f[2] * 0.6;
    float sp = 11.f;
    g_bob.vx = f[0] * sp + (float)P.vx * 0.5f; g_bob.vy = f[1] * sp + 3.f; g_bob.vz = f[2] * sp + (float)P.vz * 0.5f;
    playSound(SND_CAST, 0.7f, 0.95f + (SDL_GetTicks() % 10) / 100.f);
    P.swing = 1.f;
}
static float fishWait(int luck) { Rng r(SDL_GetTicks() * 7 + 3); return (5.f + r.f() * 17.f) * (1.f - 0.13f * luck) * (isRaining() ? 0.75f : 1.f); }   // в дождь клюёт чаще
static void rodReel(Stack& rod) {
    int luck = enchLvl(rod, EN_LUCK);
    P.swing = 1.f;
    playSound(SND_REEL, 0.6f, 1.f);
    if (g_bob.state == 3) {
        Rng r(SDL_GetTicks() * 13 + 5);
        double dx = P.x - g_bob.x, dy = P.y + 1.2 - g_bob.y, dz = P.z - g_bob.z;
        float T = 0.75f;
        if (r.range(1000) < 20) {   // 2%: со дна вытащена зачарованная книга (любое зачарование, низкие уровни чаще)
            int e = r.range(EN_COUNT), mx = EN_MAX[e], sum = mx * (mx + 1) / 2, v = r.range(sum), lvl = 1;
            for (int l = 1; l <= mx; l++) { int w = mx + 1 - l; if (v < w) { lvl = l; break; } v -= w; }
            Stack book = mkStack(I_EBOOK, 1); setEnch(book, e, lvl);
            spawnDrop(book, g_bob.x, g_bob.y + 0.2, g_bob.z, (float)dx / T, (float)dy / T + 0.5f * 20.f * T, (float)dz / T);
            spawnPuff((float)g_bob.x, (float)g_bob.y + 0.3f, (float)g_bob.z, 16, 0.75f, 0.45f, 1.f, true, 2.5f);
            playSound(SND_SPLASH, 0.7f, 1.1f); playSound(SND_ENCH, 0.7f);
            showMsg("ВЫЛОВЛЕНО: " + stackName(book) + "!", 3.f);
            spawnXp(P.x, P.y + 0.6, P.z, r.range(3, 10));
            if (g_bob.fishMob >= 0 && g_bob.fishMob < (int)g_mobs.size()) { g_mobs[g_bob.fishMob].bait = false; g_mobs[g_bob.fishMob].fleeT = 3.f; }
            P.exhaustion += 0.1f;
            damageTool(rod, 1);
            rodCancel();
            return;
        }
        int sp = g_bob.species;
        if (g_bob.fishMob >= 0 && g_bob.fishMob < (int)g_mobs.size()) { Mob& m = g_mobs[g_bob.fishMob]; if (m.type == MOB_FISH && !m.dead) { sp = m.var; m.dead = true; } }
        int cnt = 1 + ((r.range(100) < luck * 12) ? 1 : 0);
        Stack fish = mkStack(I_FISH0 + sp, cnt);
        // рыба вылетает из воды прямо к игроку
        spawnDrop(fish, g_bob.x, g_bob.y + 0.2, g_bob.z, (float)dx / T, (float)dy / T + 0.5f * 20.f * T, (float)dz / T);
        spawnPuff((float)g_bob.x, (float)g_bob.y, (float)g_bob.z, 14, 0.8f, 0.9f, 1.f, false, 3.5f);
        playSound(SND_SPLASH, 0.7f, 1.1f);
        showMsg(std::string("ПОЙМАН: ") + FISH_SPECIES[sp] + (cnt > 1 ? " x2" : ""), 1.8f);
        spawnXp(P.x, P.y + 0.6, P.z, r.range(1, 7));
        padRumble(0.5f, 0.5f, 150);
        P.exhaustion += 0.1f;
        damageTool(rod, 1);
    } else if (g_bob.state == 4) damageTool(rod, 2);   // зацепился за землю
    rodCancel();
}
static void updateBobber(float dt) {
    if (!g_bob.active) return;
    const Stack& held = P.inv[P.sel];
    double ddx = g_bob.x - P.x, ddz = g_bob.z - P.z, ddy = g_bob.y - P.y;
    if (P.dead || held.id != I_ROD || ddx * ddx + ddy * ddy + ddz * ddz > 40.0 * 40.0) { rodCancel(); return; }
    Bobber& b = g_bob;
    b.t += dt; b.stateT += dt;
    Rng r(SDL_GetTicks() * 3 + (u32)(b.t * 1000));
    if (b.state == 0) {
        const int N = 4; float h = dt / N;
        for (int i = 0; i < N && b.state == 0; i++) {
            b.vy -= 16.f * h; float dr = expf(-0.4f * h); b.vx *= dr; b.vz *= dr;
            double nx = b.x + b.vx * h, ny = b.y + b.vy * h, nz = b.z + b.vz * h;
            u8 bl = getBlock(ifloor(nx), ifloor(ny), ifloor(nz));
            if (bl == B_WATER) {
                b.x = nx; b.y = ny; b.z = nz; b.state = 1; b.stateT = 0; b.vy *= 0.2f; b.vx *= 0.2f; b.vz *= 0.2f;
                b.waitT = fishWait(g_lureLuck); b.nibT = 1.f + r.f() * 3.f;
                spawnPuff((float)b.x, (float)b.y, (float)b.z, 8, 0.85f, 0.92f, 1.f, false, 2.5f);
                playSound(SND_SPLASH, 0.45f, 1.3f);
            } else if (bl == B_LAVA) { spawnPuff((float)nx, (float)ny, (float)nz, 6, 1.f, 0.5f, 0.1f, true); playSound(SND_FIZZ, 0.5f); rodCancel(); return; }
            else if (BD[bl].solid) { b.state = 4; b.vx = b.vy = b.vz = 0; playSound(SND_PLACE, 0.3f, 1.6f); }
            else { b.x = nx; b.y = ny; b.z = nz; }
        }
        if (b.y < -10) rodCancel();
        return;
    }
    if (b.state == 4) { if (!BD[getBlock(ifloor(b.x), ifloor(b.y - 0.05), ifloor(b.z))].solid && !BD[getBlock(ifloor(b.x), ifloor(b.y), ifloor(b.z))].solid) { b.state = 0; } return; }
    // в воде: держится на поверхности
    int bx = ifloor(b.x), bz = ifloor(b.z), by = ifloor(b.y);
    if (getBlock(bx, by, bz) != B_WATER && getBlock(bx, by - 1, bz) != B_WATER) { b.state = 0; return; }
    if (getBlock(bx, by, bz) != B_WATER) by--;
    while (by < WH - 1 && getBlock(bx, by + 1, bz) == B_WATER) by++;
    float surf = by + 0.86f;
    b.dip += ((b.state == 3 ? -0.28f : 0.f) - b.dip) * std::min(1.f, dt * (b.state == 3 ? 14.f : 6.f));
    float target = surf - 0.03f + sinf(b.t * 2.1f) * 0.015f + b.dip;
    b.vy += ((target - (float)b.y) * 30.f - b.vy * 6.f) * dt;
    b.vx *= expf(-2.f * dt); b.vz *= expf(-2.f * dt);
    b.x += b.vx * dt; b.y += b.vy * dt; b.z += b.vz * dt;
    if (b.state == 1) {
        // живая рыба рядом замечает наживку
        if (b.fishMob < 0) {
            float best = 7.f * 7.f;
            for (size_t i = 0; i < g_mobs.size(); i++) {
                Mob& m = g_mobs[i]; if (m.type != MOB_FISH || m.dead || m.fleeT > 0) continue;
                float d2 = (float)((m.x - b.x) * (m.x - b.x) + (m.y + 0.15 - b.y) * (m.y + 0.15 - b.y) + (m.z - b.z) * (m.z - b.z));
                if (d2 < best) { best = d2; b.fishMob = (int)i; }
            }
            if (b.fishMob >= 0 && b.stateT > 1.5f && r.range(100) < 50) g_mobs[b.fishMob].bait = true; else b.fishMob = -1;
        }
        if (b.fishMob >= 0) {
            Mob& m = g_mobs[b.fishMob];
            if (m.type != MOB_FISH || m.dead || !m.bait) b.fishMob = -1;
            else {
                float d2 = (float)((m.x - b.x) * (m.x - b.x) + (m.z - b.z) * (m.z - b.z));
                if (d2 < 0.5f * 0.5f) { b.state = 3; b.stateT = 0; b.species = m.var; }
            }
        }
        // мелкие подёргивания поплавка — «пробует»
        b.nibT -= dt;
        if (b.nibT <= 0) { b.nibT = 1.5f + r.f() * 4.f; b.vy -= 0.6f; spawnPuff((float)b.x, (float)surf, (float)b.z, 2, 0.85f, 0.92f, 1.f, false, 0.8f); }
        b.waitT -= dt;
        if (b.waitT <= 0 && b.fishMob < 0) { b.state = 2; b.stateT = 0; b.trailAng = r.f() * 6.2831853f; b.species = pickFishSpecies(r, 3, g_lureLuck); }
    } else if (b.state == 2) {
        // невидимая рыба подплывает: дорожка пузырьков к поплавку
        float k = 1.f - std::min(1.f, b.stateT / 2.f), d = 2.5f * k + 0.2f;
        float px = (float)b.x + sinf(b.trailAng) * d, pz = (float)b.z + cosf(b.trailAng) * d;
        if (r.range(100) < 60) spawnPuff(px, surf - 0.25f, pz, 1, 0.8f, 0.9f, 1.f, true, 1.2f);
        if (b.stateT > 2.f) { b.state = 3; b.stateT = 0; }
    } else if (b.state == 3) {
        if (b.stateT < dt * 1.5f) {   // рывок вниз, всплеск
            spawnPuff((float)b.x, (float)surf, (float)b.z, 10, 0.85f, 0.92f, 1.f, false, 3.f);
            playSound(SND_BITE, 0.8f, 0.95f + r.f() * 0.1f);
            padRumble(0.7f, 0.2f, 260);   // поклёвку чувствуешь рукой
        }
        if (b.stateT > 0.95f + 0.12f * g_lureLuck) {   // не успел — сорвалась
            if (b.fishMob >= 0 && b.fishMob < (int)g_mobs.size()) { g_mobs[b.fishMob].bait = false; g_mobs[b.fishMob].fleeT = 3.f; }
            b.fishMob = -1; b.state = 1; b.stateT = 0; b.waitT = fishWait(g_lureLuck);
        }
    }
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
    if (g_in.placePressed && g_targetMob >= 0 && !g_in.dig) {
        if (tryInteractMob(g_mobs[g_targetMob])) { g_in.placePressed = false; g_in.place = false; g_placeCd = 0.3f; }
        else { g_in.dig = true; g_in.placePressed = false; g_in.place = false; g_attackCd = 0; }
    }
    // удар/копание
    if (g_in.dig) {
        if (g_targetMob >= 0) {
            g_dig.active = false;
            if (g_attackCd <= 0) {
                Mob& m = g_mobs[g_targetMob];
                if (!m.tame) g_lastHitUid = (int)m.uid;
                float dmg = 3;   // кулак
                if (isTool(held.id)) dmg = toolKind(held.id) == TK_SWORD ? SWORD_DMG[toolMat(held.id)] * 1.4f : 3.5f + toolMat(held.id) * 0.6f;
                // критический удар: в прыжке на спуске, не в воде, не на лестнице и не в полёте
                bool crit = !P.onGround && P.vy < -1.f && !P.inWater && !P.onLadder && !P.flying;
                if (crit) {
                    dmg *= 1.5f;
                    spawnCritSparks((float)m.x, (float)m.y + MD[m.type].h * 0.7f, (float)m.z, 14);
                    playSound(SND_CRIT, 0.7f, 0.95f + (SDL_GetTicks() % 10) / 100.f);
                    g_shake = std::max(g_shake, 0.18f);
                }
                dmg += enchLvl(held, EN_DMG) * 1.25f;   // «Урон»
                if (g_mode == GM_CREATIVE) dmg = 100;
                if (g_netRole == NET_CLIENT) netHitMob(m, dmg); else mobHurt(m, dmg, P.x, P.z);
                padRumble(crit ? 0.6f : 0.35f, crit ? 0.5f : 0.3f, crit ? 120 : 80);
                damageTool(held, isTool(held.id) && toolKind(held.id) == TK_SWORD ? 1 : 2);
                P.exhaustion += 0.1f; P.swing = 1.f; g_attackCd = 0.3f;
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
    // броня в руке: «поставить» — надеть (сменить на надетую)
    if (isArmor(held.id) && g_in.placePressed) {
        if (g_target.hit && tryInteract(g_target)) return;
        int pc = armorPiece(held.id);
        std::swap(P.armor[pc], held);
        playSound(SND_DIG0 + SM_STONE, 0.6f, 1.4f);
        showMsg("НАДЕТО: " + stackName(P.armor[pc]), 1.5f);
        return;
    }
    // удочка: касание — забросить, ещё касание — подсечь и вытащить
    if (held.id == I_ROD) {
        u8 tb = g_target.hit ? getBlock(g_target.x, g_target.y, g_target.z) : 0;
        bool usable = tb == B_CRAFT || tb == B_CHEST || tb == B_FURNACE || tb == B_FURNACE_LIT || tb == B_DOOR || tb == B_BED || tb == B_ANVIL;
        if (g_in.placePressed) {
            if (g_bob.active) rodReel(held);
            else if (usable) tryInteract(g_target);
            else rodCast();
            g_placeCd = 0.28f;
        }
        return;
    }
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
                if (ch >= 1.f) g_arrows.back().crit = true;   // полностью натянутый лук — критическая стрела
                if (g_netRole == NET_CLIENT) { g_arrows.back().cosmetic = true; netSendArrow(g_arrows.back(), 1.5f + 7.5f * ch * ch); }
                else netOnArrow(g_arrows.back(), -1);
                if (g_mode == GM_SURVIVAL) removeItem(I_ARROW, 1);
                playSound(SND_BOW, 0.8f, 0.9f + ch * 0.3f);
                padRumble(0.2f + ch * 0.4f, 0.5f * ch, 110);
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
    if (g_in.placePressed && held.id == B_LILY && tryPlaceLily()) { P.swing = 1.f; g_placeCd = 0.28f; return; }
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
            } else if (dist < 3.2 && d.age > 0.6f) {   // предмет плавно притягивается к игроку, чем ближе — тем сильнее
                float k = dt * (8.f + (3.2f - (float)dist) * 9.f);
                d.vx += (float)(dx / dist) * k; d.vz += (float)(dz / dist) * k; d.vy += (float)(dy / dist) * k * 0.8f + dt * 6.f;
            }
        }
        if (d.s.cnt == 0 || d.age > 300.f || d.y < -20) { g_drops[i] = g_drops.back(); g_drops.pop_back(); continue; }
        i++;
    }
}
static void updateParticles(float dt) {
    for (size_t i = 0; i < g_parts.size();) {
        Particle& p = g_parts[i];
        p.life -= dt;
        if (p.kind == 3) {   // искры: тормозятся воздухом и чуть оседают
            float k = expf(-dt * 4.f); p.vx *= k; p.vy = p.vy * k - 1.5f * dt; p.vz *= k;
            p.x += p.vx * dt; p.y += p.vy * dt; p.z += p.vz * dt;
            if (p.life <= 0) { g_parts[i] = g_parts.back(); g_parts.pop_back(); continue; }
            i++; continue;
        }
        if (p.kind) {   // лист кружится и планирует, снежинка медленно падает, покачиваясь
            p.ph += dt;
            float sw = p.kind == 1 ? 0.9f : 0.35f, fall = p.kind == 1 ? -0.55f : -0.9f;
            p.vy += (fall - p.vy) * std::min(1.f, dt * 2.f);
            float wx = sinf(p.ph * (p.kind == 1 ? 2.3f : 1.3f) + p.u) * sw, wz = cosf(p.ph * (p.kind == 1 ? 1.7f : 1.1f) + p.v) * sw * 0.7f;
            float nx = p.x + (p.vx + wx) * dt, ny = p.y + p.vy * dt, nz = p.z + (p.vz + wz) * dt;
            u8 nb = getBlock(ifloor(nx), ifloor(ny), ifloor(nz));
            if (BD[nb].solid || nb == B_WATER || isLeaves(nb)) { if (p.life > 1.2f) p.life = 1.2f; p.vy = 0; }   // лёг на землю и тает
            else { p.x = nx; p.y = ny; p.z = nz; }
            if (p.life <= 0) { g_parts[i] = g_parts.back(); g_parts.pop_back(); continue; }
            i++; continue;
        }
        if (p.glow) { p.vy += 1.f * dt; } else p.vy -= 14.f * dt;
        float nx = p.x + p.vx * dt, ny = p.y + p.vy * dt, nz = p.z + p.vz * dt;
        if (BD[getBlock(ifloor(nx), ifloor(ny), ifloor(nz))].solid) { p.vx *= 0.3f; p.vz *= 0.3f; p.vy = 0; }
        else { p.x = nx; p.y = ny; p.z = nz; }
        if (p.life <= 0) { g_parts[i] = g_parts.back(); g_parts.pop_back(); continue; }
        i++;
    }
    if (g_parts.size() > 900) g_parts.erase(g_parts.begin(), g_parts.begin() + (g_parts.size() - 900));
}
// падающие листья под кронами и снегопад в снежных краях
static void spawnAmbientParticles(float dt) {
    (void)dt;
    int leaves = 0; for (const Particle& p : g_parts) if (p.kind == 1) leaves++;
    static Rng r(2654435761u);   // общий генератор: каждый кадр — новые случайные места
    // листья: ищем листву с воздухом снизу недалеко от игрока
    for (int a = 0; a < 40 && leaves < 45; a++) {
        int x = ifloor(P.x) + r.range(-10, 11), z = ifloor(P.z) + r.range(-10, 11), y = ifloor(P.y) + r.range(-3, 12);
        u8 b = getBlock(x, y, z);
        if (!isLeaves(b) || getBlock(x, y - 1, z) != B_AIR || r.range(100) > 30) continue;
        Particle p; p.kind = 1; p.x = x + r.f(); p.y = y - 0.05f; p.z = z + r.f();
        p.vx = r.sf() * 0.3f; p.vy = -0.2f; p.vz = r.sf() * 0.3f; p.maxLife = p.life = 5.f + r.f() * 4.f;
        p.size = 0.07f + r.f() * 0.04f; p.tile = BD[b].tex[2]; p.u = r.f() * 12.f; p.v = r.f() * 12.f; p.glow = false; p.ph = r.f() * 6.f;
        bool autumn = r.range(100) < 18;   // некоторые листья желтеют
        p.r = autumn ? 1.5f : 1.f; p.g = autumn ? 1.1f : 1.f; p.b = autumn ? 0.4f : 1.f;
        g_parts.push_back(p); leaves++;
    }
}

// ============================================================================
//  ПОГОДА: ясно, дождь (в холоде — снегопад), гроза с молниями
// ============================================================================
// высота, на которой капля встречает препятствие (первый непрозрачный, лист или вода сверху вниз)
static int rainStopY(int x, int z, int fromY, int toY) {
    for (int y = fromY; y >= toY; y--) {
        u8 b = getBlock(x, y, z);
        if (b != B_AIR && b != B_TORCH && !(isSmallPlant(b) && b != B_LILY)) return y + 1;
    }
    return toY;
}
static void lightningStrike(float x, float z, bool nearPlayer);
static void updateWeather(float dt, Rng& r) {
    g_weatherT -= dt;
    if (g_netRole == NET_CLIENT) g_weatherT = 1000.f;   // погоду задаёт хозяин мира
    if (g_weatherT <= 0) {
        if (g_weather == WX_CLEAR) setWeather(r.range(100) < 28 ? WX_THUNDER : WX_RAIN, 150.f + r.f() * 270.f);
        else setWeather(WX_CLEAR, 420.f + r.f() * 780.f);
    }
    float wantR = g_weather != WX_CLEAR ? 1.f : 0.f, wantT = g_weather == WX_THUNDER ? 1.f : 0.f;
    g_rainK += (wantR - g_rainK) * std::min(1.f, dt * 0.12f);
    g_thunderK += (wantT - g_thunderK) * std::min(1.f, dt * 0.1f);
    if (g_rainK < 0.002f) g_rainK = 0;
    g_flash = std::max(0.f, g_flash - dt * 3.2f);
    for (size_t i = 0; i < g_bolts.size();) { g_bolts[i].life -= dt; if (g_bolts[i].life <= 0) { g_bolts[i] = g_bolts.back(); g_bolts.pop_back(); } else i++; }
    for (size_t i = 0; i < g_delayed.size();) {
        g_delayed[i].t -= dt;
        if (g_delayed[i].t <= 0) { playSound(g_delayed[i].snd, g_delayed[i].vol, g_delayed[i].pitch); g_delayed[i] = g_delayed.back(); g_delayed.pop_back(); }
        else i++;
    }
    // молнии
    if (g_thunderK > 0.6f && g_netRole != NET_CLIENT) {
        g_boltT -= dt;
        if (g_boltT <= 0) {
            g_boltT = 6.f + r.f() * 18.f;
            float a = r.f() * 6.2832f, d = 12.f + r.f() * 90.f;
            float x = (float)P.x + cosf(a) * d, z = (float)P.z + sinf(a) * d;
#ifndef __ANDROID__
            if (getenv("BW_BOLTS")) { g_boltT = 0.4f; x = (float)P.x + g_cam.f[0] * 45.f; z = (float)P.z + g_cam.f[2] * 45.f; }   // отладка: молния прямо по курсу
#endif
            if (precipAt(ifloor(x), ifloor(P.y), ifloor(z)) != 0) lightningStrike(x, z, d < 30.f);
        }
    }
    // открытость неба вокруг (для звука) и брызги
    int px = ifloor(P.x), py = ifloor(P.y), pz = ifloor(P.z);
    static float openT = 0; openT -= dt;
    if (openT <= 0) {
        openT = 0.25f;
        int open = 0, n = 0, rainy = 0;
        for (int dz = -6; dz <= 6; dz += 3) for (int dx = -6; dx <= 6; dx += 3) {
            n++;
            int x = px + dx, z = pz + dz;
            if (rainStopY(x, z, py + 24, py - 4) <= py + 2) { open++; if (precipAt(x, py, z) == 1) rainy++; }
        }
        float o = n ? open / (float)n : 0.f, rr = n ? rainy / (float)n : 0.f;
        g_precipOpen += (o - g_precipOpen) * 0.5f;
        float vol = g_rainK * (0.12f + 0.88f * rr) * (0.25f + 0.75f * g_precipOpen) * (g_underwaterAudio.load() ? 0.3f : 1.f);
        if (P.y > g_sea + 150) vol *= 0.5f;
        g_rainVol.store(vol * 0.55f);
    }
    if (g_rainK > 0.2f) {   // брызги от капель на земле рядом
        int want = (int)(g_rainK * 60.f * dt * 10.f) + 1;
        for (int i = 0; i < want; i++) {
            int x = px + r.range(-7, 8), z = pz + r.range(-7, 8);
            if (precipAt(x, py, z) != 1) continue;
            int y = rainStopY(x, z, py + 20, py - 10);
            if (y <= py - 10 || y > py + 20) continue;
            Particle p; p.kind = 0; p.x = x + r.f(); p.y = y + 0.05f; p.z = z + r.f();
            p.vx = r.sf() * 0.6f; p.vy = 1.2f + r.f() * 1.2f; p.vz = r.sf() * 0.6f;
            p.maxLife = p.life = 0.18f + r.f() * 0.12f; p.size = 0.025f + r.f() * 0.02f;
            p.tile = T_WHITE; p.u = 8; p.v = 8; p.glow = false; p.r = 0.75f; p.g = 0.85f; p.b = 1.f;
            g_parts.push_back(p);
        }
    }
}
static void playerHurt(float amount, double fromX, double fromZ, bool ignoreCd, int armorMode);
static void mobHurt(Mob& m, float dmg, double fromX, double fromZ);
static void lightningStrike(float x, float z, bool nearPlayer) {
    int ix = ifloor(x), iz = ifloor(z);
    int top = WH - 2;
    while (top > 1 && getBlock(ix, top, iz) == B_AIR) top--;
    if (!isLoaded(ix, iz)) top = g_sea + 6;
    Bolt b; b.x = x; b.z = z; b.yTop = std::max((float)top + 60.f, (float)P.y + 70.f); b.yBot = top + 1.f; b.life = 0.45f; b.seed = (u32)(x * 131 + z * 71 + g_tick);
    g_bolts.push_back(b);
    netOnBolt(x, z, b.seed);
    float dx = x - (float)P.x, dz = z - (float)P.z, d = sqrtf(dx * dx + dz * dz);
    g_flash = std::max(g_flash, 1.f - std::min(0.6f, d / 200.f));
    // гром: чем дальше удар, тем позже и глуше
    float delay = d / 340.f * 3.f, vol = clampf(1.1f - d / 140.f, 0.25f, 1.f);
    DelayedSnd ds; ds.t = delay; ds.snd = d < 40.f ? SND_THUNDER : SND_THUNDER2; ds.vol = vol; ds.pitch = 0.85f + (b.seed % 30) / 100.f;
    g_delayed.push_back(ds);
    if (d < 40.f) g_shake = std::max(g_shake, 0.35f * (1.f - d / 40.f));
    // поражение рядом с местом удара (в сетевой игре считает хозяин)
    if (g_netRole == NET_CLIENT) return;
    if (d < 3.5f && !P.dead && g_mode == GM_SURVIVAL) playerHurt(5.f, x, z, true, 2);
    for (Mob& m : g_mobs) {
        double mx = m.x - x, mz = m.z - z;
        if (mx * mx + mz * mz < 3.5 * 3.5 && !m.dead) { mobHurt(m, 5.f, x, z); m.burnT = 0.9f; }
    }
    (void)nearPlayer;
}
// влияние погоды на небо и свет (вызывается после расчёта неба)
static void applyWeatherSky() {
    float k = g_rainK * (0.78f + 0.17f * g_thunderK);
    const float grey[3] = {0.5f, 0.54f, 0.58f};
    for (int i = 0; i < 3; i++) {
        float gl = grey[i] * (0.25f + 0.75f * g_dayLight);
        g_skyCol[i] = lerpf(g_skyCol[i], gl, k);
        g_zenCol[i] = lerpf(g_zenCol[i], gl * 0.85f, k);
    }
    g_dayLight *= 1.f - g_rainK * 0.28f - g_thunderK * 0.17f;
    g_hazeK = lerpf(g_hazeK, 0.75f, g_rainK * 0.6f);
    if (g_flash > 0) {
        for (int i = 0; i < 3; i++) { g_skyCol[i] = std::min(1.f, g_skyCol[i] + g_flash * 0.55f); g_zenCol[i] = std::min(1.f, g_zenCol[i] + g_flash * 0.45f); }
        g_dayLight = std::min(1.2f, g_dayLight + g_flash * 0.65f);
    }
}
static void applyWeatherFX() {
    float k = g_rainK;
    g_fx.sunUp *= 1.f - k * 0.92f;
    for (int d = 0; d < 6; d++) g_fx.faceMul[d] = lerpf(g_fx.faceMul[d], 1.f, k * 0.8f);   // рассеянный свет без резких теней
    for (int i = 0; i < 3; i++) g_fx.sunCol[i] = lerpf(g_fx.sunCol[i], 0.92f, k * 0.6f);
    g_fx.wind = std::min(1.6f, g_fx.wind + k * 0.45f + g_thunderK * 0.35f);
}
// струи дождя и хлопья снега вокруг камеры: по нескольку на столбец, без лишних частиц
static void emitWeather(float t) {
    if (g_rainK <= 0.01f && g_bolts.empty()) return;
    const Camera& c = g_cam;
    int cx = ifloor(c.x), cy = ifloor(c.y), cz = ifloor(c.z);
    const int R = 11;
    float rgt[3] = {c.r[0], 0.f, c.r[2]}; float rl = sqrtf(rgt[0] * rgt[0] + rgt[2] * rgt[2]) + 1e-5f; rgt[0] /= rl; rgt[2] /= rl;
    float amb[3]; litColor(15.f, 0.f, 1.f, amb);
    for (int dz = -R; dz <= R && g_rainK > 0.01f; dz++) for (int dx = -R; dx <= R; dx++) {
        int d2 = dx * dx + dz * dz; if (d2 > R * R) continue;
        int x = cx + dx, z = cz + dz;
        int kind = precipAt(x, cy, z); if (!kind) continue;
        float topY = (float)cy + 14.f;
        int stop = rainStopY(x, z, cy + 14, cy - 12);
        if (stop >= topY) continue;
        u32 h = hash32((u32)x * 73856093u ^ (u32)z * 19349663u);
        int nDrops = kind == 1 ? 4 : 3;
        float dens = g_rainK * (d2 < 9 ? 0.75f : 1.f);
        for (int k = 0; k < nDrops; k++) {
            u32 hk = hash32(h + k * 0x9E3779B9u);
            if ((hk & 255) / 255.f > dens) continue;
            float ox = ((hk >> 8) & 255) / 255.f, oz = ((hk >> 16) & 255) / 255.f, ph = ((hk >> 24) & 255) / 255.f;
            float span = topY - stop;
            float W[4][3], UV[4][2], C[4][4];
            if (kind == 1) {
                float speed = 13.f + ph * 4.f, len = 0.75f + ph * 0.35f, w = 0.03f;
                float yy = topY - fmodf(t * speed + ph * 97.f, span + len);
                if (yy - len < stop) continue;
                float px = x + ox, pz = z + oz;
                float lean = 0.06f * g_fx.wind;   // ветер слегка наклоняет струи
                float bx = (float)(px - c.x), by = (float)(yy - c.y), bz = (float)(pz - c.z);
                const float pts[4][2] = {{-w, 0}, {w, 0}, {w, -len}, {-w, -len}};
                for (int q = 0; q < 4; q++) {
                    W[q][0] = bx + rgt[0] * pts[q][0] + lean * pts[q][1] * -1.f; W[q][1] = by + pts[q][1]; W[q][2] = bz + rgt[2] * pts[q][0];
                    tileUV(T_WHITE, 8, 8, UV[q]);
                    float a = q < 2 ? 0.08f : 0.55f;   // хвост капли прозрачнее
                    C[q][0] = 0.62f * amb[0] + 0.12f; C[q][1] = 0.7f * amb[1] + 0.12f; C[q][2] = 0.85f * amb[2] + 0.14f; C[q][3] = a * g_rainK;
                }
            } else {
                float speed = 1.4f + ph * 0.8f, s = 0.045f + ph * 0.03f;
                float yy = topY - fmodf(t * speed + ph * 53.f, span);
                if (yy < stop) continue;
                float sw = sinf(t * 1.3f + ph * 9.f) * 0.35f, sw2 = cosf(t * 1.1f + ox * 7.f) * 0.3f;
                float bx = (float)(x + ox + sw - c.x), by = (float)(yy - c.y), bz = (float)(z + oz + sw2 - c.z);
                for (int q = 0; q < 4; q++) {
                    float a = (q == 0 || q == 3) ? -s : s, b = (q < 2) ? s : -s;
                    for (int j = 0; j < 3; j++) W[q][j] = (j == 0 ? bx : (j == 1 ? by : bz)) + c.r[j] * a + c.u[j] * b;
                    tileUV(T_SPLASH, (q == 0 || q == 3) ? 1.f : 15.f, q < 2 ? 1.f : 15.f, UV[q]);
                    C[q][0] = amb[0] * 1.05f; C[q][1] = amb[1] * 1.05f; C[q][2] = amb[2] * 1.1f; C[q][3] = 0.95f * g_rainK;
                }
            }
            emitQuadRel(W, UV, C);
        }
    }
    // молнии: ломаная яркая линия с ответвлениями
    for (const Bolt& b : g_bolts) {
        Rng r(b.seed);
        float a = clampf(b.life / 0.45f, 0, 1);
        float flick = (fmodf(b.life * 23.f, 1.f) > 0.3f) ? 1.f : 0.45f;
        auto seg = [&](float x0, float y0, float z0, float x1, float y1, float z1, float w) {
            float W[4][3], UV[4][2], C[4][4];
            float sx = (float)(x0 - c.x), sy = (float)(y0 - c.y), sz = (float)(z0 - c.z), ex = (float)(x1 - c.x), ey = (float)(y1 - c.y), ez = (float)(z1 - c.z);
            const float P2[4][3] = {{sx - rgt[0] * w, sy, sz - rgt[2] * w}, {sx + rgt[0] * w, sy, sz + rgt[2] * w}, {ex + rgt[0] * w, ey, ez + rgt[2] * w}, {ex - rgt[0] * w, ey, ez - rgt[2] * w}};
            memcpy(W, P2, sizeof(W));
            for (int q = 0; q < 4; q++) { tileUV(T_WHITE, 8, 8, UV[q]); C[q][0] = 0.9f; C[q][1] = 0.93f; C[q][2] = 1.f; C[q][3] = a * flick; }
            emitQuadRel(W, UV, C, -5.f);
        };
        float x = b.x, z = b.z, y = b.yTop;
        int steps = 14; float dy = (b.yTop - b.yBot) / steps;
        for (int i = 0; i < steps; i++) {
            float nx = x + r.sf() * 2.2f, nz = z + r.sf() * 2.2f, ny = y - dy;
            if (i == steps - 1) { nx = b.x; nz = b.z; ny = b.yBot; }
            seg(x, y, z, nx, ny, nz, 0.22f);
            if (r.range(4) == 0 && i < steps - 3) {   // ответвление
                float bx2 = nx, by2 = ny, bz2 = nz;
                for (int j = 0; j < 3; j++) { float tx = bx2 + r.sf() * 3.f, ty = by2 - dy * 0.7f, tz = bz2 + r.sf() * 3.f; seg(bx2, by2, bz2, tx, ty, tz, 0.1f); bx2 = tx; by2 = ty; bz2 = tz; }
            }
            x = nx; y = ny; z = nz;
        }
    }
}

// ---------------------------------------------------------------- отрисовка сущностей
static void lightColorAt(double x, double y, double z, float* out) {
    int sky, bl; getLightAt(ifloor(x), ifloor(y), ifloor(z), sky, bl);
    litColor((float)sky, (float)bl, 1.f, out);
    if (g_shadowK > 0 && sky >= 10) {   // в тени холма или дерева мобы тоже темнеют
        float vis = sunVisAt(x, y, z);
        float k = 1.f - g_shadowK * 0.75f * (1.f - vis) * ((sky - 9) / 6.f);
        float bk = g_blockLUT[std::min(15, std::max(0, bl))];
        for (int i = 0; i < 3; i++) out[i] = std::max(out[i] * k, std::min(out[i], bk * 0.9f));
    }
}
// коробка с углами в мировых координатах относительно камеры (camSpace=false) или в пространстве камеры
static void emitBox8(const float (*corner)[3], const int tiles[6], const float col[3], float alpha, bool camSpace, float keyBias = 0.f) {
    static const int FC[6][4] = {{3, 7, 5, 1}, {6, 2, 0, 4}, {6, 7, 3, 2}, {0, 1, 5, 4}, {7, 6, 4, 5}, {2, 3, 1, 0}};   // обход против часовой снаружи
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
        case MOB_TOPTUN:   // як: длинное тело, горб, бахрома шерсти, голова низко впереди, рога в стороны
            add(0.24f, 0.30f, 0.40f, 0.2f, 0.6f, 0.2f, T_YAK_LEG, T_YAK_LEG, T_YAK_LEG, 1);
            add(-0.24f, 0.30f, 0.40f, 0.2f, 0.6f, 0.2f, T_YAK_LEG, T_YAK_LEG, T_YAK_LEG, 2);
            add(0.24f, 0.30f, -0.42f, 0.2f, 0.6f, 0.2f, T_YAK_LEG, T_YAK_LEG, T_YAK_LEG, 2);
            add(-0.24f, 0.30f, -0.42f, 0.2f, 0.6f, 0.2f, T_YAK_LEG, T_YAK_LEG, T_YAK_LEG, 1);
            add(0, 0.90f, -0.02f, 0.80f, 0.56f, 1.24f, T_YAK_BODY, T_YAK_BODY, T_YAK_TOP, 0);
            add(0, 0.58f, -0.02f, 0.86f, 0.24f, 1.16f, T_YAK_FRINGE, T_YAK_FRINGE, T_YAK_TOP, 0);
            add(0, 1.24f, 0.22f, 0.62f, 0.2f, 0.52f, T_YAK_BODY, T_YAK_BODY, T_YAK_TOP, 0);
            add(0, 0.92f, 0.80f, 0.48f, 0.46f, 0.44f, T_YAK_FACE, T_YAK_HEADSIDE, T_YAK_TOP, 0);
            add(0, 0.62f, 0.80f, 0.42f, 0.18f, 0.36f, T_YAK_FRINGE, T_YAK_FRINGE, T_YAK_TOP, 0);
            add(0, 0.80f, 1.06f, 0.32f, 0.22f, 0.12f, T_YAK_MUZZLE, T_YAK_MUZZLE, T_YAK_MUZZLE, 0);
            add(0.36f, 1.08f, 0.80f, 0.26f, 0.10f, 0.10f, T_YAK_HORN, T_YAK_HORN, T_YAK_HORN, 0);   // рога: в стороны, затем вверх и вперёд
            add(-0.36f, 1.08f, 0.80f, 0.26f, 0.10f, 0.10f, T_YAK_HORN, T_YAK_HORN, T_YAK_HORN, 0);
            add(0.47f, 1.22f, 0.82f, 0.09f, 0.22f, 0.09f, T_YAK_HORN, T_YAK_HORN, T_YAK_HORN, 0);
            add(-0.47f, 1.22f, 0.82f, 0.09f, 0.22f, 0.09f, T_YAK_HORN, T_YAK_HORN, T_YAK_HORN, 0);
            add(0.44f, 1.35f, 0.88f, 0.07f, 0.08f, 0.10f, T_YAK_HORN, T_YAK_HORN, T_YAK_HORN, 0);
            add(-0.44f, 1.35f, 0.88f, 0.07f, 0.08f, 0.10f, T_YAK_HORN, T_YAK_HORN, T_YAK_HORN, 0);
            add(0, 0.82f, -0.68f, 0.1f, 0.44f, 0.08f, T_YAK_TAIL, T_YAK_TAIL, T_YAK_TAIL, 0);
            break;
        case MOB_PUKH:     // утка: лапы, серые бока, рыжая грудка, ошейник, зелёная голова, плоский клюв
            add(0.09f, 0.08f, 0.0f, 0.05f, 0.16f, 0.05f, T_DUCK_FOOT, T_DUCK_FOOT, T_DUCK_FOOT, 1);
            add(-0.09f, 0.08f, 0.0f, 0.05f, 0.16f, 0.05f, T_DUCK_FOOT, T_DUCK_FOOT, T_DUCK_FOOT, 2);
            add(0.09f, 0.015f, 0.05f, 0.14f, 0.03f, 0.18f, T_DUCK_FOOT, T_DUCK_FOOT, T_DUCK_FOOT, 1);
            add(-0.09f, 0.015f, 0.05f, 0.14f, 0.03f, 0.18f, T_DUCK_FOOT, T_DUCK_FOOT, T_DUCK_FOOT, 2);
            add(0, 0.33f, -0.04f, 0.38f, 0.28f, 0.58f, T_DUCK_SIDE, T_DUCK_SIDE, T_DUCK_TOP, 0);
            add(0, 0.33f, 0.26f, 0.32f, 0.24f, 0.12f, T_DUCK_CHEST, T_DUCK_CHEST, T_DUCK_CHEST, 0);
            add(0, 0.44f, -0.38f, 0.22f, 0.10f, 0.14f, T_DUCK_TAIL, T_DUCK_TAIL, T_DUCK_TAIL, 0);
            add(0, 0.53f, 0.24f, 0.15f, 0.20f, 0.15f, T_DUCK_NECK, T_DUCK_NECK, T_DUCK_HEAD, 0);
            add(0, 0.68f, 0.27f, 0.22f, 0.20f, 0.26f, T_DUCK_HEAD, T_DUCK_HEADSIDE, T_DUCK_HEAD, 0);
            add(0, 0.64f, 0.47f, 0.15f, 0.05f, 0.16f, T_DUCK_BILL, T_DUCK_BILL, T_DUCK_BILL, 0);
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
// ---------------------------------------------------------------- рыба в 3D: круглое тело из колец, изгибается при плавании
static const float FISH_WIDTH[4] = {0.52f, 0.42f, 0.6f, 0.72f};   // толщина тела относительно высоты
// axis — точка на оси тела: грань видна, если смотрит от оси к камере (не зависит от порядка вершин)
static void emitFishQuad(const float (*W)[3], const float (*UV)[2], const float* col, float shade, float fa, const float* axis) {
    if (axis) {
        float cx = (W[0][0] + W[1][0] + W[2][0] + W[3][0]) * 0.25f, cy = (W[0][1] + W[1][1] + W[2][1] + W[3][1]) * 0.25f, cz = (W[0][2] + W[1][2] + W[2][2] + W[3][2]) * 0.25f;
        if ((cx - axis[0]) * cx + (cy - axis[1]) * cy + (cz - axis[2]) * cz >= 0) return;
    }
    float C[4][4];
    for (int k = 0; k < 4; k++) { C[k][0] = col[0] * shade; C[k][1] = col[1] * shade; C[k][2] = col[2] * shade; C[k][3] = fa; }
    emitQuadRel(W, UV, C);
}
static void emitFish(const Mob& m, float rx, float ry, float rz, const float* col, float fa) {
    const FishSpec& S = FISH[m.var];
    const float Lw = S.len3d, Lb = Lw * S.Lb;
    // базис рыбы: f — вперёд (к голове), rt — вправо, up — вверх
    float cp = cosf(m.pitch), sp = sinf(m.pitch), cyw = cosf(m.yaw), syw = sinf(m.yaw);
    float f[3] = {syw * cp, sp, cyw * cp}, rt[3] = {cyw, 0, -syw};
    float up[3] = {rt[1] * f[2] - rt[2] * f[1], rt[2] * f[0] - rt[0] * f[2], rt[0] * f[1] - rt[1] * f[0]};
    if (up[1] < 0) for (int i = 0; i < 3; i++) up[i] = -up[i];
    bool out = m.walk > 2.f;   // на суше лежит на боку
    if (out) { float tmp[3]; memcpy(tmp, up, 12); for (int i = 0; i < 3; i++) { up[i] = rt[i]; rt[i] = -tmp[i]; } }
    float c0[3] = {rx, ry + 0.15f, rz};
    float amp = (0.035f + std::min(0.09f, m.walk * 0.03f)) * Lw;
    auto wig = [&](float t) { return amp * sinf(m.phase - t * 5.f) * (0.12f + t * t * 1.3f); };
    auto P3 = [&](float t, float lx, float ly, float* o) {   // t — вдоль тела (0 нос), lx — вбок, ly — вверх
        float ax = Lw * 0.5f - t * Lb;
        float wx = lx + wig(t);
        for (int i = 0; i < 3; i++) o[i] = c0[i] + f[i] * ax + rt[i] * wx + up[i] * ly;
    };
    const int NS = 8, NR = 8;
    static const float TS[NS] = {0.f, 0.045f, 0.13f, 0.28f, 0.48f, 0.68f, 0.86f, 1.f};
    float ring[NS][NR][3], vv[NR];
    float ca[NR], sa[NR];
    for (int k = 0; k < NR; k++) { float a = (k + 0.5f) * 6.2831853f / NR; ca[k] = cosf(a); sa[k] = sinf(a); vv[k] = (1.f - sa[k]) * 8.f; }
    for (int i = 0; i < NS; i++) {
        float t = TS[i], ht = fishProf(S.top, t) * Lw, hb = fishProf(S.bot, t) * Lw;
        float wd = (ht + hb) * 0.5f * FISH_WIDTH[m.var];
        if (i == 0) { ht = hb = wd = 0.f; }
        for (int k = 0; k < NR; k++) P3(t, ca[k] * wd, sa[k] > 0 ? sa[k] * ht : sa[k] * hb, ring[i][k]);
    }
    int skin = T_FSKIN0 + m.var;
    for (int i = 0; i + 1 < NS; i++) for (int k = 0; k < NR; k++) {
        int k2 = (k + 1) % NR;
        float W[4][3], UV[4][2];
        memcpy(W[0], ring[i][k], 12); memcpy(W[1], ring[i + 1][k], 12); memcpy(W[2], ring[i + 1][k2], 12); memcpy(W[3], ring[i][k2], 12);
        tileUV(skin, TS[i] * 16.f, vv[k], UV[0]); tileUV(skin, TS[i + 1] * 16.f, vv[k], UV[1]);
        tileUV(skin, TS[i + 1] * 16.f, vv[k2], UV[2]); tileUV(skin, TS[i] * 16.f, vv[k2], UV[3]);
        float ny = (sa[k] + sa[k2]) * 0.5f;
        float sh = 0.72f + 0.3f * ny;
        float ax[3]; float tm = (TS[i] + TS[i + 1]) * 0.5f; P3(tm, 0, (fishProf(S.top, tm) - fishProf(S.bot, tm)) * 0.5f * Lw, ax);
        emitFishQuad(W, UV, col, sh, fa, ax);
    }
    // тонкий плавник: quad от основания вдоль тела до края (двусторонний, без отсечения)
    auto fin = [&](int tile, float t0, float t1, float y0a, float y0b, float hgt, bool down, float side, float flip) {
        float W[4][3], UV[4][2];
        P3(t0, side, y0a, W[0]); P3(t1, side, y0b, W[1]);
        float d = down ? -hgt : hgt;
        P3(t1 + hgt * 0.35f / Lb * flip, side, y0b + d, W[2]); P3(t0 + hgt * 0.25f / Lb * flip, side, y0a + d, W[3]);
        float vb = down ? 0.f : 16.f, vt = down ? 16.f : 0.f;
        tileUV(tile, 0, vb, UV[0]); tileUV(tile, 16, vb, UV[1]); tileUV(tile, 16, vt, UV[2]); tileUV(tile, 0, vt, UV[3]);
        emitFishQuad(W, UV, col, 0.9f, fa, nullptr);
    };
    float hmax = 0; for (int i = 0; i < S.nd; i++) hmax = std::max(hmax, S.dors[i].h);
    {   // спинной плавник: тайл содержит все спинные плавники
        float a0 = S.dors[0].t0, a1 = S.dors[S.nd - 1].t1;
        float y0 = fishProf(S.top, a0 / S.Lb) * Lw * 0.9f, y1 = fishProf(S.top, a1 / S.Lb) * Lw * 0.9f;
        float W[4][3], UV[4][2], H = hmax * 1.05f * Lw;
        float t0 = a0 / S.Lb, t1 = a1 / S.Lb;
        P3(t0, 0, y0, W[0]); P3(t1, 0, y1, W[1]); P3(t1, 0, y1 + H, W[2]); P3(t0, 0, y0 + H, W[3]);
        tileUV(T_FFIN0 + m.var, 0, 16, UV[0]); tileUV(T_FFIN0 + m.var, 16, 16, UV[1]); tileUV(T_FFIN0 + m.var, 16, 0, UV[2]); tileUV(T_FFIN0 + m.var, 0, 0, UV[3]);
        emitFishQuad(W, UV, col, 0.92f, fa, nullptr);
    }
    {   // анальный и брюшные плавники — тот же рисунок, перевёрнутый
        float t0 = S.anal.t0 / S.Lb, t1 = S.anal.t1 / S.Lb;
        fin(T_FFIN0 + m.var, t0, t1, -fishProf(S.bot, t0) * Lw * 0.9f, -fishProf(S.bot, t1) * Lw * 0.9f, S.anal.h * Lw, true, 0.f, 1.f);
        float p0 = S.pelv.t0 / S.Lb, p1 = S.pelv.t1 / S.Lb, wd = fishProf(S.bot, p0) * Lw * FISH_WIDTH[m.var] * 0.3f;
        for (int sd = -1; sd <= 1; sd += 2)
            fin(T_FFIN0 + m.var, p0, p1, -fishProf(S.bot, p0) * Lw * 0.7f, -fishProf(S.bot, p1) * Lw * 0.7f, S.pelv.h * Lw * 0.8f, true, sd * wd, 1.f);
    }
    {   // грудные плавники: веером в стороны, машут
        float tg = (S.eyeT * 2.25f + 0.06f) / S.Lb, wd = (fishProf(S.top, tg) + fishProf(S.bot, tg)) * 0.5f * Lw * FISH_WIDTH[m.var];
        float flap = 0.5f + 0.35f * sinf(m.phase * 1.7f);
        for (int sd = -1; sd <= 1; sd += 2) {
            float W[4][3], UV[4][2], len = 0.09f * Lw;
            float yb = -fishProf(S.bot, tg) * Lw * 0.25f;
            P3(tg, sd * wd * 0.95f, yb + 0.01f * Lw, W[0]); P3(tg, sd * wd * 0.95f, yb - 0.03f * Lw, W[1]);
            float ox = sd * (wd + len * flap), back = tg + len * 1.1f / Lb;
            P3(back, ox, yb - 0.05f * Lw, W[2]); P3(back - 0.01f, ox, yb + 0.01f * Lw, W[3]);
            int tl = T_FTAIL0 + m.var;
            tileUV(tl, 0, 6, UV[0]); tileUV(tl, 0, 10, UV[1]); tileUV(tl, 16, 13, UV[2]); tileUV(tl, 16, 3, UV[3]);
            emitFishQuad(W, UV, col, 0.85f, fa, nullptr);
        }
    }
    {   // хвост: продолжает изгиб тела
        float ped = (fishProf(S.top, 1.f) - fishProf(S.bot, 1.f)) * 0.5f * Lw;
        float tl = (1.f - S.Lb) * Lw * 1.05f, th = S.tailH * Lw;
        float slope = (wig(1.f) - wig(0.96f)) / (0.04f * Lb);
        float W[4][3], UV[4][2];
        float b0[3]; P3(1.f, 0, ped, b0);
        auto tp = [&](float s, float y, float* o) {
            float lat = slope * s * tl * 1.4f;
            for (int i = 0; i < 3; i++) o[i] = b0[i] - f[i] * s * tl + rt[i] * lat + up[i] * y;
        };
        tp(-0.04f, th, W[0]); tp(1.f, th, W[1]); tp(1.f, -th, W[2]); tp(-0.04f, -th, W[3]);
        int t = T_FTAIL0 + m.var;
        tileUV(t, 0, 0, UV[0]); tileUV(t, 16, 0, UV[1]); tileUV(t, 16, 16, UV[2]); tileUV(t, 0, 16, UV[3]);
        emitFishQuad(W, UV, col, 0.9f, fa, nullptr);
    }
    {   // глаза: круглые, чуть выпуклые наклейки на боках головы
        float et = S.eyeT / S.Lb * 0.95f, ey = fishProf(S.top, et) * S.eyeV * Lw;
        float wd = (fishProf(S.top, et) + fishProf(S.bot, et)) * 0.5f * Lw * FISH_WIDTH[m.var];
        float er = std::max(S.eyeR * Lw * 1.1f, 0.014f);
        int tl = T_FEYE0 + m.var;
        for (int sd = -1; sd <= 1; sd += 2) {
            float W[4][3], UV[4][2], lx = sd * (wd * 0.86f + 0.004f);
            P3(et - er / Lb, lx, ey + er, W[0]); P3(et + er / Lb, lx, ey + er, W[1]); P3(et + er / Lb, lx, ey - er, W[2]); P3(et - er / Lb, lx, ey - er, W[3]);
            tileUV(tl, 0, 0, UV[0]); tileUV(tl, 16, 0, UV[1]); tileUV(tl, 16, 16, UV[2]); tileUV(tl, 0, 16, UV[3]);
            float ax[3]; P3(et, 0, ey, ax);
            float bright[3] = {std::min(1.f, col[0] * 1.15f), std::min(1.f, col[1] * 1.15f), std::min(1.f, col[2] * 1.15f)};
            emitFishQuad(W, UV, bright, 1.f, fa, ax);
        }
    }
}
// поплавок и леска
static void emitFishingLine() {
    if (!g_bob.active) return;
    const Camera& c = g_cam;
    float bx = (float)(g_bob.x - c.x), by = (float)(g_bob.y - c.y), bz = (float)(g_bob.z - c.z);
    float lc[3]; lightColorAt(g_bob.x, g_bob.y + 0.3, g_bob.z, lc);
    // поплавок: красный низ, белый верх, тёмная антенна с красным кончиком
    {
        auto box = [&](float y0, float y1, float hw, float r0, float g0, float b0) {
            float corner[8][3];
            for (int k = 0; k < 8; k++) { corner[k][0] = bx + ((k & 1) ? hw : -hw); corner[k][1] = by + ((k & 2) ? y1 : y0); corner[k][2] = bz + ((k & 4) ? hw : -hw); }
            int tiles[6] = {T_WHITE, T_WHITE, T_WHITE, T_WHITE, T_WHITE, T_WHITE};
            float cc[3] = {lc[0] * r0, lc[1] * g0, lc[2] * b0};
            emitBox8(corner, tiles, cc, 1.f, false);
        };
        box(-0.08f, 0.0f, 0.065f, 0.85f, 0.14f, 0.12f);
        box(-0.12f, -0.08f, 0.035f, 0.75f, 0.12f, 0.1f);
        box(0.0f, 0.07f, 0.058f, 0.95f, 0.94f, 0.9f);
        box(0.07f, 0.19f, 0.01f, 0.15f, 0.15f, 0.15f);
        box(0.19f, 0.23f, 0.016f, 0.95f, 0.2f, 0.15f);
    }
    // кончик удочки: точка, которую запомнила рука (в пространстве камеры) -> в мир
    float tip[3];
    if (g_rodTipOK) for (int i = 0; i < 3; i++) tip[i] = c.r[i] * g_rodTipCam[0] + c.u[i] * g_rodTipCam[1] + c.f[i] * g_rodTipCam[2];
    else for (int i = 0; i < 3; i++) tip[i] = c.r[i] * 0.35f - c.u[i] * 0.1f + c.f[i] * 1.f;
    float A[3] = {tip[0], tip[1], tip[2]}, B[3] = {bx, by + 0.18f, bz};
    float dx = B[0] - A[0], dy = B[1] - A[1], dz = B[2] - A[2], L = sqrtf(dx * dx + dy * dy + dz * dz);
    float sag = g_bob.state == 3 ? 0.02f : (g_bob.state == 0 ? 0.05f * L : std::min(1.5f, 0.08f * L));
    const int N = 18;
    float pts[N + 1][3];
    for (int i = 0; i <= N; i++) {
        float t = i / (float)N;
        pts[i][0] = A[0] + dx * t; pts[i][1] = A[1] + dy * t - sag * 4.f * t * (1 - t); pts[i][2] = A[2] + dz * t;
    }
    float lineCol[3] = {lc[0] * 0.88f, lc[1] * 0.88f, lc[2] * 0.85f};
    for (int i = 0; i < N; i++) {
        float* p0 = pts[i]; float* p1 = pts[i + 1];
        float d[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        float m[3] = {(p0[0] + p1[0]) * 0.5f, (p0[1] + p1[1]) * 0.5f, (p0[2] + p1[2]) * 0.5f};
        float sx = d[1] * m[2] - d[2] * m[1], sy = d[2] * m[0] - d[0] * m[2], sz = d[0] * m[1] - d[1] * m[0];
        float sl = sqrtf(sx * sx + sy * sy + sz * sz) + 1e-6f;
        float dist = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        float w = std::max(0.004f, dist * 0.0018f);
        sx = sx / sl * w; sy = sy / sl * w; sz = sz / sl * w;
        float W[4][3] = {{p0[0] - sx, p0[1] - sy, p0[2] - sz}, {p1[0] - sx, p1[1] - sy, p1[2] - sz}, {p1[0] + sx, p1[1] + sy, p1[2] + sz}, {p0[0] + sx, p0[1] + sy, p0[2] + sz}};
        float UV[4][2]; for (int k = 0; k < 4; k++) tileUV(T_WHITE, 8, 8, UV[k]);
        float C[4][4]; for (int k = 0; k < 4; k++) { C[k][0] = lineCol[0]; C[k][1] = lineCol[1]; C[k][2] = lineCol[2]; C[k][3] = 0.95f; }
        emitQuadRel(W, UV, C);
    }
}

// ---------------------------------------------------------------- модели животных
// роль детали: 0 тело, 1..4 лапы (ПЛ, ПП, ЗЛ, ЗП), 5 голова (и всё на ней), 6 хвост
struct APart { float x, y, z, sx, sy, sz; int tf, ts, tt; int role; float r, g, b; bool unlit; };
static int animalParts(const Mob& m, APart* p, float* headPiv, float* tailPiv, float* hip) {
    int n = 0;
    auto add = [&](float x, float y, float z, float sx, float sy, float sz, int tf, int ts, int tt, int role, const float* tint = nullptr, bool unlit = false) {
        APart a = {x, y, z, sx, sy, sz, tf, ts, tt, role, tint ? tint[0] : 1.f, tint ? tint[1] : 1.f, tint ? tint[2] : 1.f, unlit}; p[n++] = a; };
    const float one[3] = {1, 1, 1};
    switch (m.type) {
        case MOB_PIG: {
            const float tint[3] = {m.var ? 0.9f : 1.f, m.var ? 0.78f : 1.f, m.var ? 0.74f : 1.f};
            for (int i = 0; i < 4; i++) add((i & 1) ? -0.19f : 0.19f, 0.18f, (i < 2) ? 0.3f : -0.3f, 0.2f, 0.36f, 0.2f, T_PIG_LEG, T_PIG_LEG, T_PIG_SKIN, 1 + i, tint);
            add(0, 0.6f, -0.02f, 0.6f, 0.5f, 0.96f, T_PIG_SKIN, T_PIG_SKIN, T_PIG_SKIN, 0, tint);
            add(0, 0.7f, 0.66f, 0.48f, 0.46f, 0.4f, T_PIG_FACE, T_PIG_SKIN, T_PIG_SKIN, 5, tint);
            add(0, 0.62f, 0.9f, 0.26f, 0.18f, 0.1f, T_PIG_SNOUT, T_PIG_SKIN, T_PIG_SKIN, 5, tint);
            add(0.17f, 0.96f, 0.62f, 0.16f, 0.06f, 0.14f, T_PIG_SKIN, T_PIG_SKIN, T_PIG_SKIN, 5, tint);   // ушки
            add(-0.17f, 0.96f, 0.62f, 0.16f, 0.06f, 0.14f, T_PIG_SKIN, T_PIG_SKIN, T_PIG_SKIN, 5, tint);
            add(0, 0.76f, -0.55f, 0.06f, 0.06f, 0.12f, T_PIG_SKIN, T_PIG_SKIN, T_PIG_SKIN, 6, tint);    // хвостик крючком
            add(0, 0.82f, -0.6f, 0.06f, 0.1f, 0.05f, T_PIG_SKIN, T_PIG_SKIN, T_PIG_SKIN, 6, tint);
            headPiv[0] = 0.75f; headPiv[1] = 0.46f; tailPiv[0] = 0.76f; tailPiv[1] = -0.5f; *hip = -0.3f;
            break;
        }
        case MOB_SHEEP: {
            float wc[3] = {((WOOL_RGB[m.var] >> 16) & 255) / 235.f, ((WOOL_RGB[m.var] >> 8) & 255) / 235.f, (WOOL_RGB[m.var] & 255) / 235.f};
            for (int i = 0; i < 4; i++) {
                float lx = (i & 1) ? -0.2f : 0.2f, lz = (i < 2) ? 0.33f : -0.33f;
                add(lx, 0.3f, lz, 0.17f, 0.6f, 0.17f, T_SHEEP_LEG, T_SHEEP_LEG, T_SHEEP_LEG, 1 + i);
                if (!m.sheared) add(lx, 0.52f, lz, 0.25f, 0.2f, 0.25f, T_SHEEP_WOOL, T_SHEEP_WOOL, T_SHEEP_WOOL, 1 + i, wc);   // «гетры» из шерсти
            }
            if (m.sheared) add(0, 0.9f, 0, 0.56f, 0.48f, 0.94f, T_SHEEP_SKIN, T_SHEEP_SKIN, T_SHEEP_SKIN, 0);
            else { add(0, 0.94f, 0, 0.82f, 0.66f, 1.12f, T_SHEEP_WOOL, T_SHEEP_WOOL, T_SHEEP_WOOL, 0, wc); add(0, 0.9f, -0.6f, 0.2f, 0.2f, 0.1f, T_SHEEP_WOOL, T_SHEEP_WOOL, T_SHEEP_WOOL, 6, wc); }
            add(0, 1.18f, 0.64f, 0.34f, 0.36f, 0.42f, T_SHEEP_FACE, T_SHEEP_SKIN, T_SHEEP_SKIN, 5);
            if (!m.sheared) add(0, 1.38f, 0.58f, 0.4f, 0.14f, 0.32f, T_SHEEP_WOOL, T_SHEEP_WOOL, T_SHEEP_WOOL, 5, wc);   // чубчик
            add(0.25f, 1.28f, 0.58f, 0.18f, 0.06f, 0.09f, T_SHEEP_SKIN, T_SHEEP_SKIN, T_SHEEP_SKIN, 5);   // уши в стороны
            add(-0.25f, 1.28f, 0.58f, 0.18f, 0.06f, 0.09f, T_SHEEP_SKIN, T_SHEEP_SKIN, T_SHEEP_SKIN, 5);
            headPiv[0] = 1.1f; headPiv[1] = 0.48f; tailPiv[0] = 0.95f; tailPiv[1] = -0.55f; *hip = -0.33f;
            break;
        }
        case MOB_CAT: {
            const float* t = CAT_TINT[m.var & 3];
            float belly[3] = {lerpf(t[0], 1.05f, 0.6f), lerpf(t[1], 1.03f, 0.6f), lerpf(t[2], 1.f, 0.6f)};
            for (int i = 0; i < 4; i++) add((i & 1) ? -0.09f : 0.09f, 0.17f, (i < 2) ? 0.24f : -0.22f, 0.09f, 0.34f, 0.09f, T_CAT_FUR, T_CAT_FUR, T_CAT_FUR, 1 + i, t);
            add(0, 0.48f, 0.0f, 0.28f, 0.28f, 0.68f, T_CAT_FUR, T_CAT_FUR, T_CAT_FUR, 0, t);
            add(0, 0.36f, 0.02f, 0.24f, 0.05f, 0.6f, T_CAT_BELLY, T_CAT_BELLY, T_CAT_BELLY, 0, belly);
            add(0, 0.62f, 0.42f, 0.3f, 0.26f, 0.26f, T_CAT_FACE, T_CAT_FUR, T_CAT_FUR, 5, t);
            add(0, 0.56f, 0.57f, 0.14f, 0.1f, 0.06f, T_CAT_BELLY, T_CAT_BELLY, T_CAT_BELLY, 5, belly);   // мордочка
            add(0.09f, 0.79f, 0.41f, 0.08f, 0.09f, 0.05f, T_CAT_FUR, T_CAT_FUR, T_CAT_FUR, 5, t);        // уши
            add(-0.09f, 0.79f, 0.41f, 0.08f, 0.09f, 0.05f, T_CAT_FUR, T_CAT_FUR, T_CAT_FUR, 5, t);
            add(0.09f, 0.84f, 0.41f, 0.04f, 0.04f, 0.04f, T_CAT_FUR, T_CAT_FUR, T_CAT_FUR, 5, t);
            add(-0.09f, 0.84f, 0.41f, 0.04f, 0.04f, 0.04f, T_CAT_FUR, T_CAT_FUR, T_CAT_FUR, 5, t);
            const float eye[3] = {0.55f, 0.95f, 0.25f}, pupil[3] = {0.05f, 0.05f, 0.04f}, nose[3] = {0.9f, 0.5f, 0.55f};
            add(0.075f, 0.66f, 0.553f, 0.07f, 0.06f, 0.01f, T_WHITE, T_WHITE, T_WHITE, 5, eye);
            add(-0.075f, 0.66f, 0.553f, 0.07f, 0.06f, 0.01f, T_WHITE, T_WHITE, T_WHITE, 5, eye);
            add(0.075f, 0.66f, 0.558f, 0.018f, 0.05f, 0.01f, T_WHITE, T_WHITE, T_WHITE, 5, pupil);
            add(-0.075f, 0.66f, 0.558f, 0.018f, 0.05f, 0.01f, T_WHITE, T_WHITE, T_WHITE, 5, pupil);
            add(0, 0.59f, 0.605f, 0.045f, 0.03f, 0.012f, T_WHITE, T_WHITE, T_WHITE, 5, nose);
            add(0, 0.6f, -0.62f, 0.06f, 0.06f, 0.56f, T_CAT_FUR, T_CAT_FUR, T_CAT_FUR, 6, t);   // хвост
            (void)one;
            headPiv[0] = 0.58f; headPiv[1] = 0.32f; tailPiv[0] = 0.6f; tailPiv[1] = -0.34f; *hip = -0.22f;
            break;
        }
        case MOB_DOG: {
            const float* t = DOG_TINT[m.var & 3];
            float light[3] = {lerpf(t[0], 1.05f, 0.55f), lerpf(t[1], 1.02f, 0.55f), lerpf(t[2], 0.98f, 0.55f)};
            for (int i = 0; i < 4; i++) add((i & 1) ? -0.11f : 0.11f, 0.21f, (i < 2) ? 0.27f : -0.27f, 0.11f, 0.42f, 0.11f, T_DOG_FUR, T_DOG_FUR, T_DOG_FUR, 1 + i, t);
            add(0, 0.58f, -0.04f, 0.34f, 0.34f, 0.76f, T_DOG_FUR, T_DOG_FUR, T_DOG_FUR, 0, t);
            add(0, 0.64f, 0.26f, 0.42f, 0.4f, 0.3f, T_DOG_FUR, T_DOG_FUR, T_DOG_FUR, 0, light);   // грудь-«грива»
            add(0, 0.82f, 0.54f, 0.34f, 0.32f, 0.3f, T_DOG_FACE, T_DOG_FUR, T_DOG_FUR, 5, t);
            add(0, 0.74f, 0.76f, 0.18f, 0.15f, 0.18f, T_DOG_MUZZLE, T_DOG_FUR, T_DOG_FUR, 5, light);
            add(0.1f, 1.02f, 0.5f, 0.09f, 0.12f, 0.05f, T_DOG_FUR, T_DOG_FUR, T_DOG_FUR, 5, t);
            add(-0.1f, 1.02f, 0.5f, 0.09f, 0.12f, 0.05f, T_DOG_FUR, T_DOG_FUR, T_DOG_FUR, 5, t);
            const float eye[3] = {0.22f, 0.14f, 0.08f}, angry[3] = {0.9f, 0.15f, 0.1f};
            const float* ec = (!m.tame && m.angryT > 0) ? angry : eye;
            add(0.085f, 0.88f, 0.693f, 0.06f, 0.05f, 0.01f, T_WHITE, T_WHITE, T_WHITE, 5, ec, ec == angry);
            add(-0.085f, 0.88f, 0.693f, 0.06f, 0.05f, 0.01f, T_WHITE, T_WHITE, T_WHITE, 5, ec, ec == angry);
            if (m.tame) add(0, 0.73f, 0.42f, 0.38f, 0.08f, 0.2f, T_COLLAR, T_COLLAR, T_COLLAR, 0);
            add(0, 0.66f, -0.6f, 0.08f, 0.08f, 0.42f, T_DOG_FUR, T_DOG_FUR, T_DOG_FUR, 6, t);
            headPiv[0] = 0.76f; headPiv[1] = 0.4f; tailPiv[0] = 0.68f; tailPiv[1] = -0.42f; *hip = -0.27f;
            break;
        }
    }
    return n;
}
static inline void rotX(float& y, float& z, float py, float pz, float a) { float yy = y - py, zz = z - pz, c = cosf(a), s = sinf(a); y = py + yy * c - zz * s; z = pz + yy * s + zz * c; }
static inline void rotY(float& x, float& z, float px, float pz, float a) { float xx = x - px, zz = z - pz, c = cosf(a), s = sinf(a); x = px + xx * c + zz * s; z = pz - xx * s + zz * c; }
static void emitAnimal(const Mob& m, float rx, float ry, float rz, const float* lit, float fa) {
    APart parts[32]; float headPiv[2], tailPiv[2], hip;
    int n = animalParts(m, parts, headPiv, tailPiv, &hip);
    float sc = mobSc(m);
    float cy = cosf(m.yaw), sy = sinf(m.yaw);
    float swingA = sinf(m.walk) * 0.7f;
    float t = g_fxTime;
    // голова: поворот к игроку и наклон (овца щиплет траву)
    float headYaw = m.headYaw, headPitch = 0;
    if (m.type == MOB_SHEEP && m.eatT > 0) { float k = std::min(1.f, std::min(m.eatT, 2.2f - m.eatT) * 4.f); headPitch = 0.95f * k + sinf(t * 18.f) * 0.06f * k; headYaw = 0; }
    // хвост: собака виляет (выше — здоровее), кошка держит трубой при ходьбе
    float tailPitch = 0, tailYaw = 0;
    if (m.type == MOB_DOG) { tailPitch = m.tame ? -0.15f - 0.9f * (1.f - m.hp / mobMaxHp(m)) : (m.angryT > 0 ? -0.6f : -0.4f); if (m.tame && !m.sit) tailYaw = sinf(t * 14.f) * 0.55f; else tailYaw = sinf(t * 2.f) * 0.1f; }
    else if (m.type == MOB_CAT) { tailPitch = m.moving ? -0.9f : -0.25f; tailYaw = sinf(t * 1.7f + m.uid) * 0.35f; }
    else if (m.type == MOB_PIG) tailYaw = sinf(t * 6.f) * 0.4f;
    bool sitting = m.tame && m.sit;
    for (int i = 0; i < n; i++) {
        APart p = parts[i];
        if (sitting && (p.role == 1 || p.role == 2)) {
            // сидит: передние лапы тянутся от груди (поднятой вместе с корпусом) прямо до земли — без зазора
            float ty = p.y + p.sy * 0.5f, tz = p.z;
            rotX(ty, tz, 0.12f, hip, -0.42f); ty -= 0.12f;
            float top = ty + 0.04f;               // небольшой заход внутрь корпуса
            p.y = top * 0.5f; p.sy = top; p.z = tz - 0.01f;
        }
        float corner[8][3];
        for (int c = 0; c < 8; c++) {
            float lx = p.x + ((c & 1) ? 0.5f : -0.5f) * p.sx;
            float ly = p.y + ((c & 2) ? 0.5f : -0.5f) * p.sy;
            float lz = p.z + ((c & 4) ? 0.5f : -0.5f) * p.sz;
            if (p.role >= 1 && p.role <= 4) {
                bool front = p.role <= 2;
                float pivY = p.y + p.sy * 0.5f;
                float a = (p.role == 1 || p.role == 4) ? swingA : -swingA;
                if (sitting) a = front ? 0.f : -1.45f;   // сидит: задние лапы подогнуты вперёд
                rotX(ly, lz, pivY, p.z, -a);
            } else if (p.role == 5) {
                rotX(ly, lz, headPiv[0], headPiv[1], headPitch);
                rotY(lx, lz, 0, headPiv[1], headYaw);
            } else if (p.role == 6) {
                rotX(ly, lz, tailPiv[0], tailPiv[1], -tailPitch);
                rotY(lx, lz, 0, tailPiv[1], tailYaw);
            }
            if (sitting && !(p.role == 1 || p.role == 2)) {   // корпус приподнят спереди, таз опущен
                const float pivY = 0.12f, sa = -0.42f;
                if (p.role == 5) {   // голова не запрокидывается: сдвигается вместе с шеей, но остаётся ровной
                    float hy2 = headPiv[0], hz2 = headPiv[1]; rotX(hy2, hz2, pivY, hip, sa);
                    ly += hy2 - headPiv[0] - 0.12f; lz += hz2 - headPiv[1];
                } else {
                    rotX(ly, lz, pivY, hip, sa);
                    ly -= 0.12f;
                    if (p.role == 3 || p.role == 4) ly = std::max(ly, 0.02f);
                }
            }
            lx *= sc; ly *= sc; lz *= sc;
            corner[c][0] = rx + lx * cy + lz * sy;
            corner[c][1] = ry + ly;
            corner[c][2] = rz - lx * sy + lz * cy;
        }
        int tiles[6] = {p.ts, p.ts, p.tt, p.ts, p.tf, p.ts};
        float col[3];
        for (int k = 0; k < 3; k++) col[k] = p.unlit ? (&p.r)[k] : lit[k] * (&p.r)[k];
        emitBox8(corner, tiles, col, fa, false);
    }
}
// питомцы, оставшиеся далеко (сидят дома): хранятся отдельно и возвращаются, когда их место снова загружено
static std::vector<Mob> g_parked;
static void parkFarPets() {
    for (size_t i = 0; i < g_parked.size();) {
        Mob& m = g_parked[i];
        double dx = m.x - P.x, dz = m.z - P.z;
        if (dx * dx + dz * dz < 80.0 * 80.0 && isLoaded(ifloor(m.x), ifloor(m.z))) { m.uid = g_mobUidNext++; g_mobs.push_back(m); g_parked[i] = g_parked.back(); g_parked.pop_back(); continue; }
        i++;
    }
}

// ---------------------------------------------------------------- другие игроки в сетевой игре
static const u32 SHIRT_RGB[8] = {0xd84a3a, 0x3a7ad8, 0x3aa858, 0xe0b030, 0x9a4ad0, 0x30b0b8, 0xe07a2a, 0x7a7a86};
static void emitRemotePlayers() {
    for (const RemotePlayer& r : g_remote) {
        if (r.id == g_myId || (r.flags & 4)) continue;
        float rx = (float)(r.x - g_cam.x), ry = (float)(r.y - g_cam.y), rz = (float)(r.z - g_cam.z);
        float d2 = rx * rx + ry * ry + rz * rz;
        if (d2 > 96.f * 96.f || !sphereVisible(rx, ry + 1, rz, 2.f)) continue;
        float fa = fogAlpha(d2);
        float lit[3]; lightColorAt(r.x, r.y + 1.2, r.z, lit);
        u32 sc = SHIRT_RGB[r.id % 8];
        float shirt[3] = {((sc >> 16) & 255) / 220.f, ((sc >> 8) & 255) / 220.f, (sc & 255) / 220.f};
        float cy = cosf(r.yaw), sy = sinf(r.yaw);
        float sw = sinf(r.walk) * std::min(1.f, r.speed / 3.f) * 0.8f;
        float att = r.swing > 0 ? sinf(r.swing * 3.1416f) * 1.6f : 0.f;
        struct B { float x, y, z, sx, sy, sz; int t[6]; int role; const float* tint; };
        const float one[3] = {1, 1, 1};
        B parts[] = {
            {0.125f, 0.42f, 0, 0.25f, 0.66f, 0.25f, {T_PL_PANTS, T_PL_PANTS, T_PL_PANTS, T_PL_SHOE, T_PL_PANTS, T_PL_PANTS}, 1, one},
            {-0.125f, 0.42f, 0, 0.25f, 0.66f, 0.25f, {T_PL_PANTS, T_PL_PANTS, T_PL_PANTS, T_PL_SHOE, T_PL_PANTS, T_PL_PANTS}, 2, one},
            {0.125f, 0.05f, 0.02f, 0.27f, 0.12f, 0.3f, {T_PL_SHOE, T_PL_SHOE, T_PL_SHOE, T_PL_SHOE, T_PL_SHOE, T_PL_SHOE}, 1, one},
            {-0.125f, 0.05f, 0.02f, 0.27f, 0.12f, 0.3f, {T_PL_SHOE, T_PL_SHOE, T_PL_SHOE, T_PL_SHOE, T_PL_SHOE, T_PL_SHOE}, 2, one},
            {0, 1.125f, 0, 0.5f, 0.75f, 0.26f, {T_PL_SHIRT, T_PL_SHIRT, T_PL_SHIRT, T_PL_SHIRT, T_PL_SHIRT_F, T_PL_SHIRT}, 0, shirt},
            {0.375f, 1.125f, 0, 0.24f, 0.74f, 0.24f, {T_PL_ARM, T_PL_ARM, T_PL_SHIRT, T_PL_ARM, T_PL_ARM, T_PL_ARM}, 3, shirt},
            {-0.375f, 1.125f, 0, 0.24f, 0.74f, 0.24f, {T_PL_ARM, T_PL_ARM, T_PL_SHIRT, T_PL_ARM, T_PL_ARM, T_PL_ARM}, 4, shirt},
            {0, 1.76f, 0, 0.5f, 0.5f, 0.5f, {T_PL_HEAD, T_PL_HEAD, T_PL_HAIR, T_PL_HEAD, T_PL_FACE, T_PL_HAIR}, 5, one},
        };
        for (const B& p : parts) {
            float corner[8][3];
            for (int c = 0; c < 8; c++) {
                float lx = p.x + ((c & 1) ? 0.5f : -0.5f) * p.sx;
                float ly = p.y + ((c & 2) ? 0.5f : -0.5f) * p.sy;
                float lz = p.z + ((c & 4) ? 0.5f : -0.5f) * p.sz;
                if (p.role >= 1 && p.role <= 4) {
                    float a = (p.role == 1 || p.role == 4) ? sw : -sw;
                    if (p.role == 3) a = sw * 0.8f - att;   // правая рука машет при ударе
                    if (p.role == 4) a = -sw * 0.8f;
                    float pivY = p.role >= 3 ? 1.45f : 0.75f;
                    rotX(ly, lz, pivY, 0, -a);
                } else if (p.role == 5) {
                    rotX(ly, lz, 1.5f, 0, clampf(-r.pitch, -1.2f, 1.2f));
                }
                corner[c][0] = rx + lx * cy + lz * sy;
                corner[c][1] = ry + ly;
                corner[c][2] = rz - lx * sy + lz * cy;
            }
            int tiles[6] = {p.t[0], p.t[1], p.t[2], p.t[3], p.t[4], p.t[5]};
            // рубашка красится только на ткани (у рук низ — кожа: оттенок слабее)
            float col[3];
            float tk = (p.tint == shirt && (p.role == 3 || p.role == 4)) ? 0.7f : 1.f;
            for (int k = 0; k < 3; k++) col[k] = lit[k] * lerpf(1.f, p.tint[k], tk);
            emitBox8(corner, tiles, col, fa, false);
        }
        // предмет в правой руке — маленький значок
        if (r.held > 0 && r.held < I_END) {
            int tile = itemTile(r.held);
            float hx = 0.39f, hy = 0.72f, hz = 0.22f;
            rotX(hy, hz, 1.45f, 0, -(sw * 0.8f - att));
            float s = 0.16f;
            float W[4][3], UV[4][2], C[4][4];
            const float qx[4] = {-s, s, s, -s}, qy[4] = {s, s, -s, -s};
            for (int k = 0; k < 4; k++) {
                float lx = hx, ly = hy + qy[k], lz = hz + qx[k];
                W[k][0] = rx + lx * cy + lz * sy; W[k][1] = ry + ly; W[k][2] = rz - lx * sy + lz * cy;
                C[k][0] = lit[0]; C[k][1] = lit[1]; C[k][2] = lit[2]; C[k][3] = fa;
            }
            tileUV(tile, 0.5f, 0.5f, UV[0]); tileUV(tile, 15.5f, 0.5f, UV[1]); tileUV(tile, 15.5f, 15.5f, UV[2]); tileUV(tile, 0.5f, 15.5f, UV[3]);
            emitQuadRel(W, UV, C);
            float W2[4][3]; memcpy(W2[0], W[1], 12); memcpy(W2[1], W[0], 12); memcpy(W2[2], W[3], 12); memcpy(W2[3], W[2], 12);   // обратная сторона
            float UV2[4][2]; memcpy(UV2[0], UV[1], 8); memcpy(UV2[1], UV[0], 8); memcpy(UV2[2], UV[3], 8); memcpy(UV2[3], UV[2], 8);
            emitQuadRel(W2, UV2, C);
        }
    }
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
        if (m.type == MOB_FISH) {   // в воде рыбу освещает свет, проходящий сквозь толщу сверху
            int fx = ifloor(m.x), fz = ifloor(m.z), fy = ifloor(m.y + 0.15), sy = fy;
            while (sy < fy + 8 && getBlock(fx, sy, fz) == B_WATER) sy++;
            if (sy != fy) {
                float sc[3]; lightColorAt(m.x, sy + 0.5, m.z, sc);
                float k = std::max(0.45f, 1.f - (sy - fy) * 0.09f);
                for (int i = 0; i < 3; i++) col[i] = std::max(col[i], sc[i] * k);
                if (m.hurtT > 0) { col[0] = std::min(1.f, col[0] + 0.5f); col[1] *= 0.45f; col[2] *= 0.45f; }
            }
            emitFish(m, rx, ry, rz, col, fa); continue;
        }
        if (isNewAnimal(m.type)) { emitAnimal(m, rx, ry, rz, col, fa); continue; }
        MPart parts[24]; int n = mobParts(m.type, parts);
        const float msc = mobSc(m);
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
                lx *= msc; ly *= msc; lz *= msc;
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
        float us = p.kind == 3 ? 16.f : 3.f;   // искры — целой картинкой, обломки — кусочком текстуры
        tileUV(p.tile, p.u, p.v, UV[0]); tileUV(p.tile, p.u + us, p.v, UV[1]); tileUV(p.tile, p.u + us, p.v + us, UV[2]); tileUV(p.tile, p.u, p.v + us, UV[3]);
        for (int k = 0; k < 4; k++) { C[k][0] = col[0]; C[k][1] = col[1]; C[k][2] = col[2]; C[k][3] = al; }
        emitQuadRel(W, UV, C);
    }
}
static void emitCrack() {
    if (!g_dig.active || g_mode == GM_CREATIVE) return;
    int stage = clampi((int)(g_dig.prog * 10.f), 0, 9);
    float e = 0.004f;
    float corner[8][3];
    for (int c = 0; c < 8; c++) {
        corner[c][0] = (float)(g_dig.x - g_cam.x) + ((c & 1) ? 1 + e : -e);
        corner[c][1] = (float)(g_dig.y - g_cam.y) + ((c & 2) ? 1 + e : -e);
        corner[c][2] = (float)(g_dig.z - g_cam.z) + ((c & 4) ? 1 + e : -e);
    }
    int tiles[6]; for (int k = 0; k < 6; k++) tiles[k] = crackTile(stage);
    float col[3] = {1, 1, 1};
    emitBox8(corner, tiles, col, 0.9f, false, -0.05f);
}
// рука и предмет в руке (в пространстве камеры)
// ---------------------------------------------------------------- рука и предмет в руке (пространство камеры: x вправо, y вверх, z вперёд)
struct Basis3 { float x[3], y[3], z[3], o[3]; };
static inline void bApply(const Basis3& B, float lx, float ly, float lz, float* out) {
    for (int i = 0; i < 3; i++) out[i] = B.o[i] + B.x[i] * lx + B.y[i] * ly + B.z[i] * lz;
}
// базис из поворотов: сначала вокруг Z (крен), затем X (наклон), затем Y (поворот)
static Basis3 bEuler(float yaw, float pitch, float roll, float ox, float oy, float oz) {
    Basis3 B;
    float cr = cosf(roll), sr = sinf(roll), cp = cosf(pitch), sp = sinf(pitch), cy = cosf(yaw), sy = sinf(yaw);
    auto rot = [&](float x, float y, float z, float* o) {
        float x1 = x * cr - y * sr, y1 = x * sr + y * cr, z1 = z;           // Z
        float y2 = y1 * cp - z1 * sp, z2 = y1 * sp + z1 * cp, x2 = x1;      // X
        o[0] = x2 * cy + z2 * sy; o[1] = y2; o[2] = -x2 * sy + z2 * cy;     // Y
    };
    rot(1, 0, 0, B.x); rot(0, 1, 0, B.y); rot(0, 0, 1, B.z);
    B.o[0] = ox; B.o[1] = oy; B.o[2] = oz;
    return B;
}
static void emitBoxB(const Basis3& B, float x0, float y0, float z0, float x1, float y1, float z1, const int tiles[6], const float col[3]) {
    float c8[8][3];
    for (int c = 0; c < 8; c++) bApply(B, (c & 1) ? x1 : x0, (c & 2) ? y1 : y0, (c & 4) ? z1 : z0, c8[c]);
    emitBox8(c8, tiles, col, 1.f, true);
}
// объёмный предмет: спрайт 16x16, «выдавленный» на 1 пиксель в глубину
static void emitExtruded(int tile, const Basis3& B, float size, const float col[3]) {
    // силуэт: при детализации от 32 берём маску 32x32 из HD-атласа — края предмета мельче и ровнее
    const int N = (g_tileRes >= 32 && (int)g_hdAtlas.size() == g_hdSize * g_hdH) ? 32 : 16;
    const float fN = 16.f / N;
    const int tx = (tile & 15) * 16, ty = (tile >> 4) * 16;
    auto opaque = [&](int u, int v) {
        if (u < 0 || v < 0 || u >= N || v >= N) return false;
        if (N == 16) return pA(g_atlasPx[(ty + v) * ATLAS + tx + u]) > 100;
        int st = g_tileRes / N, hx = (tile & 15) * g_tileRes + u * st + st / 2, hy = (tile >> 4) * g_tileRes + v * st + st / 2;
        return pA(g_hdAtlas[(size_t)hy * g_hdSize + hx]) > 110;
    };
    const float k = size / 16.f, t = k * 0.5f;
    auto P = [&](float u, float v, float d, float* o) { bApply(B, (u - 8.f) * k, (8.f - v) * k, d, o); };
    float W[4][3], UV[4][2], C[4][4];
    auto setC = [&](float sh) { for (int i = 0; i < 4; i++) { C[i][0] = col[0] * sh; C[i][1] = col[1] * sh; C[i][2] = col[2] * sh; C[i][3] = 1; } };
    // лицевая и задняя стороны целиком (прозрачные пиксели атласа не рисуются)
    for (int side = 0; side < 2; side++) {
        float d = side ? t : -t;
        if (!side) { P(0, 0, d, W[0]); P(16, 0, d, W[1]); P(16, 16, d, W[2]); P(0, 16, d, W[3]); tileUV(tile, 0, 0, UV[0]); tileUV(tile, 16, 0, UV[1]); tileUV(tile, 16, 16, UV[2]); tileUV(tile, 0, 16, UV[3]); }
        else { P(16, 0, d, W[0]); P(0, 0, d, W[1]); P(0, 16, d, W[2]); P(16, 16, d, W[3]); tileUV(tile, 16, 0, UV[0]); tileUV(tile, 0, 0, UV[1]); tileUV(tile, 0, 16, UV[2]); tileUV(tile, 16, 16, UV[3]); }
        float n[3] = {B.z[0] * (side ? 1.f : -1.f), B.z[1] * (side ? 1.f : -1.f), B.z[2] * (side ? 1.f : -1.f)};
        if (n[0] * W[0][0] + n[1] * W[0][1] + n[2] * W[0][2] >= 0) continue;   // смотрит от камеры
        setC(side ? 0.7f : 1.f);
        emitQuadCam(W, UV, C);
    }
    // боковые грани только по краю силуэта
    for (int v = 0; v < N; v++) for (int u = 0; u < N; u++) {
        if (!opaque(u, v)) continue;
        const int du[4] = {1, -1, 0, 0}, dv[4] = {0, 0, 1, -1};
        const float shd[4] = {0.62f, 0.8f, 0.5f, 0.92f};
        for (int e = 0; e < 4; e++) {
            if (opaque(u + du[e], v + dv[e])) continue;
            float u0, v0, u1, v1;
            if (e == 0) { u0 = u1 = u + 1.f; v0 = (float)v; v1 = v + 1.f; }
            else if (e == 1) { u0 = u1 = (float)u; v0 = v + 1.f; v1 = (float)v; }
            else if (e == 2) { v0 = v1 = v + 1.f; u0 = u + 1.f; u1 = (float)u; }
            else { v0 = v1 = (float)v; u0 = (float)u; u1 = u + 1.f; }
            u0 *= fN; u1 *= fN; v0 *= fN; v1 *= fN;
            P(u0, v0, -t, W[0]); P(u1, v1, -t, W[1]); P(u1, v1, t, W[2]); P(u0, v0, t, W[3]);
            float e1[3] = {W[1][0] - W[0][0], W[1][1] - W[0][1], W[1][2] - W[0][2]}, e2[3] = {W[3][0] - W[0][0], W[3][1] - W[0][1], W[3][2] - W[0][2]};
            float nn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            float dp = nn[0] * W[0][0] + nn[1] * W[0][1] + nn[2] * W[0][2];
            if (fabsf(dp) < 1e-7f) continue;
            if (dp > 0) { std::swap(W[1], W[3]); }
            for (int i = 0; i < 4; i++) tileUV(tile, (u + 0.5f) * fN, (v + 0.5f) * fN, UV[i]);
            setC(shd[e]);
            emitQuadCam(W, UV, C);
        }
    }
}
static void emitHand() {
    if (P.dead) return;
    float col[3]; lightColorAt(P.x, P.y + PL_EYE, P.z, col);
    const Stack& s = P.inv[P.sel];
    float bx = sinf(P.bobPhase) * 0.03f * P.bobAmt, by = -fabsf(cosf(P.bobPhase)) * 0.025f * P.bobAmt;
    float sw = sinf(P.swing * (float)M_PI), sw2 = sinf(sqrtf(P.swing) * (float)M_PI);
    bool bow = s.id == I_BOW;
    float pull = bow ? P.bowCharge : 0.f;
    // где ладонь: справа снизу; при взмахе уходит к центру и вперёд
    // ладонь, точка, откуда рука входит в кадр (снизу), поворот инструмента, толщина руки
    static const float HD[10] = {0.5f, -0.32f, 0.78f, 0.62f, -1.15f, 0.35f, -0.55f, 0.12f, -0.12f, 0.07f};
    float hx = HD[0] + bx - sw2 * 0.22f - pull * 0.06f, hy = HD[1] + by + sw2 * 0.10f - sw * 0.05f, hz = HD[2] + sw * 0.12f - pull * 0.08f;
    if (s.empty()) { hx += 0.04f; hy -= 0.03f; }
    if (bow) { hx -= 0.08f; hy += 0.03f; }
    // рука тянется к ладони из-за нижнего правого края экрана

    {
        const float back[3] = {HD[3], HD[4], HD[5]};
        float d[3] = {hx - back[0], hy - back[1], hz - back[2]}, L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int i = 0; i < 3; i++) d[i] /= L;
        float up[3] = {0, 1, 0};
        float xa[3] = {up[1] * d[2] - up[2] * d[1], up[2] * d[0] - up[0] * d[2], up[0] * d[1] - up[1] * d[0]};
        float xl = sqrtf(xa[0] * xa[0] + xa[1] * xa[1] + xa[2] * xa[2]); for (int i = 0; i < 3; i++) xa[i] /= xl;
        float ya[3] = {d[1] * xa[2] - d[2] * xa[1], d[2] * xa[0] - d[0] * xa[2], d[0] * xa[1] - d[1] * xa[0]};
        Basis3 A; for (int i = 0; i < 3; i++) { A.x[i] = xa[i]; A.y[i] = ya[i]; A.z[i] = d[i]; }
        // ладонь ровно там, где лежит предмет: рукоять проходит сквозь сжатый кулак
        A.o[0] = hx; A.o[1] = hy; A.o[2] = hz;
        const float W = HD[9];
        const int skin[6] = {T_SKIN, T_SKIN, T_SKIN, T_SKIN, T_SKIN, T_SKIN};
        const int sleeve[6] = {T_SLEEVE, T_SLEEVE, T_SLEEVE, T_SLEEVE, T_SLEEVE, T_SLEEVE};
        if (s.empty()) emitBoxB(A, -W, -W, -0.36f, W, W, 0.f, skin, col);
        else {
            emitBoxB(A, -W, -W, -0.36f, W, W, -0.06f, skin, col);   // предплечье
            float fc[3] = {col[0] * 0.93f, col[1] * 0.9f, col[2] * 0.88f};
            emitBoxB(A, -W * 1.12f, -W * 1.08f, -0.07f, W * 1.12f, W * 1.12f, 0.06f, skin, fc);   // кулак, чуть шире
            float kc[3] = {col[0] * 0.8f, col[1] * 0.74f, col[2] * 0.72f};
            emitBoxB(A, -W * 1.16f, W * 0.55f, 0.035f, W * 1.16f, W * 1.16f, 0.075f, skin, kc);  // согнутые пальцы спереди
        }
        emitBoxB(A, -W * 1.18f, -W * 1.18f, -L - 0.3f, W * 1.18f, W * 1.18f, -0.34f, sleeve, col);
    }
    if (s.empty()) return;
    if (itemIsCubeIcon(s.id)) {
        // блок: небольшой кубик в ладони, повёрнут углом к нам
        Basis3 B = bEuler(0.78f - sw * 0.3f, -0.42f - sw * 0.5f, 0.f, hx - 0.02f, hy + 0.165f, hz + 0.05f);
        const float h = 0.075f;
        int tiles[6]; for (int k = 0; k < 6; k++) tiles[k] = BD[s.id].tex[k];
        if (s.id == B_FURNACE) tiles[4] = T_FURN_FRONT;
        if (s.id == B_FURNACE_LIT) tiles[4] = T_FURN_FRONT_LIT;
        if (s.id == B_CHEST) tiles[4] = T_CHEST_FRONT;
        emitBoxB(B, -h, -h, -h, h, h, h, tiles, col);
        return;
    }
    int t = itemTile(s.id);
    bool tool = isTool(s.id) || s.id == I_ROD;
    g_rodTipOK = false;
    if (s.id == I_ROD) t = T_ROD_HAND;
    if (bow) {
        // лук стоит вертикально, тетива к нам; при натяжении наклоняется
        Basis3 B = bEuler(-0.55f, 0.f, -0.3f - pull * 0.1f, hx - 0.02f, hy + 0.16f, hz + 0.08f);
        emitExtruded(t, B, 0.5f, col);
        return;
    }
    // инструменты и предметы: рукоять в ладони, верх направлен вперёд-вверх
    float size = tool ? 0.46f : 0.3f;
    Basis3 B = tool ? bEuler(HD[6], HD[7], HD[8], 0, 0, 0) : bEuler(-0.5f, 0.1f, 0.f, 0, 0, 0);
    if (tool) for (int i = 0; i < 3; i++) B.x[i] = -B.x[i];   // зеркально: головка смотрит влево, к центру экрана
    // точка спрайта, которая лежит в кулаке: у инструментов — рукоять чуть выше торца, у прочего — низ предмета
    float gu = tool ? 4.f : 8.f, gv = tool ? 12.f : 14.f;
    // взмах: поворот вокруг оси «вправо» камеры
    float swA = sw * 1.1f;
    Basis3 R = bEuler(0.f, swA, 0.f, 0, 0, 0);
    Basis3 F;
    for (int i = 0; i < 3; i++) {   // F = R * B
        F.x[i] = R.x[i] * B.x[0] + R.y[i] * B.x[1] + R.z[i] * B.x[2];
        F.y[i] = R.x[i] * B.y[0] + R.y[i] * B.y[1] + R.z[i] * B.y[2];
        F.z[i] = R.x[i] * B.z[0] + R.y[i] * B.z[1] + R.z[i] * B.z[2];
    }
    float k = size / 16.f, gl[3] = {(gu - 8.f) * k, (8.f - gv) * k, 0.f};
    for (int i = 0; i < 3; i++) F.o[i] = 0;
    float g[3]; bApply(F, gl[0], gl[1], gl[2], g);
    float gp[3] = {hx, hy + (tool ? 0.f : 0.04f), hz + (tool ? 0.f : 0.02f)};
    F.o[0] = gp[0] - g[0]; F.o[1] = gp[1] - g[1]; F.o[2] = gp[2] - g[2];
    if (s.id == I_ROD) {   // кончик запоминаем для лески
        bApply(F, (14.4f - 8.f) * k, (8.f - 1.3f) * k, 0.f, g_rodTipCam); g_rodTipOK = true;
    }
    emitExtruded(t, F, size, col);
}

// ---------------------------------------------------------------- сохранение
static int g_worldSlot = 1;
static std::string g_worldName = "МИР";
static float g_playTime = 0;
static const int MAX_WORLDS = 6;

static std::string worldPath(int slot) { char b[64]; snprintf(b, sizeof(b), "buildworld_world_%d.sav", slot); return g_basePath + b; }

struct WBuf {
    std::vector<u8> d; size_t p = 0; bool bad = false; u32 ver = 2;
    void put(const void* s, size_t n) { const u8* b = (const u8*)s; d.insert(d.end(), b, b + n); }
    template <class T> void w(T v) { put(&v, sizeof(T)); }
    void get(void* o, size_t n) { if (p + n > d.size()) { bad = true; memset(o, 0, n); return; } memcpy(o, d.data() + p, n); p += n; }
    template <class T> T r() { T v; get(&v, sizeof(T)); return v; }
    void ws(const Stack& s) { w<u16>(s.id); w<u16>(s.cnt); w<u16>(s.dmg); w<u16>(s.ench); }
    Stack rs() { Stack s; s.id = r<u16>(); s.cnt = r<u16>(); s.dmg = r<u16>(); s.ench = ver >= 2 ? r<u16>() : 0; if (ver < 4 && s.id >= 64) s.id += OLD_ITEM_SHIFT; if (s.id >= I_END || (s.id >= B_COUNT && s.id < I_STICK) || (s.id >= I_ARROW + 1 && s.id < I_TOOL0)) s.clear(); return s; }
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
static const u32 SAVE_VER = 4;   // 4: высота мира 256, номера предметов сдвинуты на 64
static WorldInfo readWorldInfo(int slot) {
    WorldInfo wi; std::vector<u8> d;
    if (!readFile(worldPath(slot), d)) return wi;
    WBuf b; b.d.swap(d);
    if (b.r<u32>() != SAVE_MAGIC) return wi;
    b.ver = b.r<u32>();
    if (b.ver >= 3) b.r<u8>();
    wi.name = b.rstr(); wi.seed = b.r<u32>(); wi.mode = b.r<u8>(); b.r<float>(); wi.playTime = b.r<float>();
    wi.exists = !b.bad;
    return wi;
}

static bool saveWorld() {
    if (g_netRole == NET_CLIENT) { saveMpProfile(); return true; }   // гость хранит только свою сумку
    for (auto& kv : g_chunks) if (kv.second->modified) { std::vector<u8> v; encodeChunk(kv.second, v); g_savedChunks[kv.first] = v; }
    WBuf b;
    b.w<u32>(SAVE_MAGIC); b.w<u32>(SAVE_VER); b.w<u8>((u8)g_genVer);
    b.wstr(g_worldName); b.w<u32>(g_seed); b.w<u8>((u8)g_mode); b.w<float>(g_dayTime); b.w<float>(g_playTime); b.w<i64>(g_tick);
    b.w<double>(P.x); b.w<double>(P.y); b.w<double>(P.z); b.w<float>(g_cam.yaw); b.w<float>(g_cam.pitch);
    b.w<float>(P.health); b.w<float>(P.hunger); b.w<float>(P.exhaustion); b.w<float>(P.air);
    b.w<i32>(P.spawnX); b.w<i32>(P.spawnY); b.w<i32>(P.spawnZ); b.w<u8>((u8)P.sel); b.w<u8>(P.flying ? 1 : 0); b.w<u8>(P.dead ? 1 : 0);
    for (int i = 0; i < 36; i++) b.ws(P.inv[i]);
    for (int i = 0; i < 4; i++) b.ws(P.armor[i]);
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
    // версия 4: погода, опыт и прирученные питомцы
    b.w<u32>(0x34545845u);   // "EXT4"
    b.w<u8>((u8)g_weather); b.w<float>(g_weatherT); b.w<float>(g_rainK); b.w<float>(g_thunderK);
    b.w<i32>(P.xpLevel); b.w<float>(P.xpProg); b.w<i32>(P.xpTotal);
    std::vector<const Mob*> pets;
    for (const Mob& m : g_mobs) if (m.tame && !m.dead) pets.push_back(&m);
    for (const Mob& m : g_parked) pets.push_back(&m);
    b.w<u32>((u32)pets.size());
    for (const Mob* m : pets) {
        b.w<u8>((u8)m->type); b.w<double>(m->x); b.w<double>(m->y); b.w<double>(m->z); b.w<float>(m->yaw); b.w<float>(m->hp);
        b.w<u8>((u8)m->var); b.w<u8>(m->sit ? 1 : 0); b.w<float>(m->age); b.w<u8>((u8)m->owner);
    }
    b.w<u32>(0x43594144u); b.w<i32>(g_dayCount);   // "DAYC": счётчик суток (фазы луны)
    bool ok = writeFileAtomic(worldPath(g_worldSlot), b.d);
    return ok;
}

static void lodClear();
static void resetWorldState() {
    freeAllChunks();
    lodClear();
    g_savedChunks.clear(); g_chests.clear(); g_furnaces.clear(); g_grow.clear(); g_sched.clear(); g_pending.clear();
    g_mobs.clear(); g_drops.clear(); g_parts.clear(); g_arrows.clear(); g_bob.active = false; g_orbs.clear(); g_parked.clear();
    g_tick = 0; g_playTime = 0; g_dayTime = 0.05f; g_dayCount = 0;
    g_weather = WX_CLEAR; g_weatherT = 500.f; g_rainK = g_thunderK = g_flash = 0; g_bolts.clear(); g_delayed.clear(); g_rainVol.store(0.f);
    P = Player();
}

static bool loadWorld(int slot) {
    std::vector<u8> d;
    if (!readFile(worldPath(slot), d)) return false;
    WBuf b; b.d.swap(d);
    if (b.r<u32>() != SAVE_MAGIC) return false;
    b.ver = b.r<u32>();
    int genVer = b.ver >= 3 ? b.r<u8>() : 1;
    resetWorldState();
    g_worldSlot = slot; g_genVer = genVer;
    g_dayCount = 0;
    g_worldName = b.rstr(); u32 seed = b.r<u32>(); g_mode = b.r<u8>(); g_dayTime = b.r<float>(); g_playTime = b.r<float>(); g_tick = b.r<i64>();
    worldInitNoise(seed);
    P.x = b.r<double>(); P.y = b.r<double>(); P.z = b.r<double>(); g_cam.yaw = b.r<float>(); g_cam.pitch = b.r<float>();
    P.health = b.r<float>(); P.hunger = b.r<float>(); P.exhaustion = b.r<float>(); P.air = b.r<float>();
    P.spawnX = b.r<i32>(); P.spawnY = b.r<i32>(); P.spawnZ = b.r<i32>(); P.sel = clampi(b.r<u8>(), 0, 8); P.flying = b.r<u8>() != 0; P.dead = b.r<u8>() != 0;
    for (int i = 0; i < 36; i++) P.inv[i] = b.rs();
    if (b.ver >= 2) for (int i = 0; i < 4; i++) P.armor[i] = b.rs();
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
        if (b.ver < 4) {   // старый чанк высотой 128: дополняем воздухом до 256
            static u8 blk[CHUNK_VOL], meta[CHUNK_VOL];
            memset(blk, 0, sizeof(blk)); memset(meta, 0, sizeof(meta));
            std::vector<u8>& v = g_savedChunks[k];
            const u8* p = v.data(); const u8* e = p + v.size();
            if (rleDecode(p, e, blk, OLD_WH * 256) && rleDecode(p, e, meta, OLD_WH * 256)) {
                v.clear(); rleEncode(blk, CHUNK_VOL, v); rleEncode(meta, CHUNK_VOL, v);
            } else g_savedChunks.erase(k);
        }
    }
    if (b.ver >= 4 && !b.bad && b.p + 4 <= b.d.size() && b.r<u32>() == 0x34545845u) {
        g_weather = clampi(b.r<u8>(), 0, 2); g_weatherT = b.r<float>(); g_rainK = clampf(b.r<float>(), 0, 1); g_thunderK = clampf(b.r<float>(), 0, 1);
        P.xpLevel = std::max(0, b.r<i32>()); P.xpProg = clampf(b.r<float>(), 0, 0.999f); P.xpTotal = std::max(0, b.r<i32>());
        u32 np = b.r<u32>();
        for (u32 i = 0; i < np && i < 512 && !b.bad; i++) {
            Mob m; m.type = b.r<u8>(); m.x = b.r<double>(); m.y = b.r<double>(); m.z = b.r<double>(); m.yaw = b.r<float>(); m.hp = b.r<float>();
            m.var = b.r<u8>(); m.sit = b.r<u8>() != 0; m.age = b.r<float>(); m.owner = b.r<u8>();
            if (b.bad || !isPetType(m.type)) continue;
            m.tame = true; m.wishYaw = m.yaw; if (!(m.hp > 0)) m.hp = 10;
            g_parked.push_back(m);
        }
        if (!(g_weatherT > 0 && g_weatherT < 100000)) g_weatherT = 600;
        if (!b.bad && b.p + 8 <= b.d.size() && b.r<u32>() == 0x43594144u) g_dayCount = std::max(0, b.r<i32>());
    }
    g_grow.clear();   // саженцы будут собраны при загрузке чанков
    if (g_tick < 0) g_tick = 0;
    if (!(P.y > -50 && P.y < 400)) P.y = 100;
    return true;
}
static void deleteWorld(int slot) { remove(worldPath(slot).c_str()); }

// ---------------------------------------------------------------- настройки
struct Settings { int viewDist = 16; int renderDist = 6; float sens = 1.0f; float volume = 0.8f; int inset = 4; bool showFps = false; bool autoJump = true; float uiScale = 1.0f; int shaders = 2; int texRes = 128; float music = 0.6f; bool rumble = true; float padSens = 1.0f; bool invertY = false; bool dof = false; };
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
    fprintf(f, "%d %f %f %d %d %d %f %d %d %f %d %f %d %d %d\n", g_set.renderDist, g_set.sens, g_set.volume, g_set.inset, g_set.showFps ? 1 : 0, g_set.autoJump ? 1 : 0, g_set.uiScale, g_set.shaders, g_set.texRes, g_set.music,
            g_set.rumble ? 1 : 0, g_set.padSens, g_set.invertY ? 1 : 0, g_set.viewDist, g_set.dof ? 1 : 0);
    fclose(f);
}
static void loadSettings() {
    FILE* f = fopen((g_basePath + "buildworld_settings.cfg").c_str(), "r");
    if (!f) return;
    int rd = 6, inset = 4, fps = 0, aj = 1, sh = 2, tr = 128, rum = 1, inv = 0, vd = 16, dof = 0; float sens = 1, vol = 0.8f, ui = 1.f, mus = 0.6f, ps = 1.f;
    if (fscanf(f, "%d %f %f %d %d %d %f %d %d %f %d %f %d %d %d", &rd, &sens, &vol, &inset, &fps, &aj, &ui, &sh, &tr, &mus, &rum, &ps, &inv, &vd, &dof) >= 4) {
        g_set.dof = dof != 0;
        g_set.viewDist = clampi(vd, 4, 40);
        g_set.music = clampf(mus, 0.f, 1.f);
        g_set.rumble = rum != 0; g_set.padSens = clampf(ps, 0.3f, 3.f); g_set.invertY = inv != 0;
        g_set.shaders = clampi(sh, 0, 2);
        g_set.texRes = (tr == 16 || tr == 32 || tr == 64 || tr == 128 || tr == 256) ? tr : 128;
        g_set.renderDist = clampi(rd, 4, 12); g_set.sens = clampf(sens, 0.2f, 3.f); g_set.volume = clampf(vol, 0, 1);
        g_set.inset = clampi(inset, 0, 12); g_set.showFps = fps != 0; g_set.autoJump = aj != 0; g_set.uiScale = clampf(ui, 0.7f, 1.4f);
    }
    fclose(f);
}

// ============================================================================
//  ИНТЕРФЕЙС: текст, значки, кнопки (касание + геймпад), экраны
// ============================================================================
enum Screen { SCR_INTRO = -1, SCR_MAIN = 0, SCR_WORLDS, SCR_NEWWORLD, SCR_LOADING, SCR_GAME, SCR_PAUSE, SCR_SETTINGS, SCR_INV, SCR_DEAD, SCR_CONFIRM_DEL, SCR_MP };
static int g_screen = SCR_INTRO, g_settingsBack = SCR_MAIN;
static bool g_introSkip = false;   // пропустить заставку (кнопка «назад», клавиша)
static void screenMain();
enum InvMode { INV_PLAYER = 0, INV_CRAFT, INV_FURNACE, INV_CHEST, INV_ANVIL };
static int g_invMode = INV_PLAYER;
static int g_bookPage = 0, g_creativePage = 0;
// книга рецептов: панель слева от сетки создания
static bool g_bookOpen = false, g_bookOnlyCan = false;
static int g_bookTab = 0, g_bookSel = -1;           // вкладка и выбранный результат (для подсказки)
static int g_ghostRecipe = -1;                      // рецепт-подсказка в сетке, когда не хватает материалов
static bool g_altMode = false;           // «по одному»
static Stack g_cursor;
static Stack g_craft[9];
static Stack g_anvil[2];   // наковальня: слева предмет, справа книга

static float g_heldNameT = 0;

static float g_frameDt = 1.f / 60;   // длительность кадра для плавных анимаций интерфейса
static float uiS() { return g_H / 100.f; }                            // 1% высоты экрана
static float fontPx() { return std::max(2.f, g_H / 240.f) * g_set.uiScale; }
static int insetX() { return (int)(g_W * g_set.inset / 100.f); }
static int insetTop() { return (int)(g_H * 0.045f); }

// ---------------------------------------------------------------- текст
static SDL_Color C(int r, int g, int b, int a = 255) { SDL_Color c; c.r = (Uint8)r; c.g = (Uint8)g; c.b = (Uint8)b; c.a = (Uint8)a; return c; }
static float textWidth(const std::string& s, float px) {
    const char* p = s.c_str(); int n = 0;
    while (*p) { utf8Next(p); n++; }
    return n * 6 * px - px;
}
static void drawText(const std::string& s, float x, float y, float px, SDL_Color col, int align = 0, bool shadow = true) {
    if (align == 1) x -= textWidth(s, px) / 2; else if (align == 2) x -= textWidth(s, px);
    static std::vector<SDL_Vertex> vb; static std::vector<int> ib;
    // проходы: мягкая тень (размытая обводка снизу) и сам текст; всё одной пачкой на проход
    for (int pass = shadow ? 0 : 2; pass < 3; pass++) {
        vb.clear(); ib.clear();
        SDL_Color c = col;
        float ox = 0, oy = 0, grow = 0;
        bool darkText = col.r * 3 + col.g * 6 + col.b < 1100;   // тёмный текст на светлом фоне: тень светлая и слабая
        if (pass < 2 && darkText) continue;   // тёмный текст на светлом фоне — без тени, чище
        if (pass == 0) { c = C(0, 0, 0, (Uint8)(col.a * 0.35f)); ox = px * 0.35f; oy = px * 0.55f; grow = px * 0.45f; }   // широкая полупрозрачная тень
        else if (pass == 1) { c = C(10, 10, 14, (Uint8)(col.a * 0.75f)); ox = px * 0.3f; oy = px * 0.45f; }   // чёткая тень
        const char* p = s.c_str(); float cx = x;
        while (*p) {
            u32 ch = utf8Next(p);
            if (ch != ' ') {
                int g = glyphFor(ch);
                if (g >= 0) {
                    float u0 = (float)((g % 21) * GLYPH_CW * FONT_S) / FONT_TEX_W, v0 = (float)((g / 21) * GLYPH_CH * FONT_S) / FONT_TEX_H;
                    float u1 = u0 + (float)(GLYPH_CW * FONT_S) / FONT_TEX_W, v1 = v0 + (float)(GLYPH_CH * FONT_S) / FONT_TEX_H;
                    float X0 = cx - 0.75f * px + ox - grow, Y0 = y - 0.75f * px + oy - grow, X1 = X0 + 6 * px + 2 * grow, Y1 = Y0 + 8 * px + 2 * grow;
                    int b = (int)vb.size();
                    SDL_Vertex v; v.color = c;
                    v.position.x = X0; v.position.y = Y0; v.tex_coord.x = u0; v.tex_coord.y = v0; vb.push_back(v);
                    v.position.x = X1; v.tex_coord.x = u1; vb.push_back(v);
                    v.position.y = Y1; v.tex_coord.y = v1; vb.push_back(v);
                    v.position.x = X0; v.tex_coord.x = u0; vb.push_back(v);
                    int q[6] = {b, b + 1, b + 2, b, b + 2, b + 3}; ib.insert(ib.end(), q, q + 6);
                }
            }
            cx += 6 * px;
        }
        if (!vb.empty()) SDL_RenderGeometry(g_ren, g_fontTex, vb.data(), (int)vb.size(), ib.data(), (int)ib.size());
    }
}
static void fillRect(float x, float y, float w, float h, SDL_Color c) {
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a);
    SDL_Rect r = {(int)x, (int)y, (int)w, (int)h};
    SDL_RenderFillRect(g_ren, &r);
}
static void frameRect(float x, float y, float w, float h, float t, SDL_Color c) {
    fillRect(x, y, w, t, c); fillRect(x, y + h - t, w, t, c); fillRect(x, y, t, h, c); fillRect(x + w - t, y, t, h, c);
}
// свечение по краям экрана: цвет у края, к центру прозрачнее
static void drawEdgeGlow(int r, int g, int b, float a) {
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);   // геометрия без текстуры смешивается по режиму рисования
    float w = g_W * 0.22f, h = g_H * 0.3f;
    SDL_Color o; o.r = (Uint8)r; o.g = (Uint8)g; o.b = (Uint8)b; o.a = (Uint8)(255 * clampf(a, 0, 1)); SDL_Color z = o; z.a = 0;
    auto quad = [&](float x0, float y0, float x1, float y1, SDL_Color c0, SDL_Color c1, bool horiz) {
        SDL_Vertex v[4]; int idx[6] = {0, 1, 2, 0, 2, 3};
        float X[4] = {x0, x1, x1, x0}, Y[4] = {y0, y0, y1, y1};
        for (int i = 0; i < 4; i++) { v[i].position.x = X[i]; v[i].position.y = Y[i]; v[i].tex_coord.x = v[i].tex_coord.y = 0;
            bool first = horiz ? (i == 0 || i == 3) : (i < 2); v[i].color = first ? c0 : c1; }
        SDL_RenderGeometry(g_ren, NULL, v, 4, idx, 6);
    };
    quad(0, 0, w, (float)g_H, o, z, true); quad(g_W - w, 0, (float)g_W, (float)g_H, z, o, true);
    quad(0, 0, (float)g_W, h, o, z, false); quad(0, g_H - h, (float)g_W, (float)g_H, z, o, false);
}
// скруглённый прямоугольник с вертикальным градиентом (верх -> низ)
static void roundRect(float x, float y, float w, float h, float r, SDL_Color top, SDL_Color bot) {
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    r = std::min(r, std::min(w, h) * 0.5f);
    const int NC = 5;
    SDL_Vertex v[4 * (NC + 1) + 1]; int idx[4 * (NC + 1) * 3];
    int n = 0;
    auto colAt = [&](float py) { float k = h > 0 ? clampf((py - y) / h, 0.f, 1.f) : 0.f; SDL_Color c;
        c.r = (Uint8)(top.r + (bot.r - top.r) * k); c.g = (Uint8)(top.g + (bot.g - top.g) * k); c.b = (Uint8)(top.b + (bot.b - top.b) * k); c.a = (Uint8)(top.a + (bot.a - top.a) * k); return c; };
    v[n].position.x = x + w / 2; v[n].position.y = y + h / 2; v[n].color = colAt(y + h / 2); v[n].tex_coord.x = v[n].tex_coord.y = 0; n++;
    const float cxs[4] = {x + w - r, x + r, x + r, x + w - r}, cys[4] = {y + r, y + r, y + h - r, y + h - r};
    for (int c = 0; c < 4; c++) for (int i = 0; i <= NC; i++) {
        float a = (c * 90.f + i * 90.f / NC) * 3.14159265f / 180.f;   // 0° вправо, против часовой в экранных координатах (y вниз -> минус)
        float px = cxs[c] + cosf(a) * r, py = cys[c] - sinf(a) * r;
        v[n].position.x = px; v[n].position.y = py; v[n].color = colAt(py); v[n].tex_coord.x = v[n].tex_coord.y = 0; n++;
    }
    int m = n - 1, k = 0;
    for (int i = 0; i < m; i++) { idx[k++] = 0; idx[k++] = 1 + i; idx[k++] = 1 + (i + 1) % m; }
    SDL_RenderGeometry(g_ren, NULL, v, n, idx, k);
}
// скруглённая рамка толщиной t
static void roundFrame(float x, float y, float w, float h, float r, float t, SDL_Color c) {
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    r = std::min(r, std::min(w, h) * 0.5f);
    const int NC = 5, M = 4 * (NC + 1);
    SDL_Vertex v[M * 2]; int idx[M * 6]; int k = 0;
    const float cxs[4] = {x + w - r, x + r, x + r, x + w - r}, cys[4] = {y + r, y + r, y + h - r, y + h - r};
    for (int cc = 0; cc < 4; cc++) for (int i = 0; i <= NC; i++) {
        float a = (cc * 90.f + i * 90.f / NC) * 3.14159265f / 180.f, ca = cosf(a), sa = sinf(a);
        int j = cc * (NC + 1) + i;
        v[j * 2].position.x = cxs[cc] + ca * r; v[j * 2].position.y = cys[cc] - sa * r;
        float ri = std::max(0.f, r - t);
        v[j * 2 + 1].position.x = cxs[cc] + ca * ri; v[j * 2 + 1].position.y = cys[cc] - sa * ri;
        if (r < t) { v[j * 2 + 1].position.x += (ca > 0 ? -1 : 1) * (t - r) * fabsf(ca); v[j * 2 + 1].position.y += (sa > 0 ? 1 : -1) * (t - r) * fabsf(sa); }
        for (int q = 0; q < 2; q++) { v[j * 2 + q].color = c; v[j * 2 + q].tex_coord.x = v[j * 2 + q].tex_coord.y = 0; }
    }
    for (int j = 0; j < M; j++) { int a = j * 2, b = ((j + 1) % M) * 2; int q[6] = {a, b, a + 1, b, b + 1, a + 1}; for (int z = 0; z < 6; z++) idx[k++] = q[z]; }
    SDL_RenderGeometry(g_ren, NULL, v, M * 2, idx, k);
}
static void fillCircle(float cx, float cy, float r, SDL_Color c) {
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    const int N = 28; SDL_Vertex v[N + 1]; int idx[N * 3];
    v[0].position.x = cx; v[0].position.y = cy; v[0].color = c; v[0].tex_coord.x = v[0].tex_coord.y = 0;
    for (int i = 0; i < N; i++) {
        float a = i * 6.2831853f / N;
        v[i + 1].position.x = cx + cosf(a) * r; v[i + 1].position.y = cy + sinf(a) * r; v[i + 1].color = c; v[i + 1].tex_coord.x = v[i + 1].tex_coord.y = 0;
        idx[i * 3] = 0; idx[i * 3 + 1] = i + 1; idx[i * 3 + 2] = (i + 1) % N + 1;
    }
    SDL_RenderGeometry(g_ren, NULL, v, N + 1, idx, N * 3);
}
// ---------------------------------------------------------------- значки кнопок геймпада (Xbox / PlayStation / Nintendo)
enum PadGlyph { PG_A = 0, PG_B, PG_X, PG_Y, PG_LB, PG_RB, PG_LT, PG_RT, PG_START, PG_BACK, PG_LS, PG_RS, PG_DPAD, PG_DLR, PG_DUD };
static void thickLine(float x0, float y0, float x1, float y1, float w, SDL_Color c) {
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy) + 1e-4f, nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    SDL_Vertex v[4]; int idx[6] = {0, 1, 2, 0, 2, 3};
    float X[4] = {x0 + nx, x1 + nx, x1 - nx, x0 - nx}, Y[4] = {y0 + ny, y1 + ny, y1 - ny, y0 - ny};
    for (int i = 0; i < 4; i++) { v[i].position.x = X[i]; v[i].position.y = Y[i]; v[i].color = c; v[i].tex_coord.x = v[i].tex_coord.y = 0; }
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(g_ren, NULL, v, 4, idx, 6);
}
static void ringCircle(float cx, float cy, float r, float t, SDL_Color c);
static int g_padStyle2 = 0;   // копия стиля для рисования (задаётся из кода геймпада)
static void drawPadGlyph(int g, float cx, float cy, float r) {
    int st = g_padStyle2;
    float lw = std::max(1.5f, r * 0.2f);
    if (g <= PG_Y) {   // лицевые кнопки
        fillCircle(cx, cy + r * 0.08f, r, C(0, 0, 0, 90));
        fillCircle(cx, cy, r, C(34, 36, 44, 235));
        ringCircle(cx, cy, r, std::max(1.f, r * 0.08f), C(200, 204, 214, 200));
        if (st == 1) {   // PlayStation: крест, круг, квадрат, треугольник
            float s = r * 0.45f;
            if (g == PG_A) { SDL_Color c = C(120, 170, 255); thickLine(cx - s, cy - s, cx + s, cy + s, lw, c); thickLine(cx - s, cy + s, cx + s, cy - s, lw, c); }
            else if (g == PG_B) ringCircle(cx, cy, s * 1.05f, lw, C(255, 100, 110));
            else if (g == PG_X) { SDL_Color c = C(240, 130, 210); float q = s * 0.95f; thickLine(cx - q, cy - q, cx + q, cy - q, lw, c); thickLine(cx + q, cy - q, cx + q, cy + q, lw, c); thickLine(cx + q, cy + q, cx - q, cy + q, lw, c); thickLine(cx - q, cy + q, cx - q, cy - q, lw, c); }
            else { SDL_Color c = C(90, 220, 170); float q = s * 1.1f; thickLine(cx, cy - q, cx + q, cy + q * 0.75f, lw, c); thickLine(cx + q, cy + q * 0.75f, cx - q, cy + q * 0.75f, lw, c); thickLine(cx - q, cy + q * 0.75f, cx, cy - q, lw, c); }
        } else {
            static const char* XL[4] = {"A", "B", "X", "Y"}, *NL[4] = {"B", "A", "Y", "X"};
            static const SDL_Color XC[4] = {{110, 210, 90, 255}, {240, 90, 80, 255}, {90, 150, 250, 255}, {250, 210, 70, 255}};
            float px = r * 0.17f;
            drawText(st == 2 ? NL[g] : XL[g], cx + px * 0.3f, cy - 3.5f * px, px, st == 2 ? C(240, 240, 240) : XC[g], 1, false);
        }
        return;
    }
    if (g == PG_LS || g == PG_RS) {   // нажатие стика
        fillCircle(cx, cy, r, C(34, 36, 44, 235)); ringCircle(cx, cy, r * 0.72f, std::max(1.f, r * 0.08f), C(170, 174, 186));
        float px = r * 0.15f; drawText(g == PG_LS ? "L3" : "R3", cx + px * 0.3f, cy - 3.5f * px, px, C(240, 240, 240), 1, false);
        return;
    }
    if (g >= PG_DPAD) {   // крестовина: подсвечены нужные стороны
        float a = r * 0.36f, L = r * 0.95f;
        auto arm = [&](float dx, float dy, bool on) { roundRect(cx + dx * L * 0.5f - (dx ? L * 0.5f : a) , cy + dy * L * 0.5f - (dy ? L * 0.5f : a), dx ? L : a * 2, dy ? L : a * 2, a * 0.35f, on ? C(245, 245, 250, 245) : C(60, 62, 72, 235), on ? C(215, 218, 226, 245) : C(44, 46, 54, 235)); };
        bool lr = g == PG_DLR || g == PG_DPAD, ud = g == PG_DUD || g == PG_DPAD;
        arm(-1, 0, lr); arm(1, 0, lr); arm(0, -1, ud); arm(0, 1, ud);
        return;
    }
    // плечевые кнопки, курки, START/SELECT — капсула с подписью
    static const char* XB[6] = {"LB", "RB", "LT", "RT", "МЕНЮ", "ВИД"}, *PS[6] = {"L1", "R1", "L2", "R2", "OPTIONS", "SHARE"}, *NS[6] = {"L", "R", "ZL", "ZR", "+", "-"};
    const char* t = (st == 1 ? PS : (st == 2 ? NS : XB))[g - PG_LB];
    float px = r * 0.15f, w = std::max(r * 1.9f, textWidth(t, px) + r * 0.8f), h = r * 1.35f;
    bool trig = g == PG_LT || g == PG_RT;
    roundRect(cx - w / 2, cy - h / 2 + r * 0.08f, w, h, h * (trig ? 0.25f : 0.5f), C(0, 0, 0, 90), C(0, 0, 0, 90));
    roundRect(cx - w / 2, cy - h / 2, w, h, h * (trig ? 0.25f : 0.5f), C(48, 50, 60, 240), C(28, 30, 36, 240));
    roundFrame(cx - w / 2, cy - h / 2, w, h, h * (trig ? 0.25f : 0.5f), std::max(1.f, r * 0.08f), C(200, 204, 214, 200));
    drawText(t, cx + px * 0.3f, cy - 3.5f * px, px, C(240, 240, 240), 1, false);
}
static float padGlyphWidth(int g, float r) {
    if (g <= PG_Y || g == PG_LS || g == PG_RS || g >= PG_DPAD) return r * 2.f;
    static const char* XB[6] = {"LB", "RB", "LT", "RT", "МЕНЮ", "ВИД"}, *PS[6] = {"L1", "R1", "L2", "R2", "OPTIONS", "SHARE"}, *NS[6] = {"L", "R", "ZL", "ZR", "+", "-"};
    const char* t = (g_padStyle2 == 1 ? PS : (g_padStyle2 == 2 ? NS : XB))[g - PG_LB];
    return std::max(r * 1.9f, textWidth(t, r * 0.15f) + r * 0.8f);
}
// ряд подсказок «кнопка — действие»; align: 0 — от x вправо, 2 — заканчивается на x
struct PadHint { int g; const char* text; };
static void drawPadHints(const PadHint* h, int n, float x, float y, float r, int align, bool vertical) {
    float px = fontPx() * 0.8f, gap = r * 0.5f;
    if (vertical) {
        for (int i = 0; i < n; i++) {
            float gw = padGlyphWidth(h[i].g, r), tw = textWidth(h[i].text, px);
            float rowW = gw + gap + tw, yy = y + i * r * 2.6f;
            float x0 = align == 2 ? x - rowW : x;
            roundRect(x0 - r * 0.4f, yy - r * 1.05f, rowW + r * 0.9f, r * 2.1f, r * 0.6f, C(10, 12, 18, 120), C(10, 12, 18, 140));
            drawPadGlyph(h[i].g, x0 + gw / 2, yy, r);
            drawText(h[i].text, x0 + gw + gap, yy - 3.5f * px, px, C(245, 245, 240), 0);
        }
        return;
    }
    float total = 0; for (int i = 0; i < n; i++) total += padGlyphWidth(h[i].g, r) + gap + textWidth(h[i].text, px) + r * 1.4f;
    float xx = align == 2 ? x - total : (align == 1 ? x - total / 2 : x);
    roundRect(xx - r * 0.6f, y - r * 1.1f, total + r * 0.4f, r * 2.2f, r * 0.6f, C(10, 12, 18, 140), C(10, 12, 18, 160));
    for (int i = 0; i < n; i++) {
        float gw = padGlyphWidth(h[i].g, r);
        drawPadGlyph(h[i].g, xx + gw / 2, y, r);
        drawText(h[i].text, xx + gw + gap, y - 3.5f * px, px, C(245, 245, 240), 0);
        xx += gw + gap + textWidth(h[i].text, px) + r * 1.4f;
    }
}
static void ringCircle(float cx, float cy, float r, float t, SDL_Color c) {
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
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
static bool g_glint = false;    // рисовать значок как переливающееся свечение зачарования
static Uint8 g_glintC[3] = {160, 80, 255};
static void drawTile(int tile, float x, float y, float w, float h, Uint8 a = 255) {
    SDL_Rect src = {(tile & 15) * g_tileRes, (tile >> 4) * g_tileRes, g_tileRes, g_tileRes};
    SDL_Rect dst = {(int)x, (int)y, (int)w, (int)h};
    if (g_glint) SDL_SetTextureColorMod(g_atlasTex, g_glintC[0], g_glintC[1], g_glintC[2]);
    else SDL_SetTextureColorMod(g_atlasTex, 255, 255, 255);
    SDL_SetTextureAlphaMod(g_atlasTex, a);
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
            if (g_glint) { vv.color.r = (Uint8)(c * g_glintC[0] / 255); vv.color.g = (Uint8)(c * g_glintC[1] / 255); vv.color.b = (Uint8)(c * g_glintC[2] / 255); }
        }
        int* p = idx + f * 6; int b = f * 4;
        p[0] = b; p[1] = b + 1; p[2] = b + 2; p[3] = b; p[4] = b + 2; p[5] = b + 3;
    }
    SDL_SetTextureColorMod(g_atlasTex, 255, 255, 255); SDL_SetTextureAlphaMod(g_atlasTex, 255);
    SDL_RenderGeometry(g_ren, g_atlasTex, v, 12, idx, 18);
}
static void drawGlintIcon(int id, float x, float y, float sz) {
    float t = SDL_GetTicks() * 0.004f, k = 0.6f + 0.4f * sinf(t + x * 0.01f);
    g_glintC[0] = (Uint8)(170 * k); g_glintC[1] = (Uint8)(90 * k); g_glintC[2] = (Uint8)(255 * k);
    SDL_SetTextureBlendMode(g_atlasTex, SDL_BLENDMODE_ADD);
    g_glint = true; drawItemIcon(id, x, y, sz); g_glint = false;
    SDL_SetTextureBlendMode(g_atlasTex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(g_atlasTex, 255, 255, 255);
}
static void drawStack(const Stack& s, float x, float y, float sz) {
    if (s.empty()) return;
    if (s.ench && g_glowTex) {   // мягкое фиолетовое сияние за зачарованным предметом
        float t = SDL_GetTicks() * 0.003f, k = 0.75f + 0.25f * sinf(t + x * 0.02f);
        SDL_Rect d = {(int)(x + sz * 0.02f), (int)(y + sz * 0.02f), (int)(sz * 0.96f), (int)(sz * 0.96f)};
        SDL_SetTextureColorMod(g_glowTex, 150, 70, 255); SDL_SetTextureAlphaMod(g_glowTex, (Uint8)(200 * k));
        SDL_RenderCopy(g_ren, g_glowTex, nullptr, &d);
        SDL_SetTextureColorMod(g_glowTex, 255, 255, 255); SDL_SetTextureAlphaMod(g_glowTex, 255);
    }
    drawItemIcon(s.id, x, y, sz);
    if (s.ench) drawGlintIcon(s.id, x, y, sz);
    float px = std::max(1.5f, sz / 22.f);
    if (s.cnt > 1) drawText(std::to_string(s.cnt), x + sz - px * 1.5f, y + sz - 8.5f * px, px, C(255, 255, 255), 2);
    if ((isTool(s.id) || isArmor(s.id) || s.id == I_ROD) && s.dmg > 0) {
        float f = 1.f - s.dmg / (float)maxDur(s.id);
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
    // нажатая кнопка на мгновение «проседает»
    static std::unordered_map<int, Uint32> pressAt;
    Uint32 now = SDL_GetTicks();
    if (hit && enabled) pressAt[id] = now;
    float pk = 0.f; { auto it = pressAt.find(id); if (it != pressAt.end() && now - it->second < 160) pk = 1.f - (now - it->second) / 160.f; }
    float in = pk * h * 0.04f; x += in; y += in; w -= in * 2; h -= in * 2;
    float rr = h * 0.2f;
    roundRect(x + h * 0.03f, y + h * 0.07f, w, h, rr, C(0, 0, 0, 70), C(0, 0, 0, 70));   // тень под кнопкой
    if (!enabled) roundRect(x, y, w, h, rr, C(64, 66, 70, 210), C(46, 48, 52, 210));
    else if (foc) roundRect(x, y, w, h, rr, C(120, 168, 88, 245), C(72, 116, 52, 245));
    else roundRect(x, y, w, h, rr, C((int)(92 - 30 * pk), (int)(106 - 30 * pk), (int)(86 - 30 * pk), 238), C((int)(58 - 20 * pk), (int)(66 - 20 * pk), (int)(54 - 20 * pk), 238));
    roundRect(x + w * 0.04f, y + h * 0.06f, w * 0.92f, h * 0.36f, rr * 0.8f, C(255, 255, 255, enabled ? 34 : 12), C(255, 255, 255, 0));   // глянец сверху
    roundFrame(x, y, w, h, rr, std::max(1.5f, h * 0.035f), foc ? C(255, 236, 140) : C(16, 18, 14, 230));
    float px = fontPx() * 1.25f * scaleMul;
    while (textWidth(label, px) > w * 0.9f && px > 1.5f) px *= 0.9f;
    drawText(label, x + w / 2, y + h / 2 - 3.5f * px, px, enabled ? C(250, 250, 240) : C(140, 140, 140), 1);
    if (hit && enabled) playSound(SND_CLICK, 0.5f);
    return hit && enabled;
}
static void panel(float x, float y, float w, float h) {
    float r = uiS() * 2.2f;
    for (int i = 3; i >= 1; i--) roundRect(x - i * uiS() * 0.5f, y - i * uiS() * 0.3f + uiS() * 0.8f, w + i * uiS(), h + i * uiS() * 0.6f, r + i * uiS() * 0.5f, C(0, 0, 0, 28), C(0, 0, 0, 28));   // мягкая тень
    // светлое окно: как бумага/эмаль, а не тёмная плашка
    roundRect(x, y, w, h, r, C(240, 242, 246, 248), C(208, 213, 222, 250));
    roundRect(x + uiS() * 0.4f, y + uiS() * 0.3f, w - uiS() * 0.8f, uiS() * 5.f, r * 0.8f, C(255, 255, 255, 120), C(255, 255, 255, 0));
    roundFrame(x, y, w, h, r, std::max(1.5f, uiS() * 0.3f), C(70, 74, 88, 240));
    roundFrame(x + uiS() * 0.3f, y + uiS() * 0.3f, w - uiS() * 0.6f, h - uiS() * 0.6f, r * 0.85f, std::max(1.f, uiS() * 0.15f), C(255, 255, 255, 200));
}
static void slotBg(float x, float y, float s, bool sel, bool foc) {
    float r = s * 0.14f;
    roundRect(x, y, s, s, r, C(126, 130, 140, 235), C(158, 162, 172, 235));   // углублённая серая ячейка: темнее сверху
    roundFrame(x, y, s, s, r, std::max(1.f, s * 0.035f), sel ? C(255, 255, 255, 255) : C(96, 100, 112, 230));
    if (sel) roundFrame(x - s * 0.04f, y - s * 0.04f, s * 1.08f, s * 1.08f, r * 1.2f, std::max(1.f, s * 0.03f), C(255, 255, 255, 90));
    if (foc) roundFrame(x - s * 0.06f, y - s * 0.06f, s * 1.12f, s * 1.12f, r * 1.3f, std::max(2.f, s * 0.06f), C(255, 236, 130));
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
    if (slot.id == g_cursor.id && slot.ench == g_cursor.ench && maxStack(slot.id) > 1) {
        int room = maxStack(slot.id) - slot.cnt;
        int mv = alt ? std::min(1, room) : std::min(room, (int)g_cursor.cnt);
        slot.cnt += mv; g_cursor.cnt -= mv; if (!g_cursor.cnt) g_cursor.clear();
        return;
    }
    std::swap(slot, g_cursor);
}
static void returnLooseItems() {
    for (int i = 0; i < 2; i++) if (!g_anvil[i].empty()) { int l = addItem(g_anvil[i]); if (l) { Stack s = g_anvil[i]; s.cnt = (u16)l; spawnDrop(s, P.x, P.y + 1.2, P.z); } g_anvil[i].clear(); }
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
    if (netClientOpenContainer(x, y, z, false)) return;   // гость: печь откроется, когда хозяин пришлёт её состояние
    g_contX = x; g_contY = y; g_contZ = z;
    if (!g_furnaces.count(posKey(x, y, z))) g_furnaces[posKey(x, y, z)] = Furnace();
    openInventory(INV_FURNACE);
}
static void openAnvil() { openInventory(INV_ANVIL); playSound(SND_ANVIL, 0.5f, 1.2f); }
// результат наковальни: зачарования с книги (или со второго такого же предмета) переносятся на левый предмет
static bool anvilResult(const Stack& a, const Stack& b, Stack& out, std::string& why, int* cost = nullptr) {
    out.clear(); why.clear();
    int price = 0;
    if (a.empty() || b.empty()) return false;
    bool bBook = b.id == I_EBOOK;
    bool same = a.id == b.id && (isTool(a.id) || isArmor(a.id) || a.id == I_ROD);
    if (!bBook && !same) { why = "СПРАВА НУЖНА ЗАЧАРОВАННАЯ КНИГА"; return false; }
    if (a.id == I_BOOK) { why = "ПРОСТУЮ КНИГУ НЕЛЬЗЯ ЗАЧАРОВАТЬ"; return false; }
    if (!(a.id == I_EBOOK || isTool(a.id) || isArmor(a.id) || a.id == I_ROD)) { why = "ЭТОТ ПРЕДМЕТ НЕЛЬЗЯ ЗАЧАРОВАТЬ"; return false; }
    out = a; out.cnt = 1;
    bool changed = false, anyFit = false, capped = false;
    for (int e = 0; e < EN_COUNT; e++) {
        int lb = enchLvl(b, e); if (!lb) continue;
        if (!enchFits(a.id, e)) continue;
        anyFit = true;
        int la = enchLvl(a, e);
        int nl = (la == lb) ? std::min(EN_MAX[e], la + 1) : std::max(la, lb);   // одинаковые уровни дают следующий
        if (la == lb && la >= EN_MAX[e]) capped = true;
        if (nl != la) { setEnch(out, e, nl); changed = true; price += nl; }
    }
    if (same) {   // два одинаковых предмета ещё и чинят друг друга
        int md = maxDur(a.id), keep = (md - a.dmg) + (md - b.dmg) + md * 12 / 100;
        int nd = std::max(0, md - keep);
        if (nd < a.dmg) { out.dmg = (u16)nd; changed = true; price += 2; }
    }
    if (cost) {   // цена в уровнях опыта: сумма новых уровней чар и починка, не больше 30
        for (int e = 0; e < EN_COUNT; e++) price += enchLvl(a, e) / 2;
        *cost = clampi(price, 1, 30);
    }
    if (!changed) { out.clear(); why = capped ? "УЖЕ МАКСИМАЛЬНЫЙ УРОВЕНЬ" : (anyFit ? "НИЧЕГО НЕ ИЗМЕНИТСЯ" : "ЭТО ЗАЧАРОВАНИЕ СЮДА НЕ ПОДХОДИТ"); return false; }
    return true;
}
// ячейка брони: принимает только подходящую часть
static void armorSlot(int piece, float x, float y, float sz) {
    int id = 1040 + piece;
    bool hit = uiHit(id, R(x, y, sz, sz));
    slotBg(x, y, sz, false, uiFocused(id));
    if (P.armor[piece].empty()) drawTile(T_ARMOR0 + piece, x + sz * 0.15f, y + sz * 0.15f, sz * 0.7f, sz * 0.7f, 50);   // силуэт-подсказка
    else drawStack(P.armor[piece], x + sz * 0.04f, y + sz * 0.04f, sz * 0.92f);
    if (!hit) return;
    if (g_cursor.empty()) {
        if (!P.armor[piece].empty()) { g_cursor = P.armor[piece]; P.armor[piece].clear(); playSound(SND_CLICK, 0.4f, 1.1f); showTipS(g_cursor, x + sz / 2, y); }
        else { g_tipText = std::string("СЮДА: ") + ARMOR_PIECE[piece]; g_tipX = x + sz / 2; g_tipY = y; g_tipUntil = SDL_GetTicks() + 1500; }
    } else if (isArmor(g_cursor.id) && armorPiece(g_cursor.id) == piece) {
        std::swap(g_cursor, P.armor[piece]); playSound(SND_DIG0 + SM_STONE, 0.6f, 1.4f); showTipS(P.armor[piece], x + sz / 2, y);
    } else showMsg(std::string("СЮДА МОЖНО ПОЛОЖИТЬ ТОЛЬКО ") + ARMOR_PIECE[piece], 1.6f);
}
static void openChest(int x, int y, int z) {
    if (netClientOpenContainer(x, y, z, true)) return;
    g_contX = x; g_contY = y; g_contZ = z;
    auto& v = g_chests[posKey(x, y, z)]; if (v.size() < 27) v.resize(27);
    openInventory(INV_CHEST);
    playSound(SND_DIG0 + SM_WOOD, 0.6f, 0.7f);
}
static void sleepInBed(int x, int y, int z) {
    P.spawnX = x; P.spawnY = y + 1; P.spawnZ = z;
    if (g_netRole == NET_CLIENT) { showMsg("ТОЧКА ВОЗРОЖДЕНИЯ ЗАДАНА. НОЧЬ ПРОПУСКАЕТ ХОЗЯИН МИРА", 3.f); return; }
    if (!isNight() && g_thunderK < 0.6f) { showMsg("ТОЧКА ВОЗРОЖДЕНИЯ ЗАДАНА. СПАТЬ МОЖНО ТОЛЬКО НОЧЬЮ ИЛИ В ГРОЗУ", 3.f); return; }
    for (const Mob& m : g_mobs) if (MD[m.type].hostile && fabs(m.x - P.x) < 8 && fabs(m.z - P.z) < 8 && fabs(m.y - P.y) < 5) { showMsg("НЕЛЬЗЯ СПАТЬ: РЯДОМ ЧУДОВИЩА"); return; }
    g_dayTime = 0.01f; g_dayCount++;
    if (g_weather != WX_CLEAR) { setWeather(WX_CLEAR, 400.f + (SDL_GetTicks() % 600)); g_rainK = 0; g_thunderK = 0; }   // проснулся — небо ясное
    {   // ручные кошки иногда приносят утром подарок
        Rng r(SDL_GetTicks() * 13);
        for (Mob& m : g_mobs) if (m.type == MOB_CAT && m.tame && fabs(m.x - P.x) < 16 && fabs(m.z - P.z) < 16 && r.range(100) < 70) {
            static const int GIFTS[5] = {I_STRING, I_FLUFF, I_RAWMEAT, I_BONE, I_FISH0};
            int g = GIFTS[r.range(5)];
            spawnDrop(mkStack(g, 1), x + 0.5, y + 1.0, z + 0.5);
            m.x = x + 0.5 + HDX[r.range(4)] * 1.2; m.z = z + 0.5 + HDZ[r.range(4)] * 1.2; m.y = y + 0.1; m.sit = true;
            showMsg("КОШКА ПРИНЕСЛА ПОДАРОК!", 3.f);
        }
    }
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
    if (isTool(out) || out == I_BOW || out == I_ARROW || isArmor(out) || out == I_ROD) return 1;
    if (isBlockItem(out)) return 2;
    return 3;
}
struct BookEntry { int out; int key; u16 ench; std::vector<int> rec; int state; };   // key — предмет вместе с зачарованием
static void buildBookEntries(std::vector<BookEntry>& list) {
    list.clear();
    for (int i = 0; i < (int)g_recipes.size(); i++) {
        const Recipe& r = g_recipes[i];
        if (g_bookTab != 0 && bookCategory(r.out) != g_bookTab) continue;
        BookEntry* e = nullptr;
        int key = r.out | ((int)r.ench << 16);
        for (auto& x : list) if (x.key == key) e = &x;
        if (!e) { BookEntry ne; ne.out = r.out; ne.key = key; ne.ench = r.ench; ne.state = 3; list.push_back(ne); e = &list.back(); }
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
// создать предмет по рецепту прямо в сумку (ингредиенты берутся из сумки)
static bool craftToBag(int ri) {
    const Recipe& r = g_recipes[ri];
    int gw = craftGridW();
    if (r.w > gw || r.h > gw) { showMsg("НУЖЕН ВЕРСТАК"); return false; }
    bool creative = g_mode == GM_CREATIVE;
    std::vector<std::pair<int, int>> need; recipeNeeds(r, need);
    if (!creative) for (auto& n : need) if (countItem(n.first) < n.second) return false;
    if (!creative) for (auto& n : need) removeItem(n.first, n.second);
    Stack out = mkStack(r.out, r.cnt); out.ench = r.ench;
    int left = addItem(out);
    if (left) { out.cnt = (u16)left; spawnDrop(out, P.x, P.y + 1.2, P.z); showMsg("СУМКА ПОЛНА - ПРЕДМЕТ НА ЗЕМЛЕ"); }
    notePickup(r.out, r.cnt);
    if (!left) showMsg("+" + std::to_string(r.cnt) + " " + itemName(r.out) + " - В СУМКЕ", 1.6f);
    playSound(SND_PICKUP, 0.55f, 1.15f);
    g_ghostRecipe = -1;
    return true;
}
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
    drawText("КНИГА РЕЦЕПТОВ", x, y, px, C(140, 86, 8));
    y += px * 10;
    // вкладки
    static const char* TAB_NAMES[4] = {"ВСЕ РЕЦЕПТЫ", "ИНСТРУМЕНТЫ И ОРУЖИЕ", "СТРОИТЕЛЬСТВО", "МАТЕРИАЛЫ"};
    const int tabIcon[4] = {-1, toolId(MT_IRON, TK_PICK), B_STONEBRICK, I_IRON};
    float tw = iw / 4.f, th = bs * 0.85f;
    for (int t = 0; t < 4; t++) {
        float tx = x + t * tw;
        bool selT = g_bookTab == t;
        if (uiHit(2950 + t, R(tx + 1, y, tw - 2, th))) { g_bookTab = t; g_bookPage = 0; playSound(SND_CLICK, 0.4f, 1.1f); }
        roundRect(tx + 1, y, tw - 2, th + (selT ? 3 : 0), th * 0.18f, selT ? C(255, 255, 255, 255) : C(176, 181, 192, 235), selT ? C(228, 232, 240, 255) : C(150, 155, 166, 235));
        roundFrame(tx + 1, y, tw - 2, th + (selT ? 3 : 0), th * 0.18f, 2, uiFocused(2950 + t) ? C(255, 200, 60) : (selT ? C(90, 96, 110) : C(110, 114, 126)));
        float is = th * 0.78f;
        if (tabIcon[t] < 0) drawTile(T_BOOK, tx + tw / 2 - is / 2, y + th / 2 - is / 2, is, is);
        else drawItemIcon(tabIcon[t], tx + tw / 2 - is / 2, y + th / 2 - is / 2, is);
    }
    y += th + S * 1.2f;
    drawText(TAB_NAMES[g_bookTab], x, y, px * 0.8f, C(84, 88, 100));
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
        if (li >= (int)list.size()) { roundRect(sx + 2, sy + 2, bs - 4, bs - 4, bs * 0.13f, C(20, 22, 26, 150), C(30, 32, 38, 150)); continue; }
        const BookEntry& e = list[li];
        int wid = e.ench ? 3300 + e.rec[0] : 3000 + e.out;
        bool hit = uiHit(wid, R(sx + 2, sy + 2, bs - 4, bs - 4));
        bool foc = uiFocused(wid);
        // зелёная — можно создать сейчас, красноватая — не хватает материалов, серая — нужен верстак
        SDL_Color bg = e.state == 0 ? C(150, 205, 140, 240) : (e.state == 1 ? C(214, 160, 156, 240) : C(170, 174, 184, 240));
        SDL_Color br = e.state == 0 ? C(60, 140, 60) : (e.state == 1 ? C(170, 70, 64) : C(110, 114, 126));
        { SDL_Color bg2 = bg; bg2.r = (Uint8)(bg.r * 0.82f); bg2.g = (Uint8)(bg.g * 0.82f); bg2.b = (Uint8)(bg.b * 0.82f);
          roundRect(sx + 2, sy + 2, bs - 4, bs - 4, bs * 0.13f, bg, bg2); }
        roundFrame(sx + 2, sy + 2, bs - 4, bs - 4, bs * 0.13f, std::max(1.f, bs * 0.04f), foc ? C(255, 240, 120) : (g_bookSel == e.key ? C(240, 240, 240) : br));
        drawItemIcon(e.out, sx + bs * 0.12f, sy + bs * 0.12f, bs * 0.76f);
        if (e.ench) drawGlintIcon(e.out, sx + bs * 0.12f, sy + bs * 0.12f, bs * 0.76f);
        const Recipe& r0 = g_recipes[e.rec[0]];
        if (r0.cnt > 1) drawText(std::to_string(r0.cnt), sx + bs - px * 1.5f, sy + bs - px * 9.5f, px * 0.9f, C(255, 255, 255), 2);
        if (e.state != 0) roundRect(sx + 2, sy + 2, bs - 4, bs - 4, bs * 0.13f, C(0, 0, 0, 90), C(0, 0, 0, 90));
        if (e.state == 2) {   // значок-«замок»: рецепт делается только на верстаке
            float bx = sx + bs * 0.58f, by = sy + bs * 0.04f, bw = bs * 0.38f;
            fillCircle(bx + bw / 2, by + bw / 2, bw * 0.56f, C(255, 255, 255, 235));
            ringCircle(bx + bw / 2, by + bw / 2, bw * 0.56f, std::max(1.f, bs * 0.025f), C(90, 96, 110, 230));
            drawItemIcon(B_CRAFT, bx + bw * 0.12f, by + bw * 0.12f, bw * 0.76f);
        }
        if (foc) focusEntry = e.key;
        if (hit) {
            g_bookSel = e.key;
            int pick = -1;
            for (int ri : e.rec) if (recipeState(g_recipes[ri]) == 0) { pick = ri; break; }
            if (pick < 0) for (int ri : e.rec) if (recipeState(g_recipes[ri]) == 1) { pick = ri; break; }
            if (pick < 0) { showMsg("ЭТОТ РЕЦЕПТ ДЕЛАЕТСЯ НА ВЕРСТАКЕ"); playSound(SND_CLICK, 0.4f, 0.6f); }
            else if (!craftToBag(pick)) placeRecipe(pick);   // хватает материалов — сразу в сумку, иначе подсказка в сетке
        }
    }
    y += bs * ROWS + S * 1.5f;
    // страницы
    float pbw = S * 10, pbh = S * 7;
    if (button(2961, x, y, pbw, pbh, "<", g_bookPage > 0)) g_bookPage--;
    drawText(std::to_string(g_bookPage + 1) + " / " + std::to_string(pages), x + iw / 2, y + pbh / 2 - 3.5f * px, px, C(44, 48, 58), 1);
    if (button(2962, x + iw - pbw, y, pbw, pbh, ">", g_bookPage < pages - 1)) g_bookPage++;
    y += pbh + S * 1.5f;
    // подсказка: название и состав
    int show = focusEntry >= 0 ? focusEntry : g_bookSel;
    if (list.empty()) {
        drawText(g_bookOnlyCan ? "ПОКА НИЧЕГО НЕЛЬЗЯ СДЕЛАТЬ" : "НЕТ РЕЦЕПТОВ", x, y, px * 0.8f, C(84, 88, 100));
    } else if (show >= 0) {
        const Recipe* rp = nullptr;
        for (const auto& e : list) if (e.key == show) rp = &g_recipes[e.rec[0]];
        if (rp) {
            Stack ss = mkStack(rp->out, 1); ss.ench = rp->ench;
            float npx = px * 0.95f; std::string nm = stackName(ss);
            while (textWidth(nm, npx) > iw && npx > 1.2f) npx *= 0.9f;
            drawText(nm, x, y, npx, C(130, 84, 10));
            std::vector<std::pair<int, int>> need; recipeNeeds(*rp, need);
            float lx = x, ly = y + px * 10, ipx = px * 0.75f;
            for (auto& n : need) while (textWidth(itemName(n.first) + " " + std::to_string(n.second), ipx) > iw && ipx > 1.1f) ipx *= 0.92f;
            for (auto& n : need) {
                std::string part = itemName(n.first) + " " + std::to_string(n.second);
                bool ok = g_mode == GM_CREATIVE || countItem(n.first) >= n.second;
                float tw2 = textWidth(part + ", ", ipx);
                if (lx + tw2 > x + iw && lx > x) { lx = x; ly += ipx * 9; }
                drawText(part, lx, ly, ipx, ok ? C(30, 120, 40) : C(170, 40, 30));
                lx += tw2;
            }
            if (recipeNeedsTable(*rp)) drawText(g_invMode == INV_CRAFT ? "СЕТКА 3X3" : "НУЖЕН ВЕРСТАК", x, ly + ipx * 10, ipx, g_invMode == INV_CRAFT ? C(30, 80, 160) : C(160, 90, 20));
        }
    } else {
        const char* hint[4] = {"НАЖМИ НА РЕЦЕПТ - ПОЯВИТСЯ ОПИСАНИЕ", "ЗЕЛЁНЫЙ - МОЖНО СДЕЛАТЬ СЕЙЧАС", "КРАСНЫЙ - НЕ ХВАТАЕТ МАТЕРИАЛОВ", "ЗНАЧОК ВЕРСТАКА - НУЖЕН ВЕРСТАК"};
        const SDL_Color hc[4] = {C(84, 88, 100), C(40, 120, 44), C(170, 60, 50), C(110, 114, 126)};
        float hp = px * 0.75f; for (int k = 0; k < 4; k++) while (textWidth(hint[k], hp) > iw && hp > 1.1f) hp *= 0.93f;
        for (int k = 0; k < 4; k++) drawText(hint[k], x, y + k * hp * 9, hp, hc[k]);
    }
}

// ---------------------------------------------------------------- экран инвентаря
// взятый предмет показывается в отдельной ячейке «В РУКЕ»; касание по ней возвращает его в сумку
static void drawHandSlot(float x, float y, float w, float sz) {
    if (g_cursor.empty()) return;
    float px = fontPx() * 0.8f;
    drawText("В РУКЕ:", x, y, px, C(140, 86, 8));
    float sy = y + px * 10;
    bool hit = uiHit(2020, R(x, sy, sz, sz));
    float pulse = 0.5f + 0.5f * sinf(SDL_GetTicks() * 0.008f);
    fillRect(x, sy, sz, sz, C(60, 50, 20, 230));
    frameRect(x - 2, sy - 2, sz + 4, sz + 4, std::max(2.f, sz * 0.06f), C(255, (int)(200 + 55 * pulse), 90));
    drawStack(g_cursor, x + sz * 0.04f, sy + sz * 0.04f, sz * 0.92f);
    std::string nm = stackName(g_cursor); float npx = px;
    float tw = w - sz - px * 3; while (textWidth(nm, npx) > tw && npx > 1.1f) npx *= 0.9f;
    drawText(nm, x + sz + px * 2, sy + sz / 2 - 3.5f * npx, npx, C(44, 48, 58));
    { std::string h = "НАЖМИ НА ЯЧЕЙКУ, ЧТОБЫ ПОЛОЖИТЬ"; float hp = px * 0.7f; while (textWidth(h, hp) > w && hp > 1.f) hp *= 0.92f;
      drawText(h, x, sy + sz + px * 2, hp, C(96, 100, 112)); }
    if (hit) {
        int l = addItem(g_cursor);
        if (l) { g_cursor.cnt = (u16)l; showMsg("СУМКА ПОЛНА"); } else { g_cursor.clear(); playSound(SND_CLICK, 0.4f, 1.1f); }
    }
}
static int slotWidget(int id, Stack& s, float x, float y, float sz, bool sel = false, bool canPlace = true) {
    bool alt = false;
    bool hit = uiHit(id, R(x, y, sz, sz), &alt);
    slotBg(x, y, sz, sel, uiFocused(id));
    drawStack(s, x + sz * 0.04f, y + sz * 0.04f, sz * 0.92f);
    if (hit) {
        int before = s.id;
        slotClick(s, alt || g_altMode, canPlace); playSound(SND_CLICK, 0.35f, 1.2f);
        if (!g_cursor.empty()) showTipS(g_cursor, x + sz / 2, y);
        else if (!s.empty()) showTipS(s, x + sz / 2, y);
        else showTip(before, x + sz / 2, y);
        return 1;
    }
    return 0;
}

static std::vector<int> g_creativeItems;
static void initCreativeItems() {
    for (int b = 1; b < B_COUNT; b++) if (b != B_FURNACE_LIT) g_creativeItems.push_back(b);
    for (int i = I_STICK; i <= I_ARROW; i++) g_creativeItems.push_back(i);
    g_creativeItems.push_back(I_ROD);
    for (int i = I_PORK; i <= I_SHEARS; i++) g_creativeItems.push_back(i);
    for (int i = I_FISH0; i < I_FFRY0 + 4; i++) g_creativeItems.push_back(i);
    for (int i = I_ARMOR0; i < I_ARMOR0 + 12; i++) g_creativeItems.push_back(i);
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
    drawHandSlot(bx, y0 + ph - pad - bh - S * 2 - sz - fontPx() * 18, bw, sz);

    // верхняя часть
    float topY = gy;
    if (creative && g_invMode == INV_PLAYER) {
        drawText("ВСЕ ПРЕДМЕТЫ - НАЖМИ, ЧТОБЫ ВЗЯТЬ", gx, topY, px, C(44, 48, 58));
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
        drawText("ПАНЕЛЬ: ВЫБЕРИ ЯЧЕЙКУ", gx, hy - px * 10, px, C(84, 88, 100));
        for (int i = 0; i < 9; i++) {
            float sx = gx + i * sz;
            if (uiHit(1000 + i, R(sx, hy, sz, sz))) { P.sel = i; playSound(SND_CLICK, 0.4f); showTip(P.inv[i].id, sx + sz / 2, hy); }
            slotBg(sx, hy, sz, i == P.sel, uiFocused(1000 + i));
            drawStack(P.inv[i], sx + sz * 0.04f, hy + sz * 0.04f, sz * 0.92f);
        }
        // название под курсором геймпада
        for (auto& w : g_ui.cur) if (w.id == g_ui.focus && g_ui.pad && w.id >= 4000) drawText(itemName(g_creativeItems[w.id - 4000]), gx, hy + sz + S, px, C(130, 84, 10));
        return;
    }
    if (g_invMode == INV_PLAYER || g_invMode == INV_CRAFT) {
        int n = g_invMode == INV_CRAFT ? 3 : 2;
        bool armorUI = g_invMode == INV_PLAYER && !creative;
        float cy0 = topY + px * 11;
        float cx0 = gx + sz * (n == 3 ? 1.5f : (armorUI ? 2.6f : 2.0f));
        drawText(n == 3 ? "ВЕРСТАК" : "СОЗДАНИЕ", armorUI ? cx0 : gx, topY, px, C(44, 48, 58));
        if (armorUI) {
            drawText("БРОНЯ", gx, topY, px, C(44, 48, 58));
            for (int pc = 0; pc < 4; pc++) armorSlot(pc, gx + (pc % 2) * sz, cy0 + (pc / 2) * sz, sz);
            int ap = armorPoints(), pl = protectionLevels();
            float tp = px * 0.75f;
            std::string a1 = "ЗАЩИТА " + std::to_string(ap * 4 + pl * 4 > 80 ? 80 : ap * 4 + pl * 4) + "%";
            drawText(a1, gx, cy0 + 2 * sz + tp * 3, tp, C(60, 72, 96));
        }
        // маленькая кнопка-книжка слева от сетки (в сумке игрока — справа от результата)
        if (bookAvail) {
            float kb = sz * 0.9f, kx = armorUI ? cx0 + sz * 5.05f : gx + sz * 0.15f, ky = cy0 + (n * sz - kb) / 2;
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
        drawText(">", ax + sz * 0.3f, ay - 3.5f * px * 2, px * 2, C(44, 48, 58));
        float ox = ax + sz * 1.3f, oy = ay - sz * 0.6f;
        bool hit = uiHit(1150, R(ox, oy, sz * 1.2f, sz * 1.2f));
        slotBg(ox, oy, sz * 1.2f, res != 0, uiFocused(1150));
        if (!res && g_ghostRecipe >= 0 && g_ghostRecipe < (int)g_recipes.size()) {
            drawItemIcon(g_recipes[g_ghostRecipe].out, ox + sz * 0.1f, oy + sz * 0.1f, sz);
            fillRect(ox + 2, oy + 2, sz * 1.2f - 4, sz * 1.2f - 4, C(30, 30, 36, 150));
        }
        if (res) {
            Stack rs = mkStack(res, cnt); rs.ench = g_matchEnch; drawStack(rs, ox + sz * 0.1f, oy + sz * 0.1f, sz);
            std::string nm = itemName(res); float npx = px * 0.9f, maxW = gx + gridW - (ox - sz * 1.2f);
            while (textWidth(nm, npx) > maxW && npx > 1.2f) npx *= 0.9f;
            drawText(nm, ox + sz * 0.6f, oy + sz * 1.35f, npx, C(130, 84, 10), 1);
        }
        if (hit && res) showTip(res, ox + sz * 0.6f, oy);
        if (hit && res) {
            // готовый предмет сразу ложится в сумку; если места нет — остаток в руку
            Stack out = mkStack(res, cnt); out.ench = g_matchEnch;
            int left = addItem(out);
            bool ok = left == 0;
            if (!ok && (g_cursor.empty() || (g_cursor.id == res && g_cursor.cnt + left <= maxStack(res)))) {
                if (g_cursor.empty()) g_cursor = mkStack(res, left); else g_cursor.cnt += left;
                ok = true;
            } else if (!ok && left < cnt) { Stack d = mkStack(res, left); spawnDrop(d, P.x, P.y + 1.2, P.z); ok = true; }
            if (ok) {
                for (int i = 0; i < n * n; i++) if (!g_craft[i].empty()) { g_craft[i].cnt--; if (!g_craft[i].cnt) g_craft[i].clear(); }
                playSound(SND_PICKUP, 0.5f, 1.2f);
                notePickup(res, cnt);
                if (left == 0) showMsg("+" + std::to_string(cnt) + " " + itemName(res) + " - В СУМКЕ", 1.6f);
                g_ghostRecipe = -1;
            } else showMsg("СУМКА ПОЛНА");
        }
    } else if (g_invMode == INV_FURNACE) {
        auto it = g_furnaces.find(posKey(g_contX, g_contY, g_contZ));
        if (it == g_furnaces.end() || (getBlock(g_contX, g_contY, g_contZ) != B_FURNACE && getBlock(g_contX, g_contY, g_contZ) != B_FURNACE_LIT)) { closeInventory(); return; }
        Furnace& f = it->second;
        drawText("ПЕЧЬ", gx, topY, px, C(44, 48, 58));
        float fx = gx + sz * 2.5f, fy = topY + px * 11;
        slotWidget(1200, f.in, fx, fy, sz);
        // пламя
        float fl = f.burnMax > 0 ? f.burn / f.burnMax : 0;
        drawTile(T_FLAME, fx + sz * 0.15f, fy + sz * 1.05f, sz * 0.7f, sz * 0.7f, (Uint8)(fl > 0 ? 255 : 60));
        slotWidget(1201, f.fuel, fx, fy + sz * 1.85f, sz);
        float ax = fx + sz * 1.4f, ay = fy + sz * 0.9f;
        fillRect(ax, ay, sz * 1.6f, sz * 0.28f, C(30, 30, 30, 220));
        fillRect(ax, ay, sz * 1.6f * (f.cook / SMELT_TIME), sz * 0.28f, C(250, 210, 90));
        {   // готовое из печи нажатием сразу уходит в сумку
            float ox = ax + sz * 1.9f, oy = fy + sz * 0.45f, os = sz * 1.1f;
            bool hit = uiHit(1202, R(ox, oy, os, os));
            slotBg(ox, oy, os, !f.out.empty(), uiFocused(1202));
            drawStack(f.out, ox + os * 0.04f, oy + os * 0.04f, os * 0.92f);
            if (hit && !f.out.empty()) {
                int id = f.out.id, got = f.out.cnt;
                int left = addItem(f.out);
                got -= left; f.out.cnt = (u16)left; if (!left) f.out.clear();
                if (got > 0) { notePickup(id, got); playSound(SND_PICKUP, 0.5f, 1.1f); showTip(id, ox + os / 2, oy); }
                else showMsg("СУМКА ПОЛНА");
            }
        }
        float hpx = px * 0.8f; std::string h1 = "СВЕРХУ - ЧТО ПЛАВИТЬ", h2 = "СНИЗУ - ТОПЛИВО: УГОЛЬ, ДЕРЕВО";
        while (textWidth(h2, hpx) > gx + gridW - ax && hpx > 1.1f) hpx *= 0.9f;
        drawText(h1, ax, fy + sz * 1.75f, hpx, C(84, 88, 100));
        drawText(h2, ax, fy + sz * 1.75f + hpx * 10, hpx, C(84, 88, 100));
    } else if (g_invMode == INV_ANVIL) {
        drawText("НАКОВАЛЬНЯ", gx, topY, px, C(44, 48, 58));
        float ay0 = topY + px * 13, lx = gx + sz * 0.3f, rx = gx + sz * 2.2f, ox = gx + sz * 4.6f;
        slotWidget(1400, g_anvil[0], lx, ay0, sz);
        drawText("+", gx + sz * 1.55f, ay0 + sz / 2 - 3.5f * px * 1.6f, px * 1.6f, C(44, 48, 58), 1);
        slotWidget(1401, g_anvil[1], rx, ay0, sz);
        drawText(">", gx + sz * 3.75f, ay0 + sz / 2 - 3.5f * px * 2, px * 2, C(44, 48, 58), 1);
        Stack res; std::string why; int cost = 0;
        bool ok = anvilResult(g_anvil[0], g_anvil[1], res, why, &cost);
        bool afford = g_mode == GM_CREATIVE || P.xpLevel >= cost;
        float os = sz * 1.2f, oy = ay0 - sz * 0.1f;
        bool hit = uiHit(1402, R(ox, oy, os, os));
        slotBg(ox, oy, os, ok, uiFocused(1402));
        if (ok) drawStack(res, ox + os * 0.04f, oy + os * 0.04f, os * 0.92f);
        float tx = gx + sz * 0.3f, ty = ay0 + sz * 1.35f, tp = px * 0.85f;
        std::string line = ok ? stackName(res) : (why.empty() ? "СЛЕВА - ПРЕДМЕТ, СПРАВА - ЗАЧАРОВАННАЯ КНИГА" : why);
        float maxW = gridW - sz * 0.4f; while (textWidth(line, tp) > maxW && tp > 1.1f) tp *= 0.92f;
        drawText(line, tx, ty, tp, ok ? C(110, 50, 170) : (why.empty() ? C(84, 88, 100) : C(170, 50, 40)));
        drawText("ДВЕ ОДИНАКОВЫЕ КНИГИ - УРОВЕНЬ ВЫШЕ", tx, ty + px * 10, px * 0.7f, C(96, 100, 112));
        if (ok && g_mode != GM_CREATIVE) {   // цена в уровнях опыта
            std::string pr = "ЦЕНА: " + std::to_string(cost) + " УР. ОПЫТА";
            drawText(pr, ox + os / 2, oy + os + px * 3, px * 0.8f, afford ? C(60, 140, 30) : C(190, 50, 40), 1);
        }
        if (hit && ok && !afford) { showMsg("НЕ ХВАТАЕТ ОПЫТА: НУЖНО " + std::to_string(cost) + " УР.", 2.f); playSound(SND_CLICK, 0.5f, 0.6f); }
        if (hit && ok && afford) {
            spendLevels(cost);
            int left = addItem(res);
            if (left) { if (g_cursor.empty()) g_cursor = res; else spawnDrop(res, P.x, P.y + 1.2, P.z); }
            g_anvil[0].clear();
            if (g_anvil[1].cnt > 1) g_anvil[1].cnt--; else g_anvil[1].clear();
            playSound(SND_ANVIL, 0.8f); playSound(SND_ENCH, 0.5f);
            showMsg(stackName(res) + " - В СУМКЕ", 2.2f);
        }
    } else if (g_invMode == INV_CHEST) {
        auto& v = g_chests[posKey(g_contX, g_contY, g_contZ)];
        if (v.size() < 27) v.resize(27);
        if (getBlock(g_contX, g_contY, g_contZ) != B_CHEST) { closeInventory(); return; }
        drawText("СУНДУК", gx, topY, px, C(44, 48, 58));
        float cy0 = topY + px * 11;
        for (int i = 0; i < 27; i++) slotWidget(1300 + i, v[i], gx + (i % 9) * sz, cy0 + (i / 9) * sz, sz);
    }
    // сумка игрока
    float iy = y0 + ph - pad - sz * 4.25f;
    drawText("СУМКА", gx, iy - px * 10, px, C(44, 48, 58));
    for (int i = 9; i < 36; i++) slotWidget(1000 + i, P.inv[i], gx + ((i - 9) % 9) * sz, iy + ((i - 9) / 9) * sz, sz);
    float hy = iy + sz * 3.25f;
    for (int i = 0; i < 9; i++) slotWidget(1000 + i, P.inv[i], gx + i * sz, hy, sz, i == P.sel);
    // подсказка: имя предмета под фокусом
    if (g_ui.pad) for (auto& w : g_ui.cur) if (w.id == g_ui.focus && w.id >= 1000 && w.id < 1036 && !P.inv[w.id - 1000].empty())
        drawText(itemName(P.inv[w.id - 1000].id), bx, y0 + ph - pad - S * 22, px * 0.9f, C(130, 84, 10));
    // название предмета: под фокусом геймпада или после нажатия
    if (g_ui.pad) {
        for (auto& w : g_ui.cur) if (w.id == g_ui.focus) {
            int id = 0;
            const Stack* st = nullptr;
            if (w.id >= 1000 && w.id < 1036) st = &P.inv[w.id - 1000];
            else if (w.id >= 1100 && w.id < 1109) st = &g_craft[w.id - 1100];
            else if (w.id >= 1040 && w.id < 1044) st = &P.armor[w.id - 1040];
            if (st && !st->empty()) id = st->id;
            if (id > 0 && (g_tipText != stackName(*st) || SDL_GetTicks() > g_tipUntil)) { g_tipText = stackName(*st); g_tipX = w.r.x + w.r.w / 2.f; g_tipY = (float)w.r.y; g_tipUntil = SDL_GetTicks() + 400; }
        }
    }
    if (!g_tipText.empty() && SDL_GetTicks() < g_tipUntil) {
        float tp = px * 1.0f; while (textWidth(g_tipText, tp) > g_W * 0.9f && tp > 1.1f) tp *= 0.92f;
        float tw = textWidth(g_tipText, tp), bh = tp * 11;
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
    fillCircle(b[0], b[1] + b[2] * 0.05f, b[2] * 1.02f, C(0, 0, 0, 50));
    fillCircle(b[0], b[1], b[2], pressed ? C(255, 255, 255, 110) : C(20, 22, 28, 100));
    fillCircle(b[0], b[1] - b[2] * 0.35f, b[2] * 0.55f, C(255, 255, 255, pressed ? 30 : 14));
    ringCircle(b[0], b[1], b[2], std::max(2.f, b[2] * 0.05f), C(255, 255, 255, pressed ? 220 : 150));
    float px = fontPx() * 0.95f;
    if (icon >= 0) drawTile(icon, b[0] - b[2] * 0.45f, b[1] - b[2] * 0.7f, b[2] * 0.9f, b[2] * 0.9f, 220);
    while (textWidth(label, px) > b[2] * 1.8f && px > 1.2f) px *= 0.9f;
    drawText(label, b[0], b[1] + (icon >= 0 ? b[2] * 0.3f : -3.5f * px), px, C(255, 255, 255, 230), 1);
}
// ---------------------------------------------------------------- опыт: светящиеся шарики, уровни
static int xpToNext(int lvl) { return lvl < 16 ? 2 * lvl + 7 : (lvl < 31 ? 5 * lvl - 38 : 9 * lvl - 158); }
static int xpOrbSize(int v) { return v >= 37 ? 3 : (v >= 17 ? 2 : (v >= 7 ? 1 : 0)); }
static void spawnXp(double x, double y, double z, int total) {
    if (total <= 0 || g_mode == GM_CREATIVE) return;
    if (g_dropSink > 0) { g_sinkXp += total; return; }
    static const int SPLIT[6] = {37, 17, 7, 3, 1, 0};
    Rng r(SDL_GetTicks() * 31 + (u32)(x * 7 + z * 13));
    while (total > 0 && g_orbs.size() < 300) {
        int v = 1; for (int i = 0; SPLIT[i]; i++) if (total >= SPLIT[i]) { v = SPLIT[i]; break; }
        total -= v;
        XpOrb o; o.x = x + r.sf() * 0.3; o.y = y + 0.2; o.z = z + r.sf() * 0.3;
        o.vx = r.sf() * 2.f; o.vy = 2.5f + r.f() * 2.f; o.vz = r.sf() * 2.f; o.age = 0; o.val = v; o.ph = r.f() * 6.28f;
        g_orbs.push_back(o);
    }
}
static void addXp(int v) {
    if (v <= 0) return;
    P.xpTotal += v;
    float pts = P.xpProg * xpToNext(P.xpLevel) + v;
    int lvlBefore = P.xpLevel;
    while (pts >= xpToNext(P.xpLevel)) { pts -= xpToNext(P.xpLevel); P.xpLevel++; }
    P.xpProg = pts / xpToNext(P.xpLevel);
    if (P.xpLevel > lvlBefore) { playSound(SND_LEVEL, 0.55f, 1.f); P.levelFlash = 1.f; padRumble(0.2f, 0.4f, 120); }
}
static bool spendLevels(int n) {
    if (g_mode == GM_CREATIVE) return true;
    if (P.xpLevel < n) return false;
    P.xpLevel -= n; return true;
}
static void updateOrbs(float dt) {
    for (size_t i = 0; i < g_orbs.size();) {
        XpOrb& o = g_orbs[i];
        o.age += dt; o.ph += dt * 3.f;
        double dx = P.x - o.x, dy = P.y + 0.8 - o.y, dz = P.z - o.z;
        double d = sqrt(dx * dx + dy * dy + dz * dz);
        bool pull = !P.dead && d < 7.5 && o.age > 0.4f;
        if (pull) {   // тянется к игроку тем сильнее, чем ближе
            float k = (float)(1.0 - d / 7.5); k *= k;
            o.vx += (float)(dx / d) * 30.f * k * dt; o.vy += (float)(dy / d) * 30.f * k * dt; o.vz += (float)(dz / d) * 30.f * k * dt;
        }
        o.vy -= (pull ? 6.f : 12.f) * dt;
        o.vx *= expf(-dt * 1.5f); o.vz *= expf(-dt * 1.5f);
        double nx = o.x + o.vx * dt, ny = o.y + o.vy * dt, nz = o.z + o.vz * dt;
        if (BD[getBlock(ifloor(nx), ifloor(ny - 0.1), ifloor(nz))].solid && !pull) { o.vy = fabsf(o.vy) * 0.3f; o.vx *= 0.6f; o.vz *= 0.6f; ny = o.y; }
        o.x = nx; o.y = ny; o.z = nz;
        if (!P.dead && d < 1.0 && o.age > 0.4f) {
            addXp(o.val);
            static Rng pr(99); playSound(SND_XP, 0.25f, 0.6f + pr.f() * 0.75f);
            g_orbs[i] = g_orbs.back(); g_orbs.pop_back(); continue;
        }
        if (o.age > 300.f || o.y < -10) { g_orbs[i] = g_orbs.back(); g_orbs.pop_back(); continue; }
        i++;
    }
}
static void emitOrbs() {
    const Camera& c = g_cam;
    for (const XpOrb& o : g_orbs) {
        float rx = (float)(o.x - c.x), ry = (float)(o.y - c.y) + sinf(o.ph * 0.8f) * 0.05f, rz = (float)(o.z - c.z);
        if (rx * rx + ry * ry + rz * rz > 40 * 40) continue;
        int sz = xpOrbSize(o.val);
        float s = 0.1f + sz * 0.035f;
        float W[4][3], UV[4][2], C[4][4];
        for (int k = 0; k < 4; k++) {
            float a = (k == 0 || k == 3) ? -s : s, b = (k < 2) ? s : -s;
            for (int j = 0; j < 3; j++) W[k][j] = (j == 0 ? rx : (j == 1 ? ry : rz)) + c.r[j] * a + c.u[j] * b;
        }
        tileUV(T_XP0 + sz, 0.5f, 0.5f, UV[0]); tileUV(T_XP0 + sz, 15.5f, 0.5f, UV[1]); tileUV(T_XP0 + sz, 15.5f, 15.5f, UV[2]); tileUV(T_XP0 + sz, 0.5f, 15.5f, UV[3]);
        float pulse = 0.85f + 0.15f * sinf(o.ph * 2.f);
        for (int k = 0; k < 4; k++) { C[k][0] = 1.f * pulse; C[k][1] = 1.f; C[k][2] = 0.7f * pulse; C[k][3] = 0.95f; }
        emitQuadRel(W, UV, C, -0.05f);
    }
}
// полоса опыта над ячейками и номер уровня
static void drawXpBar(float x, float y, float w, float h) {
    roundRect(x, y, w, h, h * 0.5f, C(10, 14, 8, 200), C(24, 30, 18, 200));
    float p = clampf(P.xpProg, 0, 1);
    if (p > 0.005f) roundRect(x + 1, y + 1, std::max(h, (w - 2) * p), h - 2, (h - 2) * 0.5f, C(178, 240, 80), C(96, 170, 30));
    roundFrame(x, y, w, h, h * 0.5f, std::max(1.f, h * 0.12f), C(0, 0, 0, 220));
    if (P.xpLevel > 0) {
        float px = fontPx() * (1.05f + P.levelFlash * 0.35f);
        std::string s = std::to_string(P.xpLevel);
        float ty = y - px * 4.f;
        for (int dx = -1; dx <= 1; dx++) for (int dy = -1; dy <= 1; dy++) if (dx || dy) drawText(s, x + w / 2 + dx * 1.5f, ty + dy * 1.5f, px, C(0, 0, 0, 230), 1, false);
        drawText(s, x + w / 2, ty, px, C(160, 255, 70), 1, false);
    }
}

static void drawHotbar() {
    const TouchLayout& t = g_tl;
    {   // общая подложка панели
        float pad = t.hotS * 0.06f;
        roundRect(t.hotX - pad, t.hotY - pad + t.hotS * 0.05f, t.hotS * 9 + pad * 2, t.hotS + pad * 2, t.hotS * 0.22f, C(0, 0, 0, 60), C(0, 0, 0, 60));
        roundRect(t.hotX - pad, t.hotY - pad, t.hotS * 9 + pad * 2, t.hotS + pad * 2, t.hotS * 0.2f, C(30, 32, 40, 150), C(14, 15, 20, 175));
        roundFrame(t.hotX - pad, t.hotY - pad, t.hotS * 9 + pad * 2, t.hotS + pad * 2, t.hotS * 0.2f, std::max(1.f, t.hotS * 0.025f), C(0, 0, 0, 200));
    }
    for (int i = 0; i < 9; i++) {
        float x = t.hotX + i * t.hotS, in = t.hotS * 0.05f;
        roundRect(x + in, t.hotY + in, t.hotS - in * 2, t.hotS - in * 2, t.hotS * 0.14f, C(10, 10, 14, 120), C(40, 42, 50, 110));
        roundFrame(x + in, t.hotY + in, t.hotS - in * 2, t.hotS - in * 2, t.hotS * 0.14f, std::max(1.f, t.hotS * 0.02f), C(140, 144, 156, 110));
        drawStack(P.inv[i], x + t.hotS * 0.1f, t.hotY + t.hotS * 0.1f, t.hotS * 0.8f);
    }
    {   // выбранная ячейка: мягкое свечение и светлая рамка
        static float selX = -1; float tx = t.hotX + P.sel * t.hotS;
        if (selX < 0 || fabsf(selX - tx) > t.hotS * 9) selX = tx;
        selX += (tx - selX) * std::min(1.f, g_frameDt * 18.f);   // рамка плавно переезжает
        float sx = selX, g = t.hotS * 0.12f;
        roundRect(sx - g, t.hotY - g, t.hotS + g * 2, t.hotS + g * 2, t.hotS * 0.3f, C(255, 255, 230, 40), C(255, 255, 230, 20));
        roundFrame(sx - t.hotS * 0.02f, t.hotY - t.hotS * 0.02f, t.hotS * 1.04f, t.hotS * 1.04f, t.hotS * 0.2f, std::max(2.f, t.hotS * 0.06f), C(255, 255, 255, 245));
    }
    {   // подобранные предметы
        float px = fontPx() * 0.95f;
        float baseY = t.hotY - (g_mode == GM_SURVIVAL ? t.hotS * 0.9f : 0) - px * 28;
        for (size_t i = 0; i < g_picks.size(); i++) {
            const PickNote& p = g_picks[g_picks.size() - 1 - i];
            int a = (int)(255 * std::min(1.f, p.t));
            std::string txt = "+" + std::to_string(p.cnt) + " " + itemName(p.id);
            float tw = textWidth(txt, px), is = px * 9;
            float x = t.hotX + t.hotS * 9 - tw - is - px * 4, y = baseY - i * px * 11;
            roundRect(x - px * 2, y - px * 1.5f, tw + is + px * 6, px * 10, px * 3, C(10, 12, 18, a * 2 / 3), C(0, 0, 0, a / 2));
            drawItemIcon(p.id, x, y - px, is);
            drawText(txt, x + is + px * 2, y, px, C(255, 255, 255, a));
        }
    }
    if (g_heldNameT > 0 && !P.inv[P.sel].empty()) {
        float px = fontPx();
        drawText(stackName(P.inv[P.sel]), g_W / 2.f, t.hotY - (g_mode == GM_SURVIVAL ? t.hotS * 0.9f : 0) - px * 14, px, C(255, 255, 255, (int)(255 * std::min(1.f, g_heldNameT))), 1);
    }
}
static void drawStatusBars() {
    if (g_mode != GM_SURVIVAL) return;
    const TouchLayout& t = g_tl;
    float is = t.hotS * 0.42f;
    float barH = std::max(4.f, t.hotS * 0.11f);
    drawXpBar(t.hotX, t.hotY - t.hotS * 0.1f - barH, t.hotS * 9, barH);
    float y = t.hotY - is - t.hotS * 0.16f - barH;
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
    int ap = armorPoints();
    if (ap > 0) for (int i = 0; i < 10; i++) {
        int v = ap - i * 2;
        drawTile(v >= 2 ? T_ARMHUD_F : (v >= 1 ? T_ARMHUD_H : T_ARMHUD_E), t.hotX + i * is * 0.95f, y - is, is, is);
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
    if (g_bob.active && g_bob.state == 3) {   // поклёвка: заметная подсказка у прицела
        float k = 0.6f + 0.4f * sinf(SDL_GetTicks() * 0.018f), p2 = fontPx() * 1.3f;
        drawText("КЛЮЁТ! ЖМИ!", cx + 1, cy - cs * 2.6f + 1, p2, C(0, 0, 0, 160), 1);
        drawText("КЛЮЁТ! ЖМИ!", cx, cy - cs * 2.6f, p2, C(255, (int)(200 * k + 40), 80, 255), 1);
    }
    drawStatusBars();
    drawHotbar();
    {   // название края при входе в новый биом
        static int lastB = -1, cand = -1; static float candT = 0, showT = 0; static std::string name;
        Chunk* pc = getChunk(ifloor(P.x) >> 4, ifloor(P.z) >> 4);
        if (pc && g_genVer >= 3) {
            int b = pc->biome[(ifloor(P.z) & 15) * 16 + (ifloor(P.x) & 15)];
            if (b != cand) { cand = b; candT = 0; }
            candT += g_frameDt;
            if (candT > 1.5f && cand != lastB) { if (lastB >= 0) { name = BIOME_NAME[cand]; showT = 3.5f; } lastB = cand; }
        }
        if (showT > 0) {
            showT -= g_frameDt;
            float a = std::min(1.f, std::min((3.5f - showT) / 0.4f, showT / 0.8f));
            float tp = fontPx() * 1.15f, ty = g_H * 0.13f;
            drawText(name, g_W / 2.f + 2, ty + 2, tp, C(0, 0, 0, (int)(120 * a)), 1, false);
            drawText(name, g_W / 2.f, ty, tp, C(255, 248, 225, (int)(235 * a)), 1, false);
        }
    }
    if (g_mode == GM_SURVIVAL && g_playTime < 70.f && !P.dead) {   // подсказки новичку в первые минуты нового мира
        static const char* H[5] = {"ДЖОЙСТИК СЛЕВА - ХОДИТЬ, ПАЛЕЦ СПРАВА - ОСМАТРИВАТЬСЯ", "ЗАЖМИ ПАЛЕЦ НА ДЕРЕВЕ, ЧТОБЫ ДОБЫТЬ БРЕВНО",
                                   "ОТКРОЙ СУМКУ КНОПКОЙ С ТРЕМЯ ТОЧКАМИ И СДЕЛАЙ ДОСКИ", "ИЗ 4 ДОСОК СДЕЛАЙ ВЕРСТАК - ОН ОТКРОЕТ НОВЫЕ РЕЦЕПТЫ",
                                   "К НОЧИ ПОСТРОЙ УКРЫТИЕ И ЗАЖГИ ФАКЕЛЫ"};
        int k = (int)((g_playTime - 2.f) / 13.f); float ph = (g_playTime - 2.f) - k * 13.f;
        if (g_playTime > 2.f && k >= 0 && k < 5 && ph < 10.f) {
            float a = std::min(1.f, std::min(ph / 0.5f, (10.f - ph) / 0.6f));
            float hp = fontPx() * 0.95f; std::string hs = H[k];
            while (textWidth(hs, hp) > g_W * 0.6f && hp > 1.2f) hp *= 0.93f;
            float tw = textWidth(hs, hp), bw = tw + hp * 10, bh = hp * 12, bx = (g_W - bw) / 2, by = insetTop() + uiS() * 2 - (1.f - a) * uiS() * 3;
            roundRect(bx, by, bw, bh, bh * 0.3f, C(30, 34, 46, (int)(215 * a)), C(18, 20, 28, (int)(225 * a)));
            roundFrame(bx, by, bw, bh, bh * 0.3f, std::max(1.f, uiS() * 0.2f), C(255, 214, 110, (int)(200 * a)));
            drawText(hs, g_W / 2.f, by + bh / 2 - 3.5f * hp, hp, C(255, 244, 210, (int)(255 * a)), 1);
        }
    }
    if (g_ui.pad && !g_touchUsed && !P.dead) {   // подсказки кнопок геймпада по ситуации
        PadHint h[6]; int n = 0;
        const Stack& hs = P.inv[P.sel];
        u8 tb = g_target.hit ? getBlock(g_target.x, g_target.y, g_target.z) : 0;
        bool openable = tb == B_CRAFT || tb == B_CHEST || tb == B_FURNACE || tb == B_FURNACE_LIT || tb == B_ANVIL || tb == B_BED;
        if (g_targetMob >= 0) h[n++] = {PG_RT, "УДАРИТЬ"};
        else if (g_target.hit) h[n++] = {PG_RT, "ЛОМАТЬ"};
        if (hs.id == I_ROD) h[n++] = {PG_LT, g_bob.active ? (g_bob.state == 3 ? "ПОДСЕЧЬ!" : "СМОТАТЬ") : "ЗАБРОСИТЬ"};
        else if (hs.id == I_BOW) h[n++] = {PG_LT, "НАТЯНУТЬ ЛУК"};
        else if (openable && g_target.hit) h[n++] = {PG_LT, tb == B_BED ? "ЛЕЧЬ СПАТЬ" : "ОТКРЫТЬ"};
        else if (tb == B_DOOR && g_target.hit) h[n++] = {PG_LT, "ОТКРЫТЬ ДВЕРЬ"};
        else if (foodValue(hs.id) > 0 && (P.hunger < 20 || g_mode == GM_CREATIVE)) h[n++] = {PG_LT, "ЕСТЬ"};
        else if (isArmor(hs.id)) h[n++] = {PG_LT, "НАДЕТЬ"};
        else if (!hs.empty() && isBlockItem(hs.id) && g_target.hit) h[n++] = {PG_LT, "ПОСТАВИТЬ"};
        h[n++] = {PG_Y, "СУМКА"};
        if (!hs.empty()) h[n++] = {PG_X, "ВЫБРОСИТЬ"};
        float r = uiS() * 2.4f;
        drawPadHints(h, n, (float)(g_W - insetX()) - uiS() * 2.5f, g_H * 0.36f, r, 2, true);
    }
    float px = fontPx();
    if (g_touchUsed || !g_ui.pad) {
        // кнопка инвентаря
        float rr = t.inv.h * 0.22f;
        roundRect((float)t.inv.x, (float)t.inv.y, (float)t.inv.w, (float)t.inv.h, rr, C(30, 32, 40, 160), C(14, 15, 20, 180));
        roundFrame((float)t.inv.x, (float)t.inv.y, (float)t.inv.w, (float)t.inv.h, rr, std::max(1.5f, t.inv.h * 0.03f), C(170, 174, 186, 170));
        for (int d = -1; d <= 1; d++) fillCircle(t.inv.x + t.inv.w / 2.f + d * t.inv.w * 0.22f, t.inv.y + t.inv.h / 2.f, t.inv.w * 0.075f, C(255, 255, 255, 235));
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
    {
        float pr = t.pause.h * 0.24f;
        roundRect((float)t.pause.x, (float)t.pause.y, (float)t.pause.w, (float)t.pause.h, pr, C(30, 32, 40, 150), C(14, 15, 20, 175));
        roundFrame((float)t.pause.x, (float)t.pause.y, (float)t.pause.w, (float)t.pause.h, pr, std::max(1.5f, t.pause.h * 0.03f), C(190, 194, 206, 170));
        roundRect(t.pause.x + t.pause.w * 0.3f, t.pause.y + t.pause.h * 0.27f, t.pause.w * 0.14f, t.pause.h * 0.46f, t.pause.w * 0.04f, C(255, 255, 255), C(225, 225, 230));
        roundRect(t.pause.x + t.pause.w * 0.56f, t.pause.y + t.pause.h * 0.27f, t.pause.w * 0.14f, t.pause.h * 0.46f, t.pause.w * 0.04f, C(255, 255, 255), C(225, 225, 230));
    }
    if (g_mode == GM_CREATIVE) {
        float fr = t.fly.h * 0.24f;
        roundRect((float)t.fly.x, (float)t.fly.y, (float)t.fly.w, (float)t.fly.h, fr, P.flying ? C(110, 160, 240, 190) : C(30, 32, 40, 150), P.flying ? C(60, 100, 190, 200) : C(14, 15, 20, 175));
        roundFrame((float)t.fly.x, (float)t.fly.y, (float)t.fly.w, (float)t.fly.h, fr, std::max(1.5f, t.fly.h * 0.03f), C(190, 194, 206, 170));
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
    // фон меню: живой пейзаж — небо, солнце, облака, слои гор и холмов с деревьями, плывущие с разной скоростью
    const float W = (float)g_W, H = (float)g_H;
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    auto vtx = [](float x, float y, SDL_Color c) { SDL_Vertex v; v.position.x = x; v.position.y = y; v.color = c; v.tex_coord.x = v.tex_coord.y = 0; return v; };
    {   // небо
        SDL_Vertex v[6] = {vtx(0, 0, C(52, 98, 178)), vtx(W, 0, C(52, 98, 178)), vtx(W, H * 0.62f, C(244, 196, 150)), vtx(0, H * 0.62f, C(244, 196, 150)),
                           vtx(W, H, C(250, 214, 170)), vtx(0, H, C(250, 214, 170))};
        int idx[12] = {0, 1, 2, 0, 2, 3, 3, 2, 4, 3, 4, 5};
        SDL_RenderGeometry(g_ren, NULL, v, 6, idx, 12);
    }
    {   // солнце с сиянием
        const float warm[3] = {1.f, 0.82f, 0.5f};
        float sx = W * 0.76f, sy = H * 0.44f;
        drawGlow(sx, sy, H * 0.55f, warm, 0.35f);
        fillCircle(sx, sy, H * 0.075f, C(255, 236, 190));
        fillCircle(sx, sy, H * 0.06f, C(255, 248, 225));
    }
    // облака: мягкие «подушки» из кругов
    for (int i = 0; i < 7; i++) {
        float sp = 6.f + (i % 3) * 3.f, span = W * 1.5f;
        float cx = fmodf(i * 0.37f * W + t * sp, span) - W * 0.25f, cy = H * (0.1f + 0.06f * (i % 4)), sc = H * (0.035f + 0.012f * (i % 3));
        for (int pass = 0; pass < 2; pass++) for (int k = 0; k < 6; k++) {   // сначала тень всех «подушек», потом сами — без видимых кругов внутри
            float ox = (k - 2.5f) * sc * 0.9f, oy = -sinf(k * 0.6f + 0.3f) * sc * 0.6f, r = sc * (0.8f + 0.35f * sinf(k * 1.3f + i));
            if (pass == 0) fillCircle(cx + ox, cy + oy + sc * 0.22f, r, C(206, 160, 156, 255));
            else fillCircle(cx + ox, cy + oy, r * 0.97f, C(255, 250, 244, 255));
        }
    }
    // слои гор: ступенчатые силуэты, дальние — светлее и голубее (воздушная перспектива)
    struct Lay { float bs, base, amp, speed; SDL_Color top, bot; bool snow; };
    const Lay L[3] = {{H * 0.022f, 0.52f, 0.5f, 3.f, C(150, 160, 196), C(178, 176, 196), true},
                      {H * 0.03f, 0.6f, 0.4f, 7.f, C(96, 116, 150), C(140, 140, 160), false},
                      {H * 0.04f, 0.68f, 0.3f, 13.f, C(58, 86, 96), C(92, 104, 112), false}};
    for (int li = 0; li < 3; li++) {
        const Lay& l = L[li];
        float scroll = t * l.speed;
        int c0 = ifloor(scroll / l.bs), n = (int)(W / l.bs) + 2;
        std::vector<SDL_Vertex> vb; std::vector<int> ib;
        for (int c = 0; c < n; c++) {
            int wc = c0 + c;
            float h = nDetail.n2(wc * 0.06f + li * 31.f, li * 7.f) * 0.7f + nDetail.n2(wc * 0.17f + li * 13.f, 3.f) * 0.3f;
            float top = H * (l.base - l.amp * h);
            top = floorf(top / l.bs) * l.bs;   // ступеньки по размеру блока
            float x = c * l.bs - fmodf(scroll, l.bs) - l.bs;
            int b = (int)vb.size();
            vb.push_back(vtx(x, top, l.top)); vb.push_back(vtx(x + l.bs + 1, top, l.top)); vb.push_back(vtx(x + l.bs + 1, H, l.bot)); vb.push_back(vtx(x, H, l.bot));
            int q[6] = {b, b + 1, b + 2, b, b + 2, b + 3}; ib.insert(ib.end(), q, q + 6);
            if (l.snow && h > 0.3f) {   // снежные шапки
                int bb = (int)vb.size(); SDL_Color sw = C(236, 238, 248);
                vb.push_back(vtx(x, top, sw)); vb.push_back(vtx(x + l.bs + 1, top, sw)); vb.push_back(vtx(x + l.bs + 1, top + l.bs, sw)); vb.push_back(vtx(x, top + l.bs, sw));
                int q2[6] = {bb, bb + 1, bb + 2, bb, bb + 2, bb + 3}; ib.insert(ib.end(), q2, q2 + 6);
            }
        }
        SDL_RenderGeometry(g_ren, NULL, vb.data(), (int)vb.size(), ib.data(), (int)ib.size());
    }
    // ближние холмы из настоящих блоков, с деревьями
    {
        float bs = H / 13.f, speed = 22.f, scroll = t * speed;
        int c0 = ifloor(scroll / bs), n = (int)(W / bs) + 3;
        for (int c = -1; c < n; c++) {
            int wc = c0 + c;
            float h = nDetail.n2(wc * 0.09f, 91.f) * 0.75f + nDetail.n2(wc * 0.23f, 17.f) * 0.25f;
            int topRow = clampi(9 + (int)lroundf(-h * 7.f), 7, 11);
            float x = c * bs - fmodf(scroll, bs);
            for (int r = topRow; r <= 13; r++) {
                int tile = r == topRow ? (int)T_GRASS_SIDE : (r < topRow + 3 ? (int)T_DIRT : ((hash3i(wc, r, 0, 9) % 11 == 0) ? (int)T_COAL_ORE + (int)(hash3i(wc, r, 1, 9) % 4) : (int)T_STONE));
                drawTile(tile, x, r * bs, bs + 1, bs + 1, 255);
            }
            if (hash3i(wc, 0, 7, 3) % 7 == 0) {   // дерево
                int th = 3 + (int)(hash3i(wc, 1, 7, 3) % 2);
                for (int k = 1; k <= th; k++) drawTile(T_LOG, x, (topRow - k) * bs, bs + 1, bs + 1, 255);
                for (int ly = 0; ly < 3; ly++) for (int lx = -1; lx <= 1; lx++) {
                    if (ly == 0 && lx != 0) continue;
                    drawTile(T_LEAVES, x + lx * bs, (topRow - th - 2 + ly) * bs, bs + 1, bs + 1, 255);
                }
            }
        }
    }
    {   // затемнение снизу (кнопки читаются лучше) и мягкая виньетка
        SDL_Vertex v[4] = {vtx(0, H * 0.5f, C(8, 10, 20, 0)), vtx(W, H * 0.5f, C(8, 10, 20, 0)), vtx(W, H, C(8, 10, 20, 150)), vtx(0, H, C(8, 10, 20, 150))};
        int idx[6] = {0, 1, 2, 0, 2, 3}; SDL_RenderGeometry(g_ren, NULL, v, 4, idx, 6);
        drawEdgeGlow(8, 10, 24, 0.45f);
    }
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
#ifndef __ANDROID__
static bool g_debugHost = false;   // отладка на ПК: сразу открыть мир для сети
#endif
static std::vector<std::pair<int, int>> g_offsets;
static SDL_GameController* g_pad = nullptr;          // активный геймпад (на котором нажимали последним)
static std::vector<SDL_GameController*> g_pads;       // все подключённые
static int g_padStyle = 0;                            // 0 — Xbox, 1 — PlayStation, 2 — Nintendo (подписи кнопок)
static float g_padStickNavT = 0;
static float g_padTurnBoost = 0;                      // ускорение поворота при стике «в упор»
static SDL_JoystickID padId(SDL_GameController* g) { return g ? SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g)) : -1; }
static int padStyleOf(SDL_GameController* g) {
#if SDL_VERSION_ATLEAST(2, 0, 12)
    switch (SDL_GameControllerGetType(g)) {
        case SDL_CONTROLLER_TYPE_PS3: case SDL_CONTROLLER_TYPE_PS4: return 1;
#if SDL_VERSION_ATLEAST(2, 0, 14)
        case SDL_CONTROLLER_TYPE_PS5: return 1;
#endif
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return 2;
        case SDL_CONTROLLER_TYPE_XBOX360: case SDL_CONTROLLER_TYPE_XBOXONE: return 0;
        default: break;   // неизвестный — угадываем по названию
    }
#endif
    const char* n = SDL_GameControllerName(g);
    std::string name = n ? n : "";
    for (auto& ch : name) ch = (char)toupper((unsigned char)ch);
    if (name.find("PS") != std::string::npos || name.find("DUALSENSE") != std::string::npos || name.find("DUALSHOCK") != std::string::npos || name.find("WIRELESS CONTROLLER") != std::string::npos) return 1;
    if (name.find("NINTENDO") != std::string::npos || name.find("SWITCH") != std::string::npos) return 2;
    return 0;
}
static void padRumble(float lo, float hi, int ms) {
    if (!g_pad || !g_set.rumble || !g_ui.pad) return;
#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_GameControllerRumble(g_pad, (Uint16)(clampf(lo, 0, 1) * 65535), (Uint16)(clampf(hi, 0, 1) * 65535), (Uint32)ms);
#else
    (void)lo; (void)hi; (void)ms;
#endif
}

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
        if (e->type == SDL_APP_TERMINATING) { if (g_netRole == NET_HOST) netHostStop(); else if (g_netRole == NET_CLIENT) saveMpProfile(); }
        if (g_worldMutex) SDL_UnlockMutex(g_worldMutex);
    }
    return 1;
}

static void createTextures();
// перестроить атлас под выбранную детализацию (с учётом предела видеокарты)
static void applyTextureRes() {
    int res = g_set.texRes;
    SDL_RendererInfo info;
    if (g_ren && SDL_GetRendererInfo(g_ren, &info) == 0 && info.max_texture_width > 0)
        while (res > 16 && (16 * res > info.max_texture_width || 32 * res > info.max_texture_height)) res /= 2;
    buildHDAtlas(res);
    createTextures();
}
static void createTextures() {
    if (g_atlasTex) SDL_DestroyTexture(g_atlasTex);
    if (g_fontTex) SDL_DestroyTexture(g_fontTex);
    if ((int)g_hdAtlas.size() != g_hdSize * g_hdH || g_hdAtlas.empty()) buildHDAtlas(g_tileRes);
    g_atlasTex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, g_hdSize, g_hdH);
    SDL_UpdateTexture(g_atlasTex, NULL, g_hdAtlas.data(), g_hdSize * 4);
    SDL_SetTextureBlendMode(g_atlasTex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(g_atlasTex, g_tileRes > 16 ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);   // детальные текстуры — сглаживать, пиксельные — нет
    g_animFrameW = g_animFrameL = -1;
    g_fontTex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, FONT_TEX_W, FONT_TEX_H);
    SDL_UpdateTexture(g_fontTex, NULL, g_fontPx.data(), FONT_TEX_W * 4);
    SDL_SetTextureBlendMode(g_fontTex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(g_fontTex, SDL_ScaleModeLinear);
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
    for (auto& kv : g_chunks) { Chunk* c = kv.second; if ((abs(c->cx - pcx) > rd + 3 || abs(c->cz - pcz) > rd + 3) && !(g_netRole == NET_HOST && nearRemoteChunk(c->cx, c->cz, 5))) far.push_back(c); }
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
        if (!getChunk(cx, cz)) {
            if (g_netRole == NET_CLIENT) { netRequestChunk(cx, cz); continue; }   // гость получает чанки от хозяина
            loadOrGenChunk(cx, cz); loaded++; if (nowMs() - t0 > budgetMs * 0.5) break;
        }
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
    g_genVer = GEN_VER_NEW;
    worldInitNoise(seed); g_mode = mode;
    int sx = 0, sz = 0;
    for (int i = 0; i < 4000; i++) {
        float a = i * 0.7f, r = i * 3.f;
        int x = (int)(cosf(a) * r), z = (int)(sinf(a) * r);
        ColInfo ci = columnInfo(x, z);
        if (ci.biome != BI_OCEAN && ci.biome != BI_BEACH && ci.biome != BI_MOUNT && ci.biome != BI_SWAMP && ci.h > g_sea + 2 && ci.h < g_sea + 42) { sx = x; sz = z; break; }
    }
    P.x = sx + 0.5; P.z = sz + 0.5; P.y = g_sea + 50; P.spawnX = sx; P.spawnZ = sz; P.spawnY = g_sea + 50;
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
// ---------------------------------------------------------------- сетевая игра: сокеты, хозяин, гость
#ifndef BW_NO_NET
static int netNonblock(int fd) { int f = fcntl(fd, F_GETFL, 0); return fcntl(fd, F_SETFL, f | O_NONBLOCK); }
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif
struct Conn {
    int fd = -1; std::vector<u8> in, out; size_t outOff = 0;
    bool alive = false, hello = false; int id = 0; std::string name, addr;
    double lastRecv = 0, connectT = 0; bool connecting = false;
    double mobT = 0, keepT = 0;
};
static std::vector<Conn*> g_conns;            // у хозяина: подключённые гости
static Conn g_srv;                            // у гостя: связь с хозяином
static int g_listenFd = -1, g_udpFd = -1, g_discFd = -1;
#ifndef BW_NO_NET
static double g_beaconT = 0, g_playersT = 0, g_posT = 0;
#endif
static double g_timeT = 0;
static std::unordered_map<i64, double> g_chunkReq;   // гость: запрошенные чанки -> время запроса
static std::string g_hostIp;
struct FoundGame { std::string ip, world, host; int port; double seen; };
static std::vector<FoundGame> g_found;
static std::string g_ipText;                  // адрес, набранный на экране сетевой игры
static u32 g_mpSeed = 0;                      // гость: зерно мира хозяина (для профиля)
static std::string g_mpWorld;
static bool g_welcomed = false;
static i64 g_pendChest = 0, g_pendFurn = 0; static bool g_pendChestOn = false, g_pendFurnOn = false;
static std::vector<Stack> g_lastChestSent; static Stack g_lastFurnSent[3]; static double g_furnPollT = 0;

static void netBegin(WBuf& b, u8 type) { b.d.clear(); b.p = 0; b.ver = SAVE_VER; b.w<u32>(0); b.w<u8>(type); }
static void netSend(Conn& c, WBuf& b) {
    if (!c.alive || c.fd < 0) return;
    u32 len = (u32)(b.d.size() - 4); memcpy(b.d.data(), &len, 4);
    c.out.insert(c.out.end(), b.d.begin(), b.d.end());
    if (c.out.size() - c.outOff > (size_t)48 * 1024 * 1024) c.alive = false;   // гость не успевает принимать
}
static void netBroadcast(WBuf& b, int exceptId = -1) { for (Conn* c : g_conns) if (c->hello && c->id != exceptId) netSend(*c, b); }
static void netCloseFd(int& fd) {
#ifndef BW_NO_NET
    if (fd >= 0) close(fd);
#endif
    fd = -1;
}
// пишем и читаем без блокировки; false — связь потеряна
static bool netPump(Conn& c) {
#ifdef BW_NO_NET
    (void)c; return false;
#else
    if (c.fd < 0) return false;
    while (c.outOff < c.out.size()) {
        ssize_t n = send(c.fd, c.out.data() + c.outOff, c.out.size() - c.outOff, MSG_NOSIGNAL);
        if (n > 0) c.outOff += (size_t)n;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
        else { SDL_Log("net: send failed errno=%d", errno); return false; }
    }
    if (c.outOff == c.out.size()) { c.out.clear(); c.outOff = 0; }
    else if (c.outOff > 1024 * 1024) { c.out.erase(c.out.begin(), c.out.begin() + c.outOff); c.outOff = 0; }
    u8 buf[65536];
    for (int k = 0; k < 64; k++) {
        ssize_t n = recv(c.fd, buf, sizeof(buf), 0);
        if (n > 0) { c.in.insert(c.in.end(), buf, buf + n); c.lastRecv = nowMs(); }
        else if (n == 0) { SDL_Log("net: peer closed"); return false; }
        else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
        else { SDL_Log("net: recv failed errno=%d", errno); return false; }
    }
    return true;
#endif
}
// достать следующее целое сообщение из входного буфера
static bool netNext(Conn& c, size_t& off, u8& type, WBuf& b) {
    if (c.in.size() - off < 4) return false;
    u32 len; memcpy(&len, c.in.data() + off, 4);
    if (len < 1 || len > 32u * 1024 * 1024) { SDL_Log("net: bad frame len %u", len); c.alive = false; return false; }
    if (c.in.size() - off < 4 + (size_t)len) return false;
    type = c.in[off + 4];
    b.d.assign(c.in.begin() + off + 5, c.in.begin() + off + 4 + len); b.p = 0; b.bad = false; b.ver = SAVE_VER;
    off += 4 + len;
    return true;
}
// собственный адрес в локальной сети (по маршруту «наружу»; пакеты при этом не отправляются)
static std::string localIp() {
#ifdef BW_NO_NET
    return "";
#else
    int s = socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) return "";
    sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons(53); inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    std::string res;
    if (connect(s, (sockaddr*)&a, sizeof(a)) == 0) {
        sockaddr_in me; socklen_t l = sizeof(me);
        if (getsockname(s, (sockaddr*)&me, &l) == 0) { char t[64]; inet_ntop(AF_INET, &me.sin_addr, t, sizeof(t)); res = t; }
    }
    close(s);
    if (res == "0.0.0.0") res.clear();
    return res;
#endif
}
#ifdef __ANDROID__
// без «замка мультикаста» многие телефоны не принимают широковещательные пакеты в Wi-Fi
static void androidMulticastLock(bool on) {
    static jobject lock = nullptr;
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv(); if (!env) return;
    if (on && !lock) {
        jobject act = (jobject)SDL_AndroidGetActivity(); if (!act) return;
        jclass ctxCls = env->FindClass("android/content/Context");
        jfieldID fid = ctxCls ? env->GetStaticFieldID(ctxCls, "WIFI_SERVICE", "Ljava/lang/String;") : nullptr;
        jobject wifiStr = fid ? env->GetStaticObjectField(ctxCls, fid) : nullptr;
        jclass actCls = env->GetObjectClass(act);
        jmethodID gac = env->GetMethodID(actCls, "getApplicationContext", "()Landroid/content/Context;");
        jobject app = gac ? env->CallObjectMethod(act, gac) : nullptr;
        jobject wm = nullptr;
        if (app && wifiStr) { jclass appCls = env->GetObjectClass(app); jmethodID gss = env->GetMethodID(appCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;"); if (gss) wm = env->CallObjectMethod(app, gss, wifiStr); }
        if (env->ExceptionCheck()) { env->ExceptionClear(); wm = nullptr; }
        if (wm) {
            jclass wmCls = env->GetObjectClass(wm);
            jmethodID cml = env->GetMethodID(wmCls, "createMulticastLock", "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;");
            jstring tag = env->NewStringUTF("buildworld");
            jobject l = cml ? env->CallObjectMethod(wm, cml, tag) : nullptr;
            if (env->ExceptionCheck()) { env->ExceptionClear(); l = nullptr; }
            if (l) {
                jclass lCls = env->GetObjectClass(l); jmethodID acq = env->GetMethodID(lCls, "acquire", "()V");
                if (acq) env->CallVoidMethod(l, acq);
                if (env->ExceptionCheck()) env->ExceptionClear(); else lock = env->NewGlobalRef(l);
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(act);
    } else if (!on && lock) {
        jclass lCls = env->GetObjectClass(lock); jmethodID rel = env->GetMethodID(lCls, "release", "()V");
        if (rel) env->CallVoidMethod(lock, rel);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteGlobalRef(lock); lock = nullptr;
    }
}
#else
static void androidMulticastLock(bool) {}
#endif
// постоянный номер игрока по имени (чтобы питомцы узнавали хозяина после переподключения)
static int idForName(const std::string& n) {
    int id = 1 + (int)(hash32((u32)std::hash<std::string>()(n)) % 250);
    for (int k = 0; k < 251; k++) {
        bool used = false; for (Conn* c : g_conns) if (c->hello && c->id == id) used = true;
        if (!used) return id;
        id = id % 250 + 1;
    }
    return id;
}
static void sendStack(WBuf& b, const Stack& s) { b.ws(s); }

// ---------- хозяин
static void netSendChunk(Conn& c, int cx, int cz) {
    WBuf b; netBegin(b, M_CHUNK); b.w<i32>(cx); b.w<i32>(cz);
    Chunk* ch = getChunk(cx, cz);
    std::vector<u8> data;
    if (ch && ch->modified) encodeChunk(ch, data);
    else if (!ch) { auto it = g_savedChunks.find(ckey(cx, cz)); if (it != g_savedChunks.end()) data = it->second; }
    b.w<u8>(data.empty() ? 0 : 1);
    if (!data.empty()) { b.w<u32>((u32)data.size()); b.put(data.data(), data.size()); }
    netSend(c, b);
}
static void netSendGive(int id, double x, double y, double z) {
    if (g_sinkItems.empty() && g_sinkXp <= 0) return;
    for (Conn* c : g_conns) if (c->hello && c->id == id) {
        WBuf b; netBegin(b, M_GIVE); b.w<i32>(g_sinkXp); b.w<double>(x); b.w<double>(y); b.w<double>(z);
        b.w<u16>((u16)g_sinkItems.size());
        for (const SinkItem& it : g_sinkItems) { sendStack(b, it.s); b.w<float>((float)it.x); b.w<float>((float)it.y); b.w<float>((float)it.z); }
        netSend(*c, b);
    }
    g_sinkItems.clear(); g_sinkXp = 0;
}
static void netHurtPlayer(int id, float dmg, double fx, double fz) {
    for (Conn* c : g_conns) if (c->hello && c->id == id) { WBuf b; netBegin(b, M_HURT); b.w<float>(dmg); b.w<double>(fx); b.w<double>(fz); netSend(*c, b); }
}
static void netOnBlock(int x, int y, int z, u8 b, u8 m) {   // вызывается из setBlockRaw
    if (g_netApplying && g_netRole == NET_CLIENT) return;
    WBuf w;
    if (g_netRole == NET_HOST) { netBegin(w, M_BLOCK); w.w<i32>(x); w.w<i32>(y); w.w<i32>(z); w.w<u8>(b); w.w<u8>(m); netBroadcast(w, g_netApplying ? g_netFrom : -1); }
    else if (g_netRole == NET_CLIENT && g_srv.alive) { netBegin(w, M_SETBLOCK); w.w<i32>(x); w.w<i32>(y); w.w<i32>(z); w.w<u8>(b); w.w<u8>(m); netSend(g_srv, w); }
}
static void netOnBolt(float x, float z, u32 seed) {
    if (g_netRole != NET_HOST) return;
    WBuf b; netBegin(b, M_BOLT); b.w<float>(x); b.w<float>(z); b.w<u32>(seed); netBroadcast(b);
}
static void netOnArrow(const Arrow& a, int exceptId) {
    if (g_netRole != NET_HOST) return;
    WBuf b; netBegin(b, M_ARROWFX); b.w<double>(a.x); b.w<double>(a.y); b.w<double>(a.z); b.w<float>(a.vx); b.w<float>(a.vy); b.w<float>(a.vz); b.w<u8>(a.crit ? 1 : 0);
    netBroadcast(b, exceptId);
}
static void netSendWelcome(Conn& c) {
    WBuf b; netBegin(b, M_WELCOME);
    b.w<u8>((u8)c.id); b.w<u32>(g_seed); b.w<u8>((u8)g_genVer); b.w<u8>((u8)g_mode); b.w<float>(g_dayTime);
    b.w<i32>(P.spawnX); b.w<i32>(P.spawnY); b.w<i32>(P.spawnZ); b.wstr(g_worldName); b.wstr(g_myName);
    netSend(c, b);
}
static void netSendJoin(Conn& to, int id, const std::string& name) { WBuf b; netBegin(b, M_JOIN); b.w<u8>((u8)id); b.wstr(name); netSend(to, b); }
static void hostDropRemote(Conn* c) {
    if (c->hello) {
        WBuf b; netBegin(b, M_LEAVE); b.w<u8>((u8)c->id); netBroadcast(b, c->id);
        showMsg(c->name + " ВЫШЕЛ ИЗ ИГРЫ", 2.5f);
        for (size_t i = 0; i < g_remote.size(); i++) if (g_remote[i].id == c->id) { g_remote.erase(g_remote.begin() + i); break; }
    }
    netCloseFd(c->fd); delete c;
}
static void hostHandle(Conn& c, u8 type, WBuf& b) {
    if (type != M_HELLO && !c.hello) return;
    switch (type) {
        case M_HELLO: {
            int proto = b.r<i32>(); std::string name = b.rstr();
            if (proto != NET_PROTO) { WBuf k; netBegin(k, M_KICK); k.wstr("НЕ СОВПАДАЕТ ВЕРСИЯ ИГРЫ"); netSend(c, k); c.alive = false; return; }
            if ((int)g_conns.size() > 8) { WBuf k; netBegin(k, M_KICK); k.wstr("СЕРВЕР ЗАПОЛНЕН"); netSend(c, k); c.alive = false; return; }
            if (name.empty()) name = "ГОСТЬ";
            c.name = name; c.id = idForName(name); c.hello = true;
            netSendWelcome(c);
            netSendJoin(c, 0, g_myName);
            for (Conn* o : g_conns) if (o != &c && o->hello) { netSendJoin(c, o->id, o->name); netSendJoin(*o, c.id, c.name); }
            RemotePlayer rp; rp.id = c.id; rp.name = name; rp.x = rp.tx = P.spawnX + 0.5; rp.y = rp.ty = P.spawnY; rp.z = rp.tz = P.spawnZ + 0.5;
            g_remote.push_back(rp);
            showMsg(name + " ВОШЁЛ В ИГРУ", 2.5f); playSound(SND_LEVEL, 0.4f, 1.3f);
            g_timeT = 0;
            break;
        }
        case M_POS: {
            RemotePlayer* rp = remoteById(c.id); if (!rp) break;
            rp->tx = b.r<double>(); rp->ty = b.r<double>(); rp->tz = b.r<double>(); rp->tyaw = b.r<float>(); rp->tpitch = b.r<float>();
            rp->held = b.r<u16>(); rp->flags = b.r<u8>(); rp->hp = b.r<float>();
            if (rp->flags & 1) rp->swing = 1.f;
            rp->lastPos = nowMs();
            break;
        }
        case M_CHUNKREQ: { int cx = b.r<i32>(), cz = b.r<i32>(); if (!b.bad) netSendChunk(c, cx, cz); break; }
        case M_SETBLOCK: {
            int x = b.r<i32>(), y = b.r<i32>(), z = b.r<i32>(); u8 nb = b.r<u8>(), nm = b.r<u8>();
            if (b.bad || y < 0 || y >= WH || nb >= B_COUNT) break;
            if (!getChunk(x >> 4, z >> 4)) loadOrGenChunk(x >> 4, z >> 4);
            u8 old = getBlock(x, y, z);
            i64 k = posKey(x, y, z);
            g_dropSink = c.id;
            if (old == B_CHEST && nb != B_CHEST) { auto it = g_chests.find(k); if (it != g_chests.end()) { for (auto& s : it->second) spawnDrop(s, x + 0.5, y + 0.5, z + 0.5); g_chests.erase(it); } }
            if ((old == B_FURNACE || old == B_FURNACE_LIT) && nb != B_FURNACE && nb != B_FURNACE_LIT) {
                auto it = g_furnaces.find(k); if (it != g_furnaces.end()) { spawnDrop(it->second.in, x, y, z); spawnDrop(it->second.fuel, x, y, z); spawnDrop(it->second.out, x, y, z); g_furnaces.erase(it); } }
            g_dropSink = 0;
            netSendGive(c.id, x + 0.5, y + 0.5, z + 0.5);
            if (nb == B_CHEST && !g_chests.count(k)) g_chests[k] = std::vector<Stack>(27);
            if (nb == B_FURNACE && !g_furnaces.count(k)) g_furnaces[k] = Furnace();
            if (nb == B_SAPLING) { Grow g; g.x = x; g.y = y; g.z = z; g.t = 40.f + (SDL_GetTicks() % 60); g_grow.push_back(g); }
            g_netApplying = true; g_netFrom = c.id;
            setBlock(x, y, z, nb, nm);
            g_netApplying = false; g_netFrom = -1;
            break;
        }
        case M_HITMOB: {
            u32 uid = b.r<u32>(); float dmg = clampf(b.r<float>(), 0.f, 60.f);
            RemotePlayer* rp = remoteById(c.id);
            Mob* m = mobByUid(uid);
            if (!m || !rp) break;
            g_dropSink = c.id;
            mobHurt(*m, dmg, rp->tx, rp->tz);
            g_dropSink = 0;
            netSendGive(c.id, m->x, m->y + 0.5, m->z);
            break;
        }
        case M_USEMOB: {
            u32 uid = b.r<u32>(); Stack held = b.rs();
            Mob* m = mobByUid(uid); if (!m) break;
            Stack before = held;
            g_dropSink = c.id;
            bool ok = tryInteractMobAs(*m, held, c.id);
            g_dropSink = 0;
            WBuf o; netBegin(o, M_USEOK); o.w<u8>(ok ? 1 : 0); o.w<u16>((u16)std::max(0, (int)before.cnt - (int)held.cnt)); o.w<u16>((u16)std::max(0, (int)held.dmg - (int)before.dmg));
            netSend(c, o);
            netSendGive(c.id, m->x, m->y + 0.8, m->z);
            break;
        }
        case M_CHESTREQ: case M_CHESTSET: {
            int x = b.r<i32>(), y = b.r<i32>(), z = b.r<i32>(); i64 k = posKey(x, y, z);
            if (getBlock(x, y, z) != B_CHEST) break;
            std::vector<Stack>& v = g_chests[k]; if (v.size() < 27) v.resize(27);
            if (type == M_CHESTSET) { for (int i = 0; i < 27; i++) v[i] = b.rs(); break; }
            WBuf o; netBegin(o, M_CHEST); o.w<i32>(x); o.w<i32>(y); o.w<i32>(z); for (int i = 0; i < 27; i++) o.ws(v[i]); netSend(c, o);
            break;
        }
        case M_FURNREQ: case M_FURNSET: {
            int x = b.r<i32>(), y = b.r<i32>(), z = b.r<i32>(); i64 k = posKey(x, y, z);
            u8 fb = getBlock(x, y, z); if (fb != B_FURNACE && fb != B_FURNACE_LIT) break;
            Furnace& f = g_furnaces[k];
            if (type == M_FURNSET) { f.in = b.rs(); f.fuel = b.rs(); f.out = b.rs(); }
            WBuf o; netBegin(o, M_FURN); o.w<i32>(x); o.w<i32>(y); o.w<i32>(z); o.ws(f.in); o.ws(f.fuel); o.ws(f.out); o.w<float>(f.burn); o.w<float>(f.burnMax); o.w<float>(f.cook);
            netSend(c, o);
            break;
        }
        case M_ARROW: {
            double x = b.r<double>(), y = b.r<double>(), z = b.r<double>(); float vx = b.r<float>(), vy = b.r<float>(), vz = b.r<float>(), dmg = b.r<float>(); bool crit = b.r<u8>() != 0;
            if (b.bad) break;
            spawnArrow(x, y, z, vx, vy, vz, clampf(dmg, 0, 20), true);
            g_arrows.back().crit = crit; g_arrows.back().shooter = c.id;
            netOnArrow(g_arrows.back(), c.id);
            break;
        }
        case M_BYE: c.alive = false; break;
        default: break;
    }
}
static void netHostStop();
static bool netHostStart() {
#ifdef BW_NO_NET
    g_netStatus = "СЕТЬ НЕДОСТУПНА НА ЭТОЙ СИСТЕМЕ"; return false;
#else
    if (g_netRole != NET_NONE) return false;
    g_listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listenFd < 0) { g_netStatus = "НЕ УДАЛОСЬ ОТКРЫТЬ СЕТЬ"; return false; }
    int one = 1; setsockopt(g_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons(NET_PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_listenFd, (sockaddr*)&a, sizeof(a)) != 0 || listen(g_listenFd, 8) != 0) { netCloseFd(g_listenFd); g_netStatus = "ПОРТ 41234 ЗАНЯТ"; showMsg(g_netStatus); return false; }
    netNonblock(g_listenFd);
    g_udpFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udpFd >= 0) { setsockopt(g_udpFd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)); netNonblock(g_udpFd); }
    g_netRole = NET_HOST; g_netRoleAny = g_netRole != NET_NONE; g_myId = 0;
    g_hostIp = localIp();
    androidMulticastLock(true);
    g_beaconT = 0;
    showMsg("МИР ОТКРЫТ ДЛЯ СЕТИ" + (g_hostIp.empty() ? std::string() : ": " + g_hostIp), 3.f);
    return true;
#endif
}
static void netHostStop() {
    if (g_netRole != NET_HOST) return;
    for (Conn* c : g_conns) { WBuf k; netBegin(k, M_KICK); k.wstr("ХОЗЯИН ЗАКРЫЛ МИР"); netSend(*c, k); netPump(*c); netCloseFd(c->fd); delete c; }
    g_conns.clear(); g_remote.clear();
    netCloseFd(g_listenFd); netCloseFd(g_udpFd);
    androidMulticastLock(false);
    g_netRole = NET_NONE; g_netRoleAny = g_netRole != NET_NONE;
}
static void netHostTick(float dt) {
#ifndef BW_NO_NET
    // новые гости
    for (int k = 0; k < 4; k++) {
        sockaddr_in a; socklen_t l = sizeof(a);
        int fd = accept(g_listenFd, (sockaddr*)&a, &l);
        if (fd < 0) break;
        netNonblock(fd); int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        Conn* c = new Conn(); c->fd = fd; c->alive = true; c->lastRecv = nowMs();
        char t[64]; inet_ntop(AF_INET, &a.sin_addr, t, sizeof(t)); c->addr = t;
        g_conns.push_back(c);
    }
    // приём и обработка
    for (size_t i = 0; i < g_conns.size();) {
        Conn* c = g_conns[i];
        bool ok = netPump(*c) && c->alive;
        size_t off = 0; u8 type; WBuf b;
        int budget = 400;
        while (ok && budget-- > 0 && netNext(*c, off, type, b)) hostHandle(*c, type, b);
        if (off) c->in.erase(c->in.begin(), c->in.begin() + off);
        if (nowMs() - c->lastRecv > 15000) ok = false;   // молчит 15 секунд
        if (!ok || !c->alive) {
            SDL_Log("net: host drops client %d (alive=%d, silent=%.0f ms)", c->id, (int)c->alive, nowMs() - c->lastRecv); netPump(*c); hostDropRemote(c); g_conns.erase(g_conns.begin() + i); continue; }
        i++;
    }
    // плавное движение гостей у хозяина
    for (RemotePlayer& r : g_remote) {
        float k = std::min(1.f, dt * 12.f);
        double ox = r.x, oz = r.z;
        r.x += (r.tx - r.x) * k; r.y += (r.ty - r.y) * k; r.z += (r.tz - r.z) * k;
        float dy = r.tyaw - r.yaw; while (dy > 3.1416f) dy -= 6.2832f; while (dy < -3.1416f) dy += 6.2832f;
        r.yaw += dy * k; r.pitch += (r.tpitch - r.pitch) * k;
        r.speed = (float)(sqrt((r.x - ox) * (r.x - ox) + (r.z - oz) * (r.z - oz)) / std::max(dt, 0.001f));
        r.walk += dt * std::min(r.speed, 6.f) * 2.2f; r.swing = std::max(0.f, r.swing - dt * 3.f);
    }
    // рассылки
    g_playersT -= dt; g_timeT -= dt; g_beaconT -= dt;
    if (g_playersT <= 0) {
        g_playersT = 0.066f;
        WBuf b; netBegin(b, M_PLAYERS);
        b.w<u8>((u8)(1 + g_remote.size()));
        auto put = [&](int id, double x, double y, double z, float yaw, float pitch, u16 held, u8 flags, float hp) {
            b.w<u8>((u8)id); b.w<double>(x); b.w<double>(y); b.w<double>(z); b.w<float>(yaw); b.w<float>(pitch); b.w<u16>(held); b.w<u8>(flags); b.w<float>(hp); };
        u8 myFlags = (P.swing > 0.6f ? 1 : 0) | (P.dead ? 4 : 0) | (P.flying ? 8 : 0);
        put(0, P.x, P.y, P.z, g_cam.yaw, g_cam.pitch, (u16)P.inv[P.sel].id, myFlags, P.health);
        for (const RemotePlayer& r : g_remote) put(r.id, r.tx, r.ty, r.tz, r.tyaw, r.tpitch, r.held, r.flags, r.hp);
        netBroadcast(b);
    }
    if (g_timeT <= 0) {
        g_timeT = 1.f;
        WBuf b; netBegin(b, M_TIME); b.w<float>(g_dayTime); b.w<u8>((u8)g_weather); b.w<float>(g_rainK); b.w<float>(g_thunderK); b.w<i32>(g_dayCount); netBroadcast(b);
    }
    for (Conn* c : g_conns) {
        if (!c->hello) continue;
        c->mobT -= dt;
        if (c->mobT > 0) continue;
        c->mobT = 0.1f;
        RemotePlayer* rp = remoteById(c->id); if (!rp) continue;
        WBuf b; netBegin(b, M_MOBS);
        std::vector<const Mob*> list;
        for (const Mob& m : g_mobs) { if (m.dead) continue; double dx = m.x - rp->tx, dz = m.z - rp->tz; if (dx * dx + dz * dz < 80.0 * 80.0) list.push_back(&m); }
        if (list.size() > 400) list.resize(400);
        b.w<u16>((u16)list.size());
        for (const Mob* m : list) {
            u8 fl = (m->tame ? 1 : 0) | (m->sit ? 2 : 0) | (m->sheared ? 4 : 0) | (m->age < 0 ? 8 : 0) | (m->hurtT > 0 ? 16 : 0) | (m->angryT > 0 ? 32 : 0) | (m->moving ? 64 : 0);
            b.w<u32>(m->uid ? m->uid : (u32)(0x80000000u | (u32)(m - g_mobs.data()))); b.w<u8>((u8)m->type); b.w<u8>((u8)m->var); b.w<u8>(fl); b.w<u8>((u8)m->owner);
            b.w<float>((float)m->x); b.w<float>((float)m->y); b.w<float>((float)m->z); b.w<float>(m->yaw); b.w<float>(m->headYaw); b.w<float>(m->walk);
            b.w<float>(m->hp); b.w<float>(m->eatT); b.w<float>(m->pitch); b.w<float>(m->phase);
        }
        netSend(*c, b);
    }
    // маяк для поиска в локальной сети
    if (g_beaconT <= 0 && g_udpFd >= 0) {
        g_beaconT = 1.5f;
        if (g_hostIp.empty()) g_hostIp = localIp();
        std::string msg = "BWLAN1|" + std::to_string(NET_PORT) + "|" + g_worldName + "|" + g_myName;
        sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons(DISC_PORT);
        a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        sendto(g_udpFd, msg.data(), msg.size(), 0, (sockaddr*)&a, sizeof(a));
        if (!g_hostIp.empty()) {   // и на широковещательный адрес своей подсети /24
            std::string bc = g_hostIp.substr(0, g_hostIp.rfind('.')) + ".255";
            if (inet_pton(AF_INET, bc.c_str(), &a.sin_addr) == 1) sendto(g_udpFd, msg.data(), msg.size(), 0, (sockaddr*)&a, sizeof(a));
        }
    }
    // держим загруженными чанки вокруг гостей (там живут мобы, текут жидкости)
    int made = 0;
    for (const RemotePlayer& r : g_remote) {
        int pcx = ifloor(r.tx) >> 4, pcz = ifloor(r.tz) >> 4;
        for (int dz = -3; dz <= 3 && made < 2; dz++) for (int dx = -3; dx <= 3 && made < 2; dx++)
            if (!getChunk(pcx + dx, pcz + dz)) { loadOrGenChunk(pcx + dx, pcz + dz); made++; }
    }
    for (Conn* c : g_conns) netPump(*c);
#else
    (void)dt;
#endif
}
static bool nearRemoteChunk(int cx, int cz, int r) {
    for (const RemotePlayer& p : g_remote) { int pcx = ifloor(p.tx) >> 4, pcz = ifloor(p.tz) >> 4; if (abs(cx - pcx) <= r && abs(cz - pcz) <= r) return true; }
    return false;
}

// ---------- гость
static void netRequestChunk(int cx, int cz) {
    if (!g_srv.alive || !g_welcomed) return;
    i64 k = ckey(cx, cz);
    auto it = g_chunkReq.find(k);
    double now = nowMs();
    if (it != g_chunkReq.end() && now - it->second < 4000) return;   // уже ждём ответа
    int pending = 0; for (auto& kv : g_chunkReq) if (now - kv.second < 4000) pending++;
    if (pending >= 24) return;
    g_chunkReq[k] = now;
    WBuf b; netBegin(b, M_CHUNKREQ); b.w<i32>(cx); b.w<i32>(cz); netSend(g_srv, b);
}
static void clientInsertChunk(int cx, int cz, const std::vector<u8>* data) {
    if (getChunk(cx, cz)) return;
    Chunk* c = new Chunk(); c->cx = cx; c->cz = cz;
    bool ok = false;
    if (data) { ok = decodeChunk(c, *data); if (ok) { memset(c->light, 0, sizeof(c->light)); for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) c->biome[z * 16 + x] = columnInfo(cx * 16 + x, cz * 16 + z).biome; } }
    if (!ok) generateChunk(c);
    c->modified = false;
    g_chunks[ckey(cx, cz)] = c;
    lodFromChunk(c);
    for (int dz = -1; dz <= 1; dz++) for (int dx = -1; dx <= 1; dx++) { Chunk* n = getChunk(cx + dx, cz + dz); if (n) n->dirty = true; }
}
static bool loadMpProfile();
static void clientHandle(u8 type, WBuf& b) {
    switch (type) {
        case M_WELCOME: {
            int myId = b.r<u8>(); u32 seed = b.r<u32>(); int gv = b.r<u8>(); int mode = b.r<u8>(); float dt = b.r<float>();
            int sx = b.r<i32>(), sy = b.r<i32>(), sz = b.r<i32>(); std::string wn = b.rstr(), hn = b.rstr();
            if (b.bad) { g_srv.alive = false; break; }
            resetWorldState();
            g_netRole = NET_CLIENT; g_netRoleAny = g_netRole != NET_NONE;   // resetWorldState не трогает сеть
            g_myId = myId; g_genVer = gv; worldInitNoise(seed); g_mode = mode; g_dayTime = dt; g_mpSeed = seed; g_mpWorld = wn;
            g_worldName = wn; g_worldSlot = 0;
            P.spawnX = sx; P.spawnY = sy; P.spawnZ = sz;
            P.x = sx + 0.5; P.y = sy + 0.01; P.z = sz + 0.5;
            if (!loadMpProfile()) { g_needSpawnY = true; if (g_mode == GM_CREATIVE) giveCreativeKit(); }
            else g_needSpawnY = false;
            g_welcomed = true; g_chunkReq.clear();
            g_screen = SCR_LOADING;
            RemotePlayer host; host.id = 0; host.name = hn; g_remote.clear(); g_remote.push_back(host);
            break;
        }
        case M_CHUNK: {
            int cx = b.r<i32>(), cz = b.r<i32>(); bool has = b.r<u8>() != 0;
            std::vector<u8> data;
            if (has) { u32 n = b.r<u32>(); if (b.p + n <= b.d.size()) { data.assign(b.d.begin() + b.p, b.d.begin() + b.p + n); b.p += n; } else has = false; }
            g_chunkReq.erase(ckey(cx, cz));
            clientInsertChunk(cx, cz, has ? &data : nullptr);
            break;
        }
        case M_BLOCK: {
            int x = b.r<i32>(), y = b.r<i32>(), z = b.r<i32>(); u8 nb = b.r<u8>(), nm = b.r<u8>();
            if (b.bad || nb >= B_COUNT || !isLoaded(x, z)) break;
            u8 old = getBlock(x, y, z);
            g_netApplying = true; setBlock(x, y, z, nb, nm); g_netApplying = false;
            double dx = x + 0.5 - P.x, dy = y + 0.5 - P.y, dz = z + 0.5 - P.z; float d = (float)sqrt(dx * dx + dy * dy + dz * dz);
            if (d < 20 && old != nb && BD[nb].shape != SH_FLUID && BD[old].shape != SH_FLUID) {   // слышно, как другие строят и копают
                if (nb == B_AIR) { playSound(SND_BREAK0 + BD[old].sound, 0.7f * (1.f - d / 20.f)); spawnBreakParticles(x, y, z, BD[old].tex[0], 8); }
                else playSound(SND_PLACE, 0.6f * (1.f - d / 20.f));
            }
            break;
        }
        case M_PLAYERS: {
            int n = b.r<u8>();
            for (int i = 0; i < n && !b.bad; i++) {
                int id = b.r<u8>(); double x = b.r<double>(), y = b.r<double>(), z = b.r<double>(); float yaw = b.r<float>(), pitch = b.r<float>(); u16 held = b.r<u16>(); u8 fl = b.r<u8>(); float hp = b.r<float>();
                if (id == g_myId) continue;
                RemotePlayer* r = remoteById(id);
                if (!r) { RemotePlayer np; np.id = id; np.name = "ИГРОК"; np.x = x; np.y = y; np.z = z; g_remote.push_back(np); r = &g_remote.back(); }
                r->tx = x; r->ty = y; r->tz = z; r->tyaw = yaw; r->tpitch = pitch; r->held = held; r->flags = fl; r->hp = hp;
                if (fl & 1) r->swing = 1.f;
            }
            break;
        }
        case M_JOIN: {
            int id = b.r<u8>(); std::string name = b.rstr();
            if (id == g_myId) break;
            RemotePlayer* r = remoteById(id);
            if (!r) { RemotePlayer np; np.id = id; g_remote.push_back(np); r = &g_remote.back(); }
            bool fresh = r->name.empty() || r->name == "ИГРОК";
            r->name = name;
            if (fresh && id != 0 && g_screen == SCR_GAME) showMsg(name + " ВОШЁЛ В ИГРУ", 2.5f);
            break;
        }
        case M_LEAVE: {
            int id = b.r<u8>();
            for (size_t i = 0; i < g_remote.size(); i++) if (g_remote[i].id == id) { showMsg(g_remote[i].name + " ВЫШЕЛ ИЗ ИГРЫ", 2.5f); g_remote.erase(g_remote.begin() + i); break; }
            break;
        }
        case M_MOBS: {
            int n = b.r<u16>();
            std::unordered_map<u32, size_t> idx;
            for (size_t i = 0; i < g_mobs.size(); i++) idx[g_mobs[i].uid] = i;
            std::vector<char> seen(g_mobs.size(), 0);
            for (int i = 0; i < n && !b.bad; i++) {
                u32 uid = b.r<u32>(); int type = b.r<u8>(), var = b.r<u8>(); u8 fl = b.r<u8>(); int owner = b.r<u8>();
                float x = b.r<float>(), y = b.r<float>(), z = b.r<float>(), yaw = b.r<float>(), hy = b.r<float>(), walk = b.r<float>(), hp = b.r<float>(), eat = b.r<float>(), pitch = b.r<float>(), phase = b.r<float>();
                if (b.bad || type >= MOB_COUNT) break;
                auto it = idx.find(uid);
                Mob* m;
                if (it == idx.end()) { Mob nm; nm.uid = uid; nm.type = type; nm.x = x; nm.y = y; nm.z = z; nm.yaw = yaw; g_mobs.push_back(nm); seen.push_back(1); m = &g_mobs.back(); idx[uid] = g_mobs.size() - 1; }
                else { m = &g_mobs[it->second]; seen[it->second] = 1; }
                if (hp < m->hp - 0.01f && m->hp > 0) { double dx = m->x - P.x, dz = m->z - P.z; float d = (float)sqrt(dx * dx + dz * dz); if (d < 24) playSound(SND_HIT, 0.6f * (1.f - d / 24.f)); }
                m->type = type; m->var = var; m->tame = fl & 1; m->sit = (fl & 2) != 0; m->sheared = (fl & 4) != 0; m->age = (fl & 8) ? -1.f : 0.f;
                m->hurtT = (fl & 16) ? 0.3f : m->hurtT; m->angryT = (fl & 32) ? 1.f : 0.f; m->moving = (fl & 64) != 0; m->owner = owner;
                m->tx = x; m->ty = y; m->tz = z; m->wishYaw = yaw; m->headYaw = hy; m->walk = walk; m->hp = hp; m->eatT = eat; m->pitch = pitch; m->phase = phase;
            }
            for (size_t i = seen.size(); i-- > 0;) if (!seen[i]) { g_mobs[i] = g_mobs.back(); g_mobs.pop_back(); }
            break;
        }
        case M_TIME: {
            float dt = b.r<float>(); int w = b.r<u8>(); float rk = b.r<float>(), tk = b.r<float>();
            if (b.bad) break;
            float diff = dt - g_dayTime; if (diff > 0.5f) diff -= 1.f; if (diff < -0.5f) diff += 1.f;
            if (fabsf(diff) > 0.002f) g_dayTime = dt;
            g_weather = w; g_rainK += (rk - g_rainK) * 0.5f; g_thunderK += (tk - g_thunderK) * 0.5f;
            if (b.p + 4 <= b.d.size()) { int dc = b.r<i32>(); if (!b.bad) g_dayCount = std::max(0, dc); }
            break;
        }
        case M_HURT: { float d = b.r<float>(); double fx = b.r<double>(), fz = b.r<double>(); if (!b.bad && g_mode == GM_SURVIVAL) playerHurt(d, fx, fz); break; }
        case M_BOLT: { float x = b.r<float>(), z = b.r<float>(); b.r<u32>(); if (!b.bad) lightningStrike(x, z, false); break; }
        case M_GIVE: {
            int xp = b.r<i32>(); double x = b.r<double>(), y = b.r<double>(), z = b.r<double>(); int n = b.r<u16>();
            for (int i = 0; i < n && !b.bad; i++) { Stack s = b.rs(); float sx = b.r<float>(), sy = b.r<float>(), sz = b.r<float>(); if (!b.bad) spawnDrop(s, sx, sy, sz); }
            if (!b.bad && xp > 0) spawnXp(x, y, z, xp);
            break;
        }
        case M_CHEST: {
            int x = b.r<i32>(), y = b.r<i32>(), z = b.r<i32>(); std::vector<Stack> v(27); for (int i = 0; i < 27; i++) v[i] = b.rs();
            if (b.bad) break;
            i64 k = posKey(x, y, z); g_chests[k] = v; g_lastChestSent = v;
            if (g_pendChestOn && g_pendChest == k) { g_pendChestOn = false; g_netApplying = true; openChest(x, y, z); g_netApplying = false; }
            break;
        }
        case M_FURN: {
            int x = b.r<i32>(), y = b.r<i32>(), z = b.r<i32>(); Furnace f; f.in = b.rs(); f.fuel = b.rs(); f.out = b.rs(); f.burn = b.r<float>(); f.burnMax = b.r<float>(); f.cook = b.r<float>();
            if (b.bad) break;
            i64 k = posKey(x, y, z);
            g_furnaces[k] = f; g_lastFurnSent[0] = f.in; g_lastFurnSent[1] = f.fuel; g_lastFurnSent[2] = f.out;
            if (g_pendFurnOn && g_pendFurn == k) { g_pendFurnOn = false; g_netApplying = true; openFurnace(x, y, z); g_netApplying = false; }
            break;
        }
        case M_ARROWFX: {
            double x = b.r<double>(), y = b.r<double>(), z = b.r<double>(); float vx = b.r<float>(), vy = b.r<float>(), vz = b.r<float>(); bool crit = b.r<u8>() != 0;
            if (b.bad) break;
            spawnArrow(x, y, z, vx, vy, vz, 0, false); g_arrows.back().cosmetic = true; g_arrows.back().crit = crit;
            break;
        }
        case M_USEOK: {
            bool ok = b.r<u8>() != 0; int used = b.r<u16>(), wear = b.r<u16>();
            if (b.bad) break;
            Stack& h = heldStack();
            if (ok) P.swing = 1.f;
            if (g_mode == GM_SURVIVAL && used > 0 && !h.empty()) { h.cnt = (u16)std::max(0, (int)h.cnt - used); if (!h.cnt) h.clear(); }
            if (wear > 0 && !h.empty()) damageTool(h, wear);
            break;
        }
        case M_KICK: { std::string why = b.rstr(); g_netStatus = why; g_srv.alive = false; break; }
        default: break;
    }
}
static bool netConnect(const std::string& ip) {
#ifdef BW_NO_NET
    (void)ip; g_netStatus = "СЕТЬ НЕДОСТУПНА"; return false;
#else
    if (g_netRole != NET_NONE) return false;
    sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons(NET_PORT);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) { g_netStatus = "НЕВЕРНЫЙ АДРЕС"; return false; }
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) { g_netStatus = "ОШИБКА СЕТИ"; return false; }
    netNonblock(fd); int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int r = connect(fd, (sockaddr*)&a, sizeof(a));
    if (r != 0 && errno != EINPROGRESS) { close(fd); g_netStatus = "НЕ УДАЛОСЬ ПОДКЛЮЧИТЬСЯ"; return false; }
    g_srv = Conn(); g_srv.fd = fd; g_srv.alive = true; g_srv.connecting = true; g_srv.connectT = nowMs(); g_srv.lastRecv = nowMs(); g_srv.addr = ip;
    g_netRole = NET_CLIENT; g_netRoleAny = g_netRole != NET_NONE; g_welcomed = false; g_remote.clear();
    g_netStatus = "ПОДКЛЮЧЕНИЕ К " + ip + "...";
    return true;
#endif
}
static void saveMpProfile();
static void netClientStop(const std::string& why) {
    if (g_netRole != NET_CLIENT) return;
    if (g_welcomed) saveMpProfile();
    if (g_srv.alive && !g_srv.connecting) { WBuf b; netBegin(b, M_BYE); netSend(g_srv, b); netPump(g_srv); }
    netCloseFd(g_srv.fd); g_srv = Conn();
    g_netRole = NET_NONE; g_netRoleAny = g_netRole != NET_NONE; g_welcomed = false; g_remote.clear(); g_chunkReq.clear();
    resetWorldState();
    g_netStatus = why;
    g_screen = SCR_MP; g_ui.focus = -1;
}
static void netClientTick(float dt) {
#ifndef BW_NO_NET
    if (g_srv.connecting) {
        pollfd pf; pf.fd = g_srv.fd; pf.events = POLLOUT; pf.revents = 0;
        if (poll(&pf, 1, 0) > 0) {
            int err = 0; socklen_t l = sizeof(err); getsockopt(g_srv.fd, SOL_SOCKET, SO_ERROR, &err, &l);
            if (err) { netClientStop("НЕ УДАЛОСЬ ПОДКЛЮЧИТЬСЯ"); return; }
            g_srv.connecting = false;
            WBuf b; netBegin(b, M_HELLO); b.w<i32>(NET_PROTO); b.wstr(g_myName); netSend(g_srv, b);
            g_netStatus = "ВХОД В МИР...";
        } else if (nowMs() - g_srv.connectT > 7000) { netClientStop("ХОЗЯИН НЕ ОТВЕЧАЕТ"); return; }
        return;
    }
    bool ok = netPump(g_srv) && g_srv.alive;
    size_t off = 0; u8 type; WBuf b; int budget = 600;
    while (ok && budget-- > 0 && netNext(g_srv, off, type, b)) { clientHandle(type, b); if (g_netRole != NET_CLIENT) return; ok = g_srv.alive; }
    if (off && g_netRole == NET_CLIENT) g_srv.in.erase(g_srv.in.begin(), g_srv.in.begin() + off);
    if (nowMs() - g_srv.lastRecv > 15000) ok = false;
    if (!ok) {
        SDL_Log("net: client lost host (alive=%d, silent=%.0f ms, status=%s)", (int)g_srv.alive, nowMs() - g_srv.lastRecv, g_netStatus.c_str()); netClientStop(g_netStatus.empty() || g_netStatus.find("ВХОД") != std::string::npos ? "СВЯЗЬ С ХОЗЯИНОМ ПОТЕРЯНА" : g_netStatus); return; }
    if (!g_welcomed) return;
    // своё положение
    g_posT -= dt;
    if (g_posT <= 0) {
        g_posT = 0.05f;
        WBuf w; netBegin(w, M_POS);
        u8 fl = (P.swing > 0.6f ? 1 : 0) | (P.dead ? 4 : 0) | (P.flying ? 8 : 0);
        w.w<double>(P.x); w.w<double>(P.y); w.w<double>(P.z); w.w<float>(g_cam.yaw); w.w<float>(g_cam.pitch); w.w<u16>((u16)P.inv[P.sel].id); w.w<u8>(fl); w.w<float>(P.health);
        netSend(g_srv, w);
    }
    // плавное движение мобов и других игроков
    float k = std::min(1.f, dt * 10.f);
    for (Mob& m : g_mobs) {
        if (m.tx == 0 && m.tz == 0) continue;
        m.x += (m.tx - m.x) * k; m.y += (m.ty - m.y) * k; m.z += (m.tz - m.z) * k;
        float dy = m.wishYaw - m.yaw; while (dy > 3.1416f) dy -= 6.2832f; while (dy < -3.1416f) dy += 6.2832f; m.yaw += dy * k;
        m.hurtT = std::max(0.f, m.hurtT - dt);
        m.sndT -= dt;
        if (m.sndT <= 0) { m.sndT = 8.f + (m.uid % 7); double dx = m.x - P.x, dz = m.z - P.z; float d = (float)sqrt(dx * dx + dz * dz); if (d < 20 && m.type != MOB_FISH) playSound(MD[m.type].snd, 0.5f * (1.f - d / 20.f), m.age < 0 ? 1.3f : 1.f); }
    }
    for (RemotePlayer& r : g_remote) {
        double ox = r.x, oz = r.z;
        r.x += (r.tx - r.x) * k; r.y += (r.ty - r.y) * k; r.z += (r.tz - r.z) * k;
        float dy = r.tyaw - r.yaw; while (dy > 3.1416f) dy -= 6.2832f; while (dy < -3.1416f) dy += 6.2832f; r.yaw += dy * k; r.pitch += (r.tpitch - r.pitch) * k;
        r.speed = (float)(sqrt((r.x - ox) * (r.x - ox) + (r.z - oz) * (r.z - oz)) / std::max(dt, 0.001f));
        r.walk += dt * std::min(r.speed, 6.f) * 2.2f; r.swing = std::max(0.f, r.swing - dt * 3.f);
    }
    // открытый сундук или печь: изменения сразу уходят хозяину
    if (g_screen == SCR_INV && g_invMode == INV_CHEST) {
        i64 key = posKey(g_contX, g_contY, g_contZ);
        auto& v = g_chests[key]; if (v.size() < 27) v.resize(27);
        bool diff = g_lastChestSent.size() != 27;
        for (int i = 0; i < 27 && !diff; i++) diff = !(v[i] == g_lastChestSent[i]);
        if (diff) { WBuf w; netBegin(w, M_CHESTSET); w.w<i32>(g_contX); w.w<i32>(g_contY); w.w<i32>(g_contZ); for (int i = 0; i < 27; i++) w.ws(v[i]); netSend(g_srv, w); g_lastChestSent = v; }
    }
    if (g_screen == SCR_INV && g_invMode == INV_FURNACE) {
        i64 key = posKey(g_contX, g_contY, g_contZ);
        Furnace& f = g_furnaces[key];
        bool diff = !(f.in == g_lastFurnSent[0]) || !(f.fuel == g_lastFurnSent[1]) || !(f.out == g_lastFurnSent[2]);
        g_furnPollT -= dt;
        if (diff || g_furnPollT <= 0) {
            g_furnPollT = 0.5f;
            WBuf w; netBegin(w, diff ? M_FURNSET : M_FURNREQ); w.w<i32>(g_contX); w.w<i32>(g_contY); w.w<i32>(g_contZ);
            if (diff) { w.ws(f.in); w.ws(f.fuel); w.ws(f.out); g_lastFurnSent[0] = f.in; g_lastFurnSent[1] = f.fuel; g_lastFurnSent[2] = f.out; }
            netSend(g_srv, w);
        }
    }
    g_sched.clear(); g_pending.clear();   // жидкости и сыпучие блоки считает хозяин
    netPump(g_srv);
#else
    (void)dt;
#endif
}
// гость: удар по мобу и действие с животным выполняет хозяин
static void netHitMob(Mob& m, float dmg) {
    WBuf b; netBegin(b, M_HITMOB); b.w<u32>(m.uid); b.w<float>(dmg); netSend(g_srv, b);
    m.hurtT = 0.4f; playSound(SND_HIT, 0.8f);
}
static bool netUseMob(Mob& m) {
    const Stack& h = heldStack();
    bool plausible = (m.type == MOB_SHEEP && h.id == I_SHEARS && !m.sheared) || (!h.empty() && likesFood(m.type, h.id)) || (m.tame && m.owner == g_myId);
    if (!plausible) return false;
    WBuf b; netBegin(b, M_USEMOB); b.w<u32>(m.uid); b.ws(h); netSend(g_srv, b);
    return true;
}
static void netSendArrow(const Arrow& a, float dmg) {
    WBuf b; netBegin(b, M_ARROW); b.w<double>(a.x); b.w<double>(a.y); b.w<double>(a.z); b.w<float>(a.vx); b.w<float>(a.vy); b.w<float>(a.vz); b.w<float>(dmg); b.w<u8>(a.crit ? 1 : 0);
    netSend(g_srv, b);
}
static bool netClientOpenContainer(int x, int y, int z, bool chest) {   // true — запрос отправлен, окно откроется по ответу
    if (g_netRole != NET_CLIENT || g_netApplying) return false;
    WBuf b; netBegin(b, chest ? M_CHESTREQ : M_FURNREQ); b.w<i32>(x); b.w<i32>(y); b.w<i32>(z); netSend(g_srv, b);
    if (chest) { g_pendChest = posKey(x, y, z); g_pendChestOn = true; } else { g_pendFurn = posKey(x, y, z); g_pendFurnOn = true; }
    return true;
}
// поиск игр в локальной сети (экран «Сетевая игра»)
static void netDiscoveryTick() {
#ifndef BW_NO_NET
    if (g_discFd < 0) {
        g_discFd = socket(AF_INET, SOCK_DGRAM, 0);
        if (g_discFd < 0) return;
        int one = 1; setsockopt(g_discFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
        setsockopt(g_discFd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
        setsockopt(g_discFd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
        sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_port = htons(DISC_PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(g_discFd, (sockaddr*)&a, sizeof(a)) != 0) { netCloseFd(g_discFd); return; }
        netNonblock(g_discFd);
        androidMulticastLock(true);
    }
    char buf[512];
    for (int k = 0; k < 16; k++) {
        sockaddr_in from; socklen_t l = sizeof(from);
        ssize_t n = recvfrom(g_discFd, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &l);
        if (n <= 0) break;
        buf[n] = 0; std::string s(buf);
        if (s.compare(0, 7, "BWLAN1|") != 0) continue;
        size_t a1 = s.find('|', 7), a2 = a1 == std::string::npos ? a1 : s.find('|', a1 + 1);
        if (a2 == std::string::npos) continue;
        FoundGame g; g.port = atoi(s.substr(7, a1 - 7).c_str()); g.world = s.substr(a1 + 1, a2 - a1 - 1); g.host = s.substr(a2 + 1);
        char t[64]; inet_ntop(AF_INET, &from.sin_addr, t, sizeof(t)); g.ip = t; g.seen = nowMs();
        bool found = false; for (FoundGame& f : g_found) if (f.ip == g.ip) { f = g; found = true; }
        if (!found && g_found.size() < 8) g_found.push_back(g);
    }
    for (size_t i = 0; i < g_found.size();) { if (nowMs() - g_found[i].seen > 6000) g_found.erase(g_found.begin() + i); else i++; }
#endif
}
static void netDiscoveryStop() { if (g_discFd >= 0) { netCloseFd(g_discFd); if (g_netRole == NET_NONE) androidMulticastLock(false); } g_found.clear(); }
static void netTick(float dt) {
    if (g_netRole == NET_HOST) netHostTick(dt);
    else if (g_netRole == NET_CLIENT) netClientTick(dt);
}
// ---------- профиль гостя: своя сумка и место в чужом мире (по зерну мира)
static std::string mpProfilePath() { return g_basePath + "buildworld_mp_" + std::to_string(g_mpSeed) + ".sav"; }
static void saveMpProfile() {
    if (g_netRole != NET_CLIENT || !g_welcomed) return;
    WBuf b; b.w<u32>(0x31504D42u); b.w<u32>(SAVE_VER);
    b.w<double>(P.x); b.w<double>(P.y); b.w<double>(P.z); b.w<float>(g_cam.yaw); b.w<float>(g_cam.pitch);
    b.w<float>(P.health); b.w<float>(P.hunger); b.w<float>(P.air); b.w<i32>(P.spawnX); b.w<i32>(P.spawnY); b.w<i32>(P.spawnZ);
    b.w<u8>((u8)P.sel); b.w<u8>(P.dead ? 1 : 0); b.w<i32>(P.xpLevel); b.w<float>(P.xpProg);
    for (int i = 0; i < 36; i++) b.ws(P.inv[i]);
    for (int i = 0; i < 4; i++) b.ws(P.armor[i]);
    writeFileAtomic(mpProfilePath(), b.d);
}
static bool loadMpProfile() {
    std::vector<u8> d; if (!readFile(mpProfilePath(), d)) return false;
    WBuf b; b.d.swap(d);
    if (b.r<u32>() != 0x31504D42u) return false;
    b.ver = b.r<u32>();
    P.x = b.r<double>(); P.y = b.r<double>(); P.z = b.r<double>(); g_cam.yaw = b.r<float>(); g_cam.pitch = b.r<float>();
    P.health = b.r<float>(); P.hunger = b.r<float>(); P.air = b.r<float>(); P.spawnX = b.r<i32>(); P.spawnY = b.r<i32>(); P.spawnZ = b.r<i32>();
    P.sel = clampi(b.r<u8>(), 0, 8); P.dead = b.r<u8>() != 0; P.xpLevel = std::max(0, b.r<i32>()); P.xpProg = clampf(b.r<float>(), 0, 0.99f);
    for (int i = 0; i < 36; i++) P.inv[i] = b.rs();
    for (int i = 0; i < 4; i++) P.armor[i] = b.rs();
    if (b.bad) { P = Player(); return false; }
    return true;
}

// ---------------------------------------------------------------- имена над головами
static void drawNameTags() {
    if (g_remote.empty()) return;
    const Camera& c = g_cam;
    for (const RemotePlayer& r : g_remote) {
        if (r.id == g_myId || (r.flags & 4)) continue;
        float p[3]; toCam((float)(r.x - c.x), (float)(r.y + 2.25 - c.y), (float)(r.z - c.z), p);
        if (p[2] < 0.3f || p[2] > 64.f) continue;
        float sx = c.cx + p[0] / p[2] * c.focal, sy = c.cy - p[1] / p[2] * c.focal;
        float px = clampf(fontPx() * 6.f / p[2], fontPx() * 0.45f, fontPx() * 1.1f);
        float w = textWidth(r.name, px);
        roundRect(sx - w / 2 - px * 2, sy - px * 2, w + px * 4, px * 11, px * 2, C(0, 0, 0, 120), C(0, 0, 0, 120));
        drawText(r.name, sx, sy, px, C(255, 255, 255, 235), 1, false);
    }
}
// ---------------------------------------------------------------- имя игрока
static const char* NAME_A[12] = {"ХРАБРЫЙ", "ТИХИЙ", "БЫСТРЫЙ", "ХИТРЫЙ", "ДОБРЫЙ", "СМЕЛЫЙ", "ЛОВКИЙ", "СОННЫЙ", "ВЕСЁЛЫЙ", "МУДРЫЙ", "ЗОРКИЙ", "ЛЕСНОЙ"};
static const char* NAME_B[12] = {"ЯК", "КОТ", "ПЁС", "ЁЖ", "ВОЛК", "ЛИС", "БОБР", "ФИЛИН", "ЛОСЬ", "БАРСУК", "ЗАЯЦ", "КРОТ"};
static std::string randomName() {
    u32 h = hash32((u32)SDL_GetPerformanceCounter() ^ SDL_GetTicks() * 2654435761u);
    return std::string(NAME_A[h % 12]) + " " + NAME_B[(h >> 8) % 12] + " " + std::to_string(10 + (h >> 16) % 90);
}
static void loadPlayerName() {
    std::vector<u8> d;
    if (readFile(g_basePath + "buildworld_name.txt", d) && d.size() > 1 && d.size() < 64) g_myName.assign(d.begin(), d.end());
    while (!g_myName.empty() && (g_myName.back() == '\n' || g_myName.back() == '\r')) g_myName.pop_back();
    if (g_myName.empty()) { g_myName = randomName(); std::vector<u8> v(g_myName.begin(), g_myName.end()); writeFileAtomic(g_basePath + "buildworld_name.txt", v); }
}
// ---------------------------------------------------------------- экран «Сетевая игра»
static void screenMP() {
    drawBackdrop(g_time);
    netDiscoveryTick();
    float S = uiS();
    drawTitle("СЕТЕВАЯ ИГРА", (float)insetTop(), 2.2f);
    bool connecting = g_netRole == NET_CLIENT;
    float colW = std::min(S * 100, g_W * 0.44f), gap = S * 4;
    float lx = g_W / 2.f - colW - gap / 2, rxc = g_W / 2.f + gap / 2, y0 = insetTop() + S * 13;
    float px = fontPx();
    // слева: имя и найденные игры
    if (button(300, lx, y0, colW, S * 9, "ИМЯ: " + g_myName, !connecting, 0.85f)) {
        g_myName = randomName(); std::vector<u8> v(g_myName.begin(), g_myName.end()); writeFileAtomic(g_basePath + "buildworld_name.txt", v);
    }
    drawText("ИГРЫ В ВАШЕЙ СЕТИ WI-FI:", lx, y0 + S * 12, px * 0.9f, C(240, 240, 240));
    float gy = y0 + S * 18;
    if (g_found.empty()) {
        static const char* dots[4] = {"", ".", "..", "..."};
        drawText(std::string("ИЩУ") + dots[(SDL_GetTicks() / 400) % 4], lx, gy + S * 2, px * 0.9f, C(200, 210, 220));
        drawText("ХОЗЯИН МИРА: ПАУЗА -> ОТКРЫТЬ ДЛЯ СЕТИ", lx, gy + S * 9, px * 0.82f, C(230, 236, 246));
    }
    for (size_t i = 0; i < g_found.size() && i < 4; i++) {
        const FoundGame& f = g_found[i];
        std::string lab = f.world + " (" + f.host + ")";
        if (button(310 + (int)i, lx, gy + i * S * 10.5f, colW, S * 9, lab, !connecting, 0.8f)) { netDiscoveryStop(); netConnect(f.ip); }
    }
    // справа: адрес и цифровая клавиатура
    drawText("ИЛИ ВВЕДИТЕ АДРЕС ХОЗЯИНА:", rxc, y0 + S * 1, px * 0.9f, C(240, 240, 240));
    float fy = y0 + S * 7, fh = S * 9;
    roundRect(rxc, fy, colW, fh, S * 2, C(250, 250, 252, 240), C(228, 230, 236, 240));
    std::string shown = g_ipText + (((SDL_GetTicks() / 500) & 1) && !connecting ? "_" : "");
    drawText(shown.empty() ? " " : shown, rxc + S * 3, fy + fh / 2 - 3.5f * px * 1.1f, px * 1.1f, C(30, 34, 44), 0, false);
    static const char* KEYS[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", ".", "0", "<"};
    float kw = (colW - S * 4) / 3, kh = S * 7.6f, ky = fy + fh + S * 2;
    for (int i = 0; i < 12; i++) {
        float kx = rxc + (i % 3) * (kw + S * 2), kyy = ky + (i / 3) * (kh + S * 1.4f);
        if (button(330 + i, kx, kyy, kw, kh, KEYS[i], !connecting)) {
            if (i == 11) { if (!g_ipText.empty()) g_ipText.pop_back(); }
            else if (g_ipText.size() < 15) g_ipText += KEYS[i];
        }
    }
    float cy2 = ky + 4 * (kh + S * 1.4f);
    if (button(345, rxc, cy2, colW, S * 9, "ПОДКЛЮЧИТЬСЯ", !connecting && !g_ipText.empty())) { netDiscoveryStop(); netConnect(g_ipText); }
    // статус и подсказки
    std::string st = g_netStatus;
    if (!st.empty()) { float sp = px * 0.9f; while (textWidth(st, sp) > g_W * 0.9f && sp > 1.1f) sp *= 0.92f; drawText(st, g_W / 2.f, g_H - S * 22, sp, C(255, 230, 150), 1); }
    drawText("ДЛЯ ИГРЫ ЧЕРЕЗ ИНТЕРНЕТ НУЖЕН ПРОБРОС ПОРТА 41234 ИЛИ ОБЩАЯ VPN-СЕТЬ", g_W / 2.f, g_H - S * 15.5f, px * 0.62f, C(200, 204, 214), 1);
    if (button(399, (g_W - S * 40) / 2, g_H - S * 11, S * 40, S * 9, connecting ? "ОТМЕНА" : "НАЗАД")) {
        if (connecting) netClientStop("ПОДКЛЮЧЕНИЕ ОТМЕНЕНО");
        else { netDiscoveryStop(); g_screen = SCR_MAIN; g_ui.focus = -1; }
    }
}

static void exitToMenu() {
    if (g_netRole == NET_CLIENT) { netClientStop(""); g_screen = SCR_MAIN; return; }
    saveWorld();
    if (g_netRole == NET_HOST) netHostStop();
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
static void setActivePad(SDL_GameController* g) {
    if (!g || g == g_pad) return;
    g_pad = g; g_padStyle = g_padStyle2 = padStyleOf(g);
}
static void openPad(int idx) {
    if (!SDL_IsGameController(idx)) return;
    SDL_GameController* g = SDL_GameControllerOpen(idx);
    if (!g) return;
    for (SDL_GameController* p : g_pads) if (p == g) return;   // уже открыт (событие пришло повторно)
    g_pads.push_back(g);
    if (!g_pad) setActivePad(g);
    static const char* NM[3] = {"ГЕЙМПАД XBOX", "ГЕЙМПАД PLAYSTATION", "ГЕЙМПАД NINTENDO"};
    showMsg(std::string(NM[padStyleOf(g)]) + " ПОДКЛЮЧЁН", 2.f);
    SDL_GameController* keep = g_pad; g_pad = g; bool was = g_ui.pad; g_ui.pad = true; padRumble(0.4f, 0.4f, 180); g_ui.pad = was; g_pad = keep;   // «привет» вибрацией
}
static void closePad(SDL_JoystickID which) {
    for (size_t i = 0; i < g_pads.size(); i++) if (padId(g_pads[i]) == which) {
        bool wasActive = g_pads[i] == g_pad;
        SDL_GameControllerClose(g_pads[i]); g_pads.erase(g_pads.begin() + i);
        if (wasActive) {
            g_pad = nullptr;
            if (!g_pads.empty()) setActivePad(g_pads.front());
            else { g_ui.pad = false; g_touchUsed = true; }
            if (g_screen == SCR_GAME && !P.dead) { openPause(); showMsg("ГЕЙМПАД ОТКЛЮЧЁН - ИГРА НА ПАУЗЕ", 2.5f); }   // как на приставках
        }
        return;
    }
}
// лампа DualShock/DualSense: цвет по здоровью, синий под водой, фиолетовый в творческом
static void updatePadLED(float dt) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    static float t = 0; static int last = -1;
    t -= dt; if (t > 0 || !g_pad || !SDL_GameControllerHasLED(g_pad)) return;
    t = 0.2f;
    int r = 60, g = 90, b = 255;   // меню
    if (worldScreenActive() && g_screen != SCR_LOADING) {
        if (g_mode == GM_CREATIVE) { r = 160; g = 70; b = 255; }
        else {
            float h = clampf(P.health / 20.f, 0, 1);
            r = (int)(255 * std::min(1.f, 2.f - 2.f * h)); g = (int)(220 * std::min(1.f, 2.f * h)); b = 30;
            if (h < 0.3f && fmodf(g_time, 0.8f) < 0.4f) { r = 120; g = 0; b = 0; }   // мигает при малом здоровье
        }
        if (g_underwaterAudio.load()) { r = 30; g = 120; b = 255; }
        if (P.dead) { r = 255; g = 0; b = 0; }
    }
    int key = (r << 16) | (g << 8) | b;
    if (key != last) { SDL_GameControllerSetLED(g_pad, (Uint8)r, (Uint8)g, (Uint8)b); last = key; }
#else
    (void)dt;
#endif
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
        case SCR_MP: if (g_netRole == NET_CLIENT) netClientStop(""); netDiscoveryStop(); g_screen = SCR_MAIN; break;
        default: break;
    }
    g_ui.focus = -1;
}
// выбросить предмет из руки вперёд (одну штуку или всю стопку)
static void dropHeld(bool all) {
    Stack& h = P.inv[P.sel];
    if (h.empty()) return;
    Stack d = h; d.cnt = all ? h.cnt : 1;
    h.cnt = (u16)(h.cnt - d.cnt); if (h.cnt == 0) h.clear();
    const float* f = g_cam.f;
    spawnDrop(d, P.x + f[0] * 0.6, P.y + PL_EYE - 0.3 + f[1] * 0.6, P.z + f[2] * 0.6, f[0] * 5.f, f[1] * 5.f + 2.f, f[2] * 5.f);
    if (!g_drops.empty()) g_drops.back().age = -1.5f;   // выброшенное не прилетает обратно сразу
    playSound(SND_CLICK, 0.4f, 0.7f); padRumble(0.f, 0.2f, 40);
}
// R3: взять в руку блок, на который смотришь (в творческом — создать, в выживании — найти в сумке)
static void pickTargetBlock() {
    if (!g_target.hit) return;
    u8 b = getBlock(g_target.x, g_target.y, g_target.z);
    int id = b;
    if (b == B_GRASS) id = B_GRASS; if (b == B_FURNACE_LIT) id = B_FURNACE;
    for (int i = 0; i < 9; i++) if (P.inv[i].id == id) { selectSlot(i); return; }
    if (g_mode == GM_CREATIVE) { P.inv[P.sel] = mkStack(id, maxStack(id)); g_heldNameT = 2.f; return; }
    for (int i = 9; i < 36; i++) if (P.inv[i].id == id) { std::swap(P.inv[i], P.inv[P.sel]); g_heldNameT = 2.f; return; }
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
        case SDL_CONTROLLER_BUTTON_X: if (!game) g_ui.alt = true; else dropHeld(false); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: if (game) pickTargetBlock(); break;
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
        case SDL_CONTROLLER_BUTTON_DPAD_UP: if (!game) g_ui.navDir = 0; else if (g_mode == GM_CREATIVE) toggleFly(); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: if (!game) g_ui.navDir = 1; else dropHeld(true); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: if (!game) g_ui.navDir = 2; else selectSlot(P.sel - 1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if (!game) g_ui.navDir = 3; else selectSlot(P.sel + 1); break;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: if (game) g_in.sprint = !g_in.sprint; break;
    }
}
static void keyDown(SDL_Keycode k) {
    bool game = g_screen == SCR_GAME && !P.dead;
    if (k == SDLK_AC_BACK || k == SDLK_ESCAPE) { backAction(); return; }
    if (g_screen == SCR_MP && g_netRole == NET_NONE) {   // ввод адреса с клавиатуры
        if (((k >= '0' && k <= '9') || k == '.') && g_ipText.size() < 15) { g_ipText += (char)k; return; }
        if (k == '\b' && !g_ipText.empty()) { g_ipText.pop_back(); return; }
        if (k == SDLK_RETURN && !g_ipText.empty()) { netDiscoveryStop(); netConnect(g_ipText); return; }
    }
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
        if (lx != 0 || ly != 0) { g_in.mx = lx; g_in.mz = -ly; g_ui.pad = true; g_touchUsed = false; if (wasSprint && ly < -0.5f) g_in.sprint = true; }
        float rx = padAxis(SDL_CONTROLLER_AXIS_RIGHTX), ry = padAxis(SDL_CONTROLLER_AXIS_RIGHTY);
        if (rx != 0 || ry != 0) { g_ui.pad = true; g_touchUsed = false; }
        // кривая: точное прицеливание у центра, быстрый поворот у края; при стике «в упор» поворот разгоняется
        float mag = sqrtf(rx * rx + ry * ry);
        if (mag > 0.94f) g_padTurnBoost = std::min(1.f, g_padTurnBoost + dt * 2.2f); else g_padTurnBoost = std::max(0.f, g_padTurnBoost - dt * 6.f);
        auto curve = [](float v) { return (v < 0 ? -1.f : 1.f) * powf(fabsf(v), 1.8f); };
        float sp = 3.4f * g_set.padSens * (1.f + 0.7f * g_padTurnBoost);
        if (P.bowCharge > 0) sp *= 0.6f;   // натянутый лук — точнее
        g_cam.yaw += curve(rx) * sp * dt;
        g_cam.pitch = clampf(g_cam.pitch - curve(ry) * (g_set.invertY ? -1.f : 1.f) * sp * 0.75f * dt, -1.55f, 1.55f);
        bool r2 = padAxis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.4f, l2 = padAxis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.4f;
        if (r2 || l2) { g_ui.pad = true; g_touchUsed = false; }
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
// звуки окружения: птицы днём, сверчки ночью, гул в пещерах, ветер на высоте
static void updateAmbient(float dt) {
    static float tBird = 5, tCrick = 3, tCave = 12, tWind = 6;
    Rng r(SDL_GetTicks() * 31 + 7);
    int ex = ifloor(P.x), ey = ifloor(P.y + PL_EYE), ez = ifloor(P.z);
    g_underwaterAudio = getBlock(ex, ey, ez) == B_WATER ? 1 : 0;
    int sky, bl; getLightAt(ex, ey, ez, sky, bl);
    bool surface = sky >= 11, under = sky <= 3 && P.y < 60;
    tBird -= dt; tCrick -= dt; tCave -= dt; tWind -= dt;
    if (tBird <= 0) { tBird = 5.f + r.f() * 12.f; if (surface && !isNight() && !g_underwaterAudio) playSound(r.range(2) ? SND_BIRD : SND_BIRD2, 0.1f + r.f() * 0.12f, 0.85f + r.f() * 0.35f); }
    if (tCrick <= 0) { tCrick = 3.f + r.f() * 6.f; if (surface && isNight()) playSound(SND_CRICKET, 0.08f + r.f() * 0.07f, 0.9f + r.f() * 0.2f); }
    if (tCave <= 0) { tCave = 20.f + r.f() * 35.f; if (under) playSound(SND_CAVE, 0.35f, 0.8f + r.f() * 0.3f); }
    if (tWind <= 0) { tWind = 7.f + r.f() * 8.f; if (P.y > 95 && surface) playSound(SND_WIND, 0.25f, 0.8f + r.f() * 0.3f); }
}
static int musicMoodNow() {
    int ex = ifloor(P.x), ey = ifloor(P.y + PL_EYE), ez = ifloor(P.z);
    int sky, bl; getLightAt(ex, ey, ez, sky, bl);
    if ((sky <= 3 && P.y < 60) || isNight()) return 2;
    float sh = sunHeight();
    return sh < 0.25f ? 1 : 0;
}
static void updateGame(float dt) {
    g_playTime += dt;
    updateAmbient(dt);
    spawnAmbientParticles(dt);
    g_dayTime += dt / DAY_LEN; if (g_dayTime >= 1) { g_dayTime -= 1; g_dayCount++; }
    g_msgT = std::max(0.f, g_msgT - dt);
    g_heldNameT = std::max(0.f, g_heldNameT - dt);
    for (size_t i = 0; i < g_picks.size();) { g_picks[i].t -= dt; if (g_picks[i].t <= 0) g_picks.erase(g_picks.begin() + i); else i++; }
    g_tickAcc += dt;
    int ticks = 0;
    const bool guest = g_netRole == NET_CLIENT;   // у гостя мир считает хозяин
    while (g_tickAcc >= 0.05f && ticks < 4) { g_tickAcc -= 0.05f; g_tick++; if (!guest) runScheduledUpdates(); ticks++; }
    if (g_tickAcc > 0.2f) g_tickAcc = 0;
    if (!guest) { tickFurnaces(dt); tickGrowth(dt); }
    // физика игрока мелкими шагами
    float rem = dt;
    while (rem > 0) { float s = std::min(rem, 1.f / 60.f); updatePlayerPhysics(s); rem -= s; }
    updateSurvival(dt);
    updateInteraction(dt);
    if (P.dead && g_screen == SCR_GAME) { g_screen = SCR_DEAD; g_ui.focus = -1; }
    static Rng mobRng(777);
    updateWeather(dt, mobRng);
    if (!guest) for (Mob& m : g_mobs) if (!m.dead) updateMob(m, dt, mobRng);
    updateBobber(dt);
    for (size_t i = 0; i < g_mobs.size() && !guest;) {
        Mob& m = g_mobs[i];
        bool far = nearestPlayerDist2(m.x, m.z) > 96.0 * 96.0 || !isLoaded(ifloor(m.x), ifloor(m.z));
        if (!m.dead && far && m.tame) g_parked.push_back(m);   // питомец остаётся на своём месте и вернётся
        if (m.dead || far) { g_mobs[i] = g_mobs.back(); g_mobs.pop_back(); continue; }
        i++;
    }
    g_spawnT -= dt;
    if (g_spawnT <= 0 && !guest) { g_spawnT = 1.5f; spawnMobs(mobRng); spawnFish(mobRng); parkFarPets(); }
    updateArrows(dt);
    updateDrops(dt);
    updateOrbs(dt);
    P.levelFlash = std::max(0.f, P.levelFlash - dt * 1.5f);
    updateParticles(dt);
    g_autosaveT += dt;
    if (g_autosaveT > 60.f) { g_autosaveT = 0; saveWorld(); }
}

// размытие дали: дальний план (небо, упрощённый рельеф, облака) рисуется в текстуру,
// дважды уменьшается вдвое со сглаживанием и растягивается обратно — получается мягкое «боке», а ближний мир поверх остаётся резким
static SDL_Texture* g_dofTex[3] = {nullptr, nullptr, nullptr};
static int g_dofW = 0, g_dofH = 0;
static bool g_dofBroken = false;
static void dofFree() { for (auto& t : g_dofTex) { if (t) SDL_DestroyTexture(t); t = nullptr; } g_dofW = g_dofH = 0; }
static bool dofEnsure() {
    if (g_dofBroken) return false;
    if (g_dofTex[0] && g_dofW == g_W && g_dofH == g_H) return true;
    dofFree();
    if (!SDL_RenderTargetSupported(g_ren)) { g_dofBroken = true; return false; }
    for (int i = 0; i < 3; i++) {
        int w = std::max(1, g_W >> i * 1), h = std::max(1, g_H >> i * 1);
        if (i == 2) { w = std::max(1, g_W / 5); h = std::max(1, g_H / 5); }
        g_dofTex[i] = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, w, h);
        if (!g_dofTex[i]) { dofFree(); g_dofBroken = true; return false; }
        SDL_SetTextureScaleMode(g_dofTex[i], SDL_ScaleModeLinear);
        SDL_SetTextureBlendMode(g_dofTex[i], SDL_BLENDMODE_NONE);
    }
    g_dofW = g_W; g_dofH = g_H;
    return true;
}
static bool dofBegin() {
    if (!dofEnsure()) return false;
    if (SDL_SetRenderTarget(g_ren, g_dofTex[0]) != 0) { SDL_SetRenderTarget(g_ren, NULL); dofFree(); g_dofBroken = true; return false; }
    return true;
}
static void dofEnd() {
    // полный кадр → половина → пятая часть → экран: каждый шаг усредняет соседние пиксели
    SDL_SetRenderTarget(g_ren, g_dofTex[1]); SDL_RenderCopy(g_ren, g_dofTex[0], NULL, NULL);
    SDL_SetRenderTarget(g_ren, g_dofTex[2]); SDL_RenderCopy(g_ren, g_dofTex[1], NULL, NULL);
    SDL_SetRenderTarget(g_ren, NULL);
    SDL_RenderCopy(g_ren, g_dofTex[2], NULL, NULL);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
}
// лучи света в толще воды: наклонные полосы от поверхности вниз по направлению солнца, медленно колышутся
static void drawUnderwaterShafts() {
    if (g_shaders < 2 || g_fx.sunUp < 0.05f) return;
    int ex = ifloor(g_cam.x), ez = ifloor(g_cam.z);
    int ey = ifloor(g_cam.y);
    int surf = ey;
    while (surf < WH - 1 && getBlock(ex, surf + 1, ez) == B_WATER) surf++;
    if (surf - ey > 40) return;
    float sy = surf + 1.f - (float)g_cam.y;
    float dir[3] = {-g_fx.sun[0], -std::max(0.35f, g_fx.sun[1]), -g_fx.sun[2]};
    float dl = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]); for (float& v : dir) v /= dl;
    static std::vector<SDL_Vertex> vb; static std::vector<int> ib; vb.clear(); ib.clear();
    const float t = g_fxTime;
    const float col[3] = {0.55f, 0.95f, 0.9f};
    for (int gz = -5; gz <= 5; gz++) for (int gx = -5; gx <= 5; gx++) {
        int cx = (ex >> 1) + gx, cz = (ez >> 1) + gz;
        u32 h = hash32((u32)cx * 73856093u ^ (u32)cz * 19349663u ^ 0x51u);
        if ((h & 3) != 0) continue;   // лучи редкие
        float x0 = cx * 2.f + (h >> 8 & 255) / 255.f * 2.f - (float)g_cam.x, z0 = cz * 2.f + (h >> 16 & 255) / 255.f * 2.f - (float)g_cam.z;
        int wx = ifloor(x0 + g_cam.x), wz = ifloor(z0 + g_cam.z);
        if (!isLoaded(wx, wz) || getBlock(wx, surf, wz) != B_WATER || getBlock(wx, surf + 1, wz) != B_AIR) continue;
        float pulse = 0.5f + 0.5f * sinf(t * (0.4f + (h & 15) * 0.03f) + (h >> 4 & 63) * 0.1f);
        float a = 0.16f * pulse * g_fx.sunUp * (1.f - g_rainK);
        float len = 14.f + (h >> 24 & 15);
        float w = 0.35f + (h >> 12 & 15) / 15.f * 0.5f;
        // полоса повернута к камере
        float top[3] = {x0, sy, z0}, bot[3] = {x0 + dir[0] * len, sy + dir[1] * len, z0 + dir[2] * len};
        float mid[3] = {(top[0] + bot[0]) * 0.5f, (top[1] + bot[1]) * 0.5f, (top[2] + bot[2]) * 0.5f};
        float sx = dir[1] * mid[2] - dir[2] * mid[1], sy2 = dir[2] * mid[0] - dir[0] * mid[2], sz = dir[0] * mid[1] - dir[1] * mid[0];
        float sl = sqrtf(sx * sx + sy2 * sy2 + sz * sz) + 1e-4f; sx *= w / sl; sy2 *= w / sl; sz *= w / sl;
        float W[4][3] = {{top[0] - sx, top[1] - sy2, top[2] - sz}, {top[0] + sx, top[1] + sy2, top[2] + sz},
                         {bot[0] + sx, bot[1] + sy2, bot[2] + sz}, {bot[0] - sx, bot[1] - sy2, bot[2] - sz}};
        float Pc[4][3]; bool ok = true;
        for (int k = 0; k < 4; k++) { toCam(W[k][0], W[k][1], W[k][2], Pc[k]); if (Pc[k][2] < 0.3f) ok = false; }
        if (!ok) continue;
        int base = (int)vb.size();
        for (int k = 0; k < 4; k++) {
            SDL_Vertex v;
            v.position.x = g_cam.cx + Pc[k][0] / Pc[k][2] * g_cam.focal; v.position.y = g_cam.cy - Pc[k][1] / Pc[k][2] * g_cam.focal;
            float fade = k < 2 ? 1.f : 0.f;
            float dist = sqrtf(Pc[k][0] * Pc[k][0] + Pc[k][1] * Pc[k][1] + Pc[k][2] * Pc[k][2]);
            fade *= 1.f - smooth01(6.f, 16.f, dist);
            v.color = mkColor(col[0], col[1], col[2], a * (k < 2 ? fade : 0.f) + (k >= 2 ? 0.f : 0.f));
            v.tex_coord.x = v.tex_coord.y = 0;
            vb.push_back(v);
        }
        int id[6] = {0, 1, 2, 0, 2, 3};
        for (int i : id) ib.push_back(base + i);
    }
    if (ib.empty()) return;
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_ADD);
    SDL_RenderGeometry(g_ren, NULL, vb.data(), (int)vb.size(), ib.data(), (int)ib.size());
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
}
static void renderWorld() {
    int ex = ifloor(P.x), ey = ifloor(P.y + PL_EYE), ez = ifloor(P.z);
    u8 headB = getBlock(ex, ey, ez);
    bool underwater = headB == B_WATER, inLava = headB == B_LAVA;
    g_shaders = g_set.shaders;
    g_waterDepth = 0;
    if (underwater) { int y = ey; while (y < WH - 1 && getBlock(ex, y + 1, ez) == B_WATER) y++; g_waterDepth = y + 1 - (float)(P.y + PL_EYE); }
    updateSkyColors(underwater);
    if (!underwater) applyWeatherSky();
    g_renderDist = g_set.renderDist;
    g_viewDist = std::max(g_set.viewDist, g_renderDist);
    g_fogEnd = g_viewDist * 16.f - 6.f; g_fogStart = g_viewDist > g_renderDist ? g_fogEnd * 0.55f : g_fogEnd * 0.62f;
    if (underwater) { g_fogStart = 2.f; g_fogEnd = 18.f; }
    if (inLava) { g_fogStart = 0.f; g_fogEnd = 3.f; }
    g_cam.x = P.x; g_cam.y = P.y + PL_EYE - fabsf(sinf(P.bobPhase)) * 0.06f * P.bobAmt; g_cam.z = P.z;
    float shYaw = 0, shPitch = 0;
    if (g_shake > 0) {
        float k = g_shake * g_shake, t = g_time * 37.f;
        shYaw = (sinf(t) * 0.6f + sinf(t * 2.3f) * 0.4f) * 0.035f * k; shPitch = (cosf(t * 1.7f) * 0.6f + sinf(t * 3.1f) * 0.4f) * 0.03f * k;
        g_cam.yaw += shYaw; g_cam.pitch += shPitch;
    }
    camUpdate();
    g_cam.yaw -= shYaw; g_cam.pitch -= shPitch;   // управление камерой не сбивается
    g_shaders = g_set.shaders; g_fxTime = g_time;
    computeFX(g_dayTime, g_time);
    applyWeatherFX();
    computeShaderParams();
    updateAnimTiles(g_time);
    updateCaustics(g_time);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(g_ren, (Uint8)(g_skyCol[0] * 255), (Uint8)(g_skyCol[1] * 255), (Uint8)(g_skyCol[2] * 255), 255);
    SDL_RenderClear(g_ren);
    bool dof = g_set.dof && g_shaders > 0 && !underwater && dofBegin();
    if (dof) SDL_RenderClear(g_ren);
    if (!underwater) drawSky();
    if (!underwater) { drawLod(); emitClouds(g_time); flushPolys(g_atlasTex); }   // дальний план и облака — позади всего остального
    if (dof) dofEnd();
    findVisibleSections();
    drawSections();
    emitMobs();
    emitRemotePlayers();
    emitDrops(g_time);
    emitOrbs();
    emitArrows();
    emitFishingLine();
    emitParticles();
    if (!underwater) emitWeather(g_time);
    emitCrack();
    flushPolys(g_atlasTex);
    if (underwater) drawUnderwaterShafts();
    if (g_target.hit && !P.dead) {
        u8 b = getBlock(g_target.x, g_target.y, g_target.z);
        float y1 = (b == B_BED) ? 9 / 16.f : (b == B_LILY ? 1.5f / 16.f : 1.f);
        drawBlockOutline(g_target.x, g_target.y, g_target.z, -0.002f, -0.002f, -0.002f, 1.002f, y1 + 0.002f, 1.002f);
    }
    drawLightHalos(g_cam.x, g_cam.y, g_cam.z);
    drawNameTags();
    emitHand();
    flushPolys(g_atlasTex);
    drawLensFX(g_cam.x, g_cam.y, g_cam.z, underwater);
    if (g_set.shaders > 0) {   // цветокоррекция всего кадра
        SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_MOD);
        SDL_SetRenderDrawColor(g_ren, (Uint8)(g_grade[0] * 255), (Uint8)(g_grade[1] * 255), (Uint8)(g_grade[2] * 255), 255);
        SDL_RenderFillRect(g_ren, NULL);
        SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    }
    if (underwater) {
        if (g_set.shaders > 0) {   // толща воды тонирует всё: бирюза у поверхности, синева в глубине
            float dk = smooth01(1.f, 22.f, g_waterDepth);
            fillRect(0, 0, (float)g_W, (float)g_H, C((int)lerpf(15, 5, dk), (int)lerpf(90, 30, dk), (int)lerpf(120, 110, dk), (int)lerpf(55, 95, dk)));
        } else fillRect(0, 0, (float)g_W, (float)g_H, C(20, 60, 160, 70));
    }
    if (inLava) fillRect(0, 0, (float)g_W, (float)g_H, C(220, 90, 10, 200));
    // урон и мало здоровья: красное свечение по краям экрана (пульсирует как сердцебиение)
    {
        float a = P.hurtFlash / 0.35f * 0.75f;
        if (!P.dead && g_mode == GM_SURVIVAL && P.health <= 6.f) {
            float beat = powf(std::max(0.f, sinf(g_time * 7.5f)), 6.f);
            a = std::max(a, (0.25f + 0.35f * beat) * (1.f - P.health / 7.f) + 0.15f);
        }
        if (a > 0.01f) drawEdgeGlow(190, 0, 0, std::min(1.f, a));
    }
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
                float uv[2]; tileUV(pass ? crackTile(crack) : tiles[f], UVp[k][0], UVp[k][1], uv);
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
        for (int i = 0; i < 4; i++) if (t >= INTRO_HIT[i]) stage = std::min(9, (i + 1) * 2 + 1);
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
    float bw = S * 70, bh = S * 10.5f, bx = (g_W - bw) / 2, by = std::min(g_H * 0.43f, g_H - bh * 4.6f);
    if (button(1, bx, by, bw, bh, "ИГРАТЬ")) { refreshWorldList(); g_screen = SCR_WORLDS; g_ui.focus = -1; }
    if (button(4, bx, by + bh * 1.3f, bw, bh, "СЕТЕВАЯ ИГРА")) { g_netStatus.clear(); g_screen = SCR_MP; g_ui.focus = -1; }
    if (button(2, bx, by + bh * 2.6f, bw, bh, "НАСТРОЙКИ")) { g_settingsBack = SCR_MAIN; g_screen = SCR_SETTINGS; g_ui.focus = -1; }
    if (button(3, bx, by + bh * 3.9f, bw, bh, "ВЫХОД")) g_running = false;
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
    g_viewDist = std::max(g_set.viewDist, g_set.renderDist); manageLod(8.0, P.x, P.z, g_set.renderDist);
    int pcx = ifloor(P.x) >> 4, pcz = ifloor(P.z) >> 4;
    int need = 0, done = 0;
    for (int dz = -rd; dz <= rd; dz++) for (int dx = -rd; dx <= rd; dx++) {
        need++; Chunk* c = getChunk(pcx + dx, pcz + dz);
        if (c && c->meshed && !c->dirty) done++;
    }
    drawBackdrop(g_time);
    drawTitle("СОЗДАНИЕ МИРА...", g_H * 0.35f, 2.2f);
    float bw = g_W * 0.5f, bh = uiS() * 4, bx = (g_W - bw) / 2, by = g_H * 0.55f;
    static float shown = 0; float want = done / (float)need; shown += (want - shown) * std::min(1.f, g_frameDt * 8.f); if (want < shown - 0.5f) shown = want;
    roundRect(bx, by, bw, bh, bh / 2, C(16, 18, 22, 220), C(30, 32, 38, 220));
    if (shown > 0.01f) roundRect(bx, by, std::max(bh, bw * shown), bh, bh / 2, C(150, 220, 110), C(80, 160, 60));
    roundFrame(bx, by, bw, bh, bh / 2, std::max(1.f, bh * 0.08f), C(0, 0, 0, 200));
    {   // совет на время загрузки
        static const char* TIPS[] = {"ЗАЖМИ ЭКРАН НА БЛОКЕ, ЧТОБЫ ЕГО СЛОМАТЬ", "КОРОТКОЕ КАСАНИЕ СТАВИТ БЛОК ИЛИ ИСПОЛЬЗУЕТ ПРЕДМЕТ",
            "БРЕВНО -> ДОСКИ -> ВЕРСТАК: С ЭТОГО НАЧИНАЕТСЯ ВСЁ", "ЖЕЛЕЗО НУЖНО ПЕРЕПЛАВИТЬ В ПЕЧИ", "ФАКЕЛЫ ОТПУГИВАЮТ НОЧНЫХ ЧУДИЩ",
            "КРОВАТЬ ПЕРЕНОСИТ ТОЧКУ ВОЗРОЖДЕНИЯ", "УДОЧКОЙ ИНОГДА ВЫЛАВЛИВАЮТ ЗАЧАРОВАННЫЕ КНИГИ", "АЛМАЗЫ ПРЯЧУТСЯ ГЛУБОКО ПОД ЗЕМЛЁЙ"};
        static int tip = -1; if (tip < 0) tip = (int)(SDL_GetTicks() / 7 % 8);
        float tp = fontPx(); std::string tt = std::string("СОВЕТ: ") + TIPS[tip];
        while (textWidth(tt, tp) > g_W * 0.9f && tp > 1.2f) tp *= 0.92f;
        drawText(tt, g_W / 2.f, by + bh + uiS() * 6, tp, C(255, 240, 200), 1);
    }
    if (done >= need) {
        if (g_needSpawnY) {
            int x = ifloor(P.x), z = ifloor(P.z), y = WH - 2;
            while (y > 1 && !BD[getBlock(x, y, z)].solid && getBlock(x, y, z) != B_WATER) y--;
            P.y = y + 1.01; P.spawnY = y + 1;
            g_needSpawnY = false;
            saveWorld();
#ifndef __ANDROID__
            if (const char* up = getenv("BW_UP")) { P.y += atof(up); P.flying = true; }
            if (g_debugHost && g_netRole == NET_NONE) netHostStart();
            if (g_netRole == NET_CLIENT) if (const char* jp = getenv("BW_JOINPOS")) {   // отладка гостя: место и взгляд
                float x = 0, z = 0, yw = 0, pt = 0; sscanf(jp, "%f %f %f %f", &x, &z, &yw, &pt);
                P.x += x; P.z += z; g_cam.yaw = yw; g_cam.pitch = pt;
                int ty = WH - 2; while (ty > 1 && !BD[getBlock(ifloor(P.x), ty, ifloor(P.z))].solid) ty--; P.y = ty + 1.01;
                if (getenv("BW_CLIENTBUILD")) for (int k = 0; k < 4; k++) setBlock(ifloor(P.x) + 2, ty + 1 + k, ifloor(P.z), k == 3 ? B_TORCH : B_GLASS);
            }
            if (getenv("BW_METALS")) {   // отладка: металлы, руда, факелы и лава перед камерой
                float fx = sinf(g_cam.yaw), fz = cosf(g_cam.yaw), sx = cosf(g_cam.yaw), sz = -sinf(g_cam.yaw);
                const int ids[6] = {B_GOLD_BLOCK, B_IRON_BLOCK, B_DIAMOND_BLOCK, B_GOLD_BLOCK, B_DIAMOND_BLOCK, B_IRON_BLOCK};
                int gy = ifloor(P.y) - 1;
                for (int i = 0; i < 6; i++) {
                    int x = ifloor(P.x + fx * 5 + sx * (i - 2.5f) * 1.5f), z = ifloor(P.z + fz * 5 + sz * (i - 2.5f) * 1.5f);
                    setBlock(x, gy, z, (u8)ids[i]); setBlock(x, gy + 1, z, (u8)ids[(i + 2) % 6]);
                }
                int lx = ifloor(P.x + fx * 9 - sx * 3), lz = ifloor(P.z + fz * 9 - sz * 3);
                setBlock(lx, gy, lz, B_LAVA); setBlock(lx + 1, gy, lz, B_LAVA);
                int tx = ifloor(P.x + fx * 9 + sx * 3), tz = ifloor(P.z + fz * 9 + sz * 3);
                setBlock(tx, gy, tz, B_STONE); setBlock(tx, gy + 1, tz, B_TORCH);
            }
            if (getenv("BW_ANIMALS")) {   // отладка: выставка животных перед камерой
                Rng r(5); float fx = sinf(g_cam.yaw), fz = cosf(g_cam.yaw), sx = cosf(g_cam.yaw), sz = -sinf(g_cam.yaw);
                struct S { int t, var; bool tame, sit, sheared; float age; } list[] = {
                    {MOB_PIG, 0, false, false, false, 0}, {MOB_PIG, 1, false, false, false, -300}, {MOB_SHEEP, 0, false, false, false, 0}, {MOB_SHEEP, 3, false, false, false, 0},
                    {MOB_SHEEP, 4, false, false, true, 0}, {MOB_CAT, 1, true, true, false, 0}, {MOB_CAT, 0, false, false, false, 0}, {MOB_DOG, 0, true, true, false, 0}, {MOB_DOG, 1, false, false, false, 0}};
                int n = (int)(sizeof(list) / sizeof(list[0]));
                for (int i = 0; i < n; i++) {
                    float side = (i - (n - 1) * 0.5f) * 1.4f, fwd = 5.f + (i % 2) * 1.5f;
                    double ax = P.x + fx * fwd + sx * side, az = P.z + fz * fwd + sz * side;
                    int ty = WH - 2; while (ty > 1 && !BD[getBlock(ifloor(ax), ty, ifloor(az))].solid) ty--;
                    Mob m = makeMob(list[i].t, ax, ty + 1.02, az, r);
                    m.var = list[i].var; m.tame = list[i].tame; m.sit = list[i].sit; m.sheared = list[i].sheared; m.age = list[i].age;
                    m.yaw = g_cam.yaw + 3.1416f + (i % 3 - 1) * 0.6f; m.wishYaw = m.yaw; m.aiT = 1000.f;
                    g_mobs.push_back(m);
                }
            }
#endif
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
    if (g_netRole == NET_CLIENT) {
        if (button(62, bx, by + bh * 2.6f, bw, bh, "ОТКЛЮЧИТЬСЯ")) exitToMenu();
        drawText("ВЫ В ГОСТЯХ: " + g_mpWorld, g_W / 2.f, by + bh * 3.9f, fontPx() * 0.9f, C(180, 220, 255), 1);
    } else {
        if (button(63, bx, by + bh * 2.6f, bw, bh, g_netRole == NET_HOST ? "ЗАКРЫТЬ ДЛЯ СЕТИ" : "ОТКРЫТЬ ДЛЯ СЕТИ")) { if (g_netRole == NET_HOST) { netHostStop(); showMsg("СЕТЬ ЗАКРЫТА"); } else netHostStart(); }
        if (button(62, bx, by + bh * 3.9f, bw, bh, "СОХРАНИТЬ И ВЫЙТИ")) exitToMenu();
        if (g_netRole == NET_HOST) {
            std::string a = "АДРЕС ДЛЯ ДРУЗЕЙ: " + (g_hostIp.empty() ? std::string("НЕТ WI-FI") : g_hostIp) + "   ИГРОКОВ: " + std::to_string(1 + g_remote.size());
            drawText(a, g_W / 2.f, by + bh * 5.2f, fontPx() * 0.9f, C(255, 236, 150), 1);
            std::string names; for (const RemotePlayer& r : g_remote) names += (names.empty() ? "" : ", ") + r.name;
            if (!names.empty()) drawText(names, g_W / 2.f, by + bh * 5.9f, fontPx() * 0.75f, C(220, 226, 236), 1);
        } else drawText("МИР СОХРАНЁН", g_W / 2.f, by + bh * 5.2f, fontPx() * 0.9f, C(180, 230, 170), 1);
    }
}
static void screenDead() {
    fillRect(0, 0, (float)g_W, (float)g_H, C(120, 0, 0, 120));
    float S = uiS();
    drawTitle("ВЫ ПОГИБЛИ", g_H * 0.25f, 3.5f);
    float bw = S * 70, bh = S * 11, bx = (g_W - bw) / 2, by = g_H * 0.5f;
    if (button(70, bx, by, bw, bh, "ВОЗРОДИТЬСЯ")) { respawnPlayer(); g_needSpawnY = false; g_screen = SCR_LOADING; }
    if (button(71, bx, by + bh * 1.3f, bw, bh, "В ГЛАВНОЕ МЕНЮ")) { respawnPlayer(); exitToMenu(); }
}
static float g_rowX = 0, g_rowW = 0;   // колонка строк настроек
static void settingRow(int id, float y, const std::string& name, const std::string& val, int& delta) {
    float S = uiS();
    float w = g_rowW, x = g_rowX, h = S * 8;
    roundRect(x, y, w, h, h * 0.22f, C(40, 43, 52, 215), C(26, 28, 34, 215));
    roundFrame(x, y, w, h, h * 0.22f, std::max(1.f, S * 0.15f), C(90, 96, 112, 150));
    float np = fontPx(); while (textWidth(name, np) > w - S * 46 && np > 1.2f) np *= 0.92f;
    drawText(name, x + S * 3, y + h / 2 - 3.5f * np, np, C(240, 240, 240));
    float bw = S * 12;
    if (button(id, x + w - bw * 3.6f, y, bw, h, "<")) delta = -1;
    drawText(val, x + w - bw * 2.05f, y + h / 2 - 3.5f * fontPx(), fontPx(), C(255, 230, 140), 1);
    if (button(id + 1, x + w - bw, y, bw, h, ">")) delta = 1;
}
static void screenSettings() {
    if (g_settingsBack == SCR_MAIN) drawBackdrop(g_time); else fillRect(0, 0, (float)g_W, (float)g_H, C(0, 0, 0, 170));
    float S = uiS();
    drawTitle("НАСТРОЙКИ", (float)insetTop(), 2.2f);
    float y0 = insetTop() + S * 11, y = y0, step = S * 9.4f;
    bool two = g_W > g_H * 1.5f;   // широкий экран — две колонки
    float colW = two ? std::min(S * 105, g_W * 0.45f) : std::min(S * 150, g_W * 0.9f);
    g_rowW = colW; g_rowX = two ? g_W / 2.f - colW - S * 2 : (g_W - colW) / 2;
    const int PER_COL = 8;
    auto nextCol = [&](int i) { if (two && i == PER_COL) { y = y0; g_rowX = g_W / 2.f + S * 2; } };
    if (two) step = std::min(S * 8.8f, (g_H - y0 - S * 12) / PER_COL);
    int d, row = 0;
    char b[32];
    d = 0; settingRow(80, y, "ДАЛЬНОСТЬ ПРОРИСОВКИ", std::to_string(std::max(g_set.viewDist, g_set.renderDist)) + " ЧАНК.", d);
    if (d) { g_set.viewDist = clampi(std::max(g_set.viewDist, g_set.renderDist) + d * (g_set.viewDist >= 16 ? 4 : 2), 4, 40); if (g_set.viewDist < g_set.renderDist) g_set.renderDist = g_set.viewDist; }
    y += step; nextCol(++row);
    d = 0; settingRow(70, y, "ПОЛНАЯ ДЕТАЛИЗАЦИЯ", std::to_string(g_set.renderDist) + " ЧАНК.", d); g_set.renderDist = clampi(g_set.renderDist + d, 4, std::min(12, std::max(4, g_set.viewDist))); y += step; nextCol(++row);
    d = 0; snprintf(b, sizeof(b), "%d%%", (int)lroundf(g_set.sens * 100)); settingRow(82, y, "ЧУВСТВИТЕЛЬНОСТЬ", b, d); g_set.sens = clampf(g_set.sens + d * 0.1f, 0.2f, 3.f); y += step; nextCol(++row);
    d = 0; snprintf(b, sizeof(b), "%d%%", (int)lroundf(g_set.volume * 100)); settingRow(84, y, "ГРОМКОСТЬ", b, d); g_set.volume = clampf(g_set.volume + d * 0.1f, 0.f, 1.f); g_masterVol = g_set.volume; y += step; nextCol(++row);
    d = 0; snprintf(b, sizeof(b), "%d%%", (int)lroundf(g_set.music * 100)); settingRow(78, y, "МУЗЫКА", b, d); g_set.music = clampf(g_set.music + d * 0.1f, 0.f, 1.f); y += step; nextCol(++row);
    d = 0; snprintf(b, sizeof(b), "%d%%", g_set.inset); settingRow(86, y, "ОТСТУП ОТ КРАЁВ (ВЫРЕЗ)", b, d); g_set.inset = clampi(g_set.inset + d, 0, 12); y += step; nextCol(++row);
    d = 0; snprintf(b, sizeof(b), "%d%%", (int)lroundf(g_set.uiScale * 100)); settingRow(88, y, "РАЗМЕР ТЕКСТА", b, d); g_set.uiScale = clampf(g_set.uiScale + d * 0.1f, 0.7f, 1.4f); y += step; nextCol(++row);
    d = 0; settingRow(90, y, "АВТОПРЫЖОК", g_set.autoJump ? "ВКЛ" : "ВЫКЛ", d); if (d) g_set.autoJump = !g_set.autoJump; g_autoJump = g_set.autoJump; y += step; nextCol(++row);
    { static const int RES[5] = {16, 32, 64, 128, 256};
      static int pendingRes = 0, pendingFrames = 0;
      int ri = 0; for (int i = 0; i < 5; i++) if (RES[i] == g_set.texRes) ri = i;
      d = 0; settingRow(96, y, "ДЕТАЛИЗАЦИЯ БЛОКОВ", std::to_string(g_set.texRes) + "X" + std::to_string(g_set.texRes) + (g_set.texRes == 256 ? " УЛЬТРА" : ""), d);
      if (d) { ri = clampi(ri + d, 0, 4); if (RES[ri] != g_set.texRes) { g_set.texRes = RES[ri]; pendingRes = RES[ri]; pendingFrames = 2; } }
      if (pendingRes) {   // сначала показываем «подожди», потом генерируем (на телефоне 256 в первый раз — до полуминуты, дальше из кэша)
        float bw = S * 110, bh = S * 22;
        roundRect((g_W - bw) / 2, g_H * 0.4f, bw, bh, S * 3, C(240, 242, 246, 250), C(210, 214, 222, 250));
        drawText("ГОТОВЛЮ ТЕКСТУРЫ...", g_W / 2.f, g_H * 0.4f + S * 5, fontPx() * 1.2f, C(40, 44, 54), 1);
        drawText(pendingRes >= 256 ? "ПЕРВЫЙ РАЗ ЭТО ДОЛГО, ПОТОМ БЫСТРО" : "СЕКУНДУ", g_W / 2.f, g_H * 0.4f + S * 13, fontPx() * 0.8f, C(80, 84, 96), 1);
        if (--pendingFrames <= 0) { applyTextureRes(); pendingRes = 0; }
      }
      y += step; nextCol(++row); }
    { static const char* SH[3] = {"ВЫКЛ", "СРЕДНИЕ", "УЛЬТРА"};
      d = 0; settingRow(94, y, "ШЕЙДЕРЫ", SH[g_set.shaders], d); g_set.shaders = (g_set.shaders + d + 3) % 3; g_shaders = g_set.shaders; y += step; nextCol(++row); }
    d = 0; settingRow(92, y, "ПОКАЗ FPS", g_set.showFps ? "ВКЛ" : "ВЫКЛ", d); if (d) g_set.showFps = !g_set.showFps; y += step; nextCol(++row);
    d = 0; snprintf(b, sizeof(b), "%d%%", (int)lroundf(g_set.padSens * 100)); settingRow(76, y, "ЧУВСТВ. ГЕЙМПАДА", b, d); g_set.padSens = clampf(g_set.padSens + d * 0.1f, 0.3f, 3.f); y += step; nextCol(++row);
    d = 0; settingRow(74, y, "ВИБРАЦИЯ ГЕЙМПАДА", g_set.rumble ? "ВКЛ" : "ВЫКЛ", d); if (d) { g_set.rumble = !g_set.rumble; padRumble(0.6f, 0.6f, 200); } y += step; nextCol(++row);
    d = 0; settingRow(72, y, "ИНВЕРСИЯ ОСИ Y", g_set.invertY ? "ВКЛ" : "ВЫКЛ", d); if (d) g_set.invertY = !g_set.invertY; y += step; nextCol(++row);
    d = 0; settingRow(68, y, "РАЗМЫТИЕ ДАЛИ", g_set.shaders == 0 ? "НУЖНЫ ШЕЙДЕРЫ" : (g_set.dof ? "ВКЛ" : "ВЫКЛ"), d); if (d && g_set.shaders > 0) g_set.dof = !g_set.dof; y += step;
    float by = two ? y0 + step * PER_COL + S * 1.5f : y + S * 2;
    if (button(99, (g_W - S * 40) / 2, std::min(by, g_H - S * 11), S * 40, S * 9, "НАЗАД")) { saveSettings(); g_screen = g_settingsBack; g_ui.focus = -1; }
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
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, 0);
    if (!g_ren) { SDL_Log("Renderer: %s", SDL_GetError()); return 1; }
    SDL_GetRendererOutputSize(g_ren, &g_W, &g_H);

    initBlocks(); genAtlas(); computeBlockAvg(); initLodTables(); initAnimTiles(); genFont(); initLightLUT(); initStars(); initRecipes(); initCreativeItems(); initOffsets();
    migrateOldSaves(); loadSettings();
    g_tileRes = g_set.texRes;
    applyTextureRes();
    migrateOldSaves();
    loadSettings();
    loadPlayerName();
    g_masterVol = g_set.volume; g_autoJump = g_set.autoJump;
    initAudio();
    nDetail.init(99);
    g_worldMutex = SDL_CreateMutex();
    SDL_AddEventWatch(appEventWatch, nullptr);
    for (int i = 0; i < SDL_NumJoysticks(); i++) openPad(i);
    memset(g_keys, 0, sizeof(g_keys));

#ifndef __ANDROID__
    if (const char* j = getenv("BW_JOIN")) { g_screen = SCR_MP; netConnect(j); }   // отладка: сразу в гости
    if (const char* ld = getenv("BW_LOAD")) continueWorld(atoi(ld));   // отладка: загрузить мир из ячейки
    if (const char* sc = getenv("BW_SCREEN")) { std::string v = sc; if (v == "settings") { g_settingsBack = SCR_MAIN; g_screen = SCR_SETTINGS; } else if (v == "mp") g_screen = SCR_MP; else if (v == "main") g_screen = SCR_MAIN; }
    // отладка на ПК: BW_START="зерно x z поворот наклон [время суток]" — сразу в новый мир в творческом режиме
    if (const char* st = getenv("BW_START")) {
        unsigned sd = 777; float x = 0, z = 0, yw = 0, pt = 0, tm = -1;
        sscanf(st, "%u %f %f %f %f %f", &sd, &x, &z, &yw, &pt, &tm);
        startNewWorld(MAX_WORLDS, sd, getenv("BW_SURV") ? GM_SURVIVAL : GM_CREATIVE);
        P.x = x + 0.5; P.z = z + 0.5; P.spawnX = (int)x; P.spawnZ = (int)z;
        g_cam.yaw = yw; g_cam.pitch = pt;
        if (tm >= 0) g_dayTime = tm;
        if (getenv("BW_HOST")) g_debugHost = true;
        if (const char* wx = getenv("BW_WEATHER")) { int w = atoi(wx); setWeather(w, 1000.f); g_rainK = w ? 1.f : 0.f; g_thunderK = w == 2 ? 1.f : 0.f; g_boltT = 1.f; }
    }
#endif
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
                case SDL_CONTROLLERDEVICEREMOVED: closePad(e.cdevice.which); break;
                case SDL_CONTROLLERBUTTONDOWN:
                    for (SDL_GameController* p : g_pads) if (padId(p) == e.cbutton.which) setActivePad(p);   // какой геймпад нажали — тот и главный
                    padButton(e.cbutton.button); break;
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
        g_time += dt; g_frameDt = dt;
        {   // музыка: в меню — дневная, в игре — по времени суток и месту
            bool inWorld = worldScreenActive() && g_screen != SCR_LOADING;
            if (!inWorld) g_underwaterAudio = 0;
            updateMusic(dt, inWorld ? musicMoodNow() : 0, !inWorld && g_screen != SCR_INTRO);
            updatePadLED(dt);
            g_musicVol = g_set.music * 0.9f;
        }
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
        if (g_screen == SCR_GAME || g_screen == SCR_INV || g_screen == SCR_DEAD || (g_netRole != NET_NONE && inWorld)) updateGame(dt);   // в сетевой игре пауза не останавливает мир
        netTick(dt);
        if (inWorld) { manageChunks(7.0, g_set.renderDist); g_viewDist = std::max(g_set.viewDist, g_set.renderDist); manageLod(2.5, P.x, P.z, g_set.renderDist); }
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
            case SCR_MP: screenMP(); break;
            default: break;
        }
        if (g_ui.pad && !g_touchUsed && g_screen != SCR_GAME && g_screen != SCR_INTRO && g_screen != SCR_LOADING) {   // подсказки кнопок в меню
            PadHint h[6]; int n = 0;
            h[n++] = {PG_A, "ВЫБРАТЬ"}; h[n++] = {PG_B, g_screen == SCR_MAIN ? "ВЫХОД" : "НАЗАД"};
            if (g_screen == SCR_INV) {
                h[n++] = {PG_X, "ПОЛОВИНА"};
                if (g_mode == GM_CREATIVE || g_bookOpen) h[n++] = {PG_LB, "СТРАНИЦЫ"};
                if (g_mode != GM_CREATIVE) h[n++] = {PG_BACK, "КНИГА"};
            }
            h[n++] = {PG_DPAD, "ВЫБОР"};
            float r = uiS() * 2.f;
            drawPadHints(h, n, g_W / 2.f, g_H - uiS() * 3.4f, r, 1, false);
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
    stopMusicThread();
    if (g_audioDev) SDL_CloseAudioDevice(g_audioDev);
    for (SDL_GameController* p : g_pads) SDL_GameControllerClose(p);
    g_pads.clear(); g_pad = nullptr;
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 0;
}
