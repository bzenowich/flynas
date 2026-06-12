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
// 16384..32767 QR code data URL, 32768..49151 accounts page,
// 49152..65535 storage page, 65536..81919 network page — keep in
// sync with index.html
#define STRING_POOL_SIZE 81920

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
// Accounts page state, populated from JS like the dashboard.
// Button presses are queued as packed page actions: low 6 bits =
// action code, remaining bits = row id. JS drains the queue each
// frame via TakePageAction() and performs the API call.
// ---------------------------------------------------------------
#define PACT_NONE            0
#define PACT_USER_SUBMIT     1
#define PACT_USER_DELETE     2
#define PACT_USER_TOGGLE_SSH 3
#define PACT_USER_KEYGEN     4
#define PACT_GROUP_SUBMIT    5
#define PACT_GROUP_DELETE    6
#define PACT_GROUP_SELECT    7
#define PACT_MEMBER_TOGGLE   8
#define PACT_FOCUS           9
#define PACT_VOL_CREATE      10
#define PACT_VOL_DELETE      11
#define PACT_VOL_SCRUB       12
#define PACT_DISK_TOGGLE     13
#define PACT_SCRUB_TOGGLE    14
#define PACT_NET_MODE        15
#define PACT_NET_APPLY       16
#define PACT_TZ_SAVE         17
#define PACT_NTP_SAVE        18
// Low 6 bits = action code, remaining bits = row id
#define PACT_PACK(action, arg) ((action) | ((arg) << 6))

typedef struct {
    int id;
    Clay_String username;
    Clay_String email;
    bool totp, ssh, admin, member;
} UserRow;

typedef struct {
    int id;
    Clay_String name;
    Clay_String count;
} GroupRow;

static UserRow accUsers[MAX_ROWS];
static int accUserCount = 0;
static GroupRow accGroups[MAX_ROWS];
static int accGroupCount = 0;
static Clay_String accUserInput;
static Clay_String accGroupInput;
static Clay_String accError;
static int accFocus = -1;        // 0 = user input, 1 = group input
static int accSelectedGroup = 0; // group id, 0 = none; set by JS once members are loaded
static int accPendingDeleteUser = 0;
static int accPendingDeleteGroup = 0;
static int pendingPageAction = PACT_NONE;

CLAY_WASM_EXPORT("ClearUsers") void ClearUsers(void) {
    accUserCount = 0;
    accPendingDeleteUser = 0;
}

CLAY_WASM_EXPORT("AddUser")
void AddUser(int id,
             uint32_t nameOff, uint32_t nameLen,
             uint32_t emailOff, uint32_t emailLen,
             bool totp, bool ssh, bool admin, bool member) {
    if (accUserCount >= MAX_ROWS) return;
    accUsers[accUserCount++] = (UserRow) {
        .id = id,
        .username = poolString(nameOff, nameLen),
        .email = poolString(emailOff, emailLen),
        .totp = totp, .ssh = ssh, .admin = admin, .member = member,
    };
}

CLAY_WASM_EXPORT("ClearGroups") void ClearGroups(void) {
    accGroupCount = 0;
    accPendingDeleteGroup = 0;
}

CLAY_WASM_EXPORT("AddGroup")
void AddGroup(int id, uint32_t nameOff, uint32_t nameLen,
              uint32_t countOff, uint32_t countLen) {
    if (accGroupCount >= MAX_ROWS) return;
    accGroups[accGroupCount++] = (GroupRow) {
        .id = id,
        .name = poolString(nameOff, nameLen),
        .count = poolString(countOff, countLen),
    };
}

CLAY_WASM_EXPORT("SetAccountsInputs")
void SetAccountsInputs(uint32_t userOff, uint32_t userLen,
                       uint32_t groupOff, uint32_t groupLen) {
    accUserInput = poolString(userOff, userLen);
    accGroupInput = poolString(groupOff, groupLen);
}

CLAY_WASM_EXPORT("SetAccountsError") void SetAccountsError(uint32_t off, uint32_t len) {
    accError = poolString(off, len);
}

CLAY_WASM_EXPORT("SetSelectedGroup") void SetSelectedGroup(int gid) {
    accSelectedGroup = gid;
}

// JS pushes focus changes it makes itself (e.g. Escape to unfocus)
CLAY_WASM_EXPORT("SetAccountsFocus") void SetAccountsFocus(int f) {
    accFocus = f;
}

