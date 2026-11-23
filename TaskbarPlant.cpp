// =============================================================================
//  TaskbarPlant.cpp  ——  桌面电子盆栽（全屏自由移动版）
//
//  纯 Win32 + GDI+ + 自写 2D 物理，无任何第三方库 / 无 Electron。
//
//  实现要点：
//    1. 无边框 + WS_EX_LAYERED + UpdateLayeredWindow 做逐像素透明：任务栏以上的
//       整个工作区都是它的“活动范围”，只有花盆和植物是可见像素，透明区域的鼠标点击直接穿透。
//       窗口矩形只铺到任务栏上沿为止 —— 只要窗口矩形盖住整块显示器，Explorer 就会
//       把任务栏的 WS_EX_TOPMOST 去掉，之后活动窗口就会压到任务栏上面。
//    2. 物理边界不再是 380x280 的小窗口，而是任务栏以上的整块工作区；默认地面是任务栏上沿，
//       花盆掉到任意窗口的标题栏上时会被标题栏“接住”。
//    3. 自写物理：重力、地面碰撞、弹跳、旋转、拖拽甩出、落地扬尘。
//    4. 右键弹出自绘的像素风菜单：可切换 17 种植物、放置新花盆、移除花盆；
//       每个花盆都是独立刚体，可以同时养很多盆。
//    5. 定时器轮询前台窗口，若其覆盖整个显示器（全屏视频/游戏）则 SW_HIDE，
//       结束后 SW_SHOWNA 恢复（不抢焦点）。
//
//  编译（MSVC，x64 Native Tools 命令行）：
//    cl /nologo /EHsc /O2 /DUNICODE /D_UNICODE TaskbarPlant.cpp ^
//       /link gdiplus.lib user32.lib gdi32.lib dwmapi.lib /SUBSYSTEM:WINDOWS
//
//  编译（MinGW-w64）：
//    g++ -std=c++17 -O2 -municode -mwindows TaskbarPlant.cpp -o TaskbarPlant.exe ^
//        -lgdiplus -lgdi32 -luser32 -ldwmapi -static
//
//  依赖：GDI+（Windows 自带，链接 gdiplus.lib 即可，无需额外安装）。
// =============================================================================

#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <windowsx.h>
#include <objidl.h>      // GDI+ 头文件依赖 PROPID（WIN32_LEAN_AND_MEAN 下不会自动包含）
#include <gdiplus.h>
#include <dwmapi.h>      // DWMWA_CLOAKED：识别其它虚拟桌面上的窗口
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")

using Gdiplus::Graphics;
using Gdiplus::GraphicsState;
using Gdiplus::Brush;
using Gdiplus::SolidBrush;
using Gdiplus::Color;
using Gdiplus::RectF;
using Gdiplus::PointF;

// -----------------------------------------------------------------------------
// 常量
// -----------------------------------------------------------------------------
static const UINT_PTR TIMER_ANIM = 1;               // 60FPS 动画定时器
static const UINT_PTR TIMER_FULL = 2;               // 全屏检测定时器
static const UINT_PTR TIMER_DOCK = 3;               // 显示器/任务栏变化定时器
static const int      ANIM_INTERVAL = 16;           // ms
static const float    DT = 0.016f;                  // 固定物理步长（秒）

static const float    PIX         = 4.0f;           // 像素画“一个像素块”的边长
static const float    POT_W       = 72.0f;          // 花盆最大宽度（18 格）
static const float    POT_H       = 48.0f;          // 花盆高度（12 格）
static const float    BODY_HALF_W = 32.0f;          // 盆身半宽（16 格）

static const float    GRAVITY = 2400.0f;            // 重力加速度 px/s^2
static const float    RESTITUTION = 0.42f;          // 弹跳系数
static const float    PI_F = 3.14159265f;

// 花盆大小 / 水分
static const float    POT_SCALE_MIN = 0.65f;        // 花盆最小缩放
static const float    POT_SCALE_MAX = 1.45f;        // 花盆最大缩放
static const float    POT_SCALE_STEP = 0.1f;        // 每次调大/调小的步进
static const float    DRY_THRESHOLD = 0.40f;        // 水分低于它开始发蔫
static const float    WATER_DURATION = 2.2f;        // 浇水动画时长（秒）

// ---------------------------------------------------------------------------
// 常年微风（v3.6.0 新增）—— 让植物在没有交互时也轻轻摇。
// 这些参数是平台无关的纯数学，Linux 移植版照搬 WindForce() 与下面的
// 弹簧参数即可，不依赖任何 Win32 / GDI+ 接口。
// ---------------------------------------------------------------------------
static const float    WIND_SWAY_PERIOD  = 1.0f / 0.7f;   // 主体摆动周期（秒）
static const float    WIND_GUST_PERIOD  = 1.0f / 0.23f;  // 强弱起伏周期（秒，约 4.3s 一阵）
// 两列正弦叠加出非周期的风吹感，返回值直接加在摆动弹簧的速度上（px 级力矩）
static inline float WindForce(float t)
{
    float gust = 0.55f + 0.45f * sinf(t * 0.23f + 0.9f);              // 一阵强一阵弱
    return gust * (7.5f * sinf(t * 0.7f)
                 + 4.0f * sinf(t * 1.73f + 1.3f)
                 + 2.5f * sinf(t * 3.1f + 0.7f));
}

static const wchar_t* kWndClass = L"TaskbarPlant_Window";
static const wchar_t* kMenuClass = L"TaskbarPlant_Menu";

// 右键菜单（植物列表 + 命令）见下方“全局状态”

// -----------------------------------------------------------------------------
// 全局状态
// -----------------------------------------------------------------------------
enum PlantType {
    PLANT_CACTUS = 0,
    PLANT_SUNFLOWER,
    PLANT_FLYTRAP,
    PLANT_TULIP,
    PLANT_MUSHROOM,
    PLANT_PINE,
    PLANT_SUCCULENT,
    PLANT_BAMBOO,
    PLANT_ROSE,
    PLANT_LAVENDER,
    PLANT_CLOVER,
    PLANT_CHILI,
    PLANT_DANDELION,
    PLANT_DAISY,
    PLANT_ALOE,
    PLANT_MORNINGGLORY,
    PLANT_CHERRY,
    PLANT_COUNT
};

// 右键菜单：先是植物列表，再是命令
static const wchar_t* kVersionText = L"v3.8.3";   // 菜单底部的版本号（连点 7 次开调试模式）

enum MenuCmd {
    MENU_CMD_WATER = PLANT_COUNT,   // 浇水
    MENU_CMD_EDIT,                  // 编辑模式
    MENU_CMD_SCALE_UP,              // 花盆调大
    MENU_CMD_SCALE_DOWN,            // 花盆调小
    MENU_CMD_SPAWN,                 // 放置新花盆
    MENU_CMD_REMOVE,                // 移除这个花盆
    MENU_CMD_EXPORT,                // 导出植物图片
    MENU_CMD_QUIT,                  // 退出程序
    MENU_CMD_VERSION,               // 版本号（连点 7 次开调试模式）
    MENU_ITEM_COUNT
};

static const wchar_t* kMenuItemText[MENU_ITEM_COUNT] = {
    L"仙人掌", L"向日葵", L"食人花", L"郁金香", L"蘑菇",
    L"小松树", L"多肉", L"竹子", L"玫瑰",
    L"薰衣草", L"三叶草", L"辣椒", L"蒲公英",
    L"雏菊", L"芦荟", L"牵牛花", L"樱花",
    L"浇水", L"编辑模式", L"花盆调大", L"花盆调小",
    L"放置新花盆", L"移除这个花盆", L"导出植物图片", L"退出程序",
    kVersionText
};
static const int kMenuCount = MENU_ITEM_COUNT;
static const int kMenuW = 246;                            // 植物区双栏 + 命令区单栏
static const int kMenuColW = (kMenuW - 16) / 2;           // 植物格子宽度
static const int kPlantRows = (PLANT_COUNT + 1) / 2;      // 植物按两列排
static const int kMenuRowH = 30;
static const int kMenuSepH = 12;                          // 植物区与命令区之间的分隔带
static const int kMenuPad = 8;

// 花盆刚体：b.y 表示“花盆底部中心”的 y，angle 绕花盆几何中心旋转
struct Body {
    float x, y;          // 窗口客户区坐标
    float vx, vy;
    float angle;         // 弧度
    float angVel;
    bool  dragged;
};

struct Crumb {           // 落地扬起的泥土碎屑
    float x, y, vx, vy, life, floorY;
};

// 一个花盆实例（花盆 + 植物 + 各自的物理与交互状态）
struct Pot {
    Body      body;
    PlantType plant;
    float     sway;       // 植物摆动量（>0 向右弯）
    float     swayVel;
    bool      poking;     // 正在被拨弄
    float     dragOffX, dragOffY;
    float     surfaceY;   // 当前“脚下”的承托面
    HWND      supportHwnd;
    float     supportY;
    float     scale;      // 花盆整体大小（POT_SCALE_MIN ~ POT_SCALE_MAX）
    float     moisture;   // 泥土水分 1=湿透 0=干透
    float     waterAnim;  // 浇水动画剩余时间（秒），0 表示没在浇水
    float     shedTimer;  // 枯叶掉落节流
    float     windPh;     // 常年微风的相位偏移（每盆错开，避免多盆整齐划一地摇）
    int       slot;       // 在已保存布局 g_savedLayout 里的槽位；-1 = 还没保存过（运行时新放的盆）
};

static HWND  g_hwnd = nullptr;
static HWND  g_menu = nullptr;
static int   g_menuHover = -1;
static bool  g_menuOpen = false;
static bool  g_visible = true;

// 悬浮层覆盖整个虚拟桌面
static int   g_winX = 0, g_winY = 0, g_winW = 0, g_winH = 0;
static float g_floorY = 0.0f;        // 客户区坐标下的默认地面Y（任务栏上沿）

static HDC     g_memDC = nullptr;     // 离屏 DC
static HBITMAP g_dib = nullptr;       // 32bpp 预乘 DIB
static void*   g_bits = nullptr;

static ULONG_PTR g_gdipToken = 0;
static HFONT     g_menuFont = nullptr;

static std::vector<Pot>  g_pots;         // 场景里所有花盆
static int               g_activePot = -1;  // 右键菜单 / 拖拽针对的花盆
static float             g_spawnX = 0.0f, g_spawnY = 0.0f;  // “放置新花盆”的位置
static float             g_time = 0.0f;
static std::vector<Crumb> g_crumbs;

// 下次启动要恢复的花盆状态（配置文件里读出来的）
struct PotSeed { int plant; float x, y, scale, moisture; };
static std::vector<PotSeed> g_seeds;
// 已保存的布局（v3.8.3）：ini 里写的就是这份快照。只有编辑模式里点「保存」才会
// 用当前场景整体覆盖它；自动保存 / 退出保存只更新其中各盆的水分，位置永远不动——
// 拖动、抛掷、放新盆、移除盆只有在编辑模式里保存后才会持久化
static std::vector<PotSeed> g_savedLayout;
static float g_saveTimer = 0.0f;         // 自动保存节流

// 干枯时从植物上掉下来的枯叶碎片（带物理）
struct WiltParticle { float x, y, vx, vy, life; };
static std::vector<WiltParticle> g_wilt;

// ---------------------------------------------------------------------------
// 天气系统（v3.7 新增）：雨 / 雪 / 风雨加大 / 积水 / 积雪
// 平台无关：只依赖 g_time / g_floorY / g_pots / 4px 像素助手函数，
// Linux 移植版把这一段照搬即可（详见 README 的移植同步章节）。
// ---------------------------------------------------------------------------
enum WeatherKind { WEATHER_CLEAR = 0, WEATHER_RAIN, WEATHER_SNOW };

struct RainDrop  { float x, y, vy; };                 // 竖直下落的雨线
struct SnowFlake { float x, y, vy, ph, amp; };        // 横向摇摆着飘的雪花
struct Splash    { float x, y, vx, vy, life; };       // 落水 / 砸盆溅起的水珠

static const float PUDDLE_MAX = 6.0f;                 // 积水最大深度 px
static const float SNOW_MAX   = 6.0f;                 // 积雪最大厚度 px
static const float RAIN_SPEED_MIN = 900.0f;           // 雨滴下落速度 px/s
static const float RAIN_SPEED_MAX = 1250.0f;

static int   g_weather      = WEATHER_CLEAR;
static float g_weatherTimer = 25.0f;    // 晴天时距下一次“掷骰子”的倒计时（秒）
static float g_weatherLeft  = 0.0f;     // 当前降水的剩余时间（秒）
static float g_storm        = 0.0f;     // 风雨增幅 0~1（降水时平滑升到 1）
static float g_puddle       = 0.0f;     // 任务栏上沿的积水深度 px
static float g_snowCover    = 0.0f;     // 地面与植物上的积雪厚度 px
static std::vector<RainDrop>  g_rain;
static std::vector<SnowFlake> g_flakes;
static std::vector<Splash>    g_splash;

static float g_precip = 0.0f;             // 降水强度 0~1（起雨 / 停雨平滑过渡）
static int   g_lastWeather = WEATHER_CLEAR;  // 上一次的天气（用于切换时清空另一种粒子）

// 编辑模式：此时改动只在内存里，点“保存”才写入 ini
static bool  g_editMode = false;
static float g_drySeconds = 1800.0f;     // 干枯时间（秒），默认 30 分钟，可在编辑模式里改
static float g_hudMsgTimer = 0.0f;       // “已保存”提示
static int   g_hudHot = -1;              // 悬停的按钮

static int   g_hudX = 0, g_hudY = 0, g_hudW = 0, g_hudH = 0;
static const int HUD_PAD = 12;
static const int HUD_BTN_W = 150;
static const int HUD_BTN_H = 40;
static const int HUD_GAP = 8;

enum HudBtn { HUD_ADD = 0, HUD_DRY, HUD_SAVE, HUD_DONE, HUD_BTN_COUNT };

// 隐藏调试模式：右键菜单底部的版本号连点 7 次开启，
// 提供「干枯 / 下雨 / 下雪 / 关闭调试模式」。关闭后不留任何持久状态。
enum DebugBtn { DBG_DRY = 0, DBG_RAIN, DBG_SNOW, DBG_OFF, DBG_BTN_COUNT };
static bool  g_debugMode = false;
static int   g_debugHot = -1;
static int   g_versionClicks = 0;
static float g_versionClickTimer = 0.0f;    // 两次点击间隔超过 3 秒就清零（静默计数，无提示）

// 顶部提示条（如：保存配置时有花盆在标题栏上）
static wchar_t g_toastText[128] = { 0 };
static float   g_toastTimer = 0.0f;

static float g_mouseX = 0.0f, g_mouseY = 0.0f;

// 脏矩形重绘（全屏缓冲逐帧全清代价太大，只清理上一帧/这一帧的包围盒）
struct RectI { int l, t, r, b; };
static RectI g_prevDirty = { 0, 0, 0, 0 };
static bool  g_hasPrevDirty = false;
static bool  g_fullRedraw = true;

// -----------------------------------------------------------------------------
// 小工具
// -----------------------------------------------------------------------------
static inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline Color  C(int r, int g, int b, int a = 255) { return Color((BYTE)a, (BYTE)r, (BYTE)g, (BYTE)b); }

static void FillRectF(Graphics& g, Brush* br, float x, float y, float w, float h)
{
    if (w < 0.0f) { x += w; w = -w; }
    if (h < 0.0f) { y += h; h = -h; }
    if (w <= 0.0f || h <= 0.0f) return;
    g.FillRectangle(br, RectF(x, y, w, h));
}

// ---- 像素画基础：所有植物的边都吸附到 4px 网格上，禁止出现平滑斜边 ----

// 吸附到最近的像素格
static inline float SnapPx(float v) { return PIX * floorf(v / PIX + 0.5f); }

// 像素椭圆：按 4px 行逐行栅格化，每行宽度取整到像素格，边缘天然是阶梯
static void FillPixelEllipse(Graphics& g, Brush* br, float cx, float cy, float rx, float ry)
{
    float top  = SnapPx(cy - ry);
    int   rows = (int)floorf((2.0f * ry) / PIX + 0.5f);
    for (int i = 0; i < rows; ++i) {
        float y  = top + (float)i * PIX;
        float dy = y + PIX * 0.5f - cy;
        float k  = 1.0f - (dy * dy) / (ry * ry);
        if (k <= 0.0f) continue;
        float hw = SnapPx(rx * sqrtf(k));
        if (hw < PIX) hw = PIX;
        FillRectF(g, br, SnapPx(cx - hw), y, hw * 2.0f, PIX);
    }
}

// 像素叶片（v3.7 重绘）：基部饱满 -> 中段收窄 -> 末端收成 4px 尖，
// 叶尖逐渐放平并微微下垂，比旧版“一刀切”的阶梯叶更有机
// shade：叶底暗色描边（可选），vein：叶脉高光（可选）
static void DrawPixelLeaf(Graphics& g, float x, float y, int dir,
                          float len, float span, float halfW, Brush* br, Brush* vein,
                          Brush* shade = nullptr)
{
    int rows = (int)floorf(len / PIX + 0.5f);
    if (rows < 3) rows = 3;
    float y0 = SnapPx(y);
    for (int i = 0; i < rows; ++i) {
        float k = (float)(i + 1) / (float)rows;

        // 宽度：前 70% 缓慢收窄（饱满），后 30% 收成尖
        float hw;
        if (k < 0.7f) hw = halfW * (1.0f - k * 0.42f);
        else          hw = halfW * (1.0f - 0.7f * 0.42f) * (1.0f - (k - 0.7f) / 0.3f);
        hw = SnapPx(hw);
        if (hw < PIX * 0.5f) hw = PIX * 0.5f;

        // 横向：前 70% 匀速斜伸，后 30% 越来越平（横向步距加大）
        float fx;
        if (k <= 0.7f) fx = k / 0.7f * 0.7f;
        else           fx = 0.7f + (k - 0.7f) / 0.3f * 0.3f;
        float cx = SnapPx(x + (float)dir * span * fx);

        // 纵向：前 70% 每行上爬 4px，后 30% 每行只爬一半，叶尖放平下垂
        float rise = (k <= 0.7f) ? (float)(i + 1) * PIX
                                 : 0.7f * len + ((float)i + 1 - 0.7f * (float)rows) * PIX * 0.5f;
        float yy = y0 - rise;

        FillRectF(g, br, cx - hw, yy, hw * 2.0f, PIX);
        if (shade) FillRectF(g, shade, cx - hw, yy + PIX, hw * 2.0f, PIX * 0.5f); // 叶底阴影
        if (vein && i * 2 < rows)
            FillRectF(g, vein, cx - PIX * 0.5f, yy, PIX, PIX);
    }
}

// =============================================================================
//  一、像素风绘制（全部在“花盆局部坐标系”里画：原点＝花盆几何中心）
// =============================================================================

// 花盆本体：整块像素格拼贴，左右阶梯收腰
static void DrawPotLocal(Graphics& g)
{
    const float hw = POT_W * 0.5f;   // 36
    const float hh = POT_H * 0.5f;   // 24

    SolidBrush body(C(174, 100, 58));
    SolidBrush bodyDark(C(122, 64, 32));
    SolidBrush bodyLight(C(206, 132, 84));
    SolidBrush rim(C(208, 126, 70));
    SolidBrush rimDark(C(144, 76, 38));
    SolidBrush rimLight(C(234, 166, 110));
    SolidBrush soil(C(56, 36, 22));
    SolidBrush soilLight(C(98, 70, 46));

    // ---- 盆身：9 行像素块，逐层内收，形成阶梯轮廓 ----
    const int rows = 9;
    const int insets[9] = { 0, 0, 0, 1, 1, 1, 2, 2, 3 };
    for (int i = 0; i < rows; ++i) {
        float y  = -hh + 12.0f + (float)i * PIX;
        float x0 = -BODY_HALF_W + (float)insets[i] * PIX;
        float x1 =  BODY_HALF_W - (float)insets[i] * PIX;
        FillRectF(g, &body, x0, y, x1 - x0, PIX);            // 主色块
        FillRectF(g, &bodyDark, x1 - PIX, y, PIX, PIX);      // 右侧暗面
        FillRectF(g, &bodyLight, x0, y, PIX, PIX);           // 左侧高光
    }

    // 盆身像素纹理：两列砖缝
    for (int i = 1; i < rows; i += 2) {
        float y = -hh + 12.0f + (float)i * PIX;
        int inset = insets[i];
        float x0 = -BODY_HALF_W + (float)inset * PIX;
        FillRectF(g, &bodyDark, x0 + 3 * PIX, y, PIX, PIX);
        FillRectF(g, &bodyDark, x0 + 8 * PIX, y + PIX, PIX, PIX);
    }

    // ---- 盆沿：3 行像素块，比盆身宽一格 ----
    FillRectF(g, &rim,      -hw, -hh,                POT_W, 2 * PIX);   // 上沿
    FillRectF(g, &rimDark,  -hw, -hh + 2 * PIX,      POT_W, PIX);       // 下沿暗面
    FillRectF(g, &rimLight, -hw, -hh,                PIX, 2 * PIX);     // 左侧高光

    // 盆沿锯齿缺口（像素画常见手法）
    FillRectF(g, &rimDark, -hw + PIX,   -hh + PIX, PIX, PIX);
    FillRectF(g, &rimDark,  hw - 2 * PIX, -hh + PIX, PIX, PIX);
    FillRectF(g, &rimLight, hw - 3 * PIX, -hh,       PIX, PIX);

    // ---- 泥土：盆沿内侧 ----
    FillRectF(g, &soil,      -hw + 2 * PIX, -hh + PIX, 14 * PIX, PIX);
    FillRectF(g, &soilLight, -hw + 3 * PIX, -hh + PIX, 2 * PIX, PIX);
    FillRectF(g, &soilLight, -hw + 8 * PIX, -hh + 2 * PIX, PIX, 1.0f);
    FillRectF(g, &soilLight,  hw - 5 * PIX, -hh + PIX, PIX, PIX);
}

