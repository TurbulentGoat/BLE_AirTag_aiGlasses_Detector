#include "ble_tracker_detector.h"
#include "core/display.h"
#include "core/mykeyboard.h"
#include "core/utils.h"
#include "modules/ble/ble_common.h"
#include <algorithm>
#if !defined(LITE_VERSION)
#include "modules/ble/BLE_Suite.h"
#endif

// --- Bluetooth SIG company identifiers -------------------------------------
#define APPLE_MFG_ID 0x004C
#define TILE_MFG_ID 0x067C // Tile, Inc.  (0x0181 = Gecko Health Innovations)
#define SAMSUNG_MFG_ID 0x0075
#define META_MFG_ID 0x01AB      // Meta Platforms, Inc.
#define META_TECH_MFG_ID 0x058E // Meta Platforms Technologies, LLC (ex-Oculus)
#define LUXOTTICA_MFG_ID 0x0D53 // Luxottica Group S.p.A (Ray-Ban, Oakley)
#define SNAP_MFG_ID 0x03C2      // Snapchat Inc
#define VUZIX_MFG_ID 0x060C
#define EVEN_MFG_ID 0x10F9 // Even Realities Ltd.
#define SONY_MFG_ID 0x012D
#define EPSON_MFG_ID 0x0040
// NOTE: 0x00A3 is "Meta Watch Ltd." -- an unrelated legacy smartwatch company.
// Do not treat it as Meta/Facebook.

// --- Apple manufacturer-data payload types ---------------------------------
#define APPLE_TYPE_FINDMY 0x12          // Find My / offline finding
#define APPLE_TYPE_PAIRING 0x07         // Proximity pairing (AirPods, unregistered AirTag)
#define APPLE_FINDMY_LEN_SEPARATED 0x19 // 25-byte payload = tag away from owner

// --- 16-bit service UUIDs ---------------------------------------------------
#define UUID_TILE 0xFEED
#define UUID_SMARTTAG_REG 0xFD5A   // Samsung SmartTag, registered / offline finding
#define UUID_SMARTTAG_UNREG 0xFD59 // Samsung SmartTag, unregistered
#define UUID_META_FD5F 0xFD5F      // Ray-Ban Meta advertisements
#define UUID_META_FEB7 0xFEB7
#define UUID_META_FEB8 0xFEB8

#define RSSI_STRONG -50
#define RSSI_MEDIUM -70
#define RSSI_WEAK -85
#define SCAN_TIME_MS 8000 // made 8 seconds to match ble suite scan time

// NimBLE's default for NimBLEScan::m_maxResults (see the NimBLEScan
// constructor). Restored on the way out -- see ScanSettingsGuard below.
#define NIMBLE_DEFAULT_MAX_RESULTS 0xFF

static std::map<String, TrackedDevice> trackedDevices;

// ---------------------------------------------------------------------------
// Advertisement parsing helpers
// ---------------------------------------------------------------------------

static bool hasServiceUUID16(const NimBLEAdvertisedDevice *d, uint16_t uuid16) {
    NimBLEUUID want(uuid16);

    for (uint8_t i = 0; i < d->getServiceUUIDCount(); i++) {
        if (d->getServiceUUID(i) == want) return true;
    }
    for (uint8_t i = 0; i < d->getServiceDataCount(); i++) {
        if (d->getServiceDataUUID(i) == want) return true;
    }
    return false;
}

static bool getMfgData(const NimBLEAdvertisedDevice *d, uint16_t &id, std::string &data) {
    if (d->getManufacturerDataCount() == 0) return false;

    data = d->getManufacturerData(0);
    if (data.size() < 2) return false;

    id = ((uint16_t)(uint8_t)data[1] << 8) | (uint8_t)data[0];
    return true;
}

static bool nameContains(const String &lowerName, const char *needle) {
    if (!needle) return true;
    if (lowerName.isEmpty()) return false;
    return lowerName.indexOf(needle) != -1;
}

// ---------------------------------------------------------------------------
// Tracker matching
// ---------------------------------------------------------------------------