CLAY_WASM_EXPORT("TakePageAction") int TakePageAction(void) {
    int a = pendingPageAction;
    pendingPageAction = PACT_NONE;
    return a;
}

void HandlePageButton(Clay_ElementId elementId, Clay_PointerData pointerInfo, void *userData) {
    if (pointerInfo.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        pendingPageAction = (int)(intptr_t)userData;
    }
}

// Destructive buttons arm on first press, fire on the second
void HandleUserDelete(Clay_ElementId elementId, Clay_PointerData pointerInfo, void *userData) {
    int uid = (int)(intptr_t)userData;
    if (pointerInfo.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        if (accPendingDeleteUser == uid) {
            pendingPageAction = PACT_PACK(PACT_USER_DELETE, uid);
            accPendingDeleteUser = 0;
        } else {
            accPendingDeleteUser = uid;
        }
    }
}

void HandleGroupDelete(Clay_ElementId elementId, Clay_PointerData pointerInfo, void *userData) {
    int gid = (int)(intptr_t)userData;
    if (pointerInfo.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        if (accPendingDeleteGroup == gid) {
            pendingPageAction = PACT_PACK(PACT_GROUP_DELETE, gid);
            accPendingDeleteGroup = 0;
        } else {
            accPendingDeleteGroup = gid;
        }
    }
}

void HandleFocus(Clay_ElementId elementId, Clay_PointerData pointerInfo, void *userData) {
    if (pointerInfo.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        accFocus = (int)(intptr_t)userData;
        pendingPageAction = PACT_PACK(PACT_FOCUS, accFocus);
    }
}

// ---------------------------------------------------------------
// Storage page state, same JS-feeds-C pattern as accounts
// ---------------------------------------------------------------
typedef struct {
    int id;
    Clay_String name;
    Clay_String usage;     // "1.2 GB / 7.6 GB" or "(not mounted)"
    Clay_String disks;     // "vbd1, vbd2"
    float pct;
    bool scrub, mounted;
} StorVolRow;

typedef struct {
    Clay_String name;
    Clay_String size;
    Clay_String status;    // volume name, "system" or "free"
    bool free, selected, healthy;
} StorDiskRow;

static StorVolRow storVols[MAX_ROWS];
static int storVolCount = 0;
static StorDiskRow storDisks[MAX_ROWS];
static int storDiskCount = 0;
static Clay_String storInput;
static Clay_String storError;
static int storPendingDeleteVol = 0;

CLAY_WASM_EXPORT("ClearStorVolumes") void ClearStorVolumes(void) {
    storVolCount = 0;
    storPendingDeleteVol = 0;
}

CLAY_WASM_EXPORT("AddStorVolume")
void AddStorVolume(int id,
                   uint32_t nameOff, uint32_t nameLen,
                   uint32_t usageOff, uint32_t usageLen,
                   uint32_t disksOff, uint32_t disksLen,
                   float pct, bool scrub, bool mounted) {
    if (storVolCount >= MAX_ROWS) return;
    storVols[storVolCount++] = (StorVolRow) {
        .id = id,
        .name = poolString(nameOff, nameLen),
        .usage = poolString(usageOff, usageLen),
        .disks = poolString(disksOff, disksLen),
        .pct = pct, .scrub = scrub, .mounted = mounted,
    };
}

CLAY_WASM_EXPORT("ClearStorDisks") void ClearStorDisks(void) {
    storDiskCount = 0;
}

CLAY_WASM_EXPORT("AddStorDisk")
void AddStorDisk(uint32_t nameOff, uint32_t nameLen,
                 uint32_t sizeOff, uint32_t sizeLen,
                 uint32_t statusOff, uint32_t statusLen,
                 bool isFree, bool selected, bool healthy) {
    if (storDiskCount >= MAX_ROWS) return;
    storDisks[storDiskCount++] = (StorDiskRow) {
        .name = poolString(nameOff, nameLen),
        .size = poolString(sizeOff, sizeLen),
        .status = poolString(statusOff, statusLen),
        .free = isFree, .selected = selected, .healthy = healthy,
    };
}

CLAY_WASM_EXPORT("SetStorInput") void SetStorInput(uint32_t off, uint32_t len) {
    storInput = poolString(off, len);
}

CLAY_WASM_EXPORT("SetStorError") void SetStorError(uint32_t off, uint32_t len) {
    storError = poolString(off, len);
}

