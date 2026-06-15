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
// 49152..65535 storage page, 65536..81919 network page,
// 81920..98303 backup page, 98304..114687 monitoring page,
// 114688..131071 VMs page, 131072..147455 apps page
// — keep in sync with index.html
#define STRING_POOL_SIZE 147456

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
#define PACT_SNAP_CREATE     19
#define PACT_SNAP_DELETE     20
#define PACT_SNAP_SCHED      21
#define PACT_S3_ADD          22
#define PACT_S3_DELETE       23
#define PACT_S3_BACKUP       24
#define PACT_S3_BROWSE       25
#define PACT_BROWSE_NAV      26
#define PACT_MON_ADD         27
#define PACT_MON_DELETE      28
#define PACT_MON_PAUSE       29
#define PACT_MON_TYPE        30
#define PACT_MON_SELECT      31
#define PACT_CHAN_TOGGLE     32
#define PACT_CHAN_ADD        33
#define PACT_CHAN_DELETE     34
#define PACT_CHAN_TYPE       35
#define PACT_VM_CREATE       36
#define PACT_VM_DELETE       37
#define PACT_VM_START        38
#define PACT_VM_STOP         39
#define PACT_VM_SUSPEND      40
#define PACT_VM_RESUME       41
#define PACT_APP_INSTALL     42
#define PACT_VMNET_TOGGLE    43
#define PACT_VM_SELECT       44
#define PACT_FWD_ADD         45
#define PACT_FWD_DELETE      46
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
// Backup page state, same JS-feeds-C pattern
// ---------------------------------------------------------------
#define MAX_SNAPS 32

typedef struct {
    int id;
    Clay_String name;
} BakVolRow;

typedef struct {
    int id;
    Clay_String volume;
    Clay_String name;
    Clay_String retention;   // "manual", "hourly", ...
    Clay_String created;
} BakSnapRow;

typedef struct {
    int id;
    Clay_String name;
    Clay_String endpoint;
} BakBucketRow;

typedef struct {
    Clay_String label;       // "name  (size)  mtime" formatted in JS
    bool dir;
} BrowseRow;

static BakVolRow bakVols[MAX_ROWS];
static int bakVolCount = 0;
static BakSnapRow bakSnaps[MAX_SNAPS];
static int bakSnapCount = 0;
static BakBucketRow bakBuckets[MAX_ROWS];
static int bakBucketCount = 0;
static bool bakSchedEnabled = true;
static Clay_String bakStatus;          // backup sync status line
static bool bakRunning = false;
// S3 add form: name, endpoint, access key, secret, cryfs password
static Clay_String bakInputs[5];
static Clay_String bakError, bakInfo;
static BrowseRow browseRows[MAX_SNAPS];
static int browseRowCount = 0;
static Clay_String browsePath;
static bool browseOpen = false;
static int bakPendingDeleteSnap = 0;
static int bakPendingDeleteBucket = 0;

CLAY_WASM_EXPORT("ClearBakVolumes") void ClearBakVolumes(void) {
    bakVolCount = 0;
}

CLAY_WASM_EXPORT("AddBakVolume")
void AddBakVolume(int id, uint32_t nameOff, uint32_t nameLen) {
    if (bakVolCount >= MAX_ROWS) return;
    bakVols[bakVolCount++] = (BakVolRow) {
        .id = id,
        .name = poolString(nameOff, nameLen),
    };
}

CLAY_WASM_EXPORT("ClearSnapshots") void ClearSnapshots(void) {
    bakSnapCount = 0;
    bakPendingDeleteSnap = 0;
}

CLAY_WASM_EXPORT("AddSnapshot")
void AddSnapshot(int id,
                 uint32_t volOff, uint32_t volLen,
                 uint32_t nameOff, uint32_t nameLen,
                 uint32_t retOff, uint32_t retLen,
                 uint32_t createdOff, uint32_t createdLen) {
    if (bakSnapCount >= MAX_SNAPS) return;
    bakSnaps[bakSnapCount++] = (BakSnapRow) {
        .id = id,
        .volume = poolString(volOff, volLen),
        .name = poolString(nameOff, nameLen),
        .retention = poolString(retOff, retLen),
        .created = poolString(createdOff, createdLen),
    };
}

CLAY_WASM_EXPORT("ClearBuckets") void ClearBuckets(void) {
    bakBucketCount = 0;
    bakPendingDeleteBucket = 0;
}

CLAY_WASM_EXPORT("AddBucket")
void AddBucket(int id, uint32_t nameOff, uint32_t nameLen,
               uint32_t epOff, uint32_t epLen) {
    if (bakBucketCount >= MAX_ROWS) return;
    bakBuckets[bakBucketCount++] = (BakBucketRow) {
        .id = id,
        .name = poolString(nameOff, nameLen),
        .endpoint = poolString(epOff, epLen),
    };
}

CLAY_WASM_EXPORT("SetBakState")
void SetBakState(bool schedEnabled, bool running,
                 uint32_t statusOff, uint32_t statusLen) {
    bakSchedEnabled = schedEnabled;
    bakRunning = running;
    bakStatus = poolString(statusOff, statusLen);
}

CLAY_WASM_EXPORT("SetBakInputs")
void SetBakInputs(uint32_t o0, uint32_t l0, uint32_t o1, uint32_t l1,
                  uint32_t o2, uint32_t l2, uint32_t o3, uint32_t l3,
                  uint32_t o4, uint32_t l4) {
    bakInputs[0] = poolString(o0, l0);
    bakInputs[1] = poolString(o1, l1);
    bakInputs[2] = poolString(o2, l2);
    bakInputs[3] = poolString(o3, l3);
    bakInputs[4] = poolString(o4, l4);
}

CLAY_WASM_EXPORT("SetBakMsg")
void SetBakMsg(uint32_t errOff, uint32_t errLen,
               uint32_t infoOff, uint32_t infoLen) {
    bakError = poolString(errOff, errLen);
    bakInfo = poolString(infoOff, infoLen);
}

