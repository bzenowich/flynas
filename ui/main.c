// FlyNAS web UI — Clay layout compiled to WASM, rendered as HTML.
// All display strings are formatted in JS and written into stringPool;
// C code only stores (offset, length) references and lays out the page.

#define CLAY_IMPLEMENTATION
#include "clay.h"

double windowWidth = 1024, windowHeight = 768;
// Read by the JS glue each frame; FlyNAS only uses the HTML renderer.
uint32_t ACTIVE_RENDERER_INDEX = 0;

const uint32_t FONT_ID_BODY = 0;
const uint32_t FONT_ID_MONO = 1;

// Dark theme, TrueNAS SCALE-inspired
const Clay_Color COLOR_BG        = (Clay_Color) {20, 23, 31, 255};
const Clay_Color COLOR_SIDEBAR   = (Clay_Color) {27, 31, 42, 255};
const Clay_Color COLOR_CARD      = (Clay_Color) {35, 40, 56, 255};
const Clay_Color COLOR_CARD_EDGE = (Clay_Color) {52, 59, 80, 255};
const Clay_Color COLOR_ACCENT    = (Clay_Color) {0, 149, 213, 255};
const Clay_Color COLOR_TEXT      = (Clay_Color) {232, 234, 237, 255};
const Clay_Color COLOR_MUTED     = (Clay_Color) {154, 160, 166, 255};
const Clay_Color COLOR_TRACK     = (Clay_Color) {55, 62, 84, 255};
const Clay_Color COLOR_GOOD      = (Clay_Color) {76, 175, 80, 255};
const Clay_Color COLOR_WARN      = (Clay_Color) {255, 167, 38, 255};
const Clay_Color COLOR_BAD       = (Clay_Color) {244, 67, 54, 255};
const Clay_Color COLOR_NAV_HOVER = (Clay_Color) {40, 46, 62, 255};
const Clay_Color COLOR_NAV_ACTIVE= (Clay_Color) {0, 149, 213, 40};

// ---------------------------------------------------------------
// Frame arena for per-frame userData (cursor hints for the HTML
// renderer), same mechanism as the Clay web example.
// ---------------------------------------------------------------
typedef struct {
    void* memory;
    uintptr_t offset;
} Arena;

Arena frameArena = {};

typedef struct {
    Clay_String link;
    bool cursorPointer;
    bool disablePointerEvents;
} CustomHTMLData;

CustomHTMLData* FrameAllocateCustomData(CustomHTMLData data) {
    CustomHTMLData *customData = (CustomHTMLData *)(frameArena.memory + frameArena.offset);
    *customData = data;
    frameArena.offset += sizeof(CustomHTMLData);
    return customData;
}

// ---------------------------------------------------------------
// Dashboard state, populated from JS via the exported setters below
// ---------------------------------------------------------------
#define MAX_ROWS 16
#define STRING_POOL_SIZE 16384

typedef struct {
    Clay_String name;
    Clay_String label;
    float pct;          // 0..100
} GaugeRow;

typedef struct {
    Clay_String name;
    Clay_String size;
    Clay_String health;
    bool healthy;
} DiskRow;

static char stringPool[STRING_POOL_SIZE];

static Clay_String statusText = CLAY_STRING("Connecting...");
static Clay_String hostname = CLAY_STRING("-");
static Clay_String osRelease = CLAY_STRING("-");
static Clay_String uptimeText = CLAY_STRING("-");
static Clay_String loadText = CLAY_STRING("-");

static float cpuPct = 0;
static Clay_String cpuLabel = CLAY_STRING("-");
static float memPct = 0;
static Clay_String memLabel = CLAY_STRING("-");

static GaugeRow volumes[MAX_ROWS];
static int volumeCount = 0;
static DiskRow disks[MAX_ROWS];
static int diskCount = 0;

static Clay_String poolString(uint32_t offset, uint32_t length) {
    return (Clay_String) {
        .isStaticallyAllocated = true,
        .length = (int32_t)length,
        .chars = stringPool + offset,
    };
}