// 像素茎：逐格往上堆，越靠近顶端摆得越多；返回茎尖坐标
static PointF DrawBentStem(Graphics& g, float baseX, float baseY, float height,
                           float sway, Brush* br, float width)
{
    float w  = SnapPx(width);
    if (w < PIX) w = PIX;
    int   rows = (int)floorf(height / PIX + 0.5f);
    float y0   = SnapPx(baseY);
    float bx   = SnapPx(baseX);
    float tipX = bx;
    for (int i = 0; i < rows; ++i) {
        float k = (float)(i + 1) / (float)rows;
        tipX = SnapPx(baseX + sway * 46.0f * powf(k, 1.9f));
        FillRectF(g, br, tipX - w * 0.5f, y0 - (float)(i + 1) * PIX, w, PIX);
    }
    return PointF(tipX, y0 - (float)rows * PIX);
}

// 叶子（像素版）：v3.6 起带微风颤动，v3.7 重绘形 + 叶底阴影
static void DrawLeaf(Graphics& g, float soilY, float height, float p, float sway, int dir)
{
    SolidBrush leaf(C(100, 178, 72));
    SolidBrush vein(C(64, 126, 48));
    SolidBrush shade(C(56, 112, 44));

    float x = sway * 46.0f * powf(p, 1.9f);
    float y = soilY - height * p;
    float flutter = sinf(g_time * 1.9f + height * 0.11f + (float)dir * 2.0f)
                  * PIX * 2.0f * p;                 // 越靠叶尖颤幅越大
    DrawPixelLeaf(g, x + flutter, y + PIX * 2.0f, dir, 26.0f, 30.0f, 9.0f,
                  &leaf, &vein, &shade);
}

// 向日葵花盘：花瓣是 16x16 的像素块，花心是栅格化的像素椭圆
static void DrawSunflowerHead(Graphics& g, float cx, float cy, float sway, float t)
{
    SolidBrush petalA(C(250, 198, 46));
    SolidBrush petalB(C(232, 158, 26));
    SolidBrush core(C(120, 72, 30));
    SolidBrush coreDark(C(88, 50, 20));
    SolidBrush seed(C(58, 34, 14));

    GraphicsState st = g.Save();
    g.TranslateTransform(SnapPx(cx), SnapPx(cy));
    g.RotateTransform(sway * 12.0f + sinf(t * 1.3f) * 3.0f);

    const int n = 12;
    for (int i = 0; i < n; ++i) {
        float a  = (float)i * (2.0f * PI_F) / (float)n;
        float px = SnapPx(cosf(a) * 24.0f);
        float py = SnapPx(sinf(a) * 24.0f);
        Brush* br = (i & 1) ? (Brush*)&petalB : (Brush*)&petalA;
        FillRectF(g, br, px - 6.0f, py - 6.0f, 12.0f, 12.0f);
    }
    FillPixelEllipse(g, &core, 0.0f, 0.0f, 14.0f, 14.0f);
    FillPixelEllipse(g, &coreDark, 0.0f, 8.0f, 10.0f, 4.0f);

    for (int i = -1; i <= 1; ++i)
        for (int j = -1; j <= 1; ++j)
            FillRectF(g, &seed, (float)i * 8.0f - 2.0f, (float)j * 8.0f - 2.0f, 4.0f, 4.0f);

    g.Restore(st);
}

// 向日葵
static void DrawSunflower(Graphics& g, float soilY, float sway, float t)
{
    SolidBrush stem(C(76, 142, 58));
    const float h = 108.0f;

    PointF tip = DrawBentStem(g, 0.0f, soilY, h, sway, &stem, 7.0f);
    DrawLeaf(g, soilY, h, 0.382f, sway, -1);    // 叶位取黄金分割点 1/φ² 与 1/φ
    DrawLeaf(g, soilY, h, 0.618f, sway, +1);
    DrawSunflowerHead(g, tip.X, tip.Y - 10.0f, sway, t);
}

// 仙人掌：全部按 4px 格子拼，圆弧用栅格化的像素椭圆
static void DrawCactus(Graphics& g, float soilY, float sway)
{
    const float h = 92.0f;

    SolidBrush body(C(82, 158, 82));
    SolidBrush dark(C(54, 116, 56));
    SolidBrush light(C(122, 200, 116));
    SolidBrush spike(C(226, 244, 198));
    SolidBrush petal(C(242, 106, 140));
    SolidBrush petal2(C(252, 176, 200));

    GraphicsState st = g.Save();
    g.TranslateTransform(0.0f, SnapPx(soilY));
    g.RotateTransform(sway * 14.0f);

    // 主干：24 宽，左 4px 高光、右 8px 暗面
    FillRectF(g, &body,  -12.0f, -h, 24.0f, h + PIX);
    FillRectF(g, &dark,    4.0f, -h,  8.0f, h + PIX);
    FillRectF(g, &light, -12.0f, -h,  4.0f, h + PIX);
    FillPixelEllipse(g, &body, 0.0f, -h, 12.0f, 8.0f);
    FillPixelEllipse(g, &light, -8.0f, -h - 4.0f, 4.0f, 4.0f);

    // 左臂
    FillRectF(g, &body,   -28.0f, -64.0f, 16.0f, 12.0f);
    FillRectF(g, &body,   -28.0f, -88.0f, 12.0f, 28.0f);
    FillRectF(g, &dark,   -20.0f, -88.0f,  4.0f, 28.0f);
    FillPixelEllipse(g, &body, -22.0f, -88.0f, 6.0f, 6.0f);

    // 右臂
    FillRectF(g, &body,    12.0f, -48.0f, 16.0f, 12.0f);
    FillRectF(g, &body,    16.0f, -72.0f, 12.0f, 28.0f);
    FillRectF(g, &dark,    24.0f, -72.0f,  4.0f, 28.0f);
    FillPixelEllipse(g, &body, 22.0f, -72.0f, 6.0f, 6.0f);

    // 刺：一格一个
    for (int i = 0; i < 9; ++i) {
        float y = SnapPx(-h + 8.0f + (float)i * 8.0f);
        FillRectF(g, &spike, -16.0f, y, 4.0f, 4.0f);
        FillRectF(g, &spike,  12.0f, y + 4.0f, 4.0f, 4.0f);
    }

    // 顶端小花
    FillRectF(g, &petal,  -8.0f, -h - 20.0f, 16.0f, 8.0f);
    FillRectF(g, &petal, -12.0f, -h - 16.0f, 24.0f, 8.0f);
    FillRectF(g, &petal2, -4.0f, -h - 16.0f,  8.0f, 4.0f);

    g.Restore(st);
}

// 食人花（捕蝇草）
static void DrawFlytrap(Graphics& g, float soilY, float sway, float t, float extraOpen)
{
    SolidBrush stem(C(74, 138, 58));
    SolidBrush headFill(C(112, 178, 68));
    SolidBrush headDark(C(76, 134, 48));
    SolidBrush mouth(C(150, 32, 52));
    SolidBrush teeth(C(240, 244, 214));

    const float h = 96.0f;
    PointF tip = DrawBentStem(g, 0.0f, soilY, h, sway, &stem, 8.0f);

    // 嘴巴开合：平时轻微呼吸，被拨弄时张大
    float open = 0.30f + 0.16f * sinf(t * 2.2f) + extraOpen;
    open = Clampf(open, 0.05f, 1.0f);

    GraphicsState st = g.Save();
    g.TranslateTransform(SnapPx(tip.X), SnapPx(tip.Y) - PIX);
    g.RotateTransform(sway * 24.0f);

    // 下颚：栅格化的像素椭圆
    FillPixelEllipse(g, &headFill, 0.0f, 8.0f, 22.0f, 11.0f);
    FillRectF(g, &headDark, -16.0f, 14.0f, 32.0f, PIX);

    // 口腔
    FillPixelEllipse(g, &mouth, 0.0f, 2.0f, 12.0f, 6.0f);

    // 下齿：朝上的一排 4x4 方块
    for (int i = 0; i < 5; ++i)
        FillRectF(g, &teeth, -16.0f + (float)i * 8.0f, 0.0f, 4.0f, 4.0f);

    // 上颚（绕合页旋转打开）
    GraphicsState st2 = g.Save();
    g.RotateTransform(-open * 34.0f);
    FillPixelEllipse(g, &headFill, 0.0f, -8.0f, 22.0f, 11.0f);
    FillRectF(g, &headDark, -16.0f, -14.0f, 32.0f, PIX);
    // 上齿：朝下的一排 4x4 方块
    for (int i = 0; i < 5; ++i)
        FillRectF(g, &teeth, -16.0f + (float)i * 8.0f, -4.0f, 4.0f, 4.0f);
    g.Restore(st2);

    g.Restore(st);
}

// 郁金香
static void DrawTulip(Graphics& g, float soilY, float sway, float t)
{
    SolidBrush stem(C(84, 150, 62));
    SolidBrush leafA(C(102, 176, 74));
    SolidBrush leafB(C(72, 138, 54));
    SolidBrush petalA(C(226, 78, 106));
    SolidBrush petalB(C(196, 52, 84));
    SolidBrush petalC(C(250, 148, 168));

    const float h = 88.0f;
    PointF tip = DrawBentStem(g, 0.0f, soilY, h, sway, &stem, 8.0f);

    // 两片细长叶：阶梯像素叶
    GraphicsState ls = g.Save();
    g.TranslateTransform(0.0f, SnapPx(soilY));
    g.RotateTransform(sway * 10.0f);
    DrawPixelLeaf(g, 0.0f,  0.0f, -1, 40.0f, 24.0f, 6.0f, &leafA, nullptr);
    DrawPixelLeaf(g, 0.0f, -8.0f, +1, 36.0f, 24.0f, 6.0f, &leafB, nullptr);
    g.Restore(ls);

    // 花苞：像素块拼成的杯形，上面两个花瓣尖
    GraphicsState st = g.Save();
    g.TranslateTransform(SnapPx(tip.X), SnapPx(tip.Y));
    g.RotateTransform(sway * 16.0f + sinf(t * 1.7f) * 2.0f);

    FillRectF(g, &petalA, -16.0f, -28.0f, 32.0f, 20.0f);
    FillRectF(g, &petalB, -16.0f, -28.0f,  4.0f, 20.0f);
    FillRectF(g, &petalB,  12.0f, -28.0f,  4.0f, 20.0f);
    FillRectF(g, &petalA, -16.0f, -36.0f, 12.0f,  8.0f);
    FillRectF(g, &petalA,   4.0f, -36.0f, 12.0f,  8.0f);
    FillRectF(g, &petalB,  -4.0f, -32.0f,  8.0f,  4.0f);
    FillRectF(g, &petalC,  -4.0f, -24.0f,  8.0f, 12.0f);
    FillRectF(g, &petalA, -12.0f,  -8.0f, 24.0f,  4.0f);

    g.Restore(st);
}

// 蘑菇
static void DrawMushroom(Graphics& g, float soilY, float sway, float t)
{
    SolidBrush stalk(C(238, 226, 202));
    SolidBrush stalkShade(C(206, 190, 164));
    SolidBrush capA(C(212, 62, 62));
    SolidBrush capB(C(168, 38, 44));
    SolidBrush spot(C(250, 240, 222));
    SolidBrush gill(C(196, 170, 140));

    const float h = 62.0f;

    GraphicsState st = g.Save();
    g.TranslateTransform(0.0f, SnapPx(soilY));
    g.RotateTransform(sway * 9.0f + sinf(t * 1.1f) * 1.5f);

    // 菌柄：3 格宽
    FillRectF(g, &stalk,      -8.0f, -h + 12.0f, 16.0f, h - 12.0f);
    FillRectF(g, &stalkShade,  4.0f, -h + 12.0f,  4.0f, h - 12.0f);

    // 菌褶
    FillRectF(g, &gill, -20.0f, -h + 12.0f, 40.0f, 4.0f);

    // 菌盖：逐行加宽的像素穹顶
    FillRectF(g, &capB, -24.0f, -h + 8.0f, 48.0f, 4.0f);
    FillRectF(g, &capA, -24.0f, -h + 4.0f, 48.0f, 4.0f);
    FillRectF(g, &capA, -20.0f, -h,        40.0f, 4.0f);
    FillRectF(g, &capA, -16.0f, -h - 4.0f, 32.0f, 4.0f);
    FillRectF(g, &capA, -12.0f, -h - 8.0f, 24.0f, 4.0f);
    FillRectF(g, &capA,  -8.0f, -h - 12.0f, 16.0f, 4.0f);
    FillRectF(g, &capA,  -4.0f, -h - 16.0f, 8.0f, 4.0f);

    // 白斑
    FillRectF(g, &spot, -16.0f, -h - 4.0f, 8.0f, 4.0f);
    FillRectF(g, &spot, -12.0f, -h - 8.0f, 4.0f, 4.0f);
    FillRectF(g, &spot,   8.0f, -h + 4.0f, 4.0f, 4.0f);

    g.Restore(st);
}

// 小松树：四层阶梯树冠 + 尖顶，整棵树对齐 4px 像素格
static void DrawPine(Graphics& g, float soilY, float sway, float t)
{
    SolidBrush trunk(C(122, 82, 46));
    SolidBrush trunkDark(C(92, 58, 30));
    SolidBrush tierA(C(52, 124, 70));
    SolidBrush tierB(C(38, 98, 56));
    SolidBrush tierC(C(86, 166, 98));
    SolidBrush tierShade(C(28, 76, 46));

    GraphicsState st = g.Save();
    g.TranslateTransform(0.0f, soilY);
    g.RotateTransform(sway * 5.0f + sinf(t * 0.9f) * 1.6f);

    // 树干：树冠下方露出来的一段
    FillRectF(g, &trunk,     -6.0f, -38.0f, 12.0f, 38.0f);
    FillRectF(g, &trunkDark,  2.0f, -38.0f,  4.0f, 38.0f);

    // 三层树冠：每层由 4px 高的横条堆成，越往上越窄（阶梯轮廓）
    struct Tier { float baseY, halfW, height; };
    const Tier tiers[3] = {
        { -34.0f, 34.0f, 36.0f },
        { -66.0f, 26.0f, 34.0f },
        { -96.0f, 19.0f, 32.0f }
    };
    for (int i = 0; i < 3; ++i) {
        const Tier& tr = tiers[i];
        int rows = (int)(tr.height / PIX);
        // 越高摆动越大，树梢最活
        float dx = sway * (5.0f + (float)i * 4.0f);
        for (int s = 0; s < rows; ++s) {
            float k  = (float)s / (float)rows;               // 0=层底 1=层顶
            float hw = tr.halfW * (1.0f - k * 0.84f);
            hw = PIX * floorf(hw / PIX + 0.5f);              // 对齐像素格，避免竖条毛刺
            if (hw < PIX) hw = PIX;
            float y = tr.baseY - (float)s * PIX;
            FillRectF(g, (i & 1) ? (Brush*)&tierB : (Brush*)&tierA,
                      dx - hw, y - PIX, hw * 2.0f, PIX);
            FillRectF(g, &tierC, dx - hw, y - PIX, PIX, PIX);   // 左侧受光格
        }
        // 层底压暗，层次分明
        FillRectF(g, &tierShade, dx - tr.halfW, tr.baseY - PIX, tr.halfW * 2.0f, PIX);
    }

    // 尖顶
    float tipDx = sway * 26.0f;
    FillRectF(g, &tierA, tipDx - 10.0f, -132.0f, 20.0f, PIX);
    FillRectF(g, &tierA, tipDx -  6.0f, -136.0f, 12.0f, PIX);
    FillRectF(g, &tierB, tipDx -  3.0f, -140.0f,  6.0f, PIX);

    g.Restore(st);
}

// 多肉：莲座（俯视略压扁）
// 做法：不做平滑填充，而是在 4x4 像素格上按“极坐标分区”逐格判定——
// 每格属于哪一圈、哪一片叶子，就填那片叶子的颜色；叶片之间留一格深色缝，
// 外轮廓按叶片尖端起伏，得到真正的像素莲座。
static void DrawSucculent(Graphics& g, float soilY, float sway, float t)
{
    SolidBrush edge(C(52, 96, 66));
    SolidBrush outLit(C(176, 216, 158));     // 左上受光的外圈叶
    SolidBrush outShade(C(88, 142, 98));     // 右下背光的外圈叶
    SolidBrush inLit(C(210, 240, 184));      // 内圈叶更亮
    SolidBrush inShade(C(128, 184, 122));
    SolidBrush core(C(234, 250, 210));
    SolidBrush coreHi(C(250, 255, 234));
    Brush* pal[7] = { &edge, &outLit, &outShade, &inLit, &inShade, &core, &coreHi };

    const float SY   = 0.78f;                // 俯视压扁：判定时把 y 还原回去
    const float SX0  = -40.0f, SY0 = -36.0f;
    const int   COLS = 20,     ROWS = 18;

    int cell[18][20];
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            float ux  = SX0 + ((float)c + 0.5f) * PIX;
            float uy  = (SY0 + ((float)r + 0.5f) * PIX) / SY;
            float rad = sqrtf(ux * ux + uy * uy);
            float ang = atan2f(ux, -uy);     // 0 = 正上，顺时针为正
            int col = -1;

            // 外圈 8 片：径向越长，叶片尖端越外凸；叶缝处收进去一截
            const float stepOut = 2.0f * PI_F / 8.0f;
            float u = ang / stepOut;
            u -= floorf(u + 0.5f);                              // [-0.5, 0.5)
            float rOut = 33.0f + 6.0f * (1.0f - 2.0f * fabsf(u));
            if (rad < rOut)
                col = (fabsf(u) > 0.38f) ? 0 : ((ang <= 0.0f) ? 1 : 2);

            // 内圈 5 片：错开半格，压在外圈上面
            const float stepIn = 2.0f * PI_F / 5.0f;
            float u2 = ang / stepIn + 0.5f;
            u2 -= floorf(u2 + 0.5f);
            float rIn = 23.0f + 4.0f * (1.0f - 2.0f * fabsf(u2));
            if (rad < rIn)
                col = (fabsf(u2) > 0.40f) ? 0 : ((ang <= 0.0f) ? 3 : 4);

            // 花心
            if (rad < 11.0f) col = (rad < 6.5f) ? 6 : 5;

            cell[r][c] = col;
        }
    }

    // ---- 轮廓：空白格若与实心格相邻，就补成深色边 ----
    int out[18][20];
    for (int r = 0; r < ROWS; ++r) {
        for (int c = 0; c < COLS; ++c) {
            if (cell[r][c] >= 0) { out[r][c] = cell[r][c]; continue; }
            bool touch = (r > 0 && cell[r - 1][c] >= 0)
                      || (r + 1 < ROWS && cell[r + 1][c] >= 0)
                      || (c > 0 && cell[r][c - 1] >= 0)
                      || (c + 1 < COLS && cell[r][c + 1] >= 0);
            out[r][c] = touch ? 0 : -1;
        }
    }

    // ---- 绘制：逐行合并同色格 ----
    GraphicsState st = g.Save();
    g.TranslateTransform(0.0f, SnapPx(soilY) - 32.0f);
    g.RotateTransform(sway * 3.0f + sinf(t * 0.7f) * 0.8f);

    for (int r = 0; r < ROWS; ++r) {
        int c = 0;
        while (c < COLS) {
            int v = out[r][c];
            if (v < 0) { ++c; continue; }
            int c2 = c;
            while (c2 + 1 < COLS && out[r][c2 + 1] == v) ++c2;
            FillRectF(g, pal[v], SX0 + (float)c * PIX, SY0 + (float)r * PIX,
                      (float)(c2 - c + 1) * PIX, PIX);
            c = c2 + 1;
        }
    }

    g.Restore(st);
}

