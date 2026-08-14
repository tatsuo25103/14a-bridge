#pragma once

#include <Arduino.h>
#include <functional>
#include <time.h>

class OtaManager {
public:
    void begin();
    void service(bool safeToUpdate);
    void setSafetyCheck(std::function<bool()> check) { safetyCheck_ = check; }
    void setProgressCallback(std::function<void(const String&, int)> callback) {
        progressCallback_ = callback;
    }
    void setLocalClockProvider(
        std::function<bool(uint32_t&, uint16_t&, uint8_t&)> provider) {
        localClockProvider_ = provider;
    }
    void setRtcSyncCallback(std::function<void(const tm&)> callback) {
        rtcSyncCallback_ = callback;
    }

    bool saveWifiHex(const String& ssidHex, const String& passwordHex,
                     String& detail);
    bool clearWifi();
    void connectNow();
    bool setAutomatic(bool enabled);
    bool setSchedule(uint8_t hour, uint8_t minute);

    bool automatic() const { return automatic_; }
    uint8_t scheduleHour() const { return scheduleHour_; }
    uint8_t scheduleMinute() const { return scheduleMinute_; }
    bool hasCredentials() const { return !ssid_.isEmpty(); }
    bool connected() const;
    String ssidHex() const;
    String ipAddress() const;
    int32_t rssi() const;
    String lastStatus() const { return lastStatus_; }
    String lastDetailHex() const;
    uint32_t lastCheckEpoch() const { return lastCheckEpoch_; }
    uint32_t lastSuccessEpoch() const { return lastSuccessEpoch_; }
    uint32_t consecutiveFailures() const { return consecutiveFailures_; }
    uint32_t nextCheckSeconds() const;

    bool checkForUpdate(String& availableVersion, String& detail);
    bool installUpdate(String& detail);

private:
    struct Manifest {
        String version;
        String hardware;
        size_t size = 0;
        String primaryUrl;
        String backupUrl;
        String sha256;
        String signatureAlgorithm;
        String signature;
    };

    String ssid_;
    String password_;
    bool automatic_ = false;
    uint32_t nextAutomaticCheckMs_ = 0;
    uint32_t nextScheduledSeconds_ = 0;
    uint32_t lastScheduleEvaluationMs_ = 0;
    uint32_t lastRtcSyncMs_ = 0;
    uint8_t scheduleHour_ = 1;
    uint8_t scheduleMinute_ = 0;
    int32_t lastScheduledDay_ = -1;
    int32_t currentWindowDay_ = -1;
    String lastStatus_ = "NEVER";
    String lastDetail_ = "not checked";
    uint32_t lastCheckEpoch_ = 0;
    uint32_t lastSuccessEpoch_ = 0;
    uint32_t consecutiveFailures_ = 0;
    std::function<bool()> safetyCheck_;
    std::function<void(const String&, int)> progressCallback_;
    std::function<bool(uint32_t&, uint16_t&, uint8_t&)> localClockProvider_;
    std::function<void(const tm&)> rtcSyncCallback_;
    void reportProgress(const String& stage, int percent = -1);
    void load();
    bool save();
    void loadDiagnostics();
    void saveDiagnostics();
    void recordDiagnostic(const String& status, const String& detail,
                          bool successfulCheck);
    void scheduleAfterFailure();
    void scheduleAfterSuccess();
    bool fetchManifest(Manifest& manifest, String& detail);
    bool fetchManifestUrl(const String& url, String& json, String& detail);
    bool parseAndVerifyManifest(const String& json, Manifest& manifest,
                                String& detail);
    bool verifyManifestSignature(const Manifest& manifest, String& detail);
    bool downloadAndInstall(const Manifest& manifest, String& detail);
    bool downloadUrlAndInstall(const Manifest& manifest, const String& url,
                               String& detail);
    static bool decodeHex(const String& hex, String& decoded);
    static String encodeHex(const String& value);
    static String jsonString(const String& json, const char* key);
    static uint32_t jsonUInt(const String& json, const char* key);
    static int compareVersions(const String& left, const String& right);
};

extern OtaManager otaManager;