CLAY_WASM_EXPORT("GetStringPool") char* GetStringPool(void) {
    return stringPool;
}

CLAY_WASM_EXPORT("SetStatus") void SetStatus(uint32_t off, uint32_t len) {
    statusText = poolString(off, len);
}

CLAY_WASM_EXPORT("SetSystem")
void SetSystem(uint32_t hostOff, uint32_t hostLen,
               uint32_t osOff, uint32_t osLen,
               uint32_t upOff, uint32_t upLen,
               uint32_t loadOff, uint32_t loadLen) {
    hostname = poolString(hostOff, hostLen);
    osRelease = poolString(osOff, osLen);
    uptimeText = poolString(upOff, upLen);
    loadText = poolString(loadOff, loadLen);
}

CLAY_WASM_EXPORT("SetCpu") void SetCpu(float pct, uint32_t off, uint32_t len) {
    cpuPct = pct;
    cpuLabel = poolString(off, len);
}

CLAY_WASM_EXPORT("SetMemory") void SetMemory(float pct, uint32_t off, uint32_t len) {
    memPct = pct;
    memLabel = poolString(off, len);
}

CLAY_WASM_EXPORT("ClearVolumes") void ClearVolumes(void) {
    volumeCount = 0;
}

CLAY_WASM_EXPORT("AddVolume")
void AddVolume(uint32_t nameOff, uint32_t nameLen,
               float pct,
               uint32_t labelOff, uint32_t labelLen) {
    if (volumeCount >= MAX_ROWS) return;
    volumes[volumeCount++] = (GaugeRow) {
        .name = poolString(nameOff, nameLen),
        .label = poolString(labelOff, labelLen),
        .pct = pct,
    };
}

CLAY_WASM_EXPORT("ClearDisks") void ClearDisks(void) {
    diskCount = 0;
}

CLAY_WASM_EXPORT("AddDisk")
void AddDisk(uint32_t nameOff, uint32_t nameLen,
             uint32_t sizeOff, uint32_t sizeLen,
             uint32_t healthOff, uint32_t healthLen,
             bool healthy) {
    if (diskCount >= MAX_ROWS) return;
    disks[diskCount++] = (DiskRow) {
        .name = poolString(nameOff, nameLen),
        .size = poolString(sizeOff, sizeLen),
        .health = poolString(healthOff, healthLen),
        .healthy = healthy,
    };
}

// ---------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------
static Clay_String NAV_ITEMS[] = {
    CLAY_STRING("Dashboard"),
    CLAY_STRING("Monitoring"),
    CLAY_STRING("Network"),
    CLAY_STRING("Accounts"),
    CLAY_STRING("Storage"),
    CLAY_STRING("Backup"),
    CLAY_STRING("VMs"),
    CLAY_STRING("Apps"),
    CLAY_STRING("Settings"),
};
#define NAV_COUNT (sizeof(NAV_ITEMS) / sizeof(NAV_ITEMS[0]))

static int activePage = 0;

void HandleNavInteraction(Clay_ElementId elementId, Clay_PointerData pointerInfo, void *userData) {
    if (pointerInfo.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        activePage = (int)(intptr_t)userData;
    }
}

void NavItem(int index) {
    bool active = (activePage == index);
    CLAY(CLAY_IDI("NavItem", index), {
        .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) },
            .padding = { 24, 24, 10, 10 },
        },
        .backgroundColor = active ? COLOR_NAV_ACTIVE
                         : (Clay_Hovered() ? COLOR_NAV_HOVER : COLOR_SIDEBAR),
        .userData = FrameAllocateCustomData((CustomHTMLData) { .cursorPointer = true }),
    }) {
        Clay_OnHover(HandleNavInteraction, (void *)(intptr_t)index);
        CLAY_TEXT(NAV_ITEMS[index], CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 18,
            .textColor = active ? COLOR_ACCENT : COLOR_TEXT,
            .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
        }));
    }
}