// 竹子：三根高低错落的分节竹竿 + 小竹叶
static void DrawBamboo(Graphics& g, float soilY, float sway, float t)
{
    SolidBrush stalk(C(150, 194, 96));
    SolidBrush stalkDark(C(104, 150, 62));
    SolidBrush node(C(84, 126, 50));
    SolidBrush leaf(C(118, 182, 78));
    SolidBrush leafDark(C(84, 142, 56));

    struct Stalk { float x, h, ph; };
    // 三根竹竿高度按黄金比例递减：158 / 98 / 61（φ 链）
    const Stalk stalks[3] = {
        { -16.0f,  98.0f, 0.0f },
        {   0.0f, 158.0f, 1.2f },
        {  20.0f,  61.0f, 2.4f }
    };

    GraphicsState st = g.Save();
    g.TranslateTransform(0.0f, SnapPx(soilY));

    for (int i = 0; i < 3; ++i) {
        const Stalk& sk = stalks[i];
        // 每根竹子按高度轻微弯曲，越高摆得越多；整根对齐 4px 网格
        int segs = (int)(sk.h / 16.0f);
        for (int s = 0; s < segs; ++s) {
            float y0 = -(float)s * 16.0f;
            float k = (float)s / (float)segs;
            float dx = SnapPx(sk.x + (sway * 8.0f + sinf(t * 0.8f + sk.ph) * 1.0f) * k * k);
            FillRectF(g, &stalk,     dx - 6.0f, y0 - 16.0f, 12.0f, 16.0f);
            FillRectF(g, &stalkDark, dx + 2.0f, y0 - 16.0f,  4.0f, 16.0f);
            if (s > 0) FillRectF(g, &node, dx - 8.0f, y0 - 2.0f, 16.0f, 4.0f);   // 竹节
        }

        // 竹叶：两片阶梯像素叶，一左一右
        float dx = SnapPx(sk.x + sway * 8.0f + sinf(t * 0.8f + sk.ph) * 1.0f);
        float ty = -(float)segs * 16.0f;
        DrawPixelLeaf(g, dx,        ty + 16.0f, -1, 20.0f, 24.0f, 6.0f,
                      (i & 1) ? (Brush*)&leafDark : (Brush*)&leaf, nullptr);
        DrawPixelLeaf(g, dx,        ty + 20.0f, +1, 16.0f, 24.0f, 6.0f,
                      (i & 1) ? (Brush*)&leaf : (Brush*)&leafDark, nullptr);
    }

    g.Restore(st);
}

// 玫瑰：带刺的茎 + 层叠花瓣的花苞
static void DrawRose(Graphics& g, float soilY, float sway, float t)
{
    SolidBrush stem(C(74, 132, 62));
    SolidBrush thorn(C(58, 104, 48));
    SolidBrush petalDark(C(158, 32, 52));
    SolidBrush petal(C(206, 52, 78));
    SolidBrush petalLight(C(238, 106, 126));

    const float h = 124.0f;
    PointF tip = DrawBentStem(g, 0.0f, soilY, h, sway, &stem, 5.0f);

    // 叶子与刺（叶位取黄金分割点）
    DrawLeaf(g, soilY, h, 0.382f, sway, 1);
    DrawLeaf(g, soilY, h, 0.618f, sway, -1);
    FillRectF(g, &thorn, SnapPx(tip.X) - 4.0f, SnapPx(tip.Y) + 20.0f, 4.0f, 4.0f);
    FillRectF(g, &thorn, SnapPx(tip.X) + 4.0f, SnapPx(tip.Y) + 40.0f, 4.0f, 4.0f);

    // 花苞：外深内亮的层叠像素块
    GraphicsState st = g.Save();
    g.TranslateTransform(SnapPx(tip.X), SnapPx(tip.Y) - 12.0f);
    g.RotateTransform(sway * 14.0f + sinf(t * 1.1f) * 3.0f);

    FillRectF(g, &petalDark, -16.0f, -16.0f, 32.0f, 32.0f);
    FillRectF(g, &petalDark, -12.0f, -24.0f, 24.0f,  8.0f);
    FillRectF(g, &petal,     -12.0f, -12.0f, 24.0f, 24.0f);
    FillRectF(g, &petal,      -8.0f, -20.0f, 16.0f,  8.0f);
    FillRectF(g, &petalLight, -8.0f,  -8.0f, 16.0f, 12.0f);
    FillRectF(g, &petalDark,  -4.0f,   0.0f,  8.0f,  8.0f);
    FillRectF(g, &petalLight, -4.0f, -16.0f,  4.0f,  4.0f);

    g.Restore(st);
}

// 薰衣草（v3.8.2 再调：整体收窄——穗粒基部只占 2 格、穗身 1 格，比旧版苗条一多半；
// 四根花穗高低明显拉开（64 ~ 120px），左右倾斜互相抵消，视觉平衡、不挤在顶部）
static void DrawLavender(Graphics& g, float soilY, float sway, float t)
{
    SolidBrush stem(C(88, 146, 68));
    SolidBrush budA(C(152, 106, 202));
    SolidBrush budB(C(116, 74, 170));
    SolidBrush budLight(C(198, 160, 236));

    struct Spike { float x, h, ph, lean; int grains; };
    // 布局：最矮的一根在中间偏左（远离右穗），右穗独立在最右；
    // 四穗穗尖横向间距 ≥ 11px，互不粘连
    const Spike spikes[4] = {
        { -18.0f,  96.0f, 0.0f, -4.0f, 6 },   // 左后方，偏高，左倾
        {  -6.0f, 120.0f, 1.1f,  3.0f, 7 },   // 最高的一根，右倾
        {   6.0f,  52.0f, 2.0f,  4.0f, 5 },   // 最矮的一根，前中（比上一版右移一格像素）
        {  22.0f,  84.0f, 2.9f,  3.0f, 6 },   // 右边独立一根，右倾
    };

    GraphicsState st = g.Save();
    g.TranslateTransform(0.0f, SnapPx(soilY));

    for (int i = 0; i < 4; ++i) {
        const Spike& s = spikes[i];
        int   rows = (int)floorf(s.h / PIX + 0.5f);
        float bend = sway * 10.0f + sinf(t * 0.9f + s.ph) * 2.0f + s.lean;

        // 茎：4px 宽，越靠上越往 lean + bend 方向弯
        for (int r = 0; r < rows; ++r) {
            float k  = (float)r / (float)rows;
            float dx = SnapPx(s.x + bend * k * k);
            FillRectF(g, &stem, dx - PIX * 0.5f, -(float)(r + 1) * PIX, PIX, PIX);
        }

        // 花穗：从茎尖往上 grains 粒，基部 2 行 2 格宽、其余全部 1 格（苗条），
        // 奇偶行左右错开半格；每根穗的粒数不同
        float tipX = SnapPx(s.x + bend);
        float tipY = -(float)rows * PIX;
        for (int j = 0; j < s.grains; ++j) {
            float y   = tipY - (float)j * PIX;
            float w   = PIX * ((j < 2) ? 2.0f : 1.0f);
            float off = (j & 1) ? PIX * 0.5f : -PIX * 0.5f;
            float gx  = tipX + off;
            bool  alt = ((j & 1) ^ (i & 1)) != 0;          // 每根穗的明暗节奏错开
            Brush* main = alt ? (Brush*)&budB : (Brush*)&budA;
            FillRectF(g, main,      gx - w * 0.5f, y, w, PIX);
            FillRectF(g, &budLight, gx - w * 0.5f, y, PIX, PIX);
        }
        // 顶芽：单粒圆头收尾
        FillRectF(g, &budB, tipX - PIX * 0.5f, tipY - (float)s.grains * PIX, PIX, PIX);
    }

    g.Restore(st);
}

// 三叶草：三片心形像素叶，每片都是一格一格拼出来的
static void DrawClover(Graphics& g, float soilY, float sway, float t)
{
    SolidBrush stem(C(78, 140, 60));
    SolidBrush leafA(C(96, 176, 76));
    SolidBrush leafB(C(70, 146, 58));
    SolidBrush leafLight(C(140, 208, 108));

    GraphicsState st = g.Save();
    g.TranslateTransform(0.0f, SnapPx(soilY));
    g.RotateTransform(sway * 6.0f + sinf(t * 0.8f) * 1.2f);

    const float h = 52.0f;
    FillRectF(g, &stem, -PIX * 0.5f, -h, PIX, h);
    FillRectF(g, &stem, -PIX * 2.0f, -h - PIX, PIX * 4.0f, PIX);   // 叶柄分叉

    // 心形叶：7 格宽 / 6 格高，顶行中间留缺口
    struct LeafPos { float cx, cy; };
    const LeafPos pos[3] = {
        { -16.0f, -h - 10.0f },
        {   0.0f, -h - 18.0f },
        {  16.0f, -h - 10.0f }
    };
    // 每一行：起始格、格数（以中心为 0 格）
    const int rowStart[6] = { -3, -3, -3, -2, -1, 0 };
    const int rowCount[6] = {  2,  7,  7,  5,  3, 1 };
    for (int i = 0; i < 3; ++i) {
        float cx = SnapPx(pos[i].cx + sway * 4.0f);
        float cy = SnapPx(pos[i].cy);
        for (int r = 0; r < 6; ++r) {
            float y = cy + (float)r * PIX;
            if (r == 0) {
                // 顶行两个叶瓣，中间断开
                FillRectF(g, &leafA, cx - 3 * PIX, y, 2 * PIX, PIX);
                FillRectF(g, &leafA, cx + 1 * PIX, y, 2 * PIX, PIX);
                continue;
            }
            Brush* br = (i & 1) ? (Brush*)&leafB : (Brush*)&leafA;
            FillRectF(g, br, cx + (float)rowStart[r] * PIX, y,
                      (float)rowCount[r] * PIX, PIX);
        }
        // 叶脉高光
        FillRectF(g, &leafLight, cx - PIX * 0.5f, cy + PIX, PIX, PIX * 2.0f);
    }

    g.Restore(st);
}

// 辣椒：矮灌木 + 三只垂下来的红辣椒
static void DrawChili(Graphics& g, float soilY, float sway, float t)
{
    SolidBrush stem(C(84, 142, 60));
    SolidBrush leaf(C(92, 168, 72));
    SolidBrush leafDark(C(64, 126, 52));
    SolidBrush pod(C(212, 58, 46));
    SolidBrush podDark(C(158, 34, 30));
    SolidBrush podLight(C(244, 118, 92));

    GraphicsState st = g.Save();
    g.TranslateTransform(0.0f, SnapPx(soilY));
    g.RotateTransform(sway * 6.0f + sinf(t * 0.7f) * 1.0f);

    const float h = 84.0f;
    FillRectF(g, &stem, -PIX * 0.5f, -h - 8.0f, PIX, h + 8.0f);

    // 枝叶
    DrawPixelLeaf(g, -2.0f, -h * 0.55f, -1, 24.0f, 24.0f, 6.0f, &leaf, nullptr);
    DrawPixelLeaf(g,  2.0f, -h * 0.72f, +1, 22.0f, 24.0f, 6.0f, &leafDark, nullptr);
    DrawPixelLeaf(g, -2.0f, -h * 0.90f, -1, 18.0f, 20.0f, 6.0f, &leafDark, nullptr);
    DrawPixelLeaf(g,  2.0f, -h * 0.98f, +1, 16.0f, 20.0f, 6.0f, &leaf, nullptr);

    // 辣椒：8px 宽的红条垂下，尾端收成 4px 尖
    struct Pod { float x, top, len; };
    const Pod pods[3] = {
        { -14.0f, -h * 0.50f, 22.0f },
        {  12.0f, -h * 0.62f, 26.0f },
        {   0.0f, -h * 0.78f, 20.0f }
    };
    for (int i = 0; i < 3; ++i) {
        float x   = SnapPx(pods[i].x + sway * 5.0f);
        float top = SnapPx(pods[i].top);
        int   rows = (int)floorf(pods[i].len / PIX + 0.5f);
        for (int r = 0; r < rows; ++r) {
            float y = top + (float)r * PIX;
            FillRectF(g, &pod,      x - PIX, y, PIX * 2.0f, PIX);
            FillRectF(g, &podDark,  x + PIX * 0.5f, y, PIX * 0.5f, PIX);
            FillRectF(g, &podLight, x - PIX, y, PIX * 0.5f, PIX);
        }
        FillRectF(g, &pod, x - PIX * 0.5f, top + (float)rows * PIX, PIX, PIX);
        FillRectF(g, &leafDark, x - PIX * 1.5f, top - PIX, PIX * 3.0f, PIX);  // 蒂
    }

    g.Restore(st);
}

// 蒲公英：细茎顶着一颗白色绒球，旁边飘走一颗种子
static void DrawDandelion(Graphics& g, float soilY, float sway, float t)
{
    SolidBrush stem(C(122, 158, 82));
    SolidBrush stemDark(C(94, 128, 62));
    SolidBrush leaf(C(104, 170, 78));
    SolidBrush puffShade(C(206, 212, 200));
    SolidBrush puff(C(246, 246, 238));
    SolidBrush fly(C(255, 255, 250));

    GraphicsState st = g.Save();
    g.TranslateTransform(0.0f, SnapPx(soilY));
    g.RotateTransform(sway * 8.0f + sinf(t * 1.0f) * 2.0f);

    const float h = 104.0f;
    FillRectF(g, &stem,     -PIX,  -h, PIX * 2.0f, h);
    FillRectF(g, &stemDark,  PIX,  -h, PIX, h);

    // 贴地叶片（抬高一点，免得被花盆沿挡住）
    DrawPixelLeaf(g, -2.0f, -10.0f, -1, 34.0f, 30.0f, 6.0f, &leaf, nullptr);
    DrawPixelLeaf(g,  2.0f, -16.0f, +1, 28.0f, 26.0f, 6.0f, &leaf, nullptr);

    // 绒球：两层像素圆 + 外圈八颗绒毛
    const float cx = 0.0f, cy = -h - 12.0f;
    FillPixelEllipse(g, &puffShade, cx, cy, 20.0f, 20.0f);
    FillPixelEllipse(g, &puff,      cx, cy, 16.0f, 16.0f);
    for (int i = 0; i < 8; ++i) {
        float a = (float)i * PI_F / 4.0f;
        FillRectF(g, &puff, SnapPx(cx + cosf(a) * 22.0f) - PIX * 0.5f,
                          SnapPx(cy + sinf(a) * 22.0f) - PIX * 0.5f, PIX, PIX);
    }

    // 飘走的种子
    float fx = 28.0f + sinf(t * 0.9f) * 6.0f;
    float fy = cy - 26.0f + cosf(t * 0.7f) * 4.0f;
    FillRectF(g, &fly, SnapPx(fx), SnapPx(fy), PIX, PIX);

    g.Restore(st);
}

// 雏菊：细茎顶一朵小白花，黄色花心，花瓣一圈 8x8 像素块
static void DrawDaisy(Graphics& g, float soilY, float sway, float t)
{
    SolidBrush stem(C(96, 158, 72));
    SolidBrush petal(C(250, 250, 242));
    SolidBrush petalShade(C(216, 220, 204));
    SolidBrush core(C(244, 196, 58));
    SolidBrush coreDark(C(206, 150, 34));

    const float h = 98.0f;
    PointF tip = DrawBentStem(g, 0.0f, soilY, h, sway, &stem, 6.0f);
    DrawLeaf(g, soilY, h, 0.382f, sway, -1);    // 叶位取黄金分割点
    DrawLeaf(g, soilY, h, 0.618f, sway, +1);

    GraphicsState st = g.Save();
    g.TranslateTransform(SnapPx(tip.X), SnapPx(tip.Y) - 8.0f);
    g.RotateTransform(sway * 15.0f + sinf(t * 1.5f) * 3.0f);

    // 花瓣：8 瓣交错两色，中心补一块白底盘
    const int n = 8;
    for (int i = 0; i < n; ++i) {
        float a  = (float)i * (2.0f * PI_F) / (float)n;
        float px = SnapPx(cosf(a) * 16.0f);
        float py = SnapPx(sinf(a) * 16.0f);
        Brush* br = (i & 1) ? (Brush*)&petalShade : (Brush*)&petal;
        FillRectF(g, br, px - 4.0f, py - 4.0f, 8.0f, 8.0f);
    }
    FillPixelEllipse(g, &petal, 0.0f, 0.0f, 10.0f, 10.0f);
    FillPixelEllipse(g, &core, 0.0f, 0.0f, 7.0f, 7.0f);
    FillPixelEllipse(g, &coreDark, 2.0f, 2.0f, 3.5f, 3.5f);

    g.Restore(st);
}

// 芦荟（v3.7 重绘）：后排 3 片暗叶 + 前排 3 片亮叶，
// 叶片基部最宽、向上收窄、中段起按 k^1.6 曲线往外弯，尖端收成 4px，
// 叶面带一条高光条纹，不再有生硬的锯齿
static void DrawAloe(Graphics& g, float soilY, float sway, float t)
{
    SolidBrush back(C(62, 126, 92));        // 后排叶（背光）
    SolidBrush backDark(C(46, 100, 74));
    SolidBrush blade(C(96, 168, 118));      // 前排叶
    SolidBrush bladeDark(C(62, 126, 92));
    SolidBrush bladeLight(C(150, 208, 160));
    SolidBrush stripe(C(198, 234, 198));    // 叶面高光条纹

    struct Frond { float x, h, lean; };     // lean：叶尖横向偏移（往两侧弯）
    const Frond backF[3] = {
        { -20.0f, 78.0f, -18.0f },
        {   0.0f, 96.0f,   0.0f },
        {  20.0f, 74.0f,  20.0f },
    };
    const Frond frontF[3] = {
        { -14.0f, 62.0f, -22.0f },
        {   2.0f, 80.0f,   6.0f },
        {  16.0f, 56.0f,  24.0f },
    };

    GraphicsState st = g.Save();
    g.TranslateTransform(0.0f, SnapPx(soilY));
    g.RotateTransform(sway * 4.0f + sinf(t * 0.8f) * 1.0f);

    // 一片芦荟叶：先画后排（暗），再画前排（亮）压在上面
    struct Pal { Brush* main; Brush* dark; Brush* light; bool stripeOn; };
    const Pal pals[2] = {
        { &back,  &backDark,  &backDark,  false },
        { &blade, &bladeDark, &bladeLight, true  },
    };
    for (int layer = 0; layer < 2; ++layer) {
        const Pal& pal = pals[layer];
        const Frond* fs = (layer == 0) ? backF : frontF;
        for (int i = 0; i < 3; ++i) {
            const Frond& f = fs[i];
            int rows = (int)floorf(f.h / PIX + 0.5f);
            for (int r = 0; r < rows; ++r) {
                float k  = (float)r / (float)rows;       // 0=根部 1=叶尖
                float cx = SnapPx(f.x + f.lean * powf(k, 1.6f) + sway * 5.0f * k);
                float hw = PIX * (1.5f - k);             // 基部 3 格宽 -> 尖端 1 格
                if (hw < PIX * 0.5f) hw = PIX * 0.5f;
                float y = -(float)(r + 1) * PIX;
                FillRectF(g, pal.main,  cx - hw, y, hw * 2.0f, PIX);
                FillRectF(g, pal.dark,  cx + hw - PIX * 0.5f, y, PIX * 0.5f, PIX);
                FillRectF(g, pal.light, cx - hw, y, PIX * 0.5f, PIX);
                if (pal.stripeOn && k > 0.2f && k < 0.92f)
                    FillRectF(g, &stripe, cx - PIX * 0.5f, y, PIX, PIX);
            }
        }
    }

    g.Restore(st);
}

// 牵牛花：细茎顶着喇叭形花冠，喇叭口朝上，喉部白色带星纹
static void DrawMorningGlory(Graphics& g, float soilY, float sway, float t)
{
    SolidBrush stem(C(84, 148, 66));
    SolidBrush leafA(C(104, 174, 82));
    SolidBrush leafB(C(74, 138, 60));
    SolidBrush petal(C(108, 122, 214));
    SolidBrush petalLight(C(150, 160, 240));
    SolidBrush petalDark(C(76, 84, 172));
    SolidBrush throat(C(246, 244, 230));
    SolidBrush star(C(120, 100, 190));

    const float h = 118.0f;
    PointF tip = DrawBentStem(g, 0.0f, soilY, h, sway, &stem, 6.0f);

    // 两片细长叶
    GraphicsState ls = g.Save();
    g.TranslateTransform(0.0f, SnapPx(soilY));
    g.RotateTransform(sway * 8.0f);
    DrawPixelLeaf(g, 0.0f,  0.0f, -1, 34.0f, 26.0f, 7.0f, &leafA, nullptr);
    DrawPixelLeaf(g, 0.0f, -6.0f, +1, 28.0f, 24.0f, 6.0f, &leafB, nullptr);
    g.Restore(ls);

    // 花冠：绕茎尖轻轻摆动
    GraphicsState st = g.Save();
    g.TranslateTransform(SnapPx(tip.X), SnapPx(tip.Y) - 4.0f);
    g.RotateTransform(sway * 12.0f + sinf(t * 1.3f) * 3.0f);

    // 花萼
    FillRectF(g, &petalDark, -4.0f, -8.0f, 8.0f, 8.0f);
    // 漏斗管：从下往上逐行加宽
    FillRectF(g, &petal,      -6.0f,  -16.0f, 12.0f, 8.0f);
    FillRectF(g, &petalLight, -6.0f,  -16.0f, 4.0f,  8.0f);
    FillRectF(g, &petal,     -12.0f,  -24.0f, 24.0f, 8.0f);
    FillRectF(g, &petalLight, -12.0f, -24.0f, 4.0f,  8.0f);
    FillRectF(g, &petal,     -18.0f,  -32.0f, 36.0f, 8.0f);
    FillRectF(g, &petalDark,  14.0f,  -24.0f, 4.0f, 16.0f);   // 右侧背光
    // 喇叭口：白色喉部 + 深色星纹（中间再点一撮白）
    FillRectF(g, &throat,    -18.0f, -40.0f, 36.0f, 8.0f);
    FillRectF(g, &star,       -6.0f, -40.0f, 12.0f, 8.0f);
    FillRectF(g, &throat,     -2.0f, -40.0f, 4.0f,  8.0f);

    g.Restore(st);
}