CLAY_WASM_EXPORT("ClearBrowse") void ClearBrowse(void) {
    browseRowCount = 0;
}

CLAY_WASM_EXPORT("AddBrowseRow")
void AddBrowseRow(uint32_t labelOff, uint32_t labelLen, bool dir) {
    if (browseRowCount >= MAX_SNAPS) return;
    browseRows[browseRowCount++] = (BrowseRow) {
        .label = poolString(labelOff, labelLen),
        .dir = dir,
    };
}

CLAY_WASM_EXPORT("SetBrowsePath")
void SetBrowsePath(uint32_t off, uint32_t len, bool open) {
    browsePath = poolString(off, len);
    browseOpen = open;
}

void HandleSnapDelete(Clay_ElementId elementId, Clay_PointerData pointerInfo, void *userData) {
    int sid = (int)(intptr_t)userData;
    if (pointerInfo.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        if (bakPendingDeleteSnap == sid) {
            pendingPageAction = PACT_PACK(PACT_SNAP_DELETE, sid);
            bakPendingDeleteSnap = 0;
        } else {
            bakPendingDeleteSnap = sid;
        }
    }
}

void HandleBucketDelete(Clay_ElementId elementId, Clay_PointerData pointerInfo, void *userData) {
    int bid = (int)(intptr_t)userData;
    if (pointerInfo.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        if (bakPendingDeleteBucket == bid) {
            pendingPageAction = PACT_PACK(PACT_S3_DELETE, bid);
            bakPendingDeleteBucket = 0;
        } else {
            bakPendingDeleteBucket = bid;
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
// Monitoring page state, same JS-feeds-C pattern. A monitor row can
// be expanded (monSelected) to assign notification channels via
// inline checkboxes, like group membership on the Accounts page.
// ---------------------------------------------------------------
typedef struct {
    int id;
    Clay_String name;
    Clay_String type;      // "http", "tcp", ...
    Clay_String target;
    Clay_String status;    // "up", "down", "paused", "pending"
    Clay_String detail;    // "uptime 99% · 12 ms" formatted in JS
    bool enabled;
} MonRow;

typedef struct {
    int id;
    Clay_String name;
    Clay_String type;      // "email" | "webhook"
    Clay_String detail;    // recipient or url
    bool assigned;         // to the currently selected monitor
} ChanRow;

static MonRow monRows[MAX_ROWS];
static int monRowCount = 0;
static ChanRow chanRows[MAX_ROWS];
static int chanRowCount = 0;
static int monUp = 0, monDown = 0, monPaused = 0;
static Clay_String monSummary;
static int monSelected = 0;        // expanded monitor id, 0 = none
// add-monitor form: name, target, interval, type-cycle label
static Clay_String monInName, monInTarget, monInInterval, monAddType;
// add-channel form: name, detail (to/url), type-cycle label
static Clay_String chanInName, chanInDetail, chanAddType;
static Clay_String monError, monInfo;
static int monPendingDeleteMon = 0;
static int monPendingDeleteChan = 0;

CLAY_WASM_EXPORT("ClearMonitors") void ClearMonitors(void) {
    monRowCount = 0;
    monPendingDeleteMon = 0;
}

CLAY_WASM_EXPORT("AddMonitor")
void AddMonitor(int id,
                uint32_t nameOff, uint32_t nameLen,
                uint32_t typeOff, uint32_t typeLen,
                uint32_t targetOff, uint32_t targetLen,
                uint32_t statusOff, uint32_t statusLen,
                uint32_t detailOff, uint32_t detailLen,
                bool enabled) {
    if (monRowCount >= MAX_ROWS) return;
    monRows[monRowCount++] = (MonRow) {
        .id = id,
        .name = poolString(nameOff, nameLen),
        .type = poolString(typeOff, typeLen),
        .target = poolString(targetOff, targetLen),
        .status = poolString(statusOff, statusLen),
        .detail = poolString(detailOff, detailLen),
        .enabled = enabled,
    };
}

CLAY_WASM_EXPORT("ClearChannels") void ClearChannels(void) {
    chanRowCount = 0;
    monPendingDeleteChan = 0;
}

CLAY_WASM_EXPORT("AddChannel")
void AddChannel(int id, uint32_t nameOff, uint32_t nameLen,
                uint32_t typeOff, uint32_t typeLen,
                uint32_t detailOff, uint32_t detailLen,
                bool assigned) {
    if (chanRowCount >= MAX_ROWS) return;
    chanRows[chanRowCount++] = (ChanRow) {
        .id = id,
        .name = poolString(nameOff, nameLen),
        .type = poolString(typeOff, typeLen),
        .detail = poolString(detailOff, detailLen),
        .assigned = assigned,
    };
}

CLAY_WASM_EXPORT("SetMonSummary")
void SetMonSummary(int up, int down, int paused,
                   uint32_t lineOff, uint32_t lineLen, int selected) {
    monUp = up;
    monDown = down;
    monPaused = paused;
    monSummary = poolString(lineOff, lineLen);
    monSelected = selected;
}

CLAY_WASM_EXPORT("SetMonInputs")
void SetMonInputs(uint32_t nOff, uint32_t nLen,
                  uint32_t tOff, uint32_t tLen,
                  uint32_t iOff, uint32_t iLen,
                  uint32_t mtOff, uint32_t mtLen,
                  uint32_t cnOff, uint32_t cnLen,
                  uint32_t cdOff, uint32_t cdLen,
                  uint32_t ctOff, uint32_t ctLen) {
    monInName = poolString(nOff, nLen);
    monInTarget = poolString(tOff, tLen);
    monInInterval = poolString(iOff, iLen);
    monAddType = poolString(mtOff, mtLen);
    chanInName = poolString(cnOff, cnLen);
    chanInDetail = poolString(cdOff, cdLen);
    chanAddType = poolString(ctOff, ctLen);
}

CLAY_WASM_EXPORT("SetMonMsg")
void SetMonMsg(uint32_t errOff, uint32_t errLen,
               uint32_t infoOff, uint32_t infoLen) {
    monError = poolString(errOff, errLen);
    monInfo = poolString(infoOff, infoLen);
}

void HandleMonDelete(Clay_ElementId elementId, Clay_PointerData pointerInfo, void *userData) {
    int mid = (int)(intptr_t)userData;
    if (pointerInfo.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        if (monPendingDeleteMon == mid) {
            pendingPageAction = PACT_PACK(PACT_MON_DELETE, mid);
            monPendingDeleteMon = 0;
        } else {
            monPendingDeleteMon = mid;
        }
    }
}

void HandleChanDelete(Clay_ElementId elementId, Clay_PointerData pointerInfo, void *userData) {
    int cid = (int)(intptr_t)userData;
    if (pointerInfo.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        if (monPendingDeleteChan == cid) {
            pendingPageAction = PACT_PACK(PACT_CHAN_DELETE, cid);
            monPendingDeleteChan = 0;
        } else {
            monPendingDeleteChan = cid;
        }
    }
}

// ---------------------------------------------------------------
// VMs page state, same JS-feeds-C pattern.
// ---------------------------------------------------------------
typedef struct {
    int id;
    Clay_String name;
    Clay_String spec;     // "1 vCPU · 256 MB · 1 GB" formatted in JS
    Clay_String status;   // "running" | "stopped" | "suspended"
    Clay_String ip;       // bridge IP
} VmRow;

typedef struct {
    int id;
    Clay_String label;    // "tcp 8080 -> 80" formatted in JS
} FwdRow;

static VmRow vmRows[MAX_ROWS];
static int vmRowCount = 0;
static FwdRow fwdRows[MAX_ROWS];
static int fwdRowCount = 0;
static int vmSelected = 0;        // expanded VM id (shows port-forwards)
static Clay_String fwdHostInput, fwdGuestInput;
// add-VM form: name, cpus, ram_mb, disk_gb
static Clay_String vmInName, vmInCpus, vmInRam, vmInDisk;
static Clay_String vmError, vmInfo;
static int vmPendingDelete = 0;
static bool vmnetEnabled = false;   // flynas0 NAT network up?

CLAY_WASM_EXPORT("SetVmnet") void SetVmnet(bool e) {
    vmnetEnabled = e;
}

CLAY_WASM_EXPORT("ClearVms") void ClearVms(void) {
    vmRowCount = 0;
    vmPendingDelete = 0;
}

CLAY_WASM_EXPORT("AddVm")
void AddVm(int id, uint32_t nameOff, uint32_t nameLen,
           uint32_t specOff, uint32_t specLen,
           uint32_t statusOff, uint32_t statusLen,
           uint32_t ipOff, uint32_t ipLen) {
    if (vmRowCount >= MAX_ROWS) return;
    vmRows[vmRowCount++] = (VmRow) {
        .id = id,
        .name = poolString(nameOff, nameLen),
        .spec = poolString(specOff, specLen),
        .status = poolString(statusOff, statusLen),
        .ip = poolString(ipOff, ipLen),
    };
}

CLAY_WASM_EXPORT("ClearFwds") void ClearFwds(void) {
    fwdRowCount = 0;
}

CLAY_WASM_EXPORT("AddFwd")
void AddFwd(int id, uint32_t labelOff, uint32_t labelLen) {
    if (fwdRowCount >= MAX_ROWS) return;
    fwdRows[fwdRowCount++] = (FwdRow) {
        .id = id,
        .label = poolString(labelOff, labelLen),
    };
}

CLAY_WASM_EXPORT("SetVmSelected") void SetVmSelected(int id) {
    vmSelected = id;
}

CLAY_WASM_EXPORT("SetFwdInputs")
void SetFwdInputs(uint32_t hOff, uint32_t hLen, uint32_t gOff, uint32_t gLen) {
    fwdHostInput = poolString(hOff, hLen);
    fwdGuestInput = poolString(gOff, gLen);
}

CLAY_WASM_EXPORT("SetVmInputs")
void SetVmInputs(uint32_t nOff, uint32_t nLen,
                 uint32_t cOff, uint32_t cLen,
                 uint32_t rOff, uint32_t rLen,
                 uint32_t dOff, uint32_t dLen) {
    vmInName = poolString(nOff, nLen);
    vmInCpus = poolString(cOff, cLen);
    vmInRam = poolString(rOff, rLen);
    vmInDisk = poolString(dOff, dLen);
}

CLAY_WASM_EXPORT("SetVmMsg")
void SetVmMsg(uint32_t errOff, uint32_t errLen,
              uint32_t infoOff, uint32_t infoLen) {
    vmError = poolString(errOff, errLen);
    vmInfo = poolString(infoOff, infoLen);
}

void HandleVmDelete(Clay_ElementId elementId, Clay_PointerData pointerInfo, void *userData) {
    int vid = (int)(intptr_t)userData;
    if (pointerInfo.state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME) {
        if (vmPendingDelete == vid) {
            pendingPageAction = PACT_PACK(PACT_VM_DELETE, vid);
            vmPendingDelete = 0;
        } else {
            vmPendingDelete = vid;
        }
    }
}

// ---------------------------------------------------------------
// Apps page state (install catalog). One name/IP input pair is shared
// by all cards; clicking a card's Install uses it.
// ---------------------------------------------------------------
typedef struct {
    int id;
    Clay_String name;
    Clay_String desc;
    Clay_String spec;     // "2 vCPU · 2048 MB · 40 GB" formatted in JS
} AppRow;

static AppRow appRows[MAX_ROWS];
static int appRowCount = 0;
static Clay_String appInName, appInIp;
static Clay_String appError, appInfo;

CLAY_WASM_EXPORT("ClearApps") void ClearApps(void) {
    appRowCount = 0;
}

CLAY_WASM_EXPORT("AddApp")
void AddApp(int id, uint32_t nameOff, uint32_t nameLen,
            uint32_t descOff, uint32_t descLen,
            uint32_t specOff, uint32_t specLen) {
    if (appRowCount >= MAX_ROWS) return;
    appRows[appRowCount++] = (AppRow) {
        .id = id,
        .name = poolString(nameOff, nameLen),
        .desc = poolString(descOff, descLen),
        .spec = poolString(specOff, specLen),
    };
}

CLAY_WASM_EXPORT("SetAppInputs")
void SetAppInputs(uint32_t nOff, uint32_t nLen, uint32_t ipOff, uint32_t ipLen) {
    appInName = poolString(nOff, nLen);
    appInIp = poolString(ipOff, ipLen);
}

CLAY_WASM_EXPORT("SetAppMsg")
void SetAppMsg(uint32_t errOff, uint32_t errLen,
               uint32_t infoOff, uint32_t infoLen) {
    appError = poolString(errOff, errLen);
    appInfo = poolString(infoOff, infoLen);
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

// ---------------------------------------------------------------
// Backup page
// ---------------------------------------------------------------
void SnapshotsCard(void) {
    CARD("SnapshotsCard") {
        CLAY(CLAY_ID("SnapHead"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) },
            .childGap = 10,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        } }) {
            CardTitle(CLAY_STRING("Snapshots"));
            CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
            SmallButton(CLAY_ID("SnapSched"),
                bakSchedEnabled ? CLAY_STRING("Auto-snapshot on")
                                : CLAY_STRING("Auto-snapshot off"),
                bakSchedEnabled ? COLOR_GOOD : COLOR_MUTED, HandlePageButton,
                (void *)(intptr_t)PACT_PACK(PACT_SNAP_SCHED, 0));
        }
        for (int i = 0; i < bakVolCount; i++) {
            CLAY(CLAY_IDI("SnapVol", bakVols[i].id), { .layout = {
                .sizing = { .width = CLAY_SIZING_GROW(0) },
                .childGap = 10,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            } }) {
                CLAY_TEXT(bakVols[i].name, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_MONO, .fontSize = 16, .textColor = COLOR_TEXT }));
                CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                SmallButton(CLAY_IDI("SnapNow", bakVols[i].id), CLAY_STRING("Snapshot now"),
                    COLOR_ACCENT, HandlePageButton,
                    (void *)(intptr_t)PACT_PACK(PACT_SNAP_CREATE, bakVols[i].id));
            }
        }
        if (bakVolCount == 0) {
            CLAY_TEXT(CLAY_STRING("No volumes — create one on the Storage page"),
                CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
        } else if (bakSnapCount == 0) {
            CLAY_TEXT(CLAY_STRING("No snapshots yet"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
        }
        for (int i = 0; i < bakSnapCount; i++) {
            BakSnapRow *s = &bakSnaps[i];
            CLAY(CLAY_IDI("SnapRow", s->id), { .layout = {
                .sizing = { .width = CLAY_SIZING_GROW(0) },
                .childGap = 10,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            } }) {
                CLAY_TEXT(s->volume, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_MONO, .fontSize = 15, .textColor = COLOR_MUTED }));
                CLAY_TEXT(s->name, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_MONO, .fontSize = 15, .textColor = COLOR_TEXT }));
                CLAY_TEXT(s->retention, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 13, .textColor = COLOR_ACCENT }));
                CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                CLAY_TEXT(s->created, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 14, .textColor = COLOR_MUTED }));
                SmallButton(CLAY_IDI("SnapDel", s->id),
                    bakPendingDeleteSnap == s->id ? CLAY_STRING("Confirm?") : CLAY_STRING("Delete"),
                    COLOR_BAD, HandleSnapDelete, (void *)(intptr_t)s->id);
            }
        }
    }
}

