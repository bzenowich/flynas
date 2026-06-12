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
// 0..12287 dashboard (reset each poll), 12288..16383 auth strings,
// 16384..32767 QR code data URL — keep in sync with index.html
#define STRING_POOL_SIZE 32768

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
// Auth flow state. Screen transitions and all API calls happen in
// JS; C only renders the current screen and reports button presses
// back via TakeAction().
// ---------------------------------------------------------------
#define SCREEN_LOADING        0
#define SCREEN_SETUP_USERNAME 1
#define SCREEN_SETUP_TOTP     2
#define SCREEN_LOGIN_USERNAME 3
#define SCREEN_LOGIN_CODE     4
#define SCREEN_MAIN           5

#define ACTION_NONE   0
#define ACTION_SUBMIT 1
#define ACTION_BACK   2

static int screen = SCREEN_LOADING;
static Clay_String authInput = { .isStaticallyAllocated = true, .length = 0, .chars = "" };
static Clay_String authError = { .isStaticallyAllocated = true, .length = 0, .chars = "" };
static Clay_String authInfo  = { .isStaticallyAllocated = true, .length = 0, .chars = "" };
// PNG data URL for the TOTP QR code; HTML renderer reads this
// Clay_String through Clay_ImageElementConfig.imageData
static Clay_String qrImage   = { .isStaticallyAllocated = true, .length = 0, .chars = "" };
static int pendingAction = ACTION_NONE;

CLAY_WASM_EXPORT("SetScreen") void SetScreen(int s) {
    screen = s;
}

CLAY_WASM_EXPORT("SetAuthInput") void SetAuthInput(uint32_t off, uint32_t len) {
    authInput = poolString(off, len);
}

CLAY_WASM_EXPORT("SetAuthError") void SetAuthError(uint32_t off, uint32_t len) {
    authError = poolString(off, len);
}

CLAY_WASM_EXPORT("SetAuthInfo") void SetAuthInfo(uint32_t off, uint32_t len) {
    authInfo = poolString(off, len);
}

CLAY_WASM_EXPORT("SetAuthQr") void SetAuthQr(uint32_t off, uint32_t len) {
    qrImage = poolString(off, len);
}

CLAY_WASM_EXPORT("TakeAction") int TakeAction(void) {
    int a = pendingAction;
    pendingAction = ACTION_NONE;
    return a;
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

// ---------------------------------------------------------------
// Auth screens (setup wizard + login)
// ---------------------------------------------------------------
void HandleAuthButton(Clay_ElementId elementId, Clay_PointerData pointerInfo, void *userData) {
    if (pointerInfo.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        pendingAction = (int)(intptr_t)userData;
    }
}

void AuthButton(Clay_String label, int action, bool primary) {
    CLAY(CLAY_IDI("AuthButton", action), {
        .layout = {
            .padding = { 20, 20, 10, 10 },
            .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER },
        },
        .backgroundColor = primary
            ? (Clay_Hovered() ? (Clay_Color) {30, 169, 233, 255} : COLOR_ACCENT)
            : (Clay_Hovered() ? COLOR_NAV_HOVER : COLOR_CARD),
        .cornerRadius = CLAY_CORNER_RADIUS(6),
        .border = primary ? (Clay_BorderElementConfig) {}
                          : (Clay_BorderElementConfig) { .color = COLOR_CARD_EDGE, .width = { 1, 1, 1, 1 } },
        .userData = FrameAllocateCustomData((CustomHTMLData) { .cursorPointer = true }),
    }) {
        Clay_OnHover(HandleAuthButton, (void *)(intptr_t)action);
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 18,
            .textColor = primary ? (Clay_Color) {255, 255, 255, 255} : COLOR_TEXT,
            .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
        }));
    }
}

void AuthInputBox(Clay_String placeholder) {
    CLAY(CLAY_ID("AuthInputBox"), {
        .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(44) },
            .padding = { 12, 12, 0, 0 },
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .childGap = 2,
        },
        .backgroundColor = COLOR_BG,
        .cornerRadius = CLAY_CORNER_RADIUS(6),
        .border = { .color = COLOR_ACCENT, .width = { 1, 1, 1, 1 } },
    }) {
        if (authInput.length == 0) {
            CLAY_TEXT(placeholder, CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 18, .textColor = COLOR_MUTED }));
        } else {
            CLAY_TEXT(authInput, CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_MONO, .fontSize = 18, .textColor = COLOR_TEXT }));
        }
        // Caret
        CLAY(CLAY_ID("AuthCaret"), { .layout = {
            .sizing = { .width = CLAY_SIZING_FIXED(2), .height = CLAY_SIZING_FIXED(22) } },
            .backgroundColor = COLOR_ACCENT,
        }) {}
    }
}

