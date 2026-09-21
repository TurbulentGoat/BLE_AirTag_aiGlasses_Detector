#ifndef __BLE_TRACKER_DETECTOR_H__
#define __BLE_TRACKER_DETECTOR_H__

#include <Arduino.h>
#include <NimBLEAdvertisedDevice.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <map>
#include <vector>

enum TrackerType {
    TRACKER_UNKNOWN,
    // --- location trackers ---
    TRACKER_AIRTAG,
    TRACKER_TILE,
    TRACKER_SMARTTAG,
    TRACKER_AIRPODS,
    TRACKER_OTHER_FIND_MY,
    // --- camera / smart glasses (keep these last, see isGlassesType) ---
    GLASSES_META,
    GLASSES_SNAP,
    GLASSES_OTHER,
    GLASSES_CAMERA_GENERIC
};

inline bool isGlassesType(TrackerType t) { return t >= GLASSES_META; }

enum OwnerProximity {
    PROXIMITY_UNKNOWN,
    PROXIMITY_NEAR,   // Strong signal, stable RSSI
    PROXIMITY_MEDIUM, // Medium signal
    PROXIMITY_FAR,    // Weak signal
    PROXIMITY_MOVING_AWAY,
    PROXIMITY_MOVING_NEAR
};

struct TrackedDevice {
    String address;
    String name;
    TrackerType type;
    uint8_t confidence; // 0-100, how sure the match is
    const char *label;  // static string literal, never freed
    int currentRSSI;
    int lastRSSI;
    int averageRSSI;
    unsigned long lastSeen;
    OwnerProximity proximity;
    std::vector<int> rssiHistory; // Keep last 10 readings
};

void ble_tracker_detector();

// NimBLE 2.x hands the callback a CONST pointer. This must match or the
// override silently fails to bind.
bool isTrackerDevice(
    const NimBLEAdvertisedDevice *device, TrackerType &type, uint8_t &confidence, const char *&label
);

OwnerProximity assessProximity(const TrackedDevice &device);
String getTrackerTypeString(TrackerType type);
String getProximityString(OwnerProximity prox);

#endif