void HandleVolDelete(Clay_ElementId elementId, Clay_PointerData pointerInfo, void *userData) {
    int vid = (int)(intptr_t)userData;
    if (pointerInfo.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        if (storPendingDeleteVol == vid) {
            pendingPageAction = PACT_PACK(PACT_VOL_DELETE, vid);
            storPendingDeleteVol = 0;
        } else {
            storPendingDeleteVol = vid;
        }
    }
}

// ---------------------------------------------------------------
// Network page state, same JS-feeds-C pattern. The apply button is
// armed by JS (first click) and fires on the second click since a
// netif restart can drop the session.
// ---------------------------------------------------------------
static Clay_String netIface, netMac, netLive;
static bool netDhcp = true;
static bool netApplyArmed = false;
static Clay_String netIpInput, netMaskInput, netGwInput;
static Clay_String netTzInput, netNtpInput;
static Clay_String netNtpState;
static Clay_String netError, netInfo;

CLAY_WASM_EXPORT("SetNetConfig")
void SetNetConfig(uint32_t ifaceOff, uint32_t ifaceLen,
                  uint32_t macOff, uint32_t macLen,
                  uint32_t liveOff, uint32_t liveLen,
                  uint32_t ntpStateOff, uint32_t ntpStateLen,
                  bool dhcp, bool armed) {
    netIface = poolString(ifaceOff, ifaceLen);
    netMac = poolString(macOff, macLen);
    netLive = poolString(liveOff, liveLen);
    netNtpState = poolString(ntpStateOff, ntpStateLen);
    netDhcp = dhcp;
    netApplyArmed = armed;
}

CLAY_WASM_EXPORT("SetNetInputs")
void SetNetInputs(uint32_t ipOff, uint32_t ipLen,
                  uint32_t maskOff, uint32_t maskLen,
                  uint32_t gwOff, uint32_t gwLen,
                  uint32_t tzOff, uint32_t tzLen,
                  uint32_t ntpOff, uint32_t ntpLen) {
    netIpInput = poolString(ipOff, ipLen);
    netMaskInput = poolString(maskOff, maskLen);
    netGwInput = poolString(gwOff, gwLen);
    netTzInput = poolString(tzOff, tzLen);
    netNtpInput = poolString(ntpOff, ntpLen);
}

CLAY_WASM_EXPORT("SetNetMsg")
void SetNetMsg(uint32_t errOff, uint32_t errLen,
               uint32_t infoOff, uint32_t infoLen) {
    netError = poolString(errOff, errLen);
    netInfo = poolString(infoOff, infoLen);
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

// Polled by JS each frame to start/stop per-page data loading
CLAY_WASM_EXPORT("GetActivePage") int GetActivePage(void) {
    return activePage;
}

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

// ---------------------------------------------------------------
// Accounts page widgets
// ---------------------------------------------------------------
void SmallButton(Clay_ElementId id, Clay_String label, Clay_Color color,
                 void (*handler)(Clay_ElementId, Clay_PointerData, void *),
                 void *userData) {
    CLAY(id, {
        .layout = {
            .padding = { 10, 10, 4, 4 },
            .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER },
        },
        .backgroundColor = Clay_Hovered() ? COLOR_NAV_HOVER : COLOR_CARD,
        .cornerRadius = CLAY_CORNER_RADIUS(4),
        .border = { .color = color, .width = { 1, 1, 1, 1 } },
        .userData = FrameAllocateCustomData((CustomHTMLData) { .cursorPointer = true }),
    }) {
        Clay_OnHover(handler, userData);
        CLAY_TEXT(label, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 14, .textColor = color,
            .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
        }));
    }
}

void TextInputBox(Clay_ElementId id, Clay_String value, Clay_String placeholder, int focusIndex) {
    bool focused = (accFocus == focusIndex);
    CLAY(id, {
        .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(36) },
            .padding = { 10, 10, 0, 0 },
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            .childGap = 2,
        },
        .backgroundColor = COLOR_BG,
        .cornerRadius = CLAY_CORNER_RADIUS(6),
        .border = { .color = focused ? COLOR_ACCENT : COLOR_CARD_EDGE, .width = { 1, 1, 1, 1 } },
        .userData = FrameAllocateCustomData((CustomHTMLData) { .cursorPointer = true }),
    }) {
        Clay_OnHover(HandleFocus, (void *)(intptr_t)focusIndex);
        if (value.length == 0) {
            CLAY_TEXT(placeholder, CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED,
                .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
            }));
        } else {
            CLAY_TEXT(value, CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_MONO, .fontSize = 16, .textColor = COLOR_TEXT,
                .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
            }));
        }
        if (focused) {
            CLAY(CLAY_IDI("InputCaret", focusIndex), { .layout = {
                .sizing = { .width = CLAY_SIZING_FIXED(2), .height = CLAY_SIZING_FIXED(18) } },
                .backgroundColor = COLOR_ACCENT,
            }) {}
        }
    }
}

