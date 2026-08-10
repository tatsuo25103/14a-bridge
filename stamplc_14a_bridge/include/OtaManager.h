#pragma once

#include <Arduino.h>
#include <functional>

class OtaManager {
public:
    void begin();
    void service(bool safeToUpdate);
    void setSafetyCheck(std::function<bool()> check) { safetyCheck_ = check; }
    void setProgressCallback(std::function<void(const String&, int)> callback) {
        progressCallback_ = callback;
    }

    bool saveWifiHex(const String& ssidHex, const String& passwordHex,
                     String& detail);
    bool clearWifi();
    void connectNow();
    bool setAutomatic(bool enabled);

    bool automatic() const { return automatic_; }
    bool hasCredentials() const { return !ssid_.isEmpty(); }
    bool connected() const;
    String ssidHex() const;
    String ipAddress() const;
    int32_t rssi() const;

    bool checkForUpdate(String& availableVersion, String& detail);
    bool installUpdate(String& detail);

private:
    struct Manifest {
        String version;
        String url;
        String sha256;
    };

    String ssid_;
    String password_;
    bool automatic_ = false;
    uint32_t nextAutomaticCheckMs_ = 0;
    std::function<bool()> safetyCheck_;
    std::function<void(const String&, int)> progressCallback_;
    void reportProgress(const String& stage, int percent = -1);
    void load();
    bool save();
    bool fetchManifest(Manifest& manifest, String& detail);
    bool downloadAndInstall(const Manifest& manifest, String& detail);
    static bool decodeHex(const String& hex, String& decoded);
    static String encodeHex(const String& value);
    static String jsonString(const String& json, const char* key);
    static int compareVersions(const String& left, const String& right);
};

extern OtaManager otaManager;