void S3Card(void) {
    CLAY(CLAY_ID("S3Card"), {
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .sizing = { .width = CLAY_SIZING_FIXED(440) },
            .padding = CLAY_PADDING_ALL(20),
            .childGap = 12,
        },
        .backgroundColor = COLOR_CARD,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .color = COLOR_CARD_EDGE, .width = { 1, 1, 1, 1 } },
    }) {
        CardTitle(CLAY_STRING("Offsite backup (S3)"));
        if (bakBucketCount == 0) {
            CLAY_TEXT(CLAY_STRING("No buckets configured"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
        }
        for (int i = 0; i < bakBucketCount; i++) {
            BakBucketRow *b = &bakBuckets[i];
            CLAY(CLAY_IDI("BucketRow", b->id), { .layout = {
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                .sizing = { .width = CLAY_SIZING_GROW(0) },
                .childGap = 6,
            } }) {
                CLAY(CLAY_IDI("BucketHead", b->id), { .layout = {
                    .sizing = { .width = CLAY_SIZING_GROW(0) },
                    .childGap = 10,
                    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                } }) {
                    CLAY_TEXT(b->name, CLAY_TEXT_CONFIG({
                        .fontId = FONT_ID_MONO, .fontSize = 16, .textColor = COLOR_TEXT }));
                    CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                    CLAY_TEXT(b->endpoint, CLAY_TEXT_CONFIG({
                        .fontId = FONT_ID_BODY, .fontSize = 13, .textColor = COLOR_MUTED }));
                }
                CLAY(CLAY_IDI("BucketBtns", b->id), { .layout = {
                    .sizing = { .width = CLAY_SIZING_GROW(0) },
                    .childGap = 10,
                } }) {
                    SmallButton(CLAY_IDI("BucketSync", b->id),
                        bakRunning ? CLAY_STRING("Running...") : CLAY_STRING("Backup now"),
                        bakRunning ? COLOR_MUTED : COLOR_ACCENT, HandlePageButton,
                        (void *)(intptr_t)PACT_PACK(PACT_S3_BACKUP, b->id));
                    SmallButton(CLAY_IDI("BucketBrowse", b->id), CLAY_STRING("Browse"),
                        COLOR_ACCENT, HandlePageButton,
                        (void *)(intptr_t)PACT_PACK(PACT_S3_BROWSE, b->id));
                    CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                    SmallButton(CLAY_IDI("BucketDel", b->id),
                        bakPendingDeleteBucket == b->id ? CLAY_STRING("Confirm?") : CLAY_STRING("Delete"),
                        COLOR_BAD, HandleBucketDelete, (void *)(intptr_t)b->id);
                }
            }
        }
        if (bakStatus.length > 0) {
            CLAY_TEXT(bakStatus, CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 14,
                .textColor = bakRunning ? COLOR_WARN : COLOR_MUTED }));
        }
        CardTitle(CLAY_STRING("New bucket"));
        LabeledInput(CLAY_STRING("Bucket"), CLAY_ID("BakName"),
            bakInputs[0], CLAY_STRING("my-backups"), 8);
        LabeledInput(CLAY_STRING("Endpoint"), CLAY_ID("BakEndpoint"),
            bakInputs[1], CLAY_STRING("s3.amazonaws.com or /local/dir"), 9);
        LabeledInput(CLAY_STRING("Access key"), CLAY_ID("BakAccess"),
            bakInputs[2], CLAY_STRING("(empty for local dir)"), 10);
        LabeledInput(CLAY_STRING("Secret key"), CLAY_ID("BakSecret"),
            bakInputs[3], CLAY_STRING(""), 11);
        LabeledInput(CLAY_STRING("Password"), CLAY_ID("BakPassword"),
            bakInputs[4], CLAY_STRING("encryption password (min 8)"), 12);
        CLAY(CLAY_ID("BakAddRow"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = 10 } }) {
            CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
            SmallButton(CLAY_ID("BakAddBtn"), CLAY_STRING("Add bucket"), COLOR_ACCENT,
                HandlePageButton, (void *)(intptr_t)PACT_PACK(PACT_S3_ADD, 0));
        }
    }
}