// 樱花：褐色短干 + 三团粉色树冠，树冠里点几朵小白花，旁边飘着一片花瓣
static void DrawCherry(Graphics& g, float soilY, float sway, float t)
{
    SolidBrush trunk(C(122, 88, 66));
    SolidBrush trunkDark(C(94, 64, 46));
    SolidBrush blobA(C(244, 178, 196));
    SolidBrush blobB(C(224, 142, 168));
    SolidBrush blobC(C(252, 208, 220));
    SolidBrush flower(C(250, 236, 244));
    SolidBrush petalFly(C(248, 184, 202));

    const float h = 62.0f;      // 树干高

    GraphicsState st = g.Save();
    g.TranslateTransform(0.0f, SnapPx(soilY));
    g.RotateTransform(sway * 4.0f + sinf(t * 0.9f) * 1.4f);

    // 树干 + 一根小枝
    FillRectF(g, &trunk,     -6.0f, -h, 12.0f, h);
    FillRectF(g, &trunkDark,  2.0f, -h,  4.0f, h);
    FillRectF(g, &trunk,      6.0f, -h + 6.0f, 22.0f, 4.0f);

    // 三团树冠：像素椭圆拼贴；侧团半径 = 主团 × 1/φ，左右对称受风摆动
    FillPixelEllipse(g, &blobB, -26.0f + sway * 6.0f, -h - 8.0f,  16.0f, 12.0f);
    FillPixelEllipse(g, &blobB,  26.0f + sway * 8.0f, -h - 6.0f,  16.0f, 12.0f);
    FillPixelEllipse(g, &blobA,   0.0f + sway * 10.0f, -h - 20.0f, 26.0f, 18.0f);
    FillPixelEllipse(g, &blobC,  -8.0f + sway * 10.0f, -h - 26.0f, 12.0f, 8.0f);
    FillPixelEllipse(g, &blobC,  14.0f + sway * 8.0f,  -h - 12.0f, 8.0f,  6.0f);

    // 几朵小白花
    FillRectF(g, &flower, -14.0f + sway * 8.0f,  -h - 22.0f, 4.0f, 4.0f);
    FillRectF(g, &flower,   8.0f + sway * 10.0f, -h - 30.0f, 4.0f, 4.0f);
    FillRectF(g, &flower,  20.0f + sway * 8.0f,  -h - 14.0f, 4.0f, 4.0f);

    g.Restore(st);

    // 飘落的花瓣：从树冠右侧缓缓下坠，落到底再回到树冠循环
    float pa = fmodf(t * 0.5f, 1.0f);
    float px = 30.0f + sinf(t * 0.8f) * 10.0f + pa * 14.0f;
    float py = -h - 18.0f + pa * 46.0f;
    FillRectF(g, &petalFly, SnapPx(px), SnapPx(py), PIX, PIX);
}

static float PlantHeight(PlantType t)
{
    switch (t) {
    case PLANT_CACTUS:    return 112.0f;
    case PLANT_SUNFLOWER: return 150.0f;
    case PLANT_FLYTRAP:   return 140.0f;
    case PLANT_TULIP:     return 140.0f;
    case PLANT_MUSHROOM:  return 100.0f;
    case PLANT_PINE:      return 148.0f;
    case PLANT_SUCCULENT: return 64.0f;
    case PLANT_BAMBOO:    return 158.0f;
    case PLANT_ROSE:      return 132.0f;
    case PLANT_LAVENDER:  return 152.0f;
    case PLANT_CLOVER:    return 88.0f;
    case PLANT_CHILI:     return 128.0f;
    case PLANT_DANDELION: return 148.0f;
    case PLANT_DAISY:         return 132.0f;
    case PLANT_ALOE:          return 104.0f;
    case PLANT_MORNINGGLORY:  return 168.0f;
    case PLANT_CHERRY:        return 132.0f;
    default:              return 110.0f;
    }
}

static void DrawPlantLocal(Graphics& g, float soilY, PlantType t, float sway, bool poking)
{
    switch (t) {
    case PLANT_CACTUS:
        DrawCactus(g, soilY, sway);
        break;
    case PLANT_FLYTRAP:
        DrawFlytrap(g, soilY, sway, g_time, poking ? 0.45f : 0.0f);
        break;
    case PLANT_TULIP:
        DrawTulip(g, soilY, sway, g_time);
        break;
    case PLANT_MUSHROOM:
        DrawMushroom(g, soilY, sway, g_time);
        break;
    case PLANT_PINE:
        DrawPine(g, soilY, sway, g_time);
        break;
    case PLANT_SUCCULENT:
        DrawSucculent(g, soilY, sway, g_time);
        break;
    case PLANT_BAMBOO:
        DrawBamboo(g, soilY, sway, g_time);
        break;
    case PLANT_ROSE:
        DrawRose(g, soilY, sway, g_time);
        break;
    case PLANT_LAVENDER:
        DrawLavender(g, soilY, sway, g_time);
        break;
    case PLANT_CLOVER:
        DrawClover(g, soilY, sway, g_time);
        break;
    case PLANT_CHILI:
        DrawChili(g, soilY, sway, g_time);
        break;
    case PLANT_DANDELION:
        DrawDandelion(g, soilY, sway, g_time);
        break;
    case PLANT_DAISY:
        DrawDaisy(g, soilY, sway, g_time);
        break;
    case PLANT_ALOE:
        DrawAloe(g, soilY, sway, g_time);
        break;
    case PLANT_MORNINGGLORY:
        DrawMorningGlory(g, soilY, sway, g_time);
        break;
    case PLANT_CHERRY:
        DrawCherry(g, soilY, sway, g_time);
        break;
    case PLANT_SUNFLOWER:
    default:
        DrawSunflower(g, soilY, sway, g_time);
        break;
    }
}

// =============================================================================
//  一·五、天气系统（v3.7）：降水区域 / 雨 / 雪 / 积水 / 积雪
// =============================================================================

// 天气区域（v3.8.1）：按 x 把花盆聚成若干簇——相邻两盆之间的空隙超过
// WEATHER_ZONE_GAP 时就分成两个区域，各自下各自的雨 / 雪，
// 不再让降水落在两盆之间的空地上。每簇的包围盒：最左、最右、最高点再往上抬 60px。
struct WeatherZone { float l, r, top; };
static const float WEATHER_ZONE_GAP = 240.0f;    // 簇与簇之间允许的最大空隙 px

static std::vector<WeatherZone> g_zones;