void Sidebar(void) {
    CLAY(CLAY_ID("Sidebar"), {
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .sizing = { .width = CLAY_SIZING_FIXED(220), .height = CLAY_SIZING_GROW(0) },
            .padding = { 0, 0, 16, 16 },
            .childGap = 2,
        },
        .backgroundColor = COLOR_SIDEBAR,
    }) {
        CLAY(CLAY_ID("Logo"), { .layout = { .padding = { 24, 24, 8, 24 } } }) {
            CLAY_TEXT(CLAY_STRING("FlyNAS"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 28, .textColor = COLOR_ACCENT }));
        }
        for (int i = 0; i < (int)NAV_COUNT; i++) {
            NavItem(i);
        }
    }
}

// ---------------------------------------------------------------
// Dashboard widgets
// ---------------------------------------------------------------
void CardTitle(Clay_String title) {
    CLAY_TEXT(title, CLAY_TEXT_CONFIG({
        .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
}

void InfoLine(Clay_String label, Clay_String value) {
    CLAY_AUTO_ID({ .layout = {
        .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = 8 } }) {
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 18, .textColor = COLOR_MUTED }));
        CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
        CLAY_TEXT(value, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 18, .textColor = COLOR_TEXT }));
    }
}

Clay_Color GaugeColor(float pct) {
    if (pct >= 90) return COLOR_BAD;
    if (pct >= 75) return COLOR_WARN;
    return COLOR_ACCENT;
}

void GaugeBar(float pct) {
    float clamped = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    CLAY_AUTO_ID({
        .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(10) } },
        .backgroundColor = COLOR_TRACK,
        .cornerRadius = CLAY_CORNER_RADIUS(5),
    }) {
        CLAY_AUTO_ID({
            .layout = { .sizing = {
                .width = CLAY_SIZING_PERCENT(clamped / 100.0f),
                .height = CLAY_SIZING_GROW(0) } },
            .backgroundColor = GaugeColor(clamped),
            .cornerRadius = CLAY_CORNER_RADIUS(5),
        }) {}
    }
}

// Card container: fills available width, fixed inner padding
#define CARD(idLabel) \
    CLAY(CLAY_ID(idLabel), { \
        .layout = { \
            .layoutDirection = CLAY_TOP_TO_BOTTOM, \
            .sizing = { .width = CLAY_SIZING_GROW(0) }, \
            .padding = CLAY_PADDING_ALL(20), \
            .childGap = 12, \
        }, \
        .backgroundColor = COLOR_CARD, \
        .cornerRadius = CLAY_CORNER_RADIUS(8), \
        .border = { .color = COLOR_CARD_EDGE, .width = { 1, 1, 1, 1 } }, \
    })

void SystemCard(void) {
    CARD("SystemCard") {
        CardTitle(CLAY_STRING("System"));
        InfoLine(CLAY_STRING("Hostname"), hostname);
        InfoLine(CLAY_STRING("OS"), osRelease);
        InfoLine(CLAY_STRING("Uptime"), uptimeText);
        InfoLine(CLAY_STRING("Load"), loadText);
    }
}

void CpuCard(void) {
    CARD("CpuCard") {
        CardTitle(CLAY_STRING("CPU"));
        CLAY_TEXT(cpuLabel, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 32, .textColor = COLOR_TEXT }));
        GaugeBar(cpuPct);
    }
}

void MemoryCard(void) {
    CARD("MemoryCard") {
        CardTitle(CLAY_STRING("Memory"));
        CLAY_TEXT(memLabel, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 32, .textColor = COLOR_TEXT }));
        GaugeBar(memPct);
    }
}

void VolumesCard(void) {
    CARD("VolumesCard") {
        CardTitle(CLAY_STRING("Volumes"));
        if (volumeCount == 0) {
            CLAY_TEXT(CLAY_STRING("No HAMMER2 volumes"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 18, .textColor = COLOR_MUTED }));
        }
        for (int i = 0; i < volumeCount; i++) {
            CLAY(CLAY_IDI("VolumeRow", i), { .layout = {
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                .sizing = { .width = CLAY_SIZING_GROW(0) },
                .childGap = 6,
            } }) {
                InfoLine(volumes[i].name, volumes[i].label);
                GaugeBar(volumes[i].pct);
            }
        }
    }
}