void BrowseCard(void) {
    CARD("BrowseCard") {
        CLAY(CLAY_ID("BrowseHead"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) },
            .childGap = 10,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        } }) {
            CardTitle(CLAY_STRING("Restore browser"));
            CLAY_TEXT(browsePath, CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_MONO, .fontSize = 15, .textColor = COLOR_TEXT }));
        }
        if (browseRowCount == 0) {
            CLAY_TEXT(CLAY_STRING("(empty)"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 15, .textColor = COLOR_MUTED }));
        }
        for (int i = 0; i < browseRowCount; i++) {
            BrowseRow *r = &browseRows[i];
            CLAY(CLAY_IDI("BrowseRow", i), {
                .layout = {
                    .sizing = { .width = CLAY_SIZING_GROW(0) },
                    .childGap = 8,
                },
                .backgroundColor = (r->dir && Clay_Hovered()) ? COLOR_NAV_HOVER : COLOR_CARD,
                .userData = FrameAllocateCustomData((CustomHTMLData) {
                    .cursorPointer = r->dir }),
            }) {
                if (r->dir) {
                    Clay_OnHover(HandlePageButton,
                        (void *)(intptr_t)PACT_PACK(PACT_BROWSE_NAV, i));
                }
                CLAY_TEXT(r->label, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_MONO, .fontSize = 15,
                    .textColor = r->dir ? COLOR_ACCENT : COLOR_TEXT,
                    .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
                }));
            }
        }
    }
}