void UsersCard(void) {
    CARD("UsersCard") {
        CardTitle(CLAY_STRING("Users"));
        if (accUserCount == 0) {
            CLAY_TEXT(CLAY_STRING("No users"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
        }
        for (int i = 0; i < accUserCount; i++) {
            UserRow *u = &accUsers[i];
            CLAY(CLAY_IDI("UserRow", u->id), { .layout = {
                .sizing = { .width = CLAY_SIZING_GROW(0) },
                .childGap = 10,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            } }) {
                CLAY_TEXT(u->username, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_MONO, .fontSize = 16, .textColor = COLOR_TEXT }));
                if (u->admin) {
                    CLAY_TEXT(CLAY_STRING("admin"), CLAY_TEXT_CONFIG({
                        .fontId = FONT_ID_BODY, .fontSize = 13, .textColor = COLOR_ACCENT }));
                }
                if (u->totp) {
                    CLAY_TEXT(CLAY_STRING("2FA"), CLAY_TEXT_CONFIG({
                        .fontId = FONT_ID_BODY, .fontSize = 13, .textColor = COLOR_GOOD }));
                }
                CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                CLAY_TEXT(u->email, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 14, .textColor = COLOR_MUTED }));
                SmallButton(CLAY_IDI("UserSsh", u->id),
                    u->ssh ? CLAY_STRING("SSH on") : CLAY_STRING("SSH off"),
                    u->ssh ? COLOR_GOOD : COLOR_MUTED,
                    HandlePageButton, (void *)(intptr_t)PACT_PACK(PACT_USER_TOGGLE_SSH, u->id));
                SmallButton(CLAY_IDI("UserKey", u->id), CLAY_STRING("Keygen"), COLOR_ACCENT,
                    HandlePageButton, (void *)(intptr_t)PACT_PACK(PACT_USER_KEYGEN, u->id));
                SmallButton(CLAY_IDI("UserDel", u->id),
                    accPendingDeleteUser == u->id ? CLAY_STRING("Confirm?") : CLAY_STRING("Delete"),
                    COLOR_BAD, HandleUserDelete, (void *)(intptr_t)u->id);
            }
        }
        CLAY(CLAY_ID("UserAddRow"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) },
            .childGap = 10,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        } }) {
            TextInputBox(CLAY_ID("UserInput"), accUserInput, CLAY_STRING("new username"), 0);
            SmallButton(CLAY_ID("UserAddBtn"), CLAY_STRING("Add User"), COLOR_ACCENT,
                HandlePageButton, (void *)(intptr_t)PACT_PACK(PACT_USER_SUBMIT, 0));
        }
    }
}