void AuthPage(void) {
    CLAY(CLAY_ID("AuthRoot"), { .layout = {
        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
        .layoutDirection = CLAY_TOP_TO_BOTTOM,
        .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER },
        .childGap = 24,
    } }) {
        CLAY_TEXT(CLAY_STRING("FlyNAS"), CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 36, .textColor = COLOR_ACCENT }));

        // No early return inside a CLAY() block — it would skip the
        // implicit Clay__CloseElement and corrupt the layout tree.
        if (screen == SCREEN_LOADING) {
            CLAY_TEXT(CLAY_STRING("Loading..."), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 20, .textColor = COLOR_MUTED }));
        } else CLAY(CLAY_ID("AuthCard"), {
            .layout = {
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                .sizing = { .width = CLAY_SIZING_FIXED(420) },
                .padding = CLAY_PADDING_ALL(32),
                .childGap = 16,
            },
            .backgroundColor = COLOR_CARD,
            .cornerRadius = CLAY_CORNER_RADIUS(10),
            .border = { .color = COLOR_CARD_EDGE, .width = { 1, 1, 1, 1 } },
        }) {
            Clay_String title, prompt, placeholder, submitLabel;
            bool hasBack = false;
            bool monoInfo = false;

            switch (screen) {
            case SCREEN_SETUP_USERNAME:
                title       = CLAY_STRING("Welcome to FlyNAS");
                prompt      = CLAY_STRING("Create the admin account to get started.");
                placeholder = CLAY_STRING("username");
                submitLabel = CLAY_STRING("Continue");
                break;
            case SCREEN_SETUP_TOTP:
                title       = CLAY_STRING("Set up two-factor auth");
                prompt      = CLAY_STRING("Scan the QR code with your authenticator app (or enter the secret manually), then type the 6-digit code it shows.");
                placeholder = CLAY_STRING("123456");
                submitLabel = CLAY_STRING("Verify");
                hasBack = true;
                monoInfo = true;
                break;
            case SCREEN_LOGIN_CODE:
                title       = CLAY_STRING("Verification code");
                prompt      = CLAY_STRING("Enter your one-time code.");
                placeholder = CLAY_STRING("123456");
                submitLabel = CLAY_STRING("Verify");
                hasBack = true;
                break;
            default: // SCREEN_LOGIN_USERNAME
                title       = CLAY_STRING("Sign in");
                prompt      = CLAY_STRING("Enter your username to continue.");
                placeholder = CLAY_STRING("username");
                submitLabel = CLAY_STRING("Continue");
                break;
            }

            CLAY_TEXT(title, CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 24, .textColor = COLOR_TEXT }));
            CLAY_TEXT(prompt, CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));

            if (screen == SCREEN_SETUP_TOTP && qrImage.length > 0) {
                CLAY(CLAY_ID("AuthQrWrap"), { .layout = {
                    .sizing = { .width = CLAY_SIZING_GROW(0) },
                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER },
                } }) {
                    CLAY(CLAY_ID("AuthQr"), {
                        .layout = { .sizing = {
                            .width = CLAY_SIZING_FIXED(200),
                            .height = CLAY_SIZING_FIXED(200) } },
                        .image = { .imageData = &qrImage },
                    }) {}
                }
            }

            // Context from JS: TOTP secret during setup, email hint during login
            if (authInfo.length > 0) {
                if (monoInfo) {
                    CLAY(CLAY_ID("AuthSecretBox"), {
                        .layout = {
                            .sizing = { .width = CLAY_SIZING_GROW(0) },
                            .padding = CLAY_PADDING_ALL(12),
                            .childAlignment = { .x = CLAY_ALIGN_X_CENTER },
                        },
                        .backgroundColor = COLOR_BG,
                        .cornerRadius = CLAY_CORNER_RADIUS(6),
                        .border = { .color = COLOR_CARD_EDGE, .width = { 1, 1, 1, 1 } },
                    }) {
                        CLAY_TEXT(authInfo, CLAY_TEXT_CONFIG({
                            .fontId = FONT_ID_MONO, .fontSize = 18, .textColor = COLOR_TEXT }));
                    }
                } else {
                    CLAY_TEXT(authInfo, CLAY_TEXT_CONFIG({
                        .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_TEXT }));
                }
            }

            AuthInputBox(placeholder);

            if (authError.length > 0) {
                CLAY_TEXT(authError, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_BAD }));
            }

            CLAY(CLAY_ID("AuthButtons"), { .layout = {
                .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = 12 } }) {
                if (hasBack) {
                    AuthButton(CLAY_STRING("Back"), ACTION_BACK, false);
                }
                CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                AuthButton(submitLabel, ACTION_SUBMIT, true);
            }
        }
    }
}

Clay_RenderCommandArray CreateLayout(float deltaTime) {
    Clay_BeginLayout();
    if (screen != SCREEN_MAIN) {
        CLAY(CLAY_ID("Root"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
            .backgroundColor = COLOR_BG,
        }) {
            AuthPage();
        }
        return Clay_EndLayout(deltaTime);
    }
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