void BackupPage(void) {
    CLAY(CLAY_ID("BackupRow"), { .layout = {
        .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = 16 } }) {
        SnapshotsCard();
        S3Card();
    }
    if (browseOpen) {
        BrowseCard();
    }
    if (bakError.length > 0) {
        CLAY_TEXT(bakError, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_BAD }));
    }
    if (bakInfo.length > 0) {
        CLAY_TEXT(bakInfo, CLAY_TEXT_CONFIG({
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

// ---------------------------------------------------------------
// Monitoring page
// ---------------------------------------------------------------
Clay_Color StatusColor(Clay_String s) {
    if (s.length == 2 && s.chars[0] == 'u' && s.chars[1] == 'p') return COLOR_GOOD;
    if (s.length == 4 && s.chars[0] == 'd') return COLOR_BAD;     // down
    if (s.length == 6 && s.chars[0] == 'p' && s.chars[1] == 'a') return COLOR_MUTED; // paused
    return COLOR_WARN;   // pending
}

void MonSummaryCard(void) {
    CARD("MonSummaryCard") {
        CLAY(CLAY_ID("MonSummaryRow"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) },
            .childGap = 24,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        } }) {
            CLAY_TEXT(monSummary, CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 20, .textColor = COLOR_TEXT }));
            CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
            CLAY_TEXT(CLAY_STRING("up"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_GOOD }));
            CLAY_TEXT(CLAY_STRING("·"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
            CLAY_TEXT(CLAY_STRING("down"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_BAD }));
            CLAY_TEXT(CLAY_STRING("·"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
            CLAY_TEXT(CLAY_STRING("paused"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
        }
    }
}

void MonitorsCard(void) {
    CARD("MonitorsCard") {
        CardTitle(CLAY_STRING("Monitors"));
        if (monRowCount == 0) {
            CLAY_TEXT(CLAY_STRING("No monitors — add one below"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
        }
        for (int i = 0; i < monRowCount; i++) {
            MonRow *m = &monRows[i];
            bool open = (monSelected == m->id);
            CLAY(CLAY_IDI("MonRowWrap", m->id), { .layout = {
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                .sizing = { .width = CLAY_SIZING_GROW(0) },
                .childGap = 6,
            } }) {
                CLAY(CLAY_IDI("MonRow", m->id), { .layout = {
                    .sizing = { .width = CLAY_SIZING_GROW(0) },
                    .childGap = 10,
                    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                } }) {
                    CLAY_TEXT(CLAY_STRING("●"), CLAY_TEXT_CONFIG({
                        .fontId = FONT_ID_BODY, .fontSize = 16,
                        .textColor = StatusColor(m->status) }));
                    CLAY(CLAY_IDI("MonName", m->id), {
                        .userData = FrameAllocateCustomData((CustomHTMLData) { .cursorPointer = true }),
                    }) {
                        Clay_OnHover(HandlePageButton, (void *)(intptr_t)PACT_PACK(PACT_MON_SELECT, m->id));
                        CLAY_TEXT(m->name, CLAY_TEXT_CONFIG({
                            .fontId = FONT_ID_MONO, .fontSize = 16,
                            .textColor = open ? COLOR_ACCENT : COLOR_TEXT,
                            .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
                        }));
                    }
                    CLAY_TEXT(m->target, CLAY_TEXT_CONFIG({
                        .fontId = FONT_ID_BODY, .fontSize = 13, .textColor = COLOR_MUTED }));
                    CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                    CLAY_TEXT(m->detail, CLAY_TEXT_CONFIG({
                        .fontId = FONT_ID_BODY, .fontSize = 13, .textColor = COLOR_MUTED }));
                    SmallButton(CLAY_IDI("MonPause", m->id),
                        m->enabled ? CLAY_STRING("Pause") : CLAY_STRING("Resume"),
                        m->enabled ? COLOR_MUTED : COLOR_GOOD, HandlePageButton,
                        (void *)(intptr_t)PACT_PACK(PACT_MON_PAUSE, m->id));
                    SmallButton(CLAY_IDI("MonDel", m->id),
                        monPendingDeleteMon == m->id ? CLAY_STRING("Confirm?") : CLAY_STRING("Delete"),
                        COLOR_BAD, HandleMonDelete, (void *)(intptr_t)m->id);
                }
                if (open) {
                    CLAY(CLAY_IDI("MonChans", m->id), { .layout = {
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .sizing = { .width = CLAY_SIZING_GROW(0) },
                        .padding = { 26, 0, 2, 6 },
                        .childGap = 4,
                    } }) {
                        CLAY_TEXT(CLAY_STRING("Notify channels:"), CLAY_TEXT_CONFIG({
                            .fontId = FONT_ID_BODY, .fontSize = 13, .textColor = COLOR_MUTED }));
                        if (chanRowCount == 0) {
                            CLAY_TEXT(CLAY_STRING("(none configured)"), CLAY_TEXT_CONFIG({
                                .fontId = FONT_ID_BODY, .fontSize = 13, .textColor = COLOR_MUTED }));
                        }
                        for (int j = 0; j < chanRowCount; j++) {
                            ChanRow *c = &chanRows[j];
                            CLAY(CLAY_IDI("MonChan", c->id), {
                                .layout = {
                                    .sizing = { .width = CLAY_SIZING_GROW(0) },
                                    .childGap = 8,
                                },
                                .userData = FrameAllocateCustomData((CustomHTMLData) { .cursorPointer = true }),
                            }) {
                                Clay_OnHover(HandlePageButton,
                                    (void *)(intptr_t)PACT_PACK(PACT_CHAN_TOGGLE, c->id));
                                CLAY_TEXT(c->assigned ? CLAY_STRING("[x]") : CLAY_STRING("[ ]"),
                                    CLAY_TEXT_CONFIG({
                                        .fontId = FONT_ID_MONO, .fontSize = 15,
                                        .textColor = c->assigned ? COLOR_ACCENT : COLOR_MUTED,
                                        .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
                                    }));
                                CLAY_TEXT(c->name, CLAY_TEXT_CONFIG({
                                    .fontId = FONT_ID_MONO, .fontSize = 15, .textColor = COLOR_TEXT,
                                    .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
                                }));
                            }
                        }
                    }
                }
            }
        }
        // Add-monitor form
        CLAY(CLAY_ID("MonAddRow"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) },
            .childGap = 10,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        } }) {
            TextInputBox(CLAY_ID("MonName"), monInName, CLAY_STRING("name"), 13);
            SmallButton(CLAY_ID("MonType"), monAddType, COLOR_ACCENT, HandlePageButton,
                (void *)(intptr_t)PACT_PACK(PACT_MON_TYPE, 0));
            TextInputBox(CLAY_ID("MonTarget"), monInTarget,
                CLAY_STRING("https://host/ or host:port"), 14);
            TextInputBox(CLAY_ID("MonInterval"), monInInterval, CLAY_STRING("60s"), 15);
            SmallButton(CLAY_ID("MonAddBtn"), CLAY_STRING("Add"), COLOR_ACCENT,
                HandlePageButton, (void *)(intptr_t)PACT_PACK(PACT_MON_ADD, 0));
        }
    }
}

void ChannelsCard(void) {
    CLAY(CLAY_ID("ChannelsCard"), {
        .layout = {
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .sizing = { .width = CLAY_SIZING_FIXED(400) },
            .padding = CLAY_PADDING_ALL(20),
            .childGap = 12,
        },
        .backgroundColor = COLOR_CARD,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .color = COLOR_CARD_EDGE, .width = { 1, 1, 1, 1 } },
    }) {
        CardTitle(CLAY_STRING("Notification channels"));
        if (chanRowCount == 0) {
            CLAY_TEXT(CLAY_STRING("No channels configured"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
        }
        for (int i = 0; i < chanRowCount; i++) {
            ChanRow *c = &chanRows[i];
            CLAY(CLAY_IDI("ChanRow", c->id), { .layout = {
                .sizing = { .width = CLAY_SIZING_GROW(0) },
                .childGap = 8,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            } }) {
                CLAY_TEXT(c->name, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_MONO, .fontSize = 15, .textColor = COLOR_TEXT }));
                CLAY_TEXT(c->type, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 13, .textColor = COLOR_ACCENT }));
                CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                CLAY_TEXT(c->detail, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 12, .textColor = COLOR_MUTED }));
                SmallButton(CLAY_IDI("ChanDel", c->id),
                    monPendingDeleteChan == c->id ? CLAY_STRING("Confirm?") : CLAY_STRING("Delete"),
                    COLOR_BAD, HandleChanDelete, (void *)(intptr_t)c->id);
            }
        }
        CLAY(CLAY_ID("ChanAddName"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) },
            .childGap = 10,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        } }) {
            TextInputBox(CLAY_ID("ChanName"), chanInName, CLAY_STRING("channel name"), 16);
            SmallButton(CLAY_ID("ChanType"), chanAddType, COLOR_ACCENT, HandlePageButton,
                (void *)(intptr_t)PACT_PACK(PACT_CHAN_TYPE, 0));
        }
        LabeledInput(CLAY_STRING("Target"), CLAY_ID("ChanDetail"),
            chanInDetail, CLAY_STRING("email or webhook URL"), 17);
        CLAY(CLAY_ID("ChanAddRow"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = 10 } }) {
            CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
            SmallButton(CLAY_ID("ChanAddBtn"), CLAY_STRING("Add channel"), COLOR_ACCENT,
                HandlePageButton, (void *)(intptr_t)PACT_PACK(PACT_CHAN_ADD, 0));
        }
    }
}