static void RebuildWeatherZones()
{
    g_zones.clear();
    // v3.8.3：没有花盆就没有天气——以前这里会生成一个全屏区域，导致
    // 一盆花都没有时雨雪照样铺满整个屏幕。本函数每个物理帧都会随
    // 花盆的当前位置重新聚类（拖拽中的花盆实时跟随）。
    if (g_pots.empty() || g_winW <= 0) return;

    // 按 x 排序（盆数很少，插入排序足够）
    std::vector<size_t> order(g_pots.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    for (size_t i = 1; i < order.size(); ++i)
        for (size_t j = i; j > 0 && g_pots[order[j]].body.x < g_pots[order[j - 1]].body.x; --j) {
            size_t t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }

    float cl = 1e9f, cr = -1e9f, ctop = 1e9f;
    for (size_t i = 0; i < order.size(); ++i) {
        const Pot& p = g_pots[order[i]];
        const float halfW = (POT_W * 0.5f + 44.0f) * p.scale;
        const float pTop  = p.body.y - POT_H * 0.5f * p.scale
                          - (PlantHeight(p.plant) + 44.0f) * p.scale;

        // 与当前簇右边缘的空隙超过阈值：先收拢当前簇，再开新簇
        if (cl <= cr && p.body.x - halfW - cr > WEATHER_ZONE_GAP) {
            WeatherZone z = { Clampf(cl, 0.0f, (float)g_winW),
                              Clampf(cr, 0.0f, (float)g_winW),
                              Clampf(ctop - 60.0f, 20.0f, g_floorY - 80.0f) };
            g_zones.push_back(z);
            cl = 1e9f; cr = -1e9f; ctop = 1e9f;
        }
        if (p.body.x - halfW < cl) cl = p.body.x - halfW;
        if (p.body.x + halfW > cr) cr = p.body.x + halfW;
        if (pTop < ctop) ctop = pTop;
    }
    if (cl <= cr) {
        WeatherZone z = { Clampf(cl, 0.0f, (float)g_winW),
                          Clampf(cr, 0.0f, (float)g_winW),
                          Clampf(ctop - 60.0f, 20.0f, g_floorY - 80.0f) };
        g_zones.push_back(z);
    }
}

// 水珠：花盆砸进积水 / 雨滴落地时溅起
static void SpawnSplash(float x, float y, int count)
{
    if (g_splash.size() > 120) return;
    for (int i = 0; i < count; ++i) {
        Splash s;
        s.x = x + (float)((rand() % 61) - 30);
        s.y = y - (float)(rand() % 3);
        s.vx = (float)((rand() % 220) - 110);
        s.vy = -(float)(150 + rand() % 160);
        s.life = 0.35f + (float)(rand() % 20) / 100.0f;
        g_splash.push_back(s);
    }
}

static void UpdateWeather(float dt)
{
    // ---- 天气调度：晴天每隔一阵掷一次骰子，中签就下一场雨或雪 ----
    if (g_weather == WEATHER_CLEAR) {
        g_weatherTimer -= dt;
        if (g_weatherTimer <= 0.0f) {
            g_weatherTimer = 20.0f;
            int roll = rand() % 100;
            if (roll < 35) {                       // 35% 下雨
                g_weather = WEATHER_RAIN;
                g_weatherLeft = 18.0f + (float)(rand() % 25);
            } else if (roll < 52) {                // 17% 下雪
                g_weather = WEATHER_SNOW;
                g_weatherLeft = 25.0f + (float)(rand() % 30);
            }
        }
    } else {
        g_weatherLeft -= dt;
        if (g_weatherLeft <= 0.0f) {
            g_weather = WEATHER_CLEAR;
            g_weatherTimer = 45.0f + (float)(rand() % 90);
        }
    }

    // 风力增幅平滑起落：下雨下雪时风变大
    float target = (g_weather != WEATHER_CLEAR) ? 1.0f : 0.0f;
    g_storm += (target - g_storm) * Clampf(dt * 0.8f, 0.0f, 1.0f);
    // 降水强度也做平滑过渡（v3.8）：起雨/停雨时粒子密度、云的透明度都跟着渐变
    g_precip += (target - g_precip) * Clampf(dt * 0.6f, 0.0f, 1.0f);

    // 天气切换时清掉另一种降水粒子，避免雨滴和雪花同屏重叠（v3.8.2）
    if (g_weather != g_lastWeather) {
        if (g_weather == WEATHER_SNOW)      g_rain.clear();
        else if (g_weather == WEATHER_RAIN) g_flakes.clear();
        g_lastWeather = g_weather;
    }

    RebuildWeatherZones();

    // ---- 降水粒子生成：每个天气区域各自铺密度，密度随 g_precip 渐变（雨量已调小）----
    for (size_t zi = 0; zi < g_zones.size(); ++zi) {
        const WeatherZone& z = g_zones[zi];
        const float zw = z.r - z.l;
        if (zw < 20.0f) continue;

        if (g_weather == WEATHER_RAIN && g_rain.size() < 70) {
            int n = (int)(zw / 55.0f) + 1;
            if (n > 6) n = 6;
            n = (int)(n * g_precip + 0.5f);          // 起雨/停雨时密度平滑起落
            for (int i = 0; i < n; ++i) {
                RainDrop d;
                d.x  = z.l + (float)(rand() % 1000) * 0.001f * zw;
                // 出生点 = 云底边缘（云带底边 = z.top，阴影条 z.top-4~z.top），
                // 雨的最上沿与云的最下沿相接触，雨像是从云里落出来的
                d.y  = z.top + (float)(rand() % 5);
                d.vy = RAIN_SPEED_MIN + (float)(rand() % 1000) * 0.001f
                     * (RAIN_SPEED_MAX - RAIN_SPEED_MIN);
                g_rain.push_back(d);
            }
        }
        if (g_weather == WEATHER_SNOW && g_flakes.size() < 150) {
            int n = (int)(zw / 30.0f) + 1;
            if (n > 16) n = 16;
            n = (int)(n * g_precip + 0.5f);          // 降雪密度同样平滑起落
            for (int i = 0; i < n; ++i) {
                SnowFlake f;
                f.x   = z.l + (float)(rand() % 1000) * 0.001f * zw;
                // 与雨一致：雪从云底边缘出生，最上沿与云的最下沿相接触
                f.y   = z.top + (float)(rand() % 5);
                f.vy  = 55.0f + (float)(rand() % 80);
                f.ph  = (float)(rand() % 628) * 0.01f;
                f.amp = 10.0f + (float)(rand() % 16);
                g_flakes.push_back(f);
            }
        }
    }

    // ---- 雨滴：落到植物上补水，落到地面归入积水并溅水花 ----
    for (size_t i = 0; i < g_rain.size();) {
        RainDrop& d = g_rain[i];
        d.y += d.vy * dt;
        bool hit = false;
        // 先查是否碰到某个花盆 / 植物顶（v3.8：只在真正接触的一瞬溅起，
        // 不再把整个轮廓上方的雨都“吸掉”，雨会从植物两侧自然落下）
        for (size_t pi = 0; pi < g_pots.size(); ++pi) {
            Pot& p = g_pots[pi];
            const float halfW = 30.0f * p.scale;
            if (d.x < p.body.x - halfW || d.x > p.body.x + halfW) continue;
            float topY = p.body.y - POT_H * 0.5f * p.scale
                       - (PlantHeight(p.plant) + 4.0f) * p.scale;
            if (d.y >= topY) {
                if (!g_editMode)
                    p.moisture = Clampf(p.moisture + 0.012f, 0.0f, 1.0f);   // 下雨补水
                SpawnSplash(d.x, d.y, 2);                                   // 碰到植物溅起
                hit = true;
                break;
            }
        }
        // 落到地面（积雪上也算）
        if (!hit && d.y >= g_floorY - g_snowCover - 2.0f) {
            if (g_puddle < PUDDLE_MAX) g_puddle += 0.05f;
            if ((rand() % 3) == 0)
                SpawnSplash(d.x, g_floorY - g_puddle - g_snowCover, 2);
            hit = true;
        }
        if (hit || d.y > g_floorY + 20.0f) {
            g_rain[i] = g_rain.back();
            g_rain.pop_back();
        } else {
            ++i;
        }
    }

    // ---- 雪花：慢慢飘，落到地面增加积雪 ----
    for (size_t i = 0; i < g_flakes.size();) {
        SnowFlake& f = g_flakes[i];
        f.y += f.vy * dt;
        f.x += sinf(g_time * 0.9f + f.ph) * f.amp * (0.6f + g_storm * 1.6f) * dt;
        if (f.y >= g_floorY - g_snowCover) {
            if (g_snowCover < SNOW_MAX) g_snowCover += 0.05f;
            g_flakes[i] = g_flakes.back();
            g_flakes.pop_back();
        } else {
            ++i;
        }
    }

    // ---- 水珠：轻重力，落到地面或到时间就消失 ----
    for (size_t i = 0; i < g_splash.size();) {
        Splash& s = g_splash[i];
        s.vy += 1500.0f * dt;
        s.x += s.vx * dt;
        s.y += s.vy * dt;
        s.life -= dt;
        if (s.life <= 0.0f || s.y > g_floorY - g_snowCover) {
            g_splash[i] = g_splash.back();
            g_splash.pop_back();
        } else {
            ++i;
        }
    }

    // ---- 积水 / 积雪演化（v3.8：雪要化成水再消失，融雪汇入积水）----
    // v3.8.3：融雪速度改按"雪是否接触水"判定——雪带贴着积水（puddle > 0.5px）
    // 就以 0.55/s 快速融化成水，无论当前是雨天、雪天还是晴天（旧逻辑只看天气：
    // 下雪天贴水的雪照样增长、晴天要熬 75 秒，雪压在水上就是不化）。
    // 干燥环境（没有积水）才保留晴天慢融。
    float melt = dt * 0.08f;                                   // 干燥：慢慢融化
    if (g_weather == WEATHER_RAIN) melt = dt * 0.55f;          // 雨水直接打在雪上
    if (g_puddle > 0.5f)           melt = dt * 0.55f;          // 雪贴着水：遇水即融
    if (melt > g_snowCover) melt = g_snowCover;
    g_snowCover = Clampf(g_snowCover - melt, 0.0f, SNOW_MAX);
    g_puddle    = Clampf(g_puddle + melt * 0.8f, 0.0f, PUDDLE_MAX);

    if (g_weather == WEATHER_RAIN) {
        g_puddle = Clampf(g_puddle + dt * 0.30f, 0.0f, PUDDLE_MAX);
    } else if (g_weather == WEATHER_SNOW) {
        g_puddle = Clampf(g_puddle - dt * 0.12f, 0.0f, PUDDLE_MAX);   // 雪天水慢慢收干
    } else {
        g_puddle = Clampf(g_puddle - dt * 0.20f, 0.0f, PUDDLE_MAX);   // 晴天蒸发
    }
}

// =============================================================================
//  二、物理
// =============================================================================

// 桌面/任务栏等系统窗口不算可站立平台
static bool IsShellWindow(HWND h)
{
    wchar_t cls[64] = { 0 };
    GetClassNameW(h, cls, 63);
    return wcscmp(cls, L"Progman") == 0
        || wcscmp(cls, L"WorkerW") == 0
        || wcscmp(cls, L"Shell_TrayWnd") == 0
        || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0
        || wcscmp(cls, L"TaskListThumbnailWnd") == 0;
}

// 旋转后花盆最低点相对花盆中心的偏移（用于地面碰撞）
static float PotMaxY(float angle, float scale)
{
    const float hw = POT_W * 0.5f * scale;
    const float hh = POT_H * 0.5f * scale;
    const float c = cosf(angle);
    const float s = sinf(angle);

    float maxY = -1e9f;
    for (int sx = -1; sx <= 1; sx += 2) {
        for (int sy = -1; sy <= 1; sy += 2) {
            float ry = (float)sx * hw * s + (float)sy * hh * c;
            if (ry > maxY) maxY = ry;
        }
    }
    return maxY;
}

// 浇水动画：几串水滴从植物上方落到泥土里（画在花盆局部坐标，随花盆一起旋转/缩放）
static void DrawWaterAnim(Graphics& g, const Pot& p)
{
    if (p.waterAnim <= 0.0f) return;

    float prog = 1.0f - p.waterAnim / WATER_DURATION;    // 0 -> 1
    SolidBrush water(C(92, 176, 232));
    SolidBrush waterLight(C(188, 228, 252));

    const float topY = -PlantHeight(p.plant) * 0.85f - 20.0f;
    const float botY = -POT_H * 0.5f + 2.0f;             // 土面
    for (int i = 0; i < 7; ++i) {
        // 每条水线错开相位，循环下落
        float phase = fmodf(prog * 2.4f + (float)i * 0.143f, 1.0f);
        float x = SnapPx(-24.0f + (float)i * 8.0f);
        float y = SnapPx(topY + (botY - topY) * phase);
        FillRectF(g, &water, x, y, PIX, PIX * 2.0f);
        FillRectF(g, &waterLight, x, y, PIX, PIX);
    }
}

// 查找花盆脚下可站立的平台：默认是任务栏上沿，其次是最上层窗口的标题栏顶边
struct SupportQuery { float cx; float lowest; float best; HWND bestHwnd; };
static SupportQuery g_sq;

// 已确认的承托面存在于每个花盆自身的状态里（见 Pot::supportHwnd / supportY）

// 被 DWM 隐藏的窗口（典型：位于其它虚拟桌面上的窗口）不能当承托面
static bool IsCloakedWindow(HWND h)
{
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != 0;
    return false;
}

static BOOL CALLBACK EnumSupportProc(HWND h, LPARAM lp)
{
    SupportQuery* q = (SupportQuery*)lp;

    if (h == g_hwnd) return TRUE;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    if (IsShellWindow(h)) return TRUE;

    // 只认“有标题栏的普通顶层窗口”
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (!(style & WS_CAPTION) || (style & WS_CHILD)) return TRUE;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    RECT r;
    if (!GetWindowRect(h, &r)) return TRUE;
    if (q->cx < (float)r.left || q->cx >= (float)r.right) return TRUE;

    float topInClient = (float)(r.top - g_winY);

    // 顶边必须“刚好在花盆脚下”：高太多说明花盆还在半空，低太多说明已经钻进窗口内部
    if (topInClient > q->lowest + 4.0f) return TRUE;
    if (topInClient < q->lowest - 64.0f) return TRUE;

    // 再确认落在标题栏高度范围内（避免把工具栏、状态栏当成地面）
    int capH = GetSystemMetrics(SM_CYCAPTION)
             + GetSystemMetrics(SM_CYSIZEFRAME)
             + GetSystemMetrics(SM_CXPADDEDBORDER);
    if (q->lowest - topInClient > (float)capH + 6) return TRUE;

    // 关键一：窗口必须“真的在当前桌面上”。其它虚拟桌面的窗口 IsWindowVisible
    // 依旧返回 TRUE，但被 DWM 遮罩，放上去花盆会在“有地面/没地面”之间反复跳。
    if (IsCloakedWindow(h)) return TRUE;

    // 关键二：窗口必须真的露在那儿（有些程序关闭后句柄还挂着，或被别的窗口盖住）
    POINT pt;
    pt.x = (LONG)q->cx + g_winX;
    pt.y = (LONG)(topInClient + 12.0f) + g_winY;   // 标题栏内、花盆投影之下
    HWND under = WindowFromPoint(pt);
    if (under) {
        DWORD upid = 0;
        GetWindowThreadProcessId(under, &upid);
        // 探针可能落在自己的投影上，那是本进程的窗口，直接放行
        if (upid != GetCurrentProcessId() && GetAncestor(under, GA_ROOT) != h) return TRUE;
    }

    q->best = topInClient;      // 取最上层的那个窗口
    q->bestHwnd = h;
    return FALSE;               // EnumWindows 按 Z 序从上到下，找到即可停
}

static float FindSupportY(Pot& p, float cx, float lowest)
{
    // 1) 现有承托面仍有效就直接沿用：逐帧重新枚举会在两个候选之间来回切换，
    //    表现出来就是花盆在原地震颤 / 抽搐。
    if (p.supportHwnd) {
        RECT r = { 0, 0, 0, 0 };
        bool alive = IsWindow(p.supportHwnd) && IsWindowVisible(p.supportHwnd)
                  && !IsIconic(p.supportHwnd) && !IsCloakedWindow(p.supportHwnd)
                  && GetWindowRect(p.supportHwnd, &r) != FALSE;
        if (alive) {
            float topInClient = (float)(r.top - g_winY);
            if (cx >= (float)r.left && cx < (float)r.right
                && fabsf(topInClient - p.supportY) < 1.0f
                && lowest >= p.supportY - 6.0f && lowest <= p.supportY + 6.0f)
                return p.supportY;
        }
        p.supportHwnd = nullptr;    // 承托面没了（窗口关闭 / 移动 / 换桌面）
    }

    // 2) 重新找
    g_sq.cx = cx;
    g_sq.lowest = lowest;
    g_sq.best = g_floorY;
    g_sq.bestHwnd = nullptr;
    EnumWindows(EnumSupportProc, (LPARAM)&g_sq);
    if (g_sq.bestHwnd) {
        p.supportHwnd = g_sq.bestHwnd;
        p.supportY = g_sq.best;
    }
    return g_sq.best;
}

static void SpawnDust(float x, float y, int count)
{
    if (g_crumbs.size() > 150) return;
    for (int i = 0; i < count; ++i) {
        Crumb c;
        c.x = x + (float)((rand() % 41) - 20);
        c.y = y - (float)(rand() % 4);
        c.vx = (float)((rand() % 240) - 120);
        c.vy = -(float)(rand() % 260) - 40.0f;
        c.life = 0.45f + (float)(rand() % 30) / 100.0f;
        c.floorY = y;
        g_crumbs.push_back(c);
    }
}

// 单个花盆的物理
static void UpdatePot(Pot& p, float dt)
{
    Body& b = p.body;
    const float hw = POT_W * 0.5f * p.scale;
    const float hh = POT_H * 0.5f * p.scale;

    // 水分：平时慢慢蒸发，浇水动画期间吸饱；编辑模式暂停蒸发
    if (p.waterAnim > 0.0f) {
        p.waterAnim -= dt;
        if (p.waterAnim < 0.0f) p.waterAnim = 0.0f;
        p.moisture = Clampf(p.moisture + dt / 1.2f, 0.0f, 1.0f);
        p.swayVel += sinf(g_time * 11.0f) * 0.06f;   // 被水浇得轻轻晃（低频，不抖）
    } else if (!g_editMode) {
        p.moisture = Clampf(p.moisture - dt / g_drySeconds, 0.0f, 1.0f);
    }

    // 缺水：从植株上不断掉落枯叶碎片（有物理，落到地面才消失）
    if (!g_editMode && p.moisture < DRY_THRESHOLD) {
        p.shedTimer -= dt;
        if (p.shedTimer <= 0.0f && g_wilt.size() < 240) {
            float dry = (DRY_THRESHOLD - p.moisture) / DRY_THRESHOLD;
            p.shedTimer = 0.55f - 0.40f * dry;        // 越干掉得越快
            float lx = (float)((rand() % 41) - 20);
            float ly = -10.0f - (float)(rand() % (int)(PlantHeight(p.plant) * 0.75f + 1.0f));
            float c = cosf(b.angle), s = sinf(b.angle);
            float cx = b.x, cy = b.y - hh;
            WiltParticle w;
            w.x = cx + (lx * c - ly * s) * p.scale;
            w.y = cy + (lx * s + ly * c) * p.scale;
            w.vx = (float)((rand() % 70) - 35);
            w.vy = 10.0f + (float)(rand() % 40);
            w.life = 3.0f;
            g_wilt.push_back(w);
        }
    } else {
        p.shedTimer = 0.0f;
    }

    if (b.dragged) {
        // 拖拽时位置由鼠标直接给，这里只让花盆随甩动方向翻转
        b.angle += b.angVel * dt;
        b.angVel *= 0.96f;
    } else {
        b.vy += GRAVITY * dt;
        b.x += b.vx * dt;
        b.y += b.vy * dt;
        b.angle += b.angVel * dt;
    }

    // 角度归一化，避免无限增长
    while (b.angle > PI_F)  b.angle -= 2.0f * PI_F;
    while (b.angle < -PI_F) b.angle += 2.0f * PI_F;

    // 左右边界：整块虚拟桌面（多显示器也能滚过去）
    if (b.x < hw) {
        b.x = hw;
        if (b.vx < 0.0f) b.vx = -b.vx * 0.45f;
    }
    if (b.x > (float)g_winW - hw) {
        b.x = (float)g_winW - hw;
        if (b.vx > 0.0f) b.vx = -b.vx * 0.45f;
    }

    // ---------- 承托面：任务栏上沿，或鼠标下方窗口的标题栏 ----------
    float maxY = PotMaxY(b.angle, p.scale);
    float lowest = b.y - hh + maxY;        // 花盆最低点（客户区坐标）
    float surface = g_floorY;
    if (!b.dragged) surface = FindSupportY(p, b.x, lowest);

    // 任务栏上沿永远是“最底的地面”：窗口下沿比任务栏还低时也不能让花盆
    // 落到任务栏里面去（否则看起来就是花盆压在任务栏上）。
    if (surface > g_floorY) surface = g_floorY;
    p.surfaceY = surface;

    const float limit = surface + hh - maxY;
    if (b.y > limit) {
        b.y = limit;
        if (b.vy > 0.0f) {
            if (b.vy > 260.0f) SpawnDust(b.x, surface, 10);    // 落地扬尘
            float impact = b.vy;
            // 砸进积水里会溅起水花（v3.7）
            if (g_puddle > 1.5f && surface >= g_floorY - 1.0f && impact > 200.0f)
                SpawnSplash(b.x, surface - g_puddle, 9);
            b.vy = -b.vy * RESTITUTION;                        // 回弹
            if (fabsf(b.vy) < 55.0f) b.vy = 0.0f;              // 能量太小就停下
            b.vx *= 0.90f;
            b.angVel += b.vx * 0.0016f;                        // 斜向落地带一点旋转
            if (impact > 1000.0f) b.angVel += 0.4f;
            b.angVel = Clampf(b.angVel, -9.0f, 9.0f);
        }

        // 静止后的摩擦 + 自动回正（重心在盆底的直觉效果）
        if (!b.dragged) {
            b.vx *= 0.93f;
            b.angVel *= 0.86f;
            b.angle += -b.angle * 0.15f;
            if (fabsf(b.angle) < 0.004f) b.angle = 0.0f;
            if (fabsf(b.vx) < 2.0f) b.vx = 0.0f;
            if (fabsf(b.angVel) < 0.03f) b.angVel = 0.0f;
        }
    }

    // 兜底：无论走哪条路径（旋转、拖拽、窗口承托），花盆最低点都不许越过
    // 任务栏上沿——否则看起来就是花盆压在任务栏上。
    {
        float hardLimit = g_floorY + hh - maxY;
        if (b.y > hardLimit) {
            b.y = hardLimit;
            if (b.vy > 0.0f) b.vy = 0.0f;
        }
    }

    // 顶部不设硬边界：花盆可以停在贴着屏幕顶边（最大化窗口）的窗口上

    b.vx = Clampf(b.vx, -3200.0f, 3200.0f);
    b.vy = Clampf(b.vy, -3200.0f, 3200.0f);

    // 植物摆动：阻尼弹簧（v3.6 调软：更慢、更柔）
    const float stiffness = 72.0f;               // 原 110：弯得少了，回弹慢了
    const float damping = 4.6f;                  // 原 6.5：欠阻尼更明显，像有韧性
    p.swayVel += (-stiffness * p.sway - damping * p.swayVel) * dt;
    // 常年微风：静止时也轻轻摇；下雨下雪时风力加大（v3.7）
    p.swayVel += WindForce(g_time + p.windPh) * (1.0f + g_storm * 2.2f) * dt;
    p.sway += p.swayVel * dt;
    const float swayLimit = 1.3f + g_storm * 0.5f;    // 大风天允许摆得更多
    p.sway = Clampf(p.sway, -swayLimit, swayLimit);
}

static void UpdatePhysics(float dt)
{
    UpdateWeather(dt);          // 天气（降水粒子 / 积水 / 积雪 / 风力增幅）

    for (size_t i = 0; i < g_pots.size(); ++i)
        UpdatePot(g_pots[i], dt);

    // 枯叶碎片：轻重力 + 横向飘，落到地面就慢慢消失
    for (size_t i = 0; i < g_wilt.size();) {
        WiltParticle& w = g_wilt[i];
        w.vy += 420.0f * dt;
        w.vx += sinf(g_time * 2.4f + w.y * 0.03f) * 24.0f * dt;
        w.x += w.vx * dt;
        w.y += w.vy * dt;
        w.life -= dt;
        if (w.y > g_floorY - 3.0f) {
            w.y = g_floorY - 3.0f;
            w.vy = 0.0f;
            w.vx *= 0.85f;
            w.life -= dt * 3.0f;
        }
        if (w.life <= 0.0f) {
            g_wilt[i] = g_wilt.back();
            g_wilt.pop_back();
        } else {
            ++i;
        }
    }

    // 泥土碎屑
    for (size_t i = 0; i < g_crumbs.size();) {
        Crumb& c = g_crumbs[i];
        c.vy += 1800.0f * dt;
        c.x += c.vx * dt;
        c.y += c.vy * dt;
        if (c.y > c.floorY) {
            c.y = c.floorY;
            c.vy *= -0.3f;
            c.vx *= 0.6f;
        }
        c.life -= dt;
        if (c.life <= 0.0f) {
            g_crumbs[i] = g_crumbs.back();
            g_crumbs.pop_back();
        } else {
            ++i;
        }
    }
}

// =============================================================================
//  三、渲染（GDI+ -> 32bpp DIB -> UpdateLayeredWindow）
// =============================================================================

// 天气的地面层（v3.7）：任务栏上沿的积雪（白色厚毯，顶面起伏）与积水（半透明水面）
// v3.8.1：每个天气区域各自一段，互不相邻的簇之间不留水雪
static void DrawWeatherFloor(Graphics& g)
{
    RebuildWeatherZones();
    for (size_t zi = 0; zi < g_zones.size(); ++zi) {
        const WeatherZone& z = g_zones[zi];
        if (z.r <= z.l) continue;

        // 积雪
        if (g_snowCover > 0.5f) {
            SolidBrush snow(C(242, 246, 250));
            SolidBrush snowShade(C(204, 216, 230));
            float y = g_floorY - g_snowCover;
            FillRectF(g, &snow, z.l, y, z.r - z.l, g_snowCover);
            // 顶面隔格起伏 + 底部压暗，像自然的雪堆
            for (float x = SnapPx(z.l); x < z.r; x += PIX) {
                int cx = (int)(x / PIX);
                if (cx & 1)
                    FillRectF(g, &snow, x, y - PIX * 0.5f, PIX, PIX * 0.5f);
                if ((cx & 3) == 0)
                    FillRectF(g, &snowShade, x, g_floorY - PIX * 0.5f, PIX, PIX * 0.5f);
            }
        }

        // 积水：半透明水面（水面最高时也没过雪的下沿），最上面一条高光
        if (g_puddle > 0.5f) {
            SolidBrush water(C(64, 138, 198, 150));
            SolidBrush waterHi(C(188, 226, 250, 180));
            float y = g_floorY - g_puddle - g_snowCover * 0.5f;
            FillRectF(g, &water, z.l, y, z.r - z.l, g_puddle);
            FillRectF(g, &waterHi, z.l, y, z.r - z.l, PIX * 0.5f);
        }
    }
}

// 每种植物"能积雪的位置"（v3.8.2）：花盆局部坐标里真实冠层的顶点集合。
// 雪只落在这些位置上，不再浮在无形的轮廓框上。
// 新增植物时在这里补几行即可（x 中心 / y 高度 / 半宽，均为局部坐标）。
struct SnowSpot { float x, y, hw; };
static int PlantSnowSpots(PlantType t, SnowSpot* spots, int maxSpots)
{
    const float soilY = -POT_H * 0.5f + 3.0f;    // 土面（与 DrawPlantLocal 一致）
    int n = 0;
    #define ADD_SPOT(px, py, phw) if (n < maxSpots) { spots[n].x = (px); spots[n].y = (py); spots[n].hw = (phw); ++n; }
    switch (t) {
    case PLANT_CACTUS:
        ADD_SPOT(  0.0f, soilY - 100.0f, 10.0f)      // 主干顶
        ADD_SPOT(-22.0f, soilY -  94.0f,  5.0f)      // 左臂顶
        ADD_SPOT( 22.0f, soilY -  78.0f,  5.0f)      // 右臂顶
        break;
    case PLANT_SUNFLOWER:
        ADD_SPOT(0.0f, soilY - 140.0f, 14.0f)        // 花盘顶
        break;
    case PLANT_FLYTRAP:
        ADD_SPOT(0.0f, soilY - 116.0f, 13.0f)        // 颚顶
        break;
    case PLANT_TULIP:
        ADD_SPOT(0.0f, soilY - 122.0f, 13.0f)        // 花苞顶
        break;
    case PLANT_MUSHROOM:
        ADD_SPOT(0.0f, soilY -  76.0f, 12.0f)        // 菌盖顶
        break;
    case PLANT_PINE:
        ADD_SPOT(0.0f, soilY -  72.0f, 20.0f)        // 三层树冠的"肩"
        ADD_SPOT(0.0f, soilY - 102.0f, 15.0f)
        ADD_SPOT(0.0f, soilY - 130.0f, 11.0f)
        break;
    case PLANT_SUCCULENT:
        ADD_SPOT(0.0f, soilY -  66.0f, 28.0f)        // 莲座顶
        break;
    case PLANT_BAMBOO:
        ADD_SPOT(  0.0f, soilY - 158.0f, 3.0f)       // 三根竹竿顶
        ADD_SPOT(-16.0f, soilY -  98.0f, 3.0f)
        ADD_SPOT( 20.0f, soilY -  61.0f, 3.0f)
        break;
    case PLANT_ROSE:
        ADD_SPOT(0.0f, soilY - 156.0f, 11.0f)        // 花苞顶
        break;
    case PLANT_LAVENDER:
        ADD_SPOT(-22.0f, soilY - 124.0f, 3.0f)       // 四根花穗顶（窄穗版）
        ADD_SPOT( -3.0f, soilY - 152.0f, 3.0f)
        ADD_SPOT( 10.0f, soilY -  76.0f, 3.0f)
        ADD_SPOT( 25.0f, soilY - 112.0f, 3.0f)
        break;
    case PLANT_CLOVER:
        ADD_SPOT(-16.0f, soilY - 62.0f, 9.0f)        // 三片心形叶顶
        ADD_SPOT(  0.0f, soilY - 70.0f, 9.0f)
        ADD_SPOT( 16.0f, soilY - 62.0f, 9.0f)
        break;
    case PLANT_CHILI:
        ADD_SPOT(0.0f, soilY - 90.0f, 3.0f)          // 茎顶
        break;
    case PLANT_DANDELION:
        ADD_SPOT(0.0f, soilY - 136.0f, 16.0f)        // 绒球顶
        break;
    case PLANT_DAISY:
        ADD_SPOT(0.0f, soilY - 124.0f, 14.0f)        // 花头顶
        break;
    case PLANT_ALOE:
        ADD_SPOT(  0.0f, soilY - 96.0f, 5.0f)        // 中间最高叶尖
        ADD_SPOT(-38.0f, soilY - 64.0f, 4.0f)        // 左侧叶尖
        ADD_SPOT(  8.0f, soilY - 80.0f, 4.0f)        // 前排叶尖
        ADD_SPOT( 40.0f, soilY - 58.0f, 4.0f)        // 右侧叶尖
        break;
    case PLANT_MORNINGGLORY:
        ADD_SPOT(0.0f, soilY - 162.0f, 14.0f)        // 喇叭口顶
        break;
    case PLANT_CHERRY:
        ADD_SPOT(   0.0f, soilY - 98.0f, 20.0f)      // 主树冠顶
        ADD_SPOT( -26.0f, soilY - 82.0f, 11.0f)      // 左侧树冠
        ADD_SPOT(  26.0f, soilY - 82.0f, 11.0f)      // 右侧树冠
        break;
    default:
        ADD_SPOT(0.0f, soilY - PlantHeight(t) + 6.0f, 10.0f)
        break;
    }
    #undef ADD_SPOT
    return n;
}

// 植物冠层上的积雪（v3.8.2 重做）：只在 PlantSnowSpots 给出的真实冠层顶上
// 铺交错白格，雪厚时再叠一行；盆沿也积一条。画在花盆局部坐标系里。
static void DrawSnowCapLocal(Graphics& g, const Pot& p)
{
    if (g_snowCover < 0.8f) return;
    SolidBrush snow(C(246, 250, 254));
    SolidBrush shade(C(204, 216, 230));
    const float lv = Clampf(g_snowCover / SNOW_MAX, 0.0f, 1.0f);

    SnowSpot spots[6];
    int n = PlantSnowSpots(p.plant, spots, 6);
    for (int i = 0; i < n; ++i) {
        const SnowSpot& s = spots[i];
        float y = SnapPx(s.y);
        int cells = (int)(s.hw / PIX) + 1;
        // 贴着冠层顶的一行（隔格留缝，露出下面的植物）
        for (int c = -cells; c <= cells; ++c) {
            if (((c + i) & 1) != 0) continue;
            Brush* br = ((c & 3) == 0) ? (Brush*)&shade : (Brush*)&snow;
            FillRectF(g, br, SnapPx(s.x + (float)c * PIX), y - PIX, PIX, PIX);
        }
        // 雪厚时在冠层顶上方再叠一行错缝的
        if (lv > 0.45f) {
            for (int c = -cells + 1; c <= cells - 1; ++c) {
                if (((c + i) & 1) == 0) continue;
                FillRectF(g, &snow, SnapPx(s.x + (float)c * PIX), y - PIX * 2.0f, PIX, PIX);
            }
        }
    }

    // 盆沿上的那条雪
    float rimDepth = PIX * lv;
    FillRectF(g, &snow, -POT_W * 0.5f, -POT_H * 0.5f - rimDepth, POT_W, rimDepth);
}

// 天气时的云（v3.8.2 定稿）：**一整条云盖住整个降水区域的落雨面**——
// 云底正好压在雨滴/雪花生成的位置（z.top），像整片天空都在下雨/下雪；
// 云体是横跨全区域的连续横带，顶部凸包高低起伏（确定性正弦 + 缓慢漂移，无状态），
// 底部一条阴影压出厚度；灰蓝云体 + 受光高光，深浅壁纸上都有轮廓
static void DrawClouds(Graphics& g)
{
    if (g_precip < 0.02f) return;
    const int alpha = (int)(Clampf(g_precip, 0.0f, 1.0f) * 185.0f);
    SolidBrush cloud(C(210, 220, 238, alpha));       // 灰蓝云体
    SolidBrush cloudLight(C(248, 250, 254, alpha));  // 顶部受光
    SolidBrush cloudShade(C(168, 180, 202, alpha));  // 底部阴影

    RebuildWeatherZones();
    for (size_t zi = 0; zi < g_zones.size(); ++zi) {
        const WeatherZone& z = g_zones[zi];
        const float zw = z.r - z.l;
        if (zw < 60.0f) continue;

        const float base = z.top;                    // 云底 = 降水粒子的起点
        // 连续的云体横带：底边与雨/雪的起点齐平
        FillRectF(g, &cloud, z.l, base - 12.0f, zw, 12.0f);
        // 顶部凸包：沿区域宽度排布，高低按确定性正弦起伏，整体缓慢漂移
        const int bumps = (int)(zw / 46.0f) + 1;
        for (int i = 0; i <= bumps; ++i) {
            float bx = z.l + zw * (float)i / (float)bumps
                     + sinf(g_time * 0.10f + (float)i * 1.7f + (float)zi) * 6.0f;
            float r  = 20.0f + 8.0f * sinf((float)i * 2.4f + (float)zi * 1.3f);
            FillPixelEllipse(g, &cloud,      bx, base - 12.0f, r, r * 0.55f);
            FillPixelEllipse(g, &cloudLight, bx, base - 16.0f, r * 0.55f, r * 0.30f);
        }
        // 底部阴影条：压出云的厚度
        FillRectF(g, &cloudShade, z.l, base - PIX, zw, PIX);
    }
}

// ---- 顶部提示条（v3.8）：保存配置时若有花盆在标题栏上则弹出提示 ----
static const int TOAST_W = 470;
static const int TOAST_H = 44;

static int ToastTop()
{
    int y = 20;
    if (g_editMode)  y += g_hudH + 8;
    if (g_debugMode) y += HUD_BTN_H + HUD_PAD * 2 + 8;
    return y;
}

static void ShowToast(const wchar_t* text, float dur = 3.0f)
{
    wcsncpy(g_toastText, text, 127);
    g_toastText[127] = 0;
    g_toastTimer = dur;
    g_fullRedraw = true;
}

static void DrawToast(Graphics& g)
{
    if (g_toastTimer <= 0.0f) return;
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

    const float x = (float)((g_winW - TOAST_W) / 2);
    const float y = (float)ToastTop();
    SolidBrush panel(C(26, 20, 16, 235));
    FillRectF(g, &panel, x, y, (float)TOAST_W, (float)TOAST_H);
    SolidBrush edge(C(196, 148, 92, 240));
    FillRectF(g, &edge, x, y, (float)TOAST_W, 2.0f);
    FillRectF(g, &edge, x, y + TOAST_H - 2, (float)TOAST_W, 2.0f);
    FillRectF(g, &edge, x, y, 2.0f, (float)TOAST_H);
    FillRectF(g, &edge, x + TOAST_W - 2, y, 2.0f, (float)TOAST_H);

    static Gdiplus::Font* font = nullptr;
    static Gdiplus::StringFormat* fmt = nullptr;
    if (!font) font = new Gdiplus::Font(L"Microsoft YaHei", 14.0f,
                                        Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    if (!fmt) {
        fmt = new Gdiplus::StringFormat();
        fmt->SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt->SetLineAlignment(Gdiplus::StringAlignmentCenter);
    }
    SolidBrush txt(C(246, 240, 230));
    RectF layout(x, y + 6.0f, (float)TOAST_W, (float)TOAST_H - 12.0f);
    g.DrawString(g_toastText, -1, font, layout, fmt, &txt);
}

// ---- 调试模式工具条（v3.8）：干枯 / 下雨 / 下雪 / 关闭调试模式 ----
static int g_dbgX = 0, g_dbgY = 0, g_dbgW = 0, g_dbgH = 0;

static void DebugLayout()
{
    g_dbgW = HUD_PAD * 2 + HUD_BTN_W * DBG_BTN_COUNT + HUD_GAP * (DBG_BTN_COUNT - 1);
    g_dbgH = HUD_BTN_H + HUD_PAD * 2;
    g_dbgX = (g_winW - g_dbgW) / 2;
    g_dbgY = 20;
    if (g_editMode) g_dbgY = 20 + g_hudH + 8;    // 编辑模式工具条下面
}

static void DebugButtonRect(int i, float& bx, float& by)
{
    bx = (float)(g_dbgX + HUD_PAD + i * (HUD_BTN_W + HUD_GAP));
    by = (float)(g_dbgY + HUD_PAD);
}

static int DebugButtonAt(float mx, float my)
{
    if (!g_debugMode) return -1;
    DebugLayout();
    for (int i = 0; i < DBG_BTN_COUNT; ++i) {
        float bx, by;
        DebugButtonRect(i, bx, by);
        if (mx >= bx && mx <= bx + HUD_BTN_W && my >= by && my <= by + HUD_BTN_H)
            return i;
    }
    return -1;
}

static void DebugAction(int btn)
{
    switch (btn) {
    case DBG_DRY:                                 // 全部干枯（测试枯叶效果）
        for (size_t i = 0; i < g_pots.size(); ++i) g_pots[i].moisture = 0.0f;
        g_fullRedraw = true;
        break;
    case DBG_RAIN:
        g_weather = WEATHER_RAIN;
        g_weatherLeft = 30.0f;
        g_weatherTimer = 90.0f;
        break;
    case DBG_SNOW:
        g_weather = WEATHER_SNOW;
        g_weatherLeft = 30.0f;
        g_weatherTimer = 90.0f;
        break;
    case DBG_OFF:                                 // 关闭调试：不留任何持久状态
        g_debugMode = false;
        g_debugHot = -1;
        g_fullRedraw = true;
        break;
    }
}

static void DrawDebugHud(Graphics& g)
{
    DebugLayout();
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

    SolidBrush panel(C(26, 20, 16, 235));
    FillRectF(g, &panel, (float)g_dbgX, (float)g_dbgY, (float)g_dbgW, (float)g_dbgH);
    SolidBrush edge(C(196, 148, 92, 240));
    FillRectF(g, &edge, (float)g_dbgX, (float)g_dbgY, (float)g_dbgW, 2.0f);
    FillRectF(g, &edge, (float)g_dbgX, (float)(g_dbgY + g_dbgH - 2), (float)g_dbgW, 2.0f);
    FillRectF(g, &edge, (float)g_dbgX, (float)g_dbgY, 2.0f, (float)g_dbgH);
    FillRectF(g, &edge, (float)(g_dbgX + g_dbgW - 2), (float)g_dbgY, 2.0f, (float)g_dbgH);

    static Gdiplus::Font* font = nullptr;
    static Gdiplus::StringFormat* fmt = nullptr;
    if (!font) font = new Gdiplus::Font(L"Microsoft YaHei", 15.0f,
                                        Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    if (!fmt) {
        fmt = new Gdiplus::StringFormat();
        fmt->SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt->SetLineAlignment(Gdiplus::StringAlignmentCenter);
    }

    const wchar_t* labels[DBG_BTN_COUNT] = { L"干枯", L"下雨", L"下雪", L"关闭调试模式" };
    for (int i = 0; i < DBG_BTN_COUNT; ++i) {
        float bx, by;
        DebugButtonRect(i, bx, by);
        bool hot = (i == g_debugHot);

        SolidBrush b(hot ? C(122, 74, 42) : C(74, 52, 36));
        FillRectF(g, &b, bx, by, (float)HUD_BTN_W, (float)HUD_BTN_H);
        SolidBrush line(hot ? C(236, 190, 128) : C(150, 112, 72));
        FillRectF(g, &line, bx, by, (float)HUD_BTN_W, 2.0f);
        FillRectF(g, &line, bx, by + HUD_BTN_H - 2, (float)HUD_BTN_W, 2.0f);
        FillRectF(g, &line, bx, by, 2.0f, (float)HUD_BTN_H);
        FillRectF(g, &line, bx + HUD_BTN_W - 2, by, 2.0f, (float)HUD_BTN_H);

        SolidBrush txt(C(246, 240, 230));
        RectF layout(bx, by + 8.0f, (float)HUD_BTN_W, 24.0f);
        g.DrawString(labels[i], -1, font, layout, fmt, &txt);
    }
}

static void DrawScene(Graphics& g)
{
    // 淡云（降水时才有，画在最底层）
    DrawClouds(g);

    // 雨线画在花盆后面：雨从植物身后落下，碰到植物的一瞬溅起（v3.8）
    if (!g_rain.empty()) {
        SolidBrush rain(C(120, 176, 228, 220));
        for (size_t i = 0; i < g_rain.size(); ++i)
            FillRectF(g, &rain, g_rain[i].x, g_rain[i].y, PIX, PIX * 2.5f);
    }

    // 泥土碎屑
    if (!g_crumbs.empty()) {
        SolidBrush crumb(C(96, 66, 42));
        for (size_t i = 0; i < g_crumbs.size(); ++i)
            FillRectF(g, &crumb, g_crumbs[i].x, g_crumbs[i].y, 3.0f, 3.0f);
    }

    // 天气地面层：积水 + 积雪（画在花盆后面，盆底会压在水雪之上）
    DrawWeatherFloor(g);

    for (size_t i = 0; i < g_pots.size(); ++i) {
        const Pot& p = g_pots[i];
        const Body& b = p.body;
        const float hh = POT_H * 0.5f * p.scale;

        // 花盆最低点，用来算投影高度
        float lowest = b.y - hh + PotMaxY(b.angle, p.scale);

        // 承托面上的投影：只有拿在手里 / 落在半空时才画（v3.8），
        // 稳稳放在任务栏或窗口标题栏上不再显示
        float lift = Clampf((p.surfaceY - lowest) / 160.0f, 0.0f, 1.0f);
        if (lowest < p.surfaceY - 8.0f) {
            float sw = (92.0f - lift * 44.0f) * p.scale;
            int   sa = (int)(100.0f - lift * 70.0f);
            SolidBrush shadow(C(0, 0, 0, sa));
            // 投影整块留在承托面之上，不能漏到任务栏上
            FillPixelEllipse(g, &shadow, b.x, p.surfaceY - 6.0f, sw * 0.5f, 4.0f);
        }

        // 花盆 + 植物共用同一个变换，保证一起旋转、一起缩放
        GraphicsState st = g.Save();
        g.TranslateTransform(b.x, b.y - hh);
        g.RotateTransform(b.angle * 57.2957795f);
        g.ScaleTransform(p.scale, p.scale);
        DrawPlantLocal(g, -POT_H * 0.5f + 3.0f, p.plant, p.sway, p.poking);
        DrawPotLocal(g);
        DrawWaterAnim(g, p);
        DrawSnowCapLocal(g, p);      // 植物顶部的积雪（v3.7）
        g.Restore(st);
    }

    // 枯叶碎片
    if (!g_wilt.empty()) {
        SolidBrush leafA(C(176, 146, 60));
        SolidBrush leafB(C(132, 100, 42));
        for (size_t i = 0; i < g_wilt.size(); ++i) {
            const WiltParticle& w = g_wilt[i];
            FillRectF(g, (i & 1) ? (Brush*)&leafA : (Brush*)&leafB,
                      w.x, w.y, PIX, PIX);
        }
    }

    // 天气粒子（v3.7）：雪花 / 水珠（雨线已改画在花盆后面）
    if (!g_flakes.empty()) {
        SolidBrush flake(C(240, 246, 252));
        for (size_t i = 0; i < g_flakes.size(); ++i)
            FillRectF(g, &flake, g_flakes[i].x, g_flakes[i].y, PIX, PIX);
    }
    if (!g_splash.empty()) {
        SolidBrush drop(C(150, 208, 244, 210));
        for (size_t i = 0; i < g_splash.size(); ++i)
            FillRectF(g, &drop, g_splash[i].x, g_splash[i].y, PIX, PIX);
    }

    // 顶部提示条（v3.8）
    DrawToast(g);
}

static void DestroyBackBuffer()
{
    if (g_dib) { DeleteObject(g_dib); g_dib = nullptr; }
    if (g_memDC) { DeleteDC(g_memDC); g_memDC = nullptr; }
    g_bits = nullptr;
}

static bool CreateBackBuffer(int w, int h)
{
    DestroyBackBuffer();

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;      // 负数 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC hScreen = GetDC(nullptr);
    g_memDC = CreateCompatibleDC(hScreen);
    ReleaseDC(nullptr, hScreen);
    if (!g_memDC) return false;

    g_dib = CreateDIBSection(g_memDC, &bi, DIB_RGB_COLORS, &g_bits, nullptr, 0);
    if (!g_dib || !g_bits) return false;

    SelectObject(g_memDC, g_dib);
    return true;
}

// 一帧里需要改动的矩形：所有花盆（当前帧 + 上一帧）的包围盒并集
static void SpriteBounds(RectI& rc)
{
    float l = 1e9f, t = 1e9f, r = -1e9f, b = -1e9f;

    for (size_t i = 0; i < g_pots.size(); ++i) {
        const Pot& p = g_pots[i];
        const float hh = POT_H * 0.5f * p.scale;
        const float cx = p.body.x;
        const float cy = p.body.y - hh;          // 旋转中心（花盆几何中心）

        // 精灵在“花盆局部坐标”里的外框：上面是植物（含摆动量），下面是盆底。
        // 余量必须给足：玫瑰的花苞绕茎尖旋转后，顶点比 PlantHeight 还高一截，
        // 蒲公英的绒球也顶得很高。超出包围盒的像素不会被 ClearRect 擦掉，
        // 留在缓冲里就成了“拖影”。
        const float halfW = 112.0f * p.scale;
        const float topY  = -(PlantHeight(p.plant) + 80.0f) * p.scale;
        const float botY  = hh + 10.0f;

        // 按当前角度把四个角转到屏幕坐标再取轴对齐包围盒。
        // 只按“植物朝上”估算的话，旋转超过 90° 后植物横躺/朝下就会跑出矩形，
        // 没被清掉的像素就留成了长长拖影。
        const float a = p.body.angle;
        const float ca = cosf(a), sa = sinf(a);
        const float xs[2] = { -halfW, halfW };
        const float ys[2] = { topY, botY };
        for (int k = 0; k < 2; ++k) {
            for (int j = 0; j < 2; ++j) {
                float px = cx + xs[k] * ca - ys[j] * sa;
                float py = cy + xs[k] * sa + ys[j] * ca;
                if (px < l) l = px;
                if (px > r) r = px;
                if (py < t) t = py;
                if (py > b) b = py;
            }
        }

        // 投影
        if (p.surfaceY + 12.0f > b) b = p.surfaceY + 12.0f;
        if (p.surfaceY - 12.0f < t) t = p.surfaceY - 12.0f;
        if (cx - 54.0f * p.scale < l) l = cx - 54.0f * p.scale;
        if (cx + 54.0f * p.scale > r) r = cx + 54.0f * p.scale;
    }

    // 枯叶碎片
    for (size_t i = 0; i < g_wilt.size(); ++i) {
        const WiltParticle& w = g_wilt[i];
        if (w.x - 8.0f < l) l = w.x - 8.0f;
        if (w.x + 8.0f > r) r = w.x + 8.0f;
        if (w.y - 8.0f < t) t = w.y - 8.0f;
        if (w.y + 8.0f > b) b = w.y + 8.0f;
    }

    // 碎屑
    for (size_t i = 0; i < g_crumbs.size(); ++i) {
        const Crumb& c = g_crumbs[i];
        if (c.x - 8.0f < l) l = c.x - 8.0f;
        if (c.x + 8.0f > r) r = c.x + 8.0f;
        if (c.y - 8.0f < t) t = c.y - 8.0f;
        if (c.y + 8.0f > b) b = c.y + 8.0f;
    }

    // 天气粒子（v3.7）：雨线 / 雪花 / 水珠
    for (size_t i = 0; i < g_rain.size(); ++i) {
        const RainDrop& d = g_rain[i];
        if (d.x - 8.0f < l) l = d.x - 8.0f;
        if (d.x + 8.0f > r) r = d.x + 8.0f;
        if (d.y - 8.0f < t) t = d.y - 8.0f;
        if (d.y + 20.0f > b) b = d.y + 20.0f;
    }
    for (size_t i = 0; i < g_flakes.size(); ++i) {
        const SnowFlake& f = g_flakes[i];
        if (f.x - 8.0f < l) l = f.x - 8.0f;
        if (f.x + 8.0f > r) r = f.x + 8.0f;
        if (f.y - 8.0f < t) t = f.y - 8.0f;
        if (f.y + 8.0f > b) b = f.y + 8.0f;
    }
    for (size_t i = 0; i < g_splash.size(); ++i) {
        const Splash& s = g_splash[i];
        if (s.x - 8.0f < l) l = s.x - 8.0f;
        if (s.x + 8.0f > r) r = s.x + 8.0f;
        if (s.y - 8.0f < t) t = s.y - 8.0f;
        if (s.y + 8.0f > b) b = s.y + 8.0f;
    }

    // 地面天气层：积水 / 积雪按天气区域各一段（v3.8.1 分区）
    if (g_puddle > 0.5f || g_snowCover > 0.5f) {
        RebuildWeatherZones();
        for (size_t zi = 0; zi < g_zones.size(); ++zi) {
            const WeatherZone& z = g_zones[zi];
            if (z.l - 4.0f < l) l = z.l - 4.0f;
            if (z.r + 4.0f > r) r = z.r + 4.0f;
        }
        float ytop = g_floorY - g_snowCover - g_puddle - 6.0f;
        if (ytop < t) t = ytop;
        if (g_floorY + 4.0f > b) b = g_floorY + 4.0f;
    }

    // 云（v3.8.2）：盖住每个天气区域整个落雨面，区域顶部往上约 40px
    if (g_precip > 0.02f) {
        RebuildWeatherZones();
        for (size_t zi = 0; zi < g_zones.size(); ++zi) {
            const WeatherZone& z = g_zones[zi];
            if (z.l - 4.0f < l) l = z.l - 4.0f;
            if (z.r + 4.0f > r) r = z.r + 4.0f;
            float ctop = z.top - 44.0f;
            if (ctop < t) t = ctop;
            float cbot = z.top + 4.0f;
            if (cbot > b) b = cbot;
        }
    }

    // 顶部提示条
    if (g_toastTimer > 0.0f) {
        float tl = (float)((g_winW - TOAST_W) / 2) - 2.0f;
        float tr = tl + TOAST_W + 4.0f;
        float tt = (float)ToastTop() - 2.0f;
        float tb = tt + TOAST_H + 4.0f;
        if (tl < l) l = tl;
        if (tr > r) r = tr;
        if (tt < t) t = tt;
        if (tb > b) b = tb;
    }

    if (l > r || t > b) { rc.l = rc.t = rc.r = rc.b = 0; return; }

    rc.l = (int)floorf(l) - 2;
    rc.t = (int)floorf(t) - 2;
    rc.r = (int)ceilf(r) + 2;
    rc.b = (int)ceilf(b) + 2;
}

static void ClearRect(const RectI& rc)
{
    int l = rc.l < 0 ? 0 : rc.l;
    int t = rc.t < 0 ? 0 : rc.t;
    int r = rc.r > g_winW ? g_winW : rc.r;
    int b = rc.b > g_winH ? g_winH : rc.b;
    if (r <= l || b <= t) return;
    for (int y = t; y < b; ++y)
        memset((unsigned char*)g_bits + ((size_t)y * (size_t)g_winW + (size_t)l) * 4,
               0, (size_t)(r - l) * 4);
}

// UpdateLayeredWindow 要求预乘 alpha：只处理脏矩形内的像素
static void PremultiplyRect(const RectI& rc)
{
    int l = rc.l < 0 ? 0 : rc.l;
    int t = rc.t < 0 ? 0 : rc.t;
    int r = rc.r > g_winW ? g_winW : rc.r;
    int b = rc.b > g_winH ? g_winH : rc.b;
    if (r <= l || b <= t) return;

    for (int y = t; y < b; ++y) {
        unsigned char* p = (unsigned char*)g_bits + ((size_t)y * (size_t)g_winW + (size_t)l) * 4;
        for (int x = l; x < r; ++x, p += 4) {
            unsigned a = p[3];
            if (a == 255) continue;
            if (a == 0) { p[0] = p[1] = p[2] = 0; continue; }
            p[0] = (unsigned char)((p[0] * a + 127) / 255);
            p[1] = (unsigned char)((p[1] * a + 127) / 255);
            p[2] = (unsigned char)((p[2] * a + 127) / 255);
        }
    }
}

// =============================================================================
//  编辑模式工具条（画在主窗口顶部，随场景一起提交）
// =============================================================================
static void HudLayout()
{
    g_hudW = HUD_PAD * 2 + HUD_BTN_W * HUD_BTN_COUNT + HUD_GAP * (HUD_BTN_COUNT - 1);
    g_hudH = HUD_BTN_H + HUD_PAD * 2;
    g_hudX = (g_winW - g_hudW) / 2;
    g_hudY = 20;
}

static void HudButtonRect(int i, float& bx, float& by)
{
    bx = (float)(g_hudX + HUD_PAD + i * (HUD_BTN_W + HUD_GAP));
    by = (float)(g_hudY + HUD_PAD);
}

static int HudButtonAt(float mx, float my)
{
    if (!g_editMode) return -1;
    HudLayout();
    for (int i = 0; i < HUD_BTN_COUNT; ++i) {
        float bx, by;
        HudButtonRect(i, bx, by);
        if (mx >= bx && mx <= bx + HUD_BTN_W && my >= by && my <= by + HUD_BTN_H)
            return i;
    }
    return -1;
}

static void DrawEditHud(Graphics& g)
{
    HudLayout();

    // 必须用 GDI+ 自己的文字光栅化：走 GDI 的话写进 DIB 的 alpha 是 0，预乘后文字会消失
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

    // 面板底色 + 像素边框
    SolidBrush panel(C(26, 20, 16, 235));
    FillRectF(g, &panel, (float)g_hudX, (float)g_hudY, (float)g_hudW, (float)g_hudH);
    SolidBrush edge(C(196, 148, 92, 240));
    FillRectF(g, &edge, (float)g_hudX, (float)g_hudY, (float)g_hudW, 2.0f);
    FillRectF(g, &edge, (float)g_hudX, (float)(g_hudY + g_hudH - 2), (float)g_hudW, 2.0f);
    FillRectF(g, &edge, (float)g_hudX, (float)g_hudY, 2.0f, (float)g_hudH);
    FillRectF(g, &edge, (float)(g_hudX + g_hudW - 2), (float)g_hudY, 2.0f, (float)g_hudH);

    static Gdiplus::Font* font = nullptr;
    static Gdiplus::StringFormat* fmt = nullptr;
    if (!font) font = new Gdiplus::Font(L"Microsoft YaHei", 15.0f,
                                       Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    if (!fmt) {
        fmt = new Gdiplus::StringFormat();
        fmt->SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt->SetLineAlignment(Gdiplus::StringAlignmentCenter);
    }

    wchar_t dry[64];
    int ds = (int)g_drySeconds;
    if (ds >= 60 && ds % 60 == 0)
        wsprintfW(dry, L"干枯 %d 分钟", ds / 60);
    else
        wsprintfW(dry, L"干枯 %d 秒", ds);
    const wchar_t* labels[HUD_BTN_COUNT] = {
        L"+ 新花盆", dry,
        (g_hudMsgTimer > 0.0f ? L"已保存" : L"保存"), L"完成"
    };

    for (int i = 0; i < HUD_BTN_COUNT; ++i) {
        float bx, by;
        HudButtonRect(i, bx, by);
        bool hot = (i == g_hudHot);

        SolidBrush b(hot ? C(122, 74, 42) : C(74, 52, 36));
        FillRectF(g, &b, bx, by, (float)HUD_BTN_W, (float)HUD_BTN_H);
        SolidBrush line(hot ? C(236, 190, 128) : C(150, 112, 72));
        FillRectF(g, &line, bx, by, (float)HUD_BTN_W, 2.0f);
        FillRectF(g, &line, bx, by + HUD_BTN_H - 2, (float)HUD_BTN_W, 2.0f);
        FillRectF(g, &line, bx, by, 2.0f, (float)HUD_BTN_H);
        FillRectF(g, &line, bx + HUD_BTN_W - 2, by, 2.0f, (float)HUD_BTN_H);

        SolidBrush txt(C(246, 240, 230));
        RectF layout(bx, by + 8.0f, (float)HUD_BTN_W, 24.0f);
        g.DrawString(labels[i], -1, font, layout, fmt, &txt);
    }
}

static void Present()
{
    if (!g_hwnd || !g_visible || !g_bits || g_winW <= 0 || g_winH <= 0) return;

    // 这一帧精灵的包围盒
    RectI cur;
    SpriteBounds(cur);

    // 需要清理/重绘的区域：这一帧 + 上一帧（擦掉残影）
    RectI rc;
    if (g_fullRedraw) {
        rc.l = 0; rc.t = 0; rc.r = g_winW; rc.b = g_winH;
    } else {
        rc = cur;
        if (g_hasPrevDirty) {
            if (g_prevDirty.l < rc.l) rc.l = g_prevDirty.l;
            if (g_prevDirty.t < rc.t) rc.t = g_prevDirty.t;
            if (g_prevDirty.r > rc.r) rc.r = g_prevDirty.r;
            if (g_prevDirty.b > rc.b) rc.b = g_prevDirty.b;
        }
    }

    // 编辑模式工具条：并进脏矩形，保证悬停/提示文字变化能被提交
    if (g_editMode) {
        HudLayout();
        if (g_hudX - 2 < rc.l) rc.l = g_hudX - 2;
        if (g_hudY - 2 < rc.t) rc.t = g_hudY - 2;
        if (g_hudX + g_hudW + 2 > rc.r) rc.r = g_hudX + g_hudW + 2;
        if (g_hudY + g_hudH + 2 > rc.b) rc.b = g_hudY + g_hudH + 2;
    }

    // 调试模式工具条（v3.8）
    if (g_debugMode) {
        DebugLayout();
        if (g_dbgX - 2 < rc.l) rc.l = g_dbgX - 2;
        if (g_dbgY - 2 < rc.t) rc.t = g_dbgY - 2;
        if (g_dbgX + g_dbgW + 2 > rc.r) rc.r = g_dbgX + g_dbgW + 2;
        if (g_dbgY + g_dbgH + 2 > rc.b) rc.b = g_dbgY + g_dbgH + 2;
    }

    if (rc.l < 0) rc.l = 0;
    if (rc.t < 0) rc.t = 0;
    if (rc.r > g_winW) rc.r = g_winW;
    if (rc.b > g_winH) rc.b = g_winH;

    ClearRect(rc);

    {
        Graphics gfx(g_memDC);
        gfx.SetSmoothingMode(Gdiplus::SmoothingModeNone);        // 关抗锯齿 -> 像素感
        gfx.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        gfx.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        // 硬裁剪：任何像素都不许画到任务栏上沿以下（任务栏区域归任务栏）
        if (g_floorY > 0.0f) gfx.SetClip(RectF(0.0f, 0.0f, (float)g_winW, g_floorY));
        DrawScene(gfx);
        if (g_editMode) DrawEditHud(gfx);
        if (g_debugMode) DrawDebugHud(gfx);
    }
    PremultiplyRect(rc);

    POINT src = { 0, 0 };
    SIZE  size = { g_winW, g_winH };
    POINT dst = { g_winX, g_winY };
    BLENDFUNCTION blend;
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    if (g_fullRedraw) {
        HDC hScreen = GetDC(nullptr);
        UpdateLayeredWindow(g_hwnd, hScreen, &dst, &size, g_memDC, &src, 0, &blend, ULW_ALPHA);
        ReleaseDC(nullptr, hScreen);
    } else {
        // 只把变动的区域提交给桌面，避免每帧整屏 4MB 级别的拷贝
        RECT dirty = { rc.l + g_winX, rc.t + g_winY, rc.r + g_winX, rc.b + g_winY };
        UPDATELAYEREDWINDOWINFO info;
        ZeroMemory(&info, sizeof(info));
        info.cbSize = sizeof(info);
        info.pptDst = &dst;
        info.psize = &size;
        info.hdcSrc = g_memDC;
        info.pptSrc = &src;
        info.pblend = &blend;
        info.dwFlags = ULW_ALPHA;
        info.prcDirty = &dirty;
        UpdateLayeredWindowIndirect(g_hwnd, &info);
    }

    g_prevDirty = cur;
    g_hasPrevDirty = true;
    g_fullRedraw = false;
}

// =============================================================================
//  三·五、配置持久化 / 导出植物图片
// =============================================================================

static std::wstring ExeDir()
{
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return std::wstring();
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return std::wstring();
    *(slash + 1) = 0;
    return std::wstring(exe);
}

static std::wstring ConfigPath()
{
    return ExeDir() + L"TaskbarPlant.ini";
}

// 读上次保存的花盆清单：植物 / 位置 / 大小 / 水分
static void LoadConfig()
{
    g_seeds.clear();
    HANDLE f = CreateFileW(ConfigPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    static char buf[16384];
    DWORD got = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(f);
    buf[got] = 0;

    const char* p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (!*p) break;
        if (*p == '#') { while (*p && *p != '\n') ++p; continue; }

        // dry <秒>：满水到干透的时间
        if (strncmp(p, "dry", 3) == 0) {
            float d = g_drySeconds;
            if (sscanf(p + 3, "%f", &d) == 1)
                g_drySeconds = Clampf(d, 60.0f, 86400.0f);
            while (*p && *p != '\n') ++p;
            continue;
        }

        PotSeed s = { 0, 0.0f, 0.0f, 1.0f, 1.0f };
        int n = sscanf(p, "%d %f %f %f %f", &s.plant, &s.x, &s.y, &s.scale, &s.moisture);
        if (n >= 3) {
            if (s.plant < 0 || s.plant >= PLANT_COUNT) s.plant = 0;
            s.scale = Clampf(s.scale, POT_SCALE_MIN, POT_SCALE_MAX);
            s.moisture = Clampf(s.moisture, 0.0f, 1.0f);
            g_seeds.push_back(s);
        }
        while (*p && *p != '\n') ++p;
    }
}

// 写盘（v3.8.3 重做）：ini 里的布局 = 上一次在编辑模式里点「保存」时的场景快照。
// manual=true 表示用户在编辑模式里点了「保存」——用当前场景整体覆盖布局快照；
// 自动保存（每 5 秒）与退出保存传 false——只把各盆的水分回写进快照，
// **位置、数量、大小永远不动**。拖动、抛掷、放新盆、移除盆这些改动，
// 只有进编辑模式点「保存」才会持久化到 ini。
static void SaveConfig(bool manual = false)
{
    if (manual) {
        g_savedLayout.clear();
        for (size_t i = 0; i < g_pots.size(); ++i) {
            const Pot& p = g_pots[i];
            PotSeed s = { (int)p.plant, p.body.x, p.body.y, p.scale, p.moisture };
            g_savedLayout.push_back(s);
            g_pots[i].slot = (int)i;            // 每盆绑定到新布局的槽位
        }
    } else {
        for (size_t i = 0; i < g_pots.size(); ++i) {
            const Pot& p = g_pots[i];
            if (p.slot >= 0 && p.slot < (int)g_savedLayout.size())
                g_savedLayout[p.slot].moisture = p.moisture;
        }
    }

    HANDLE f = CreateFileW(ConfigPath().c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    DWORD wr = 0;
    const char* hdr = "# TaskbarPlant: plant x y scale moisture\r\n";
    WriteFile(f, hdr, (DWORD)strlen(hdr), &wr, nullptr);

    char dry[32];
    int dn = sprintf(dry, "dry %d\r\n", (int)g_drySeconds);
    WriteFile(f, dry, (DWORD)dn, &wr, nullptr);

    for (size_t i = 0; i < g_savedLayout.size(); ++i) {
        const PotSeed& s = g_savedLayout[i];
        char line[96];
        int n = sprintf(line, "%d %.1f %.1f %.3f %.3f\r\n",
                        s.plant, s.x, s.y, s.scale, s.moisture);
        WriteFile(f, line, (DWORD)n, &wr, nullptr);
    }
    CloseHandle(f);

    // 有花盆站在窗口标题栏上时提示——ini 只记录客户区坐标，
    // 重启后这些花盆只会落回任务栏上（只在编辑模式手动保存时提示）
    if (manual) {
        for (size_t i = 0; i < g_pots.size(); ++i) {
            if (g_pots[i].supportHwnd) {
                ShowToast(L"提示：标题栏上的花盆只能保存在任务栏，重启后会落回任务栏");
                break;
            }
        }
    }
}

static int GetEncoderClsid(const WCHAR* mime, CLSID* clsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<BYTE> buf(size);
    Gdiplus::ImageCodecInfo* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) { *clsid = codecs[i].Clsid; return (int)i; }
    }
    return -1;
}

// 把每一种植物连花盆一起画成透明 PNG，放进 exe 同目录的 Plants\ 文件夹
static void ExportPlantImages()
{
    std::wstring dir = ExeDir();
    if (dir.empty()) return;
    dir += L"Plants";
    CreateDirectoryW(dir.c_str(), nullptr);

    CLSID png = { 0 };
    if (GetEncoderClsid(L"image/png", &png) < 0) return;

    for (int i = 0; i < PLANT_COUNT; ++i) {
        // 画布必须装得下整株植物：植株站在盆口，且玫瑰的花苞绕茎尖旋转后会高过 PlantHeight
        const float plantTop = POT_H * 0.5f - 3.0f + PlantHeight((PlantType)i) + 80.0f;
        const int   w = 224;
        const float originY = 16.0f + plantTop;                      // 图像顶部到盆心的距离
        const int   h = (int)(originY + POT_H * 0.5f + 16.0f);       // 底部留出盆身 + 边距

        Gdiplus::Bitmap bmp(w, h, PixelFormat32bppARGB);
        Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
        g.Clear(Color(0, 0, 0, 0));
        g.TranslateTransform((float)w * 0.5f, originY);
        // 和游戏内一致：植株根部落在盆口，不能陷进盆身里
        DrawPlantLocal(g, -POT_H * 0.5f + 3.0f, (PlantType)i, 0.0f, false);
        DrawPotLocal(g);

        wchar_t file[MAX_PATH];
        wsprintfW(file, L"%s\\%s.png", dir.c_str(), kMenuItemText[i]);
        bmp.Save(file, &png, nullptr);
    }
}

// =============================================================================
//  四、铺满工作区（不碰任务栏）/ 计算地面 / 智能隐藏
// =============================================================================

static void SpawnPot(float x, float y, PlantType plant);   // 定义在交互部分

static bool g_layoutReady = false;     // 是否已经完成首次布局（放好第一个花盆）

static void LayoutWindow()
{
    // 任务栏及其所在显示器
    HWND hTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT tb = { 0, 0, 0, 0 };
    bool hasTray = (hTray != nullptr) && GetWindowRect(hTray, &tb);

    HMONITOR mon = MonitorFromWindow(hTray ? hTray : GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    bool hasMon = GetMonitorInfoW(mon, &mi) != FALSE;

    RECT m = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (hasMon) m = mi.rcMonitor;

    // 悬浮层只铺“工作区”，窗口矩形绝不覆盖任务栏。
    // 这一点很关键：只要有一个窗口的矩形盖住了整块显示器（哪怕区域被
    // SetWindowRgn 挖空、像素全透明），Explorer 就会把它当成“全屏应用”，
    // 把任务栏的 WS_EX_TOPMOST 去掉。任务栏一旦不再是 topmost，
    // 之后任何被激活的窗口都会被抬到任务栏上面，把任务栏盖住。
    RECT area = m;
    if (hasMon && mi.rcWork.right > mi.rcWork.left && mi.rcWork.bottom > mi.rcWork.top)
        area = mi.rcWork;                      // 系统已把任务栏 / 其它 AppBar 扣掉

    int trayH = tb.bottom - tb.top;
    int trayW = tb.right - tb.left;
    bool horizontalBar = hasTray && trayH >= 2 && trayW > trayH && tb.top > m.top + 20;
    // 自动隐藏的任务栏：工作区通常是整块屏幕，按任务栏上沿再收一下，
    // 免得悬浮层又变成“盖住整块显示器”的全屏窗口
    if (horizontalBar && tb.top < area.bottom && tb.bottom <= m.bottom + 2)
        area.bottom = tb.top;

    int vx = area.left;
    int vy = area.top;
    int vw = area.right - area.left;
    int vh = area.bottom - area.top;

    // 客户区底边就是任务栏上沿，也是花盆的默认地面
    float floorY = (float)vh;

    // 什么都没变就别反复重设窗口（避免闪烁）
    if (g_layoutReady && vx == g_winX && vy == g_winY && vw == g_winW && vh == g_winH
        && floorY == g_floorY) {
        return;
    }

    bool sizeChanged = (g_winW != vw || g_winH != vh);
    g_winX = vx;
    g_winY = vy;
    g_winW = vw;
    g_winH = vh;
    g_floorY = floorY;

    if (sizeChanged && g_hwnd) {
        if (!CreateBackBuffer(g_winW, g_winH)) return;
    }

    // 首次：恢复上次的花盆（植物 / 数量 / 位置 / 大小 / 水分）
    if (!g_layoutReady) {
        g_layoutReady = true;
        if (!g_seeds.empty()) {
            for (size_t i = 0; i < g_seeds.size(); ++i) {
                const PotSeed& s = g_seeds[i];
                SpawnPot(s.x, s.y, (PlantType)s.plant);
                Pot& np = g_pots.back();
                np.scale = s.scale;
                np.moisture = s.moisture;
                np.slot = (int)i;                       // 从 ini 恢复的盆绑定到布局槽位
                np.body.x = Clampf(s.x, POT_W * 0.5f * s.scale,
                                   (float)g_winW - POT_W * 0.5f * s.scale);
                np.body.y = Clampf(s.y, POT_H * s.scale, g_floorY);
                np.body.vy = 0.0f;
            }
            g_savedLayout = g_seeds;                    // 启动布局 = ini 里的布局
        } else {
            SpawnPot((float)((m.left + m.right) / 2 - vx), g_floorY, PLANT_SUNFLOWER);
            Pot& np = g_pots.back();
            np.body.vy = 0.0f;
            np.slot = 0;                                // 首次运行的默认盆视为已保存
            PotSeed s = { (int)np.plant, np.body.x, np.body.y, np.scale, np.moisture };
            g_savedLayout.push_back(s);
        }
    } else {
        // 分辨率 / 任务栏变化后把花盆夹回工作区内
        for (size_t i = 0; i < g_pots.size(); ++i) {
            Body& b = g_pots[i].body;
            float hw = POT_W * 0.5f * g_pots[i].scale;
            b.x = Clampf(b.x, hw, (float)g_winW - hw);
            b.y = Clampf(b.y, POT_H * g_pots[i].scale, g_floorY);
        }
    }

    if (g_hwnd)
        SetWindowPos(g_hwnd, HWND_TOPMOST, g_winX, g_winY, g_winW, g_winH,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);

    g_fullRedraw = true;
    g_hasPrevDirty = false;
}

// 前台窗口是否铺满了它所在显示器的整个屏幕
static bool IsForegroundFullscreen()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg == g_hwnd || (g_menu && fg == g_menu)) return false;
    if (!IsWindowVisible(fg) || IsIconic(fg)) return false;
    if (IsShellWindow(fg)) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return false;

    RECT r;
    if (!GetWindowRect(fg, &r)) return false;

    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    const RECT& m = mi.rcMonitor;

    // 留 1px 误差，避免边框像素导致漏判
    return r.left <= m.left + 1 && r.top <= m.top + 1 &&
           r.right >= m.right - 1 && r.bottom >= m.bottom - 1;
}

static void UpdateVisibility()
{
    bool fs = IsForegroundFullscreen();
    if (fs) {
        if (g_visible) {
            if (g_menuOpen && g_menu) DestroyWindow(g_menu);
            ShowWindow(g_hwnd, SW_HIDE);
            g_visible = false;
        }
    } else if (!g_visible) {
        SetWindowPos(g_hwnd, HWND_TOPMOST, g_winX, g_winY, g_winW, g_winH,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(g_hwnd, SW_SHOWNA);   // 显示但不抢焦点
        g_visible = true;
        g_fullRedraw = true;
        Present();
    }
}

// =============================================================================
//  五、鼠标交互
// =============================================================================

// 窗口坐标 -> 某个花盆的局部坐标
static void WorldToLocal(const Pot& p, float wx, float wy, float& lx, float& ly)
{
    float cx = p.body.x;
    float cy = p.body.y - POT_H * 0.5f * p.scale;
    float dx = wx - cx;
    float dy = wy - cy;
    float c = cosf(p.body.angle);
    float s = sinf(p.body.angle);
    lx = dx * c + dy * s;
    ly = -dx * s + dy * c;
}

static bool HitPot(const Pot& p, float wx, float wy)
{
    float lx, ly;
    WorldToLocal(p, wx, wy, lx, ly);
    return fabsf(lx) <= (POT_W * 0.5f + 6.0f) * p.scale &&
           ly >= (-POT_H * 0.5f - 10.0f) * p.scale &&
           ly <= (POT_H * 0.5f + 4.0f) * p.scale;
}

static bool HitPlant(const Pot& p, float wx, float wy)
{
    float lx, ly;
    WorldToLocal(p, wx, wy, lx, ly);
    return fabsf(lx) <= 42.0f * p.scale &&
           ly >= -(POT_H * 0.5f + PlantHeight(p.plant)) * p.scale &&
           ly <= -POT_H * 0.5f * p.scale;
}

// 点到了哪个花盆（后画的在上面，所以从后往前找）
static int PickPot(float wx, float wy, bool wantPlant)
{
    for (int i = (int)g_pots.size() - 1; i >= 0; --i) {
        if (wantPlant ? HitPlant(g_pots[i], wx, wy) : HitPot(g_pots[i], wx, wy))
            return i;
    }
    return -1;
}

static void SpawnPot(float x, float y, PlantType plant);   // 定义见下

// 编辑模式工具条上的四个按钮
static void HudAction(int btn)
{
    switch (btn) {
    case HUD_ADD: {
        // 在屏幕中间附近随机撒一个新花盆
        float cx = (float)g_winW * 0.5f + (float)((rand() % 401) - 200);
        SpawnPot(cx, g_floorY, (PlantType)(rand() % PLANT_COUNT));
        break;
    }
    case HUD_DRY: {
        // 在几档常用时长之间循环（5 分钟 ~ 4 小时）
        static const float kDry[] = { 300.0f, 900.0f, 1800.0f, 3600.0f, 7200.0f, 14400.0f };
        const int n = (int)(sizeof(kDry) / sizeof(kDry[0]));
        int cur = 2;                     // 默认对应 30 分钟
        for (int i = 0; i < n; ++i)
            if (fabsf(kDry[i] - g_drySeconds) < 1.0f) { cur = i; break; }
        g_drySeconds = kDry[(cur + 1) % n];
        g_fullRedraw = true;
        break;
    }
    case HUD_SAVE:
        SaveConfig(true);                // 只有点这里才写 ini（手动保存，带提示）
        g_hudMsgTimer = 2.0f;
        g_fullRedraw = true;
        break;
    case HUD_DONE:
        g_editMode = false;
        g_hudHot = -1;
        g_fullRedraw = true;
        break;
    }
}

static void OnLButtonDown(HWND hwnd, LPARAM lp)
{
    float mx = (float)GET_X_LPARAM(lp);
    float my = (float)GET_Y_LPARAM(lp);

    int hud = HudButtonAt(mx, my);
    if (hud >= 0) { HudAction(hud); return; }

    // 调试模式工具条（v3.8）
    int dbg = DebugButtonAt(mx, my);
    if (dbg >= 0) { DebugAction(dbg); return; }

    int pot = PickPot(mx, my, false);
    if (pot >= 0) {
        Pot& p = g_pots[pot];
        g_activePot = pot;
        SetCapture(hwnd);
        p.body.dragged = true;
        p.poking = false;
        p.dragOffX = mx - p.body.x;
        p.dragOffY = my - p.body.y;
        p.body.angVel *= 0.2f;
        return;
    }

    pot = PickPot(mx, my, true);
    if (pot >= 0) {
        Pot& p = g_pots[pot];
        g_activePot = pot;
        SetCapture(hwnd);
        p.poking = true;
        p.body.dragged = false;
        p.swayVel = 0.0f;
    }
}

static void OnMouseMove(LPARAM lp)
{
    float mx = (float)GET_X_LPARAM(lp);
    float my = (float)GET_Y_LPARAM(lp);
    g_mouseX = mx;
    g_mouseY = my;

    // 编辑模式：工具条按钮悬停高亮
    int hot = HudButtonAt(mx, my);
    if (hot != g_hudHot) {
        g_hudHot = hot;
        g_fullRedraw = true;
    }

    // 调试模式工具条悬停高亮（v3.8）
    int dhot = DebugButtonAt(mx, my);
    if (dhot != g_debugHot) {
        g_debugHot = dhot;
        g_fullRedraw = true;
    }

    if (g_activePot < 0 || g_activePot >= (int)g_pots.size()) return;
    Pot& p = g_pots[g_activePot];
    if (!p.body.dragged && !p.poking) return;

    if (p.body.dragged) {
        // 拖拽时只受桌面四边限制，可以停在任意窗口的标题栏上方
        float hw = POT_W * 0.5f * p.scale;
        float tx = Clampf(mx - p.dragOffX, hw, (float)g_winW - hw);
        float ty = Clampf(my - p.dragOffY, POT_H * p.scale, g_floorY);

        // 用相邻两帧位移估算甩出速度（限幅，避免快速拖动变成“发射炮弹”）
        float instVx = (tx - p.body.x) / DT;
        float instVy = (ty - p.body.y) / DT;
        p.body.vx = Clampf(p.body.vx * 0.4f + instVx * 0.6f, -1500.0f, 1500.0f);
        p.body.vy = Clampf(p.body.vy * 0.4f + instVy * 0.6f, -1500.0f, 1500.0f);
        p.body.x = tx;
        p.body.y = ty;
        p.body.angVel = Clampf(p.body.vx * 0.0009f, -2.5f, 2.5f);

        // 植物被拖着甩，产生惯性摆动（方向与加速度相反）
        p.swayVel = Clampf(p.swayVel - p.body.vx * 0.0045f, -16.0f, 16.0f);
    } else if (p.poking) {
        // 直接拖着植物走：茎被鼠标拉弯
        float bend = Clampf((mx - p.body.x) / 55.0f, -1.3f, 1.3f);
        p.sway += (bend - p.sway) * 0.35f;
        p.swayVel = 0.0f;
    }
}

static void OnLButtonUp()
{
    if (g_activePot >= 0 && g_activePot < (int)g_pots.size()) {
        Pot& p = g_pots[g_activePot];
        if (p.body.dragged || p.poking) ReleaseCapture();
        p.body.dragged = false;
        if (p.poking) {
            p.poking = false;
            p.swayVel -= p.sway * 4.0f;      // 松手后弹回去
        }
        p.body.vx = Clampf(p.body.vx, -1500.0f, 1500.0f);
        p.body.vy = Clampf(p.body.vy, -1500.0f, 1500.0f);
        p.body.angVel = Clampf(p.body.angVel, -7.0f, 7.0f);
    }
}

// 在指定位置放一个新花盆（x 为盆底中心，y 为盆底）
static void SpawnPot(float x, float y, PlantType plant)
{
    Pot p;
    ZeroMemory(&p, sizeof(Pot));
    p.scale = 1.0f;
    p.moisture = 1.0f;
    p.body.x = Clampf(x, POT_W * 0.5f, (float)g_winW - POT_W * 0.5f);
    p.body.y = Clampf(y, POT_H, (float)g_winH);
    p.body.vy = -160.0f;          // 放下来时轻轻一跳
    p.body.angVel = 0.0f;
    p.plant = plant;
    p.swayVel = 3.0f;
    p.windPh = (float)(rand() % 1000) * 0.00628f;    // 0 ~ 2π，每盆错开风相位
    p.surfaceY = g_floorY;
    p.slot = -1;               // 新放的盆还没有保存过，不进布局快照
    g_pots.push_back(p);
    g_fullRedraw = true;
}

static void ApplyMenuChoice(int idx)
{
    Pot* p = (g_activePot >= 0 && g_activePot < (int)g_pots.size()) ? &g_pots[g_activePot] : nullptr;

    if (idx >= 0 && idx < PLANT_COUNT) {
        if (!p) return;                     // 没点中花盆时，植物菜单不生效
        p->plant = (PlantType)idx;
        p->sway = 0.0f;
        p->swayVel = 3.0f;                  // 换植物时晃一下
        p->body.vy = -90.0f;                // 轻轻一跳
        g_fullRedraw = true;
    } else if (idx == MENU_CMD_WATER) {
        if (p) {
            p->waterAnim = WATER_DURATION;
            p->swayVel = 2.2f;
            g_fullRedraw = true;
        }
    } else if (idx == MENU_CMD_EDIT) {
        g_editMode = !g_editMode;
        g_hudHot = -1;
        g_hudMsgTimer = 0.0f;
        g_fullRedraw = true;
    } else if (idx == MENU_CMD_SCALE_UP || idx == MENU_CMD_SCALE_DOWN) {
        if (p) {
            float ns = p->scale + ((idx == MENU_CMD_SCALE_UP) ? POT_SCALE_STEP : -POT_SCALE_STEP);
            p->scale = Clampf(ns, POT_SCALE_MIN, POT_SCALE_MAX);
            p->body.vy = -90.0f;
            g_fullRedraw = true;
        }
    } else if (idx == MENU_CMD_SPAWN) {
        SpawnPot(g_spawnX, g_spawnY, p ? p->plant : PLANT_SUNFLOWER);
    } else if (idx == MENU_CMD_REMOVE) {
        if (p) {
            g_pots.erase(g_pots.begin() + g_activePot);
            g_activePot = -1;
            g_fullRedraw = true;
        }
    } else if (idx == MENU_CMD_EXPORT) {
        ExportPlantImages();
    } else if (idx == MENU_CMD_QUIT) {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);   // 回到消息循环后再退出，避免在本窗口过程里销毁自己
    } else if (idx == MENU_CMD_VERSION) {
        // 版本号连点 7 次 = 打开调试模式（菜单保持打开，静默计数，无任何提示）
        g_versionClicks++;
        g_versionClickTimer = 3.0f;
        if (g_versionClicks >= 7) {
            g_versionClicks = 0;
            g_versionClickTimer = 0.0f;
            g_debugMode = true;
            g_fullRedraw = true;
            if (g_menu) DestroyWindow(g_menu);   // 开启成功才关菜单
        }
    }
}

// =============================================================================
//  六、像素风右键菜单
// =============================================================================

// 命令区起始 y（植物区 + 分隔带之后）
static inline int MenuCmdTop()
{
    return kMenuPad + kPlantRows * kMenuRowH + kMenuSepH;
}

static int MenuHeight()
{
    return MenuCmdTop() + (MENU_ITEM_COUNT - PLANT_COUNT) * kMenuRowH + kMenuPad;
}

// 菜单项矩形：植物两列排，命令单列排
static void MenuItemRect(int idx, RECT& r)
{
    if (idx < PLANT_COUNT) {
        int row = idx / 2;
        int col = idx % 2;
        r.left = kMenuPad + col * kMenuColW;
        r.right = r.left + kMenuColW - 4;
        r.top = kMenuPad + row * kMenuRowH;
        r.bottom = r.top + kMenuRowH - 2;
    } else {
        int c = idx - PLANT_COUNT;
        r.left = kMenuPad;
        r.right = kMenuW - kMenuPad;
        r.top = MenuCmdTop() + c * kMenuRowH;
        r.bottom = r.top + kMenuRowH - 2;
    }
}

static int MenuItemFromPoint(LPARAM lp)
{
    POINT p;
    p.x = GET_X_LPARAM(lp);
    p.y = GET_Y_LPARAM(lp);
    for (int i = 0; i < kMenuCount; ++i) {
        RECT r;
        MenuItemRect(i, r);
        r.bottom += 2;              // 把行间隙也算进命中区
        if (PtInRect(&r, p)) return i;
    }
    return -1;
}

static void PaintMenu(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);

    HBRUSH bg = CreateSolidBrush(RGB(38, 26, 18));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    // 双像素外框
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(198, 120, 68));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 1, 1, rc.right - 1, rc.bottom - 1);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    HGDIOBJ oldFont = SelectObject(dc, g_menuFont);
    SetBkMode(dc, TRANSPARENT);

    // 每种植物一个代表色
    static const COLORREF kDots[PLANT_COUNT] = {
        RGB(78, 154, 78),   // 仙人掌
        RGB(247, 197, 49),  // 向日葵
        RGB(150, 60, 70),   // 食人花
        RGB(226, 78, 106),  // 郁金香
        RGB(212, 62, 62),   // 蘑菇
        RGB(46, 116, 66),   // 小松树
        RGB(126, 180, 134), // 多肉
        RGB(150, 194, 96),  // 竹子
        RGB(206, 52, 78),   // 玫瑰
        RGB(152, 106, 202), // 薰衣草
        RGB(96, 176, 76),   // 三叶草
        RGB(212, 58, 46),   // 辣椒
        RGB(240, 240, 226), // 蒲公英
        RGB(244, 196, 58),  // 雏菊
        RGB(96, 168, 118),  // 芦荟
        RGB(108, 122, 214), // 牵牛花
        RGB(244, 178, 196)  // 樱花
    };
    // 命令项的代表色：浇水 / 编辑 / 调大 / 调小 / 放置 / 移除 / 导出 / 退出 / 版本号
    static const COLORREF kCmdDots[MENU_ITEM_COUNT - PLANT_COUNT] = {
        RGB(92, 176, 232), RGB(180, 170, 120), RGB(196, 148, 92), RGB(150, 112, 72),
        RGB(120, 170, 110), RGB(190, 90, 80), RGB(96, 150, 190), RGB(130, 130, 130),
        RGB(160, 160, 160)
    };

    // 植物区与命令区之间的一条分隔线
    {
        RECT sep;
        sep.left = kMenuPad;
        sep.right = kMenuW - kMenuPad;
        sep.top = MenuCmdTop() - kMenuSepH / 2 - 1;
        sep.bottom = sep.top + 2;
        HBRUSH sb = CreateSolidBrush(RGB(96, 64, 40));
        FillRect(dc, &sep, sb);
        DeleteObject(sb);
    }

    for (int i = 0; i < kMenuCount; ++i) {
        RECT row;
        MenuItemRect(i, row);

        if (i == g_menuHover) {
            HBRUSH hb = CreateSolidBrush(RGB(122, 74, 42));
            FillRect(dc, &row, hb);
            DeleteObject(hb);
        }

        // 左侧小色块，代替图标
        COLORREF dot = (i < PLANT_COUNT) ? kDots[i] : kCmdDots[i - PLANT_COUNT];
        RECT dotRc = { row.left + 8, row.top + 7, row.left + 20, row.top + 19 };
        HBRUSH db = CreateSolidBrush(dot);
        FillRect(dc, &dotRc, db);
        DeleteObject(db);

        RECT textRc = row;
        textRc.left += 28;
        SetTextColor(dc, (i == g_menuHover) ? RGB(255, 240, 210) : RGB(216, 188, 152));
        DrawTextW(dc, kMenuItemText[i], -1, &textRc, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    }

    SelectObject(dc, oldFont);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:
        PaintMenu(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE: {
        int idx = MenuItemFromPoint(lp);
        if (idx != g_menuHover) {
            g_menuHover = idx;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN:
        return 0;

    case WM_LBUTTONUP: {
        int idx = MenuItemFromPoint(lp);
        if (idx == MENU_CMD_VERSION) {
            ApplyMenuChoice(idx);        // 版本号项不关菜单，方便连续点击 7 次（v3.8.2）
        } else if (idx >= 0) {
            ApplyMenuChoice(idx);
            DestroyWindow(hwnd);
        }
        return 0;
    }

    // 捕获鼠标后，点到菜单外面也在本窗口收到消息，直接关掉
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
        DestroyWindow(hwnd);
        return 0;

    case WM_NCDESTROY:
        g_menu = nullptr;
        g_menuOpen = false;
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowPlantMenu(HWND owner, LPARAM lp)
{
    if (g_menuOpen && g_menu) DestroyWindow(g_menu);

    float mx = (float)GET_X_LPARAM(lp);
    float my = (float)GET_Y_LPARAM(lp);

    // 右键点中哪个花盆就操作哪个；点在空白处时只保留“放置新花盆 / 退出”
    g_activePot = PickPot(mx, my, false);
    if (g_activePot < 0) g_activePot = PickPot(mx, my, true);
    g_spawnX = mx;
    g_spawnY = my;

    POINT pt;
    pt.x = GET_X_LPARAM(lp);
    pt.y = GET_Y_LPARAM(lp);
    ClientToScreen(owner, &pt);

    int h = MenuHeight();
    int x = pt.x - kMenuW;          // 开在光标左上方
    int y = pt.y - h - 6;

    RECT work;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = 0;
        work.top = 0;
        work.right = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    if (x < work.left) x = work.left;
    if (x + kMenuW > work.right) x = work.right - kMenuW;
    if (y < work.top) y = work.top;
    if (y + h > work.bottom) y = work.bottom - h;

    g_menuHover = -1;
    g_menu = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kMenuClass, L"",
                             WS_POPUP, x, y, kMenuW, h,
                             owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_menu) return;

    g_menuOpen = true;
    ShowWindow(g_menu, SW_SHOWNOACTIVATE);
    // 主窗口每帧都会重设 ULW，可能把菜单压到下面，这里再顶一次
    SetWindowPos(g_menu, HWND_TOPMOST, x, y, kMenuW, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(g_menu);
    SetCapture(g_menu);             // 用鼠标捕获实现“点外面关闭”
    InvalidateRect(g_menu, nullptr, TRUE);
}

// =============================================================================
//  七、主窗口过程 & 入口
// =============================================================================

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TIMER:
        switch (wp) {
        case TIMER_ANIM:
            g_time += DT;
            UpdatePhysics(DT);
            if (g_hudMsgTimer > 0.0f) {
                g_hudMsgTimer -= DT;
                if (g_hudMsgTimer <= 0.0f) {
                    g_hudMsgTimer = 0.0f;
                    g_fullRedraw = true;
                }
            }
            // 提示条倒计时（v3.8）
            if (g_toastTimer > 0.0f) {
                g_toastTimer -= DT;
                if (g_toastTimer <= 0.0f) {
                    g_toastTimer = 0.0f;
                    g_fullRedraw = true;
                }
            }
            // 版本号连点间隔超时清零（v3.8，静默）
            if (g_versionClickTimer > 0.0f) {
                g_versionClickTimer -= DT;
                if (g_versionClickTimer <= 0.0f) {
                    g_versionClickTimer = 0.0f;
                    g_versionClicks = 0;
                }
            }
            Present();
            // 编辑模式下改动只在内存里，点“保存”才写盘
            g_saveTimer += DT;
            if (!g_editMode && g_saveTimer >= 5.0f) {
                g_saveTimer = 0.0f;
                SaveConfig();
            }
            break;
        case TIMER_FULL:
            UpdateVisibility();
            break;
        case TIMER_DOCK:
            LayoutWindow();
            break;
        }
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;           // 永不抢焦点

    case WM_LBUTTONDOWN:
        OnLButtonDown(hwnd, lp);
        return 0;

    case WM_MOUSEMOVE:
        OnMouseMove(lp);
        return 0;

    case WM_LBUTTONUP:
        OnLButtonUp();
        return 0;

    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        return 0;

    case WM_RBUTTONUP:
        ShowPlantMenu(hwnd, lp);
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        LayoutWindow();
        Present();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ANIM);
        KillTimer(hwnd, TIMER_FULL);
        KillTimer(hwnd, TIMER_DOCK);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    // 关闭 DPI 虚拟化，保证 GetWindowRect / UpdateLayeredWindow 都用物理像素
    SetProcessDPIAware();

    Gdiplus::GdiplusStartupInput gsi;
    if (Gdiplus::GdiplusStartup(&g_gdipToken, &gsi, nullptr) != Gdiplus::Ok)
        return 1;

    srand((unsigned)GetTickCount());

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kWndClass;
    if (!RegisterClassExW(&wc)) return 1;

    WNDCLASSEXW mc;
    ZeroMemory(&mc, sizeof(mc));
    mc.cbSize = sizeof(mc);
    mc.lpfnWndProc = MenuWndProc;
    mc.hInstance = hInstance;
    mc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mc.lpszClassName = kMenuClass;
    if (!RegisterClassExW(&mc)) return 1;

    // 关键样式：
    //   WS_EX_LAYERED     -> UpdateLayeredWindow 逐像素透明（透明处点击穿透）
    //   WS_EX_TOOLWINDOW  -> 不出现在 Alt+Tab / 任务栏按钮
    //   WS_EX_TOPMOST     -> 常驻最上层，才能停在别的窗口标题栏上
    //   WS_EX_NOACTIVATE  -> 点击不抢焦点
    //   WS_POPUP          -> 无边框
    // 初始就按工作区创建，避免启动瞬间出现一个盖住任务栏的全屏窗口
    RECT work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = 0;
        work.top = 0;
        work.right = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kWndClass, L"桌面电子盆栽", WS_POPUP,
        work.left, work.top, work.right - work.left, work.bottom - work.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!g_hwnd) return 1;

    LoadConfig();                   // 读上次的花盆清单（植物 / 数量 / 位置 / 大小 / 水分）
    LayoutWindow();                 // 铺满工作区（任务栏以上）+ 计算地面 + 建离屏缓冲
    if (!g_bits) return 1;

    g_menuFont = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                             L"Microsoft YaHei");

    ShowWindow(g_hwnd, SW_SHOWNA);  // 显示但不抢焦点
    g_visible = true;
    Present();

    SetTimer(g_hwnd, TIMER_ANIM, ANIM_INTERVAL, nullptr);   // ~60 FPS
    SetTimer(g_hwnd, TIMER_FULL, 350, nullptr);             // 全屏检测
    SetTimer(g_hwnd, TIMER_DOCK, 250, nullptr);            // 显示器/任务栏变化

    MSG msg = { 0 };
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_menuFont) DeleteObject(g_menuFont);
    if (!g_editMode) SaveConfig();   // 退出前存一次（编辑模式里没保存的改动不写盘）
    DestroyBackBuffer();
    Gdiplus::GdiplusShutdown(g_gdipToken);
    return (int)msg.wParam;
}