void GroupsCard(void) {
    CLAY(CLAY_ID("GroupsCard"), {
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .sizing = { .width = CLAY_SIZING_FIXED(360) },
            .padding = CLAY_PADDING_ALL(20),
            .childGap = 12,
        },
        .backgroundColor = COLOR_CARD,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .color = COLOR_CARD_EDGE, .width = { 1, 1, 1, 1 } },
    }) {
        CardTitle(CLAY_STRING("Groups"));
        if (accGroupCount == 0) {
            CLAY_TEXT(CLAY_STRING("No groups"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
        }
        for (int i = 0; i < accGroupCount; i++) {
            GroupRow *g = &accGroups[i];
            bool open = (accSelectedGroup == g->id);
            CLAY(CLAY_IDI("GroupRow", g->id), { .layout = {
                .sizing = { .width = CLAY_SIZING_GROW(0) },
                .childGap = 10,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            } }) {
                CLAY(CLAY_IDI("GroupName", g->id), {
                    .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } },
                    .userData = FrameAllocateCustomData((CustomHTMLData) { .cursorPointer = true }),
                }) {
                    Clay_OnHover(HandlePageButton, (void *)(intptr_t)PACT_PACK(PACT_GROUP_SELECT, g->id));
                    CLAY_TEXT(g->name, CLAY_TEXT_CONFIG({
                        .fontId = FONT_ID_BODY, .fontSize = 16,
                        .textColor = open ? COLOR_ACCENT : COLOR_TEXT,
                        .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
                    }));
                }
                CLAY_TEXT(g->count, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 14, .textColor = COLOR_MUTED }));
                SmallButton(CLAY_IDI("GroupDel", g->id),
                    accPendingDeleteGroup == g->id ? CLAY_STRING("Confirm?") : CLAY_STRING("Delete"),
                    COLOR_BAD, HandleGroupDelete, (void *)(intptr_t)g->id);
            }
            if (open) {
                CLAY(CLAY_IDI("GroupMembers", g->id), { .layout = {
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .sizing = { .width = CLAY_SIZING_GROW(0) },
                    .padding = { 16, 0, 2, 6 },
                    .childGap = 4,
                } }) {
                    for (int j = 0; j < accUserCount; j++) {
                        UserRow *u = &accUsers[j];
                        CLAY(CLAY_IDI("Member", u->id), {
                            .layout = {
                                .sizing = { .width = CLAY_SIZING_GROW(0) },
                                .childGap = 8,
                            },
                            .userData = FrameAllocateCustomData((CustomHTMLData) { .cursorPointer = true }),
                        }) {
                            Clay_OnHover(HandlePageButton, (void *)(intptr_t)PACT_PACK(PACT_MEMBER_TOGGLE, u->id));
                            CLAY_TEXT(u->member ? CLAY_STRING("[x]") : CLAY_STRING("[ ]"),
                                CLAY_TEXT_CONFIG({
                                    .fontId = FONT_ID_MONO, .fontSize = 15,
                                    .textColor = u->member ? COLOR_ACCENT : COLOR_MUTED,
                                    .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
                                }));
                            CLAY_TEXT(u->username, CLAY_TEXT_CONFIG({
                                .fontId = FONT_ID_MONO, .fontSize = 15, .textColor = COLOR_TEXT,
                                .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
                            }));
                        }
                    }
                }
            }
        }
        CLAY(CLAY_ID("GroupAddRow"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) },
            .childGap = 10,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        } }) {
            TextInputBox(CLAY_ID("GroupInput"), accGroupInput, CLAY_STRING("new group"), 1);
            SmallButton(CLAY_ID("GroupAddBtn"), CLAY_STRING("Add Group"), COLOR_ACCENT,
                HandlePageButton, (void *)(intptr_t)PACT_PACK(PACT_GROUP_SUBMIT, 0));
        }
    }
}