static bool matchTracker(
    const NimBLEAdvertisedDevice *device, const String &lowerName, TrackerType &type, uint8_t &confidence,
    const char *&label
) {
    // 1. Service UUIDs -- the most specific signal available.
    if (hasServiceUUID16(device, UUID_TILE)) {
        type = TRACKER_TILE;
        confidence = 95;
        label = "Tile tracker";
        return true;
    }
    if (hasServiceUUID16(device, UUID_SMARTTAG_REG) || hasServiceUUID16(device, UUID_SMARTTAG_UNREG)) {
        type = TRACKER_SMARTTAG;
        confidence = 95;
        label = "Samsung SmartTag";
        return true;
    }

    // 2. Manufacturer data.
    uint16_t mfgId = 0;
    std::string mfg;
    if (getMfgData(device, mfgId, mfg)) {
        if (mfgId == APPLE_MFG_ID && mfg.size() >= 4) {
            uint8_t appleType = (uint8_t)mfg[2];
            uint8_t appleLen = (uint8_t)mfg[3];

            if (appleType == APPLE_TYPE_FINDMY) {
                // 0x12 + 0x19 -> separated tag broadcasting its rotating key.
                // 0x12 + 0x02 -> owner's device nearby, or an iPhone/Mac advertising its own Find My
                // presence.
                if (appleLen == APPLE_FINDMY_LEN_SEPARATED) {
                    type = TRACKER_AIRTAG;
                    confidence = 95;
                    label = "AirTag (separated)";
                } else {
                    type = TRACKER_OTHER_FIND_MY;
                    confidence = 70;
                    label = "Find My device";
                }
                return true;
            }
            if (appleType == APPLE_TYPE_PAIRING) {
                if (nameContains(lowerName, "airpod")) {
                    type = TRACKER_AIRPODS;
                    confidence = 90;
                    label = "AirPods";
                } else {
                    type = TRACKER_OTHER_FIND_MY;
                    confidence = 60;
                    label = "Apple accessory (pairing)";
                }
                return true;
            }
            // Any other Apple type is Continuity chatter from a
            // phone, watch or Mac. Not a tracker.
        }

        if (mfgId == TILE_MFG_ID) {
            type = TRACKER_TILE;
            confidence = 90;
            label = "Tile tracker";
            return true;
        }

        // Deliberately NOT matching SAMSUNG_MFG_ID on its own: every Samsung
        // phone, TV and earbud in range broadcasts it. SmartTags are caught by
        // their service UUID above.
        (void)SAMSUNG_MFG_ID;
    }

    // 3. Name fallback for devices in pairing / unregistered mode.
    if (nameContains(lowerName, "airpod")) {
        type = TRACKER_AIRPODS;
        confidence = 80;
        label = "AirPods";
        return true;
    }
    if (nameContains(lowerName, "tile")) {
        type = TRACKER_TILE;
        confidence = 75;
        label = "Tile tracker";
        return true;
    }
    if (nameContains(lowerName, "smarttag")) {
        type = TRACKER_SMARTTAG;
        confidence = 80;
        label = "Samsung SmartTag";
        return true;
    }
    if (nameContains(lowerName, "chipolo")) {
        type = TRACKER_OTHER_FIND_MY;
        confidence = 80;
        label = "Chipolo tracker";
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Camera / smart-glasses matching
// ---------------------------------------------------------------------------

struct GlassesRule {
    int32_t companyId;
    uint16_t serviceUuid16;
    const char *namePattern;
    TrackerType type;
    uint8_t confidence;
    const char *label;
};

static const GlassesRule GLASSES_RULES[] = {
    // --- Meta: company ID together with a Meta service UUID ---------------
    {META_MFG_ID,      UUID_META_FD5F, nullptr,         GLASSES_META,           95, "Meta glasses"           },
    {META_TECH_MFG_ID, UUID_META_FD5F, nullptr,         GLASSES_META,           95, "Meta glasses"           },
    {-1,               UUID_META_FD5F, nullptr,         GLASSES_META,           90, "Meta device (FD5F)"     },
    {-1,               UUID_META_FEB7, nullptr,         GLASSES_META,           82, "Meta device (FEB7)"     },
    {-1,               UUID_META_FEB8, nullptr,         GLASSES_META,           82, "Meta device (FEB8)"     },

    // --- Meta company ID alone --------------------------------
    {META_MFG_ID,      0,              nullptr,         GLASSES_META,           80, "Meta device"            },
    {META_TECH_MFG_ID, 0,              nullptr,         GLASSES_META,           80, "Meta device"            },
    {LUXOTTICA_MFG_ID, 0,              nullptr,         GLASSES_META,           88, "Luxottica smart glasses"},
    {LUXOTTICA_MFG_ID, 0,              "ray",           GLASSES_META,           92, "Ray-Ban Meta"           },
    {LUXOTTICA_MFG_ID, 0,              "oakley",        GLASSES_META,           92, "Oakley Meta"            },

    // --- other vendors: company ID + product name -------------------------
    {SNAP_MFG_ID,      0,              nullptr,         GLASSES_SNAP,           88, "Snap Spectacles"        },
    {SNAP_MFG_ID,      0,              "spectacles",    GLASSES_SNAP,           92, "Snap Spectacles"        },
    {VUZIX_MFG_ID,     0,              nullptr,         GLASSES_OTHER,          85, "Vuzix glasses"          },
    {EVEN_MFG_ID,      0,              nullptr,         GLASSES_OTHER,          85, "Even Realities glasses" },
    {SONY_MFG_ID,      0,              "smarteyeglass", GLASSES_OTHER,          90, "Sony SmartEyeglass"     },
    {EPSON_MFG_ID,     0,              "moverio",       GLASSES_OTHER,          90, "Epson Moverio"          },

    // --- advertised-name matches ------------------------------------------
    {-1,               0,              "ray-ban",       GLASSES_META,           85, "Ray-Ban Meta"           },
    {-1,               0,              "rayban",        GLASSES_META,           85, "Ray-Ban Meta"           },
    {-1,               0,              "ray ban",       GLASSES_META,           85, "Ray-Ban Meta"           },
    {-1,               0,              "meta_rb",       GLASSES_META,           88, "Meta Ray-Ban"           },
    {-1,               0,              "oakley meta",   GLASSES_META,           88, "Oakley Meta"            },
    {-1,               0,              "meta glasses",  GLASSES_META,           85, "Meta glasses"           },
    {-1,               0,              "spectacles",    GLASSES_SNAP,           85, "Snap Spectacles"        },
    {-1,               0,              "vuzix",         GLASSES_OTHER,          85, "Vuzix glasses"          },
    {-1,               0,              "rayneo",        GLASSES_OTHER,          85, "TCL RayNeo"             },
    {-1,               0,              "xreal",         GLASSES_OTHER,          80, "XREAL glasses"          },
    {-1,               0,              "nreal",         GLASSES_OTHER,          80, "Nreal glasses"          },
    {-1,               0,              "rokid",         GLASSES_OTHER,          80, "Rokid glasses"          },
    {-1,               0,              "inmo",          GLASSES_OTHER,          78, "INMO glasses"           },
    {-1,               0,              "solos",         GLASSES_OTHER,          78, "Solos AirGo"            },
    {-1,               0,              "even g",        GLASSES_OTHER,          78, "Even Realities"         },
    {-1,               0,              "echo frames",   GLASSES_OTHER,          80, "Amazon Echo Frames"     },
    {-1,               0,              "focals",        GLASSES_OTHER,          78, "North Focals"           },
    {-1,               0,              "moverio",       GLASSES_OTHER,          82, "Epson Moverio"          },
    {-1,               0,              "google glass",  GLASSES_OTHER,          85, "Google Glass"           },
    {-1,               0,              "heycyan",       GLASSES_OTHER,          85, "HeyCyan glasses"        },
    {-1,               0,              "myvu",          GLASSES_OTHER,          80, "Meizu MYVU"             },
    {-1,               0,              "halliday",      GLASSES_OTHER,          78, "Halliday glasses"       },
    // --- might work ------------------------------------------
    {-1,               0,              "anko",          GLASSES_CAMERA_GENERIC, 65, "Anko/Kmart glasses?"    },
    {-1,               0,              "kmart",         GLASSES_CAMERA_GENERIC, 65, "Kmart glasses?"         },
    {-1,               0,              "ai glasses",    GLASSES_CAMERA_GENERIC, 70, "Generic AI glasses"     },
    {-1,               0,              "smart glasses", GLASSES_CAMERA_GENERIC, 70, "Generic smart glasses"  },
    {-1,               0,              "glasses",       GLASSES_CAMERA_GENERIC, 60, "Name contains 'glasses'"},
    {-1,               0,              "camera",        GLASSES_CAMERA_GENERIC, 55, "Name contains 'camera'" },
};

static bool matchGlasses(
    const NimBLEAdvertisedDevice *device, const String &lowerName, TrackerType &type, uint8_t &confidence,
    const char *&label
) {
    uint16_t mfgId = 0;
    std::string mfg;
    bool haveMfg = getMfgData(device, mfgId, mfg);

    if (haveMfg && (mfgId == META_TECH_MFG_ID || mfgId == META_MFG_ID)) {
        if (mfg.find("META_RB_GLASS") != std::string::npos) {
            type = GLASSES_META;
            confidence = 99;
            label = "Meta Ray-Ban (confirmed)";
            return true;
        }
    }

    const GlassesRule *best = nullptr;
    for (const GlassesRule &r : GLASSES_RULES) {
        if (r.companyId >= 0 && (!haveMfg || mfgId != (uint16_t)r.companyId)) continue;
        if (r.serviceUuid16 != 0 && !hasServiceUUID16(device, r.serviceUuid16)) continue;
        if (r.namePattern != nullptr && !nameContains(lowerName, r.namePattern)) continue;

        if (best == nullptr || r.confidence > best->confidence) best = &r;
    }

    if (best == nullptr) return false;

    type = best->type;
    confidence = best->confidence;
    label = best->label;
    return true;
}

// ---------------------------------------------------------------------------

bool isTrackerDevice(
    const NimBLEAdvertisedDevice *device, TrackerType &type, uint8_t &confidence, const char *&label
) {
    type = TRACKER_UNKNOWN;
    confidence = 0;
    label = "Unknown";
    if (!device) return false;

    String lowerName = device->getName().c_str();
    lowerName.toLowerCase();

    if (matchGlasses(device, lowerName, type, confidence, label)) return true;
    if (matchTracker(device, lowerName, type, confidence, label)) return true;

    return false;
}

// ---------------------------------------------------------------------------
// Proximity
// ---------------------------------------------------------------------------

OwnerProximity assessProximity(const TrackedDevice &device) {
    int rssi = device.currentRSSI;
    size_t n = device.rssiHistory.size();

    if (n >= 6) {
        size_t block = n / 3;
        int oldSum = 0, newSum = 0;

        for (size_t i = 0; i < block; i++) {
            oldSum += device.rssiHistory[i];
            newSum += device.rssiHistory[n - 1 - i];
        }

        int oldAvg = oldSum / (int)block;
        int newAvg = newSum / (int)block;

        if (newAvg < oldAvg - 6) return PROXIMITY_MOVING_AWAY;
        if (newAvg > oldAvg + 6) return PROXIMITY_MOVING_NEAR;
    }

    if (rssi > RSSI_STRONG) return PROXIMITY_NEAR;
    if (rssi > RSSI_MEDIUM) return PROXIMITY_MEDIUM;
    return PROXIMITY_FAR;
}

String getTrackerTypeString(TrackerType type) {
    switch (type) {
        case TRACKER_AIRTAG: return "AirTag";
        case TRACKER_TILE: return "Tile";
        case TRACKER_SMARTTAG: return "SmartTag";
        case TRACKER_AIRPODS: return "AirPods";
        case TRACKER_OTHER_FIND_MY: return "Find My";
        case GLASSES_META: return "META GLASSES";
        case GLASSES_SNAP: return "SPECTACLES";
        case GLASSES_OTHER: return "GLASSES";
        case GLASSES_CAMERA_GENERIC: return "CAMERA?";
        default: return "Unknown";
    }
}

String getProximityString(OwnerProximity prox) {
    switch (prox) {
        case PROXIMITY_NEAR: return "NEAR";
        case PROXIMITY_MEDIUM: return "MEDIUM";
        case PROXIMITY_FAR: return "FAR";
        case PROXIMITY_MOVING_AWAY: return "MOVING AWAY";
        case PROXIMITY_MOVING_NEAR: return "MOVING NEAR";
        default: return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// Scan callbacks
// ---------------------------------------------------------------------------

class TrackerDeviceCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice *advertisedDevice) override {
        TrackerType type;
        uint8_t confidence;
        const char *label;
        if (!isTrackerDevice(advertisedDevice, type, confidence, label)) return;

        String address = advertisedDevice->getAddress().toString().c_str();
        int rssi = advertisedDevice->getRSSI();

        auto it = trackedDevices.find(address);
        if (it == trackedDevices.end()) {
            TrackedDevice newDevice;
            newDevice.address = address;
            newDevice.name = advertisedDevice->getName().c_str();
            newDevice.type = type;
            newDevice.confidence = confidence;
            newDevice.label = label;
            newDevice.currentRSSI = rssi;
            newDevice.lastRSSI = rssi;
            newDevice.averageRSSI = rssi;
            newDevice.lastSeen = millis();
            newDevice.rssiHistory.push_back(rssi);
            newDevice.proximity = assessProximity(newDevice);

            trackedDevices[address] = newDevice;
            return;
        }

        TrackedDevice &device = it->second;
        device.lastRSSI = device.currentRSSI;
        device.currentRSSI = rssi;
        device.lastSeen = millis();

        if (confidence > device.confidence) {
            device.type = type;
            device.confidence = confidence;
            device.label = label;
        }
        if (device.name.isEmpty()) device.name = advertisedDevice->getName().c_str();

        device.rssiHistory.push_back(rssi);
        if (device.rssiHistory.size() > 10) device.rssiHistory.erase(device.rssiHistory.begin());

        int sum = 0;
        for (int val : device.rssiHistory) sum += val;
        device.averageRSSI = sum / (int)device.rssiHistory.size();

        device.proximity = assessProximity(device);
    }
};

static TrackerDeviceCallbacks trackerCallbacks;

// ---------------------------------------------------------------------------
// Scan-object state guard
// ---------------------------------------------------------------------------

// NimBLE keeps a single NimBLEScan instance for the life of the firmware.
// Bruce's stopBLEStack() calls BLEDevice::deinit() without clearAll, and
// NimBLEDevice::deinit(false) leaves m_pScan allocated -- so that object, and
// every setting written to it, survives a teardown and is handed straight back
// by the next BLEDevice::getScan().
//
// ble_scan_setup() re-applies the callbacks, active-scan flag, interval, window
// and duplicate filter, but it does NOT touch maxResults. Leaving maxResults at
// 0 here therefore breaks every later scan that reads getResults() -- including
// Bruce's own BLE Scan menu, whose g_scanCallbacks is an empty class and which
// relies entirely on the buffered result list. It reports "No devices found"
// until the board is rebooted.
//
// This guard restores the NimBLEScan constructor defaults on every exit path.
namespace {
struct ScanSettingsGuard {
    BLEScan *scan;

    explicit ScanSettingsGuard(BLEScan *s) : scan(s) {}

    ScanSettingsGuard(const ScanSettingsGuard &) = delete;
    ScanSettingsGuard &operator=(const ScanSettingsGuard &) = delete;

    ~ScanSettingsGuard() {
        if (scan == nullptr) return;
        // Passing nullptr restores NimBLE's internal default callbacks, and the
        // second argument restores the duplicate filter to its default (on).
        scan->setScanCallbacks(nullptr, false);
        scan->setMaxResults(NIMBLE_DEFAULT_MAX_RESULTS);
    }
};
} // namespace

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void displayTrackerInfo(const String &address, const TrackedDevice &device) {
    tft.fillScreen(bruceConfig.bgColor);
    drawMainBorder();
    tft.setTextColor(bruceConfig.priColor);
    tft.drawCentreString(
        isGlassesType(device.type) ? "-=Camera Device=-" : "-=Tracker Info=-", tftWidth / 2, 28, SMOOTH_FONT
    );

    tft.setTextSize(1);
    tft.setTextColor(bruceConfig.priColor);
    tft.drawString(String(device.label), 10, 48);
    tft.drawString("Match: " + String(device.confidence) + "%", 10, 66);
    tft.drawString("Addr: " + address, 10, 84);
    tft.drawString("RSSI: " + String(device.currentRSSI) + " / avg " + String(device.averageRSSI), 10, 102);

    uint16_t proxColor = bruceConfig.priColor;
    if (device.proximity == PROXIMITY_NEAR || device.proximity == PROXIMITY_MOVING_NEAR) {
        proxColor = TFT_RED;
    } else if (device.proximity == PROXIMITY_MEDIUM) {
        proxColor = TFT_YELLOW;
    } else {
        proxColor = TFT_GREEN;
    }

    tft.setTextColor(proxColor);
    tft.drawString("Status: " + getProximityString(device.proximity), 10, 120);

    tft.setTextColor(bruceConfig.priColor);
    tft.drawCentreString("Press " + String(BTN_ALIAS) + " to back", tftWidth / 2, tftHeight - 20, 1);

    delay(300);
    while (!check(SelPress)) { yield(); }
}

void ble_tracker_detector() {
    tft.fillScreen(bruceConfig.bgColor);
    displayTextLine("Scanning..");

    trackedDevices.clear();

    // Only tear the stack down at the end if we were the ones who brought it
    // up. Another module (BLE Suite, BLE HID, the BLE server) may already own
    // an active session, and killing it from here would drop its connections.
    bool bleWasActiveBefore = BLEConnected || (BLEDevice::getServer() != nullptr);
#if !defined(LITE_VERSION)
    bleWasActiveBefore =
        bleWasActiveBefore || BLEStateManager::isBLEActive() || BLEStateManager::getActiveClientCount() > 0;
#endif

    if (!ble_scan_setup() || !pBLEScan) { return; }

    {
        // Restores maxResults and the callbacks when this block exits, by any
        // path. See the comment on ScanSettingsGuard above -- without it, the
        // next module to call getResults() silently sees nothing.
        ScanSettingsGuard guard(pBLEScan);

        // true == report duplicates, so RSSI history builds up across the scan.
        pBLEScan->setScanCallbacks(&trackerCallbacks, true);

        // Callback-only mode: process each advert in onResult and buffer
        // nothing, which keeps heap use flat over a long scan.
        pBLEScan->setMaxResults(0);
        pBLEScan->clearResults();

        displayTextLine("Nearly done... " + String(SCAN_TIME_MS / 1000) + "s...");

        pBLEScan->getResults(SCAN_TIME_MS, false);
        pBLEScan->stop();
    } // guard restores the scan object's defaults here

    if (!bleWasActiveBefore) {
#if !defined(LITE_VERSION)
        if (!BLEStateManager::isBLEActive()) { stopBLEStack(); }
#else
        stopBLEStack();
#endif
    }

    if (trackedDevices.empty()) {
        displayError("Nothing detected");
        delay(1500);
        return;
    }

    // Glasses first, then trackers, each highest-confidence first.
    std::vector<const TrackedDevice *> sorted;
    for (auto &entry : trackedDevices) sorted.push_back(&entry.second);
    std::sort(sorted.begin(), sorted.end(), [](const TrackedDevice *a, const TrackedDevice *b) {
        if (isGlassesType(a->type) != isGlassesType(b->type)) return isGlassesType(a->type);
        return a->confidence > b->confidence;
    });

    options.clear();
    for (const TrackedDevice *device : sorted) {
        String displayName = "[" + getTrackerTypeString(device->type) + "] ";
        displayName += device->name.isEmpty() ? device->address : device->name;

        String address = device->address;
        options.emplace_back(displayName.c_str(), [address]() {
            auto it = trackedDevices.find(address);
            if (it != trackedDevices.end()) displayTrackerInfo(it->first, it->second);
        });
    }

    addOptionToMainMenu();
    loopOptions(options);

    options.clear();
    trackedDevices.clear();
}