void MonitoringPage(void) {
    MonSummaryCard();
    CLAY(CLAY_ID("MonitoringRow"), { .layout = {
        .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = 16 } }) {
        MonitorsCard();
        ChannelsCard();
    }
    if (monError.length > 0) {
        CLAY_TEXT(monError, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_BAD }));
    }
    if (monInfo.length > 0) {
        CLAY_TEXT(monInfo, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_GOOD }));
    }
}

// ---------------------------------------------------------------
// VMs page
// ---------------------------------------------------------------
static bool cs_eq(Clay_String s, const char *lit) {
    int n = 0;
    while (lit[n]) n++;
    if (s.length != n) return false;
    for (int i = 0; i < n; i++)
        if (s.chars[i] != lit[i]) return false;
    return true;
}

Clay_Color VmStatusColor(Clay_String s) {
    if (cs_eq(s, "running")) return COLOR_GOOD;
    if (cs_eq(s, "suspended")) return COLOR_WARN;
    return COLOR_MUTED;   // stopped
}

void VmsCard(void) {
    CARD("VmsCard") {
        CLAY(CLAY_ID("VmHead"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) },
            .childGap = 10,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        } }) {
            CardTitle(CLAY_STRING("Virtual machines"));
            CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
            CLAY_TEXT(CLAY_STRING("NAT network"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 14, .textColor = COLOR_MUTED }));
            SmallButton(CLAY_ID("VmnetToggle"),
                vmnetEnabled ? CLAY_STRING("On") : CLAY_STRING("Off"),
                vmnetEnabled ? COLOR_GOOD : COLOR_MUTED, HandlePageButton,
                (void *)(intptr_t)PACT_PACK(PACT_VMNET_TOGGLE, 0));
        }
        if (vmRowCount == 0) {
            CLAY_TEXT(CLAY_STRING("No VMs — create one below"), CLAY_TEXT_CONFIG({
                .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_MUTED }));
        }
        for (int i = 0; i < vmRowCount; i++) {
            VmRow *v = &vmRows[i];
            bool running = cs_eq(v->status, "running");
            bool suspended = cs_eq(v->status, "suspended");
            bool stopped = !running && !suspended;
            bool sel = (vmSelected == v->id);
            CLAY(CLAY_IDI("VmRowWrap", v->id), { .layout = {
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                .sizing = { .width = CLAY_SIZING_GROW(0) },
                .childGap = 6,
            } }) {
            CLAY(CLAY_IDI("VmRow", v->id), { .layout = {
                .sizing = { .width = CLAY_SIZING_GROW(0) },
                .childGap = 10,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            } }) {
                CLAY_TEXT(CLAY_STRING("●"), CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 16,
                    .textColor = VmStatusColor(v->status) }));
                CLAY(CLAY_IDI("VmName", v->id), {
                    .userData = FrameAllocateCustomData((CustomHTMLData) { .cursorPointer = true }),
                }) {
                    Clay_OnHover(HandlePageButton, (void *)(intptr_t)PACT_PACK(PACT_VM_SELECT, v->id));
                    CLAY_TEXT(v->name, CLAY_TEXT_CONFIG({
                        .fontId = FONT_ID_MONO, .fontSize = 16,
                        .textColor = sel ? COLOR_ACCENT : COLOR_TEXT,
                        .userData = FrameAllocateCustomData((CustomHTMLData) { .disablePointerEvents = true }),
                    }));
                }
                CLAY_TEXT(v->ip, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_MONO, .fontSize = 13, .textColor = COLOR_MUTED }));
                CLAY_TEXT(v->spec, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 13, .textColor = COLOR_MUTED }));
                CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                CLAY_TEXT(v->status, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 14,
                    .textColor = VmStatusColor(v->status) }));
                if (running) {
                    SmallButton(CLAY_IDI("VmSuspend", v->id), CLAY_STRING("Suspend"),
                        COLOR_ACCENT, HandlePageButton,
                        (void *)(intptr_t)PACT_PACK(PACT_VM_SUSPEND, v->id));
                    SmallButton(CLAY_IDI("VmStop", v->id), CLAY_STRING("Stop"),
                        COLOR_WARN, HandlePageButton,
                        (void *)(intptr_t)PACT_PACK(PACT_VM_STOP, v->id));
                } else if (suspended) {
                    SmallButton(CLAY_IDI("VmResume", v->id), CLAY_STRING("Resume"),
                        COLOR_GOOD, HandlePageButton,
                        (void *)(intptr_t)PACT_PACK(PACT_VM_RESUME, v->id));
                    SmallButton(CLAY_IDI("VmStop", v->id), CLAY_STRING("Stop"),
                        COLOR_WARN, HandlePageButton,
                        (void *)(intptr_t)PACT_PACK(PACT_VM_STOP, v->id));
                } else {
                    SmallButton(CLAY_IDI("VmStart", v->id), CLAY_STRING("Start"),
                        COLOR_GOOD, HandlePageButton,
                        (void *)(intptr_t)PACT_PACK(PACT_VM_START, v->id));
                    SmallButton(CLAY_IDI("VmDel", v->id),
                        vmPendingDelete == v->id ? CLAY_STRING("Confirm?") : CLAY_STRING("Delete"),
                        COLOR_BAD, HandleVmDelete, (void *)(intptr_t)v->id);
                }
            }
            if (sel) {
                CLAY(CLAY_IDI("VmFwds", v->id), { .layout = {
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .sizing = { .width = CLAY_SIZING_GROW(0) },
                    .padding = { 26, 0, 2, 6 },
                    .childGap = 4,
                } }) {
                    CLAY_TEXT(CLAY_STRING("Port forwards (host -> guest):"),
                        CLAY_TEXT_CONFIG({ .fontId = FONT_ID_BODY, .fontSize = 13,
                            .textColor = COLOR_MUTED }));
                    if (fwdRowCount == 0) {
                        CLAY_TEXT(CLAY_STRING("(none)"), CLAY_TEXT_CONFIG({
                            .fontId = FONT_ID_BODY, .fontSize = 13, .textColor = COLOR_MUTED }));
                    }
                    for (int j = 0; j < fwdRowCount; j++) {
                        CLAY(CLAY_IDI("FwdRow", fwdRows[j].id), { .layout = {
                            .sizing = { .width = CLAY_SIZING_GROW(0) },
                            .childGap = 10,
                            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                        } }) {
                            CLAY_TEXT(fwdRows[j].label, CLAY_TEXT_CONFIG({
                                .fontId = FONT_ID_MONO, .fontSize = 14, .textColor = COLOR_TEXT }));
                            CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                            SmallButton(CLAY_IDI("FwdDel", fwdRows[j].id), CLAY_STRING("Remove"),
                                COLOR_BAD, HandlePageButton,
                                (void *)(intptr_t)PACT_PACK(PACT_FWD_DELETE, fwdRows[j].id));
                        }
                    }
                    CLAY(CLAY_IDI("FwdAdd", v->id), { .layout = {
                        .sizing = { .width = CLAY_SIZING_GROW(0) },
                        .childGap = 8,
                        .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                    } }) {
                        TextInputBox(CLAY_ID("FwdHost"), fwdHostInput, CLAY_STRING("host port"), 24);
                        CLAY_TEXT(CLAY_STRING("->"), CLAY_TEXT_CONFIG({
                            .fontId = FONT_ID_BODY, .fontSize = 14, .textColor = COLOR_MUTED }));
                        TextInputBox(CLAY_ID("FwdGuest"), fwdGuestInput, CLAY_STRING("guest port"), 25);
                        SmallButton(CLAY_ID("FwdAddBtn"), CLAY_STRING("Forward"), COLOR_ACCENT,
                            HandlePageButton, (void *)(intptr_t)PACT_PACK(PACT_FWD_ADD, v->id));
                    }
                }
            }
            }
        }
        // Add-VM form
        CLAY(CLAY_ID("VmAddRow"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) },
            .childGap = 10,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        } }) {
            TextInputBox(CLAY_ID("VmName"), vmInName, CLAY_STRING("name"), 18);
            TextInputBox(CLAY_ID("VmCpus"), vmInCpus, CLAY_STRING("vCPU"), 19);
            TextInputBox(CLAY_ID("VmRam"), vmInRam, CLAY_STRING("RAM MB"), 20);
            TextInputBox(CLAY_ID("VmDisk"), vmInDisk, CLAY_STRING("disk GB"), 21);
            SmallButton(CLAY_ID("VmAddBtn"), CLAY_STRING("Create"), COLOR_ACCENT,
                HandlePageButton, (void *)(intptr_t)PACT_PACK(PACT_VM_CREATE, 0));
        }
    }
}