// ---------------------------------------------------------------
// Storage page
// ---------------------------------------------------------------
void StorVolumesCard(void) {
    CARD("StorVolumesCard") {
        CardTitle(CLAY_STRING("Volumes"));
        if (storVolCount == 0) {
            CLAY_TEXT(CLAY_STRING("No volumes — select free disks and create one"),
                CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
        }
        for (int i = 0; i < storVolCount; i++) {
            StorVolRow *v = &storVols[i];
            CLAY(CLAY_IDI("StorVol", v->id), { .layout = {
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                .sizing = { .width = CLAY_SIZING_GROW(0) },
                .childGap = 8,
            } }) {
                CLAY(CLAY_IDI("StorVolHead", v->id), { .layout = {
                    .sizing = { .width = CLAY_SIZING_GROW(0) },
                    .childGap = 10,
                    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                } }) {
                    CLAY_TEXT(v->name, CLAY_TEXT_CONFIG({
                        .fontId = FONT_ID_MONO, .fontSize = 18, .textColor = COLOR_TEXT }));
                    CLAY_TEXT(v->disks, CLAY_TEXT_CONFIG({
                        .fontId = FONT_ID_BODY, .fontSize = 14, .textColor = COLOR_MUTED }));
                    CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                    CLAY_TEXT(v->usage, CLAY_TEXT_CONFIG({
                        .fontId = FONT_ID_BODY, .fontSize = 15,
                        .textColor = v->mounted ? COLOR_TEXT : COLOR_WARN }));
                }
                if (v->mounted) {
                    GaugeBar(v->pct);
                }
                CLAY(CLAY_IDI("StorVolBtns", v->id), { .layout = {
                    .sizing = { .width = CLAY_SIZING_GROW(0) },
                    .childGap = 10,
                } }) {
                    SmallButton(CLAY_IDI("VolScrub", v->id), CLAY_STRING("Scrub now"),
                        COLOR_ACCENT, HandlePageButton,
                        (void *)(intptr_t)PACT_PACK(PACT_VOL_SCRUB, v->id));
                    SmallButton(CLAY_IDI("VolAuto", v->id),
                        v->scrub ? CLAY_STRING("Auto-scrub on") : CLAY_STRING("Auto-scrub off"),
                        v->scrub ? COLOR_GOOD : COLOR_MUTED, HandlePageButton,
                        (void *)(intptr_t)PACT_PACK(PACT_SCRUB_TOGGLE, v->id));
                    CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                    SmallButton(CLAY_IDI("VolDel", v->id),
                        storPendingDeleteVol == v->id ? CLAY_STRING("Confirm?") : CLAY_STRING("Delete"),
                        COLOR_BAD, HandleVolDelete, (void *)(intptr_t)v->id);
                }
            }
        }
    }
}

void StorDisksCard(void) {
    CLAY(CLAY_ID("StorDisksCard"), {
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .sizing = { .width = CLAY_SIZING_FIXED(380) },
            .padding = CLAY_PADDING_ALL(20),
            .childGap = 12,
        },
        .backgroundColor = COLOR_CARD,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .color = COLOR_CARD_EDGE, .width = { 1, 1, 1, 1 } },
    }) {
        CardTitle(CLAY_STRING("Disks"));
        for (int i = 0; i < storDiskCount; i++) {
            StorDiskRow *d = &storDisks[i];
            CLAY(CLAY_IDI("StorDisk", i), {
                .layout = {
                    .sizing = { .width = CLAY_SIZING_GROW(0) },
                    .childGap = 8,
                    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                },
                .userData = FrameAllocateCustomData((CustomHTMLData) {
                    .cursorPointer = d->free }),
            }) {
                if (d->free) {
                    Clay_OnHover(HandlePageButton,
                        (void *)(intptr_t)PACT_PACK(PACT_DISK_TOGGLE, i));
                }
                CLAY_TEXT(d->free ? (d->selected ? CLAY_STRING("[x]") : CLAY_STRING("[ ]"))
                                  : CLAY_STRING("   "),
                    CLAY_TEXT_CONFIG({
                        .fontId = FONT_ID_MONO, .fontSize = 15,
                        .textColor = d->selected ? COLOR_ACCENT : COLOR_MUTED,
                        .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
                    }));
                CLAY_TEXT(d->name, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_MONO, .fontSize = 15, .textColor = COLOR_TEXT,
                    .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
                }));
                CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                CLAY_TEXT(d->size, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 14, .textColor = COLOR_MUTED,
                    .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
                }));
                CLAY_TEXT(d->status, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 14,
                    .textColor = d->free ? COLOR_GOOD : (d->healthy ? COLOR_MUTED : COLOR_WARN),
                    .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
                }));
            }
        }
        CLAY(CLAY_ID("StorAddRow"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) },
            .childGap = 10,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        } }) {
            TextInputBox(CLAY_ID("StorInput"), storInput, CLAY_STRING("volume name"), 2);
            SmallButton(CLAY_ID("StorCreateBtn"), CLAY_STRING("Create"), COLOR_ACCENT,
                HandlePageButton, (void *)(intptr_t)PACT_PACK(PACT_VOL_CREATE, 0));
        }
    }
}

void StoragePage(void) {
    CLAY(CLAY_ID("StorageRow"), { .layout = {
        .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = 16 } }) {
        StorVolumesCard();
        StorDisksCard();
    }
    if (storError.length > 0) {
        CLAY_TEXT(storError, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_BAD }));
    }
}

// ---------------------------------------------------------------
// Network page
// ---------------------------------------------------------------
void LabeledInput(Clay_String label, Clay_ElementId id, Clay_String value,
                  Clay_String placeholder, int focusIndex) {
    CLAY_AUTO_ID({ .layout = {
        .sizing = { .width = CLAY_SIZING_GROW(0) },
        .childGap = 10,
        .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
    } }) {
        CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(110) } } }) {
            CLAY_TEXT(label, CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
        }
        TextInputBox(id, value, placeholder, focusIndex);
    }
}