void DisksCard(void) {
    CARD("DisksCard") {
        CardTitle(CLAY_STRING("Disks"));
        if (diskCount == 0) {
            CLAY_TEXT(CLAY_STRING("No disks detected"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 18, .textColor = COLOR_MUTED }));
        }
        for (int i = 0; i < diskCount; i++) {
            CLAY(CLAY_IDI("DiskRow", i), { .layout = {
                .sizing = { .width = CLAY_SIZING_GROW(0) },
                .childGap = 8,
            } }) {
                CLAY_TEXT(disks[i].name, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_MONO, .fontSize = 18, .textColor = COLOR_TEXT }));
                CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                CLAY_TEXT(disks[i].size, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 18, .textColor = COLOR_MUTED }));
                CLAY_TEXT(disks[i].health, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 18,
                    .textColor = disks[i].healthy ? COLOR_GOOD : COLOR_WARN }));
            }
        }
    }
}

void DashboardPage(void) {
    CLAY(CLAY_ID("CardsRow1"), { .layout = {
        .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = 16 } }) {
        SystemCard();
        CpuCard();
        MemoryCard();
    }
    CLAY(CLAY_ID("CardsRow2"), { .layout = {
        .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = 16 } }) {
        VolumesCard();
        DisksCard();
    }
}

void PlaceholderPage(void) {
    CLAY(CLAY_ID("Placeholder"), { .layout = {
        .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(200) },
        .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER },
    } }) {
        CLAY_TEXT(CLAY_STRING("Not implemented yet"), CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 24, .textColor = COLOR_MUTED }));
    }
}

Clay_RenderCommandArray CreateLayout(float deltaTime) {
    Clay_BeginLayout();
    CLAY(CLAY_ID("Root"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
        .backgroundColor = COLOR_BG,
    }) {
        Sidebar();
        CLAY(CLAY_ID("Main"), { .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
        } }) {
            CLAY(CLAY_ID("Header"), { .layout = {
                .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(56) },
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                .padding = { 24, 24, 0, 0 },
                .childGap = 16,
            }, .backgroundColor = COLOR_SIDEBAR }) {
                CLAY_TEXT(NAV_ITEMS[activePage], CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 22, .textColor = COLOR_TEXT }));
                CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                CLAY_TEXT(statusText, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
            }
            CLAY(CLAY_ID("Content"), {
                .layout = {
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .padding = CLAY_PADDING_ALL(24),
                    .childGap = 16,
                },
                .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
            }) {
                if (activePage == 0) {
                    DashboardPage();
                } else {
                    PlaceholderPage();
                }
            }
        }
    }
    return Clay_EndLayout(deltaTime);
}

CLAY_WASM_EXPORT("SetScratchMemory") void SetScratchMemory(void * memory) {
    frameArena.memory = memory;
}

CLAY_WASM_EXPORT("UpdateDrawFrame") Clay_RenderCommandArray UpdateDrawFrame(
    float width, float height,
    float mouseWheelX, float mouseWheelY,
    float mousePositionX, float mousePositionY,
    bool isTouchDown, bool isMouseDown,
    bool arrowKeyDownPressedThisFrame, bool arrowKeyUpPressedThisFrame,
    bool dKeyPressedThisFrame, float deltaTime)
{
    frameArena.offset = 0;
    windowWidth = width;
    windowHeight = height;
    Clay_SetLayoutDimensions((Clay_Dimensions) { width, height });
    Clay_SetExternalScrollHandlingEnabled(true);
    Clay_SetCullingEnabled(false);
    Clay_SetPointerState((Clay_Vector2) { mousePositionX, mousePositionY }, isMouseDown || isTouchDown);
    Clay_UpdateScrollContainers(isTouchDown, (Clay_Vector2) { mouseWheelX, mouseWheelY }, deltaTime);
    return CreateLayout(deltaTime);
}

int main() {
    return 0;
}