void VmsPage(void) {
    VmsCard();
    if (vmError.length > 0) {
        CLAY_TEXT(vmError, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_BAD }));
    }
    if (vmInfo.length > 0) {
        CLAY_TEXT(vmInfo, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_GOOD }));
    }
}

// ---------------------------------------------------------------
// Apps page
// ---------------------------------------------------------------
void AppsCard(void) {
    CARD("AppsCard") {
        CardTitle(CLAY_STRING("App catalog"));
        // Shared install target: VM name + optional static IP
        CLAY(CLAY_ID("AppInstallRow"), { .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0) },
            .childGap = 10,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        } }) {
            CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(70) } } }) {
                CLAY_TEXT(CLAY_STRING("Install as"), CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 14, .textColor = COLOR_MUTED }));
            }
            TextInputBox(CLAY_ID("AppName"), appInName, CLAY_STRING("VM name"), 22);
            TextInputBox(CLAY_ID("AppIp"), appInIp, CLAY_STRING("static IP (optional)"), 23);
        }
        for (int i = 0; i < appRowCount; i++) {
            AppRow *a = &appRows[i];
            CLAY(CLAY_IDI("AppRow", a->id), { .layout = {
                .sizing = { .width = CLAY_SIZING_GROW(0) },
                .childGap = 12,
                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
            } }) {
                CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_FIXED(110) } } }) {
                    CLAY_TEXT(a->name, CLAY_TEXT_CONFIG({
                        .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_TEXT }));
                }
                CLAY_TEXT(a->desc, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 14, .textColor = COLOR_MUTED }));
                CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                CLAY_TEXT(a->spec, CLAY_TEXT_CONFIG({
                    .fontId = FONT_ID_BODY, .fontSize = 13, .textColor = COLOR_MUTED }));
                SmallButton(CLAY_IDI("AppInstall", a->id), CLAY_STRING("Install"),
                    COLOR_ACCENT, HandlePageButton,
                    (void *)(intptr_t)PACT_PACK(PACT_APP_INSTALL, a->id));
            }
        }
    }
}

void AppsPage(void) {
    AppsCard();
    if (appError.length > 0) {
        CLAY_TEXT(appError, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_BAD }));
    }
    if (appInfo.length > 0) {
        CLAY_TEXT(appInfo, CLAY_TEXT_CONFIG({
            .fontId = FONT_ID_BODY, .fontSize = 16, .textColor = COLOR_GOOD }));
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
                } else if (activePage == 1) {
                    MonitoringPage();
                } else if (activePage == 2) {
                    NetworkPage();
                } else if (activePage == 3) {
                    AccountsPage();
                } else if (activePage == 4) {
                    StoragePage();
                } else if (activePage == 5) {
                    BackupPage();
                } else if (activePage == 6) {
                    VmsPage();
                } else if (activePage == 7) {
                    AppsPage();
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