void NetConfigCard(void) {
    CARD("NetConfigCard") {
        CardTitle(CLAY_STRING("IP Configuration"));
        InfoLine(CLAY_STRING("Interface"), netIface);
        InfoLine(CLAY_STRING("MAC"), netMac);
        InfoLine(CLAY_STRING("Current"), netLive);
        CLAY(CLAY_ID("NetModeRow"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) },
            .childGap = 10,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        } }) {
            CLAY_TEXT(CLAY_STRING("Mode"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
            CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
            SmallButton(CLAY_ID("NetMode"),
                netDhcp ? CLAY_STRING("DHCP") : CLAY_STRING("Static"),
                COLOR_ACCENT, HandlePageButton,
                (void *)(intptr_t)PACT_PACK(PACT_NET_MODE, 0));
        }
        if (!netDhcp) {
            LabeledInput(CLAY_STRING("IP address"), CLAY_ID("NetIp"),
                netIpInput, CLAY_STRING("192.168.1.10"), 3);
            LabeledInput(CLAY_STRING("Netmask"), CLAY_ID("NetMask"),
                netMaskInput, CLAY_STRING("255.255.255.0"), 4);
            LabeledInput(CLAY_STRING("Gateway"), CLAY_ID("NetGw"),
                netGwInput, CLAY_STRING("192.168.1.1"), 5);
        }
        CLAY(CLAY_ID("NetApplyRow"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) },
            .childGap = 10,
        } }) {
            CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
            SmallButton(CLAY_ID("NetApply"),
                netApplyArmed ? CLAY_STRING("Restart network?") : CLAY_STRING("Apply"),
                netApplyArmed ? COLOR_WARN : COLOR_ACCENT, HandlePageButton,
                (void *)(intptr_t)PACT_PACK(PACT_NET_APPLY, 0));
        }
    }
}

void NetTimeCard(void) {
    CLAY(CLAY_ID("NetTimeCard"), {
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .sizing = { .width = CLAY_SIZING_FIXED(420) },
            .padding = CLAY_PADDING_ALL(20),
            .childGap = 12,
        },
        .backgroundColor = COLOR_CARD,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .color = COLOR_CARD_EDGE, .width = { 1, 1, 1, 1 } },
    }) {
        CardTitle(CLAY_STRING("Time"));
        LabeledInput(CLAY_STRING("Timezone"), CLAY_ID("NetTz"),
            netTzInput, CLAY_STRING("America/New_York"), 6);
        CLAY(CLAY_ID("NetTzRow"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = 10 } }) {
            CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
            SmallButton(CLAY_ID("NetTzSave"), CLAY_STRING("Save timezone"),
                COLOR_ACCENT, HandlePageButton,
                (void *)(intptr_t)PACT_PACK(PACT_TZ_SAVE, 0));
        }
        LabeledInput(CLAY_STRING("NTP server"), CLAY_ID("NetNtp"),
            netNtpInput, CLAY_STRING("pool.ntp.org"), 7);
        CLAY(CLAY_ID("NetNtpRow"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) },
            .childGap = 10,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        } }) {
            CLAY_TEXT(netNtpState, CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 14, .textColor = COLOR_MUTED }));
            CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
            SmallButton(CLAY_ID("NetNtpSave"), CLAY_STRING("Save NTP"),
                COLOR_ACCENT, HandlePageButton,
                (void *)(intptr_t)PACT_PACK(PACT_NTP_SAVE, 0));
        }
    }
}

void NetworkPage(void) {
    CLAY(CLAY_ID("NetworkRow"), { .layout = {
        .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = 16 } }) {
        NetConfigCard();
        NetTimeCard();
    }
    if (netError.length > 0) {
        CLAY_TEXT(netError, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_BAD }));
    }
    if (netInfo.length > 0) {
        CLAY_TEXT(netInfo, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_GOOD }));
    }
}

void AccountsPage(void) {
    CLAY(CLAY_ID("AccountsRow"), { .layout = {
        .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = 16 } }) {
        UsersCard();
        GroupsCard();
    }
    if (accError.length > 0) {
        CLAY_TEXT(accError, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_BAD }));
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
                } else if (activePage == 2) {
                    NetworkPage();
                } else if (activePage == 3) {
                    AccountsPage();
                } else if (activePage == 4) {
                    StoragePage();
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
