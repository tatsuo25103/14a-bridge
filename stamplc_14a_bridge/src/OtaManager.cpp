#include "OtaManager.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <time.h>

extern const uint8_t githubRootsStart[] asm("_binary_certs_github_roots_pem_start");
extern const uint8_t otaSigningPublicStart[]
    asm("_binary_certs_ota_signing_public_pem_start");

namespace {
constexpr char CURRENT_VERSION[] = "1.0.4";
constexpr char HARDWARE_ID[] = "M5STACK_STAMPPLC";
constexpr char SIGNATURE_ALGORITHM[] = "ECDSA_P256_SHA256";
constexpr char PRIMARY_MANIFEST_URL[] =
    "https://raw.githubusercontent.com/tatsuo25103/14a-bridge/main/"
    "stamplc_14a_bridge/release/ota_manifest.json";
constexpr char BACKUP_MANIFEST_URL[] =
    "https://github.com/tatsuo25103/14a-bridge/releases/latest/download/"
    "ota_manifest.json";
constexpr char RAW_DOWNLOAD_PREFIX[] =
    "https://raw.githubusercontent.com/tatsuo25103/14a-bridge/main/"
    "stamplc_14a_bridge/release/";
constexpr char RELEASE_DOWNLOAD_PREFIX[] =
    "https://github.com/tatsuo25103/14a-bridge/releases/download/";
constexpr uint32_t FIRST_AUTO_CHECK_MS = 60000;
constexpr uint32_t AUTO_CHECK_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL;
constexpr uint32_t FAILURE_BACKOFF_MS[] = {
    15UL * 60UL * 1000UL,
    30UL * 60UL * 1000UL,
    60UL * 60UL * 1000UL,
    6UL * 60UL * 60UL * 1000UL,
    24UL * 60UL * 60UL * 1000UL,
};
constexpr size_t MAX_MANIFEST_BYTES = 8192;
constexpr size_t MAX_FIRMWARE_BYTES = 0x300000;

bool validSha256(const String& value) {
    if (value.length() != 64) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        if (!isxdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

String digestHex(const uint8_t digest[32]) {
    static const char hex[] = "0123456789abcdef";
    String result;
    result.reserve(64);
    for (uint8_t i = 0; i < 32; ++i) {
        result += hex[digest[i] >> 4];
        result += hex[digest[i] & 0x0F];
    }
    return result;
}

bool allowedDownloadUrl(const String& url) {
    return url.startsWith(RAW_DOWNLOAD_PREFIX) ||
           url.startsWith(RELEASE_DOWNLOAD_PREFIX);
}

String manifestPayload(const String& version, const String& hardware,
                       size_t size, const String& sha256,
                       const String& primaryUrl, const String& backupUrl) {
    String payload;
    payload.reserve(256 + primaryUrl.length() + backupUrl.length());
    payload += "14A_BRIDGE\n";
    payload += hardware + "\n";
    payload += version + "\n";
    payload += String(static_cast<unsigned long>(size)) + "\n";
    payload += sha256 + "\n";
    payload += primaryUrl + "\n";
    payload += backupUrl + "\n";
    return payload;
}
}  // namespace

OtaManager otaManager;

void OtaManager::reportProgress(const String& stage, int percent) {
    if (progressCallback_) progressCallback_(stage, percent);
}

void OtaManager::begin() {
    load();
    loadDiagnostics();
    nextAutomaticCheckMs_ = FIRST_AUTO_CHECK_MS;
    if (hasCredentials()) connectNow();
}

void OtaManager::load() {
    Preferences p;
    if (!p.begin("bridgewifi", true)) return;
    ssid_ = p.getString("ssid", "");
    password_ = p.getString("password", "");
    automatic_ = p.getBool("auto_ota", false);
    p.end();
}

bool OtaManager::save() {
    Preferences p;
    if (!p.begin("bridgewifi", false)) return false;
    p.putString("ssid", ssid_);
    p.putString("password", password_);
    p.putBool("auto_ota", automatic_);
    p.end();
    return true;
}

void OtaManager::loadDiagnostics() {
    Preferences p;
    if (!p.begin("bridgeota", true)) return;
    lastStatus_ = p.getString("status", "NEVER");
    lastDetail_ = p.getString("detail", "not checked");
    lastCheckEpoch_ = p.getULong("last_check", 0);
    lastSuccessEpoch_ = p.getULong("last_ok", 0);
    consecutiveFailures_ = p.getULong("failures", 0);
    p.end();
}

void OtaManager::saveDiagnostics() {
    Preferences p;
    if (!p.begin("bridgeota", false)) return;
    p.putString("status", lastStatus_);
    p.putString("detail", lastDetail_.substring(0, 180));
    p.putULong("last_check", lastCheckEpoch_);
    p.putULong("last_ok", lastSuccessEpoch_);
    p.putULong("failures", consecutiveFailures_);
    p.end();
}

void OtaManager::recordDiagnostic(const String& status, const String& detail,
                                  bool successfulCheck) {
    lastStatus_ = status;
    lastDetail_ = detail;
    const time_t now = time(nullptr);
    if (now > 1704067200) lastCheckEpoch_ = static_cast<uint32_t>(now);
    if (successfulCheck) {
        consecutiveFailures_ = 0;
        if (now > 1704067200) lastSuccessEpoch_ = static_cast<uint32_t>(now);
    } else if (consecutiveFailures_ < 1000) {
        ++consecutiveFailures_;
    }
    saveDiagnostics();
}

void OtaManager::scheduleAfterFailure() {
    const size_t count = sizeof(FAILURE_BACKOFF_MS) / sizeof(FAILURE_BACKOFF_MS[0]);
    const size_t index = min<size_t>(consecutiveFailures_ > 0
                                        ? consecutiveFailures_ - 1 : 0,
                                    count - 1);
    const uint32_t base = FAILURE_BACKOFF_MS[index];
    const uint32_t jitter = esp_random() % max<uint32_t>(base / 10, 1);
    nextAutomaticCheckMs_ = millis() + base + jitter;
}

void OtaManager::scheduleAfterSuccess() {
    const uint32_t jitter = esp_random() % (60UL * 60UL * 1000UL);
    nextAutomaticCheckMs_ = millis() + AUTO_CHECK_INTERVAL_MS + jitter;
}

uint32_t OtaManager::nextCheckSeconds() const {
    const int32_t remaining = static_cast<int32_t>(nextAutomaticCheckMs_ - millis());
    return remaining <= 0 ? 0 : static_cast<uint32_t>(remaining) / 1000UL;
}

String OtaManager::lastDetailHex() const { return encodeHex(lastDetail_); }

bool OtaManager::decodeHex(const String& hex, String& decoded) {
    decoded = "";
    if ((hex.length() & 1U) != 0) return false;
    decoded.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        char pair[3] = {hex[i], hex[i + 1], 0};
        char* end = nullptr;
        const unsigned long value = strtoul(pair, &end, 16);
        if (end != pair + 2 || value == 0) return false;
        decoded += static_cast<char>(value);
    }
    return true;
}

String OtaManager::encodeHex(const String& value) {
    static const char hex[] = "0123456789ABCDEF";
    String result;
    result.reserve(value.length() * 2);
    for (size_t i = 0; i < value.length(); ++i) {
        const uint8_t c = static_cast<uint8_t>(value[i]);
        result += hex[c >> 4];
        result += hex[c & 0x0F];
    }
    return result;
}

bool OtaManager::saveWifiHex(const String& ssidHex, const String& passwordHex,
                             String& detail) {
    String ssid;
    String password;
    if (!decodeHex(ssidHex, ssid) || !decodeHex(passwordHex, password)) {
        detail = "SSID or password encoding is invalid";
        return false;
    }
    if (ssid.length() == 0 || ssid.length() > 32) {
        detail = "SSID must contain 1..32 bytes";
        return false;
    }
    if (!password.isEmpty() && (password.length() < 8 || password.length() > 63)) {
        detail = "password must be empty or contain 8..63 bytes";
        return false;
    }
    ssid_ = ssid;
    password_ = password;
    if (!save()) {
        detail = "could not save Wi-Fi settings";
        return false;
    }
    connectNow();
    detail = "saved; connection started";
    return true;
}

bool OtaManager::clearWifi() {
    WiFi.disconnect(true, true);
    ssid_ = "";
    password_ = "";
    automatic_ = false;
    Preferences p;
    if (!p.begin("bridgewifi", false)) return false;
    const bool ok = p.clear();
    p.end();
    return ok;
}

void OtaManager::connectNow() {
    if (!hasCredentials()) return;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.begin(ssid_.c_str(), password_.c_str());
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
}

bool OtaManager::setAutomatic(bool enabled) {
    const bool previous = automatic_;
    automatic_ = enabled;
    if (!save()) {
        automatic_ = previous;
        return false;
    }
    if (enabled) {
        nextAutomaticCheckMs_ = millis() + 5000;
    }
    return true;
}

bool OtaManager::connected() const { return WiFi.status() == WL_CONNECTED; }
String OtaManager::ssidHex() const { return encodeHex(ssid_); }
String OtaManager::ipAddress() const {
    return connected() ? WiFi.localIP().toString() : String("-");
}
int32_t OtaManager::rssi() const { return connected() ? WiFi.RSSI() : 0; }

String OtaManager::jsonString(const String& json, const char* key) {
    const String token = String('"') + key + "\"";
    int position = json.indexOf(token);
    if (position < 0) return "";
    position = json.indexOf(':', position + token.length());
    if (position < 0) return "";
    position = json.indexOf('"', position + 1);
    if (position < 0) return "";
    const int end = json.indexOf('"', position + 1);
    if (end < 0) return "";
    return json.substring(position + 1, end);
}

uint32_t OtaManager::jsonUInt(const String& json, const char* key) {
    const String token = String('"') + key + "\"";
    int position = json.indexOf(token);
    if (position < 0) return 0;
    position = json.indexOf(':', position + token.length());
    if (position < 0) return 0;
    ++position;
    while (position < static_cast<int>(json.length()) && isspace(json[position]))
        ++position;
    int end = position;
    while (end < static_cast<int>(json.length()) && isdigit(json[end])) ++end;
    if (end == position) return 0;
    return static_cast<uint32_t>(strtoul(json.substring(position, end).c_str(),
                                        nullptr, 10));
}

int OtaManager::compareVersions(const String& left, const String& right) {
    int li = 0;
    int ri = 0;
    for (uint8_t part = 0; part < 3; ++part) {
        const int ldot = left.indexOf('.', li);
        const int rdot = right.indexOf('.', ri);
        const int lv = left.substring(li, ldot < 0 ? left.length() : ldot).toInt();
        const int rv = right.substring(ri, rdot < 0 ? right.length() : rdot).toInt();
        if (lv != rv) return lv < rv ? -1 : 1;
        li = ldot < 0 ? left.length() : ldot + 1;
        ri = rdot < 0 ? right.length() : rdot + 1;
    }
    return 0;
}

bool OtaManager::verifyManifestSignature(const Manifest& manifest,
                                         String& detail) {
    if (manifest.signatureAlgorithm != SIGNATURE_ALGORITHM) {
        detail = "manifest signature algorithm is unsupported";
        return false;
    }
    uint8_t signature[128];
    size_t signatureLength = 0;
    const int decodeResult = mbedtls_base64_decode(
        signature, sizeof(signature), &signatureLength,
        reinterpret_cast<const uint8_t*>(manifest.signature.c_str()),
        manifest.signature.length());
    if (decodeResult != 0 || signatureLength == 0) {
        detail = "manifest signature encoding is invalid";
        return false;
    }

    const String payload = manifestPayload(
        manifest.version, manifest.hardware, manifest.size, manifest.sha256,
        manifest.primaryUrl, manifest.backupUrl);
    uint8_t digest[32];
    if (mbedtls_sha256_ret(
            reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length(),
            digest, 0) != 0) {
        detail = "manifest digest calculation failed";
        return false;
    }

    mbedtls_pk_context publicKey;
    mbedtls_pk_init(&publicKey);
    int result = mbedtls_pk_parse_public_key(
        &publicKey, otaSigningPublicStart,
        strlen(reinterpret_cast<const char*>(otaSigningPublicStart)) + 1);
    if (result == 0) {
        result = mbedtls_pk_verify(&publicKey, MBEDTLS_MD_SHA256, digest,
                                   sizeof(digest), signature, signatureLength);
    }
    mbedtls_pk_free(&publicKey);
    if (result != 0) {
        detail = "manifest signature verification failed";
        return false;
    }
    return true;
}

bool OtaManager::fetchManifestUrl(const String& url, String& json,
                                  String& detail) {
    WiFiClientSecure client;
    client.setCACert(reinterpret_cast<const char*>(githubRootsStart));
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(15000);
    http.useHTTP10(true);
    if (!http.begin(client, url)) {
        detail = "could not open update manifest URL";
        return false;
    }
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        detail = "manifest HTTP " + String(code) + " " +
                 HTTPClient::errorToString(code);
        http.end();
        return false;
    }
    const int length = http.getSize();
    if (length <= 0 || length > static_cast<int>(MAX_MANIFEST_BYTES)) {
        detail = "manifest size is invalid";
        http.end();
        return false;
    }
    json = http.getString();
    http.end();
    return true;
}

bool OtaManager::fetchManifest(Manifest& manifest, String& detail) {
    if (!connected()) {
        detail = "Wi-Fi is not connected";
        return false;
    }
    if (time(nullptr) < 1704067200) {
        configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
        const uint32_t deadline = millis() + 10000;
        while (time(nullptr) < 1704067200 &&
               static_cast<int32_t>(millis() - deadline) < 0)
            delay(50);
        if (time(nullptr) < 1704067200) {
            detail = "network time is unavailable; HTTPS validation cannot start";
            return false;
        }
    }

    String json;
    String primaryDetail;
    if (!fetchManifestUrl(PRIMARY_MANIFEST_URL, json, primaryDetail)) {
        String backupDetail;
        if (!fetchManifestUrl(BACKUP_MANIFEST_URL, json, backupDetail)) {
            detail = "primary: " + primaryDetail + "; backup: " + backupDetail;
            return false;
        }
    }
    manifest.version = jsonString(json, "version");
    manifest.hardware = jsonString(json, "hardware");
    manifest.size = jsonUInt(json, "size");
    manifest.primaryUrl = jsonString(json, "primary_url");
    manifest.backupUrl = jsonString(json, "backup_url");
    manifest.sha256 = jsonString(json, "sha256");
    manifest.signatureAlgorithm = jsonString(json, "signature_alg");
    manifest.signature = jsonString(json, "signature");
    manifest.sha256.toLowerCase();
    if (manifest.version.isEmpty() || manifest.hardware != HARDWARE_ID ||
        manifest.size == 0 || manifest.size > MAX_FIRMWARE_BYTES ||
        !allowedDownloadUrl(manifest.primaryUrl) ||
        !allowedDownloadUrl(manifest.backupUrl) ||
        !validSha256(manifest.sha256) || manifest.signature.isEmpty()) {
        detail = "manifest fields are missing or invalid";
        return false;
    }
    if (!verifyManifestSignature(manifest, detail)) return false;
    detail = compareVersions(manifest.version, CURRENT_VERSION) > 0
        ? "signed update available" : "signed manifest is up to date";
    return true;
}

bool OtaManager::checkForUpdate(String& availableVersion, String& detail) {
    reportProgress("CHECKING");
    Manifest manifest;
    if (!fetchManifest(manifest, detail)) {
        recordDiagnostic("ERROR", detail, false);
        reportProgress("ERROR");
        return false;
    }
    availableVersion = manifest.version;
    const bool updateFound = compareVersions(manifest.version, CURRENT_VERSION) > 0;
    recordDiagnostic(updateFound ? "UPDATE_FOUND" : "UP_TO_DATE", detail, true);
    reportProgress(updateFound ? "UPDATE FOUND" : "UP TO DATE", 100);
    return true;
}

bool OtaManager::downloadUrlAndInstall(const Manifest& manifest,
                                       const String& url, String& detail) {
    if (safetyCheck_ && !safetyCheck_()) {
        detail = "RSE changed or Modbus control became busy; update cancelled";
        return false;
    }
    WiFiClientSecure client;
    client.setCACert(reinterpret_cast<const char*>(githubRootsStart));
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(20000);
    http.useHTTP10(true);
    if (!http.begin(client, url)) {
        detail = "could not open firmware URL";
        return false;
    }
    const int code = http.GET();
    const int length = http.getSize();
    if (code != HTTP_CODE_OK || length <= 0 ||
        static_cast<size_t>(length) != manifest.size) {
        detail = "firmware HTTP " + String(code) + " or size " +
                 String(length) + " does not match signed manifest " +
                 String(static_cast<unsigned long>(manifest.size));
        http.end();
        return false;
    }
    if (!Update.begin(manifest.size, U_FLASH)) {
        detail = "OTA partition is unavailable or too small: " + String(Update.errorString());
        http.end();
        return false;
    }

    reportProgress("DOWNLOADING", 0);

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buffer[1024];
    int remaining = length;
    int lastPercent = -1;
    int lastUiPercent = -1;
    bool ok = true;
    uint32_t lastDataAt = millis();
    while (remaining > 0) {
        if (safetyCheck_ && !safetyCheck_()) {
            ok = false;
            detail = "RSE changed during download; update cancelled before activation";
            break;
        }
        const size_t available = stream->available();
        if (available == 0) {
            if (!http.connected()) { ok = false; break; }
            if (millis() - lastDataAt > 20000) {
                detail = "firmware download stalled for 20 seconds";
                ok = false;
                break;
            }
            delay(1);
            continue;
        }
        const size_t wanted = min<size_t>(sizeof(buffer), min<int>(remaining, available));
        const int count = stream->readBytes(buffer, wanted);
        if (count <= 0 || Update.write(buffer, count) != static_cast<size_t>(count)) {
            ok = false;
            break;
        }
        mbedtls_sha256_update_ret(&sha, buffer, count);
        lastDataAt = millis();
        remaining -= count;
        const int percent = static_cast<int>((static_cast<int64_t>(length - remaining) * 100) / length);
        if (percent != lastUiPercent) {
            lastUiPercent = percent;
            reportProgress("DOWNLOADING", percent);
        }
        if (percent / 10 != lastPercent / 10) {
            lastPercent = percent;
            Serial.printf("OTA PROGRESS=%d\r\n", percent);
        }
    }
    uint8_t digest[32];
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);
    http.end();

    const String actualSha = digestHex(digest);
    reportProgress("VERIFYING", 100);
    if (!ok || remaining != 0 || actualSha != manifest.sha256 ||
        (safetyCheck_ && !safetyCheck_())) {
        Update.abort();
        if (detail.isEmpty())
            detail = actualSha != manifest.sha256 ? "firmware SHA-256 mismatch" :
                     "firmware download interrupted or RSE changed";
        return false;
    }
    const esp_partition_t* runningPartition = esp_ota_get_running_partition();
    Preferences bootState;
    bool rollbackRecorded = bootState.begin("bridgeboot", false);
    if (rollbackRecorded) {
        rollbackRecorded = bootState.putString("target", manifest.version) > 0;
        rollbackRecorded = bootState.putUChar(
            "previous", runningPartition
                            ? static_cast<uint8_t>(runningPartition->subtype) : 0xFF) > 0 &&
            rollbackRecorded;
        rollbackRecorded = bootState.putUChar("attempts", 0) > 0 && rollbackRecorded;
        bootState.end();
    }
    if (!rollbackRecorded) {
        Update.abort();
        detail = "could not save OTA rollback state; current firmware kept";
        return false;
    }
    if (!Update.end(true)) {
        Preferences clearBootState;
        if (clearBootState.begin("bridgeboot", false)) {
            clearBootState.clear();
            clearBootState.end();
        }
        detail = "firmware image rejected: " + String(Update.errorString());
        return false;
    }
    reportProgress("INSTALLING", 100);
    detail = "verified; rebooting into " + manifest.version;
    return true;
}

bool OtaManager::downloadAndInstall(const Manifest& manifest, String& detail) {
    String primaryDetail;
    if (downloadUrlAndInstall(manifest, manifest.primaryUrl, primaryDetail))
        return true;
    Update.abort();
    if (safetyCheck_ && !safetyCheck_()) {
        detail = primaryDetail;
        return false;
    }
    String backupDetail;
    if (manifest.backupUrl != manifest.primaryUrl &&
        downloadUrlAndInstall(manifest, manifest.backupUrl, backupDetail))
        return true;
    Update.abort();
    detail = "primary: " + primaryDetail + "; backup: " + backupDetail;
    return false;
}

bool OtaManager::installUpdate(String& detail) {
    reportProgress("CHECKING");
    Manifest manifest;
    if (!fetchManifest(manifest, detail)) {
        recordDiagnostic("ERROR", detail, false);
        reportProgress("ERROR");
        return false;
    }
    if (compareVersions(manifest.version, CURRENT_VERSION) <= 0) {
        detail = "already running the latest version " + String(CURRENT_VERSION);
        recordDiagnostic("UP_TO_DATE", detail, true);
        reportProgress("UP TO DATE", 100);
        return false;
    }
    recordDiagnostic("INSTALLING", "installing signed " + manifest.version, true);
    if (!downloadAndInstall(manifest, detail)) {
        recordDiagnostic("ERROR", detail, false);
        reportProgress("ERROR");
        return false;
    }
    recordDiagnostic("INSTALLED", detail, true);
    reportProgress("RESTARTING", 100);
    Serial.println("OTA STATUS=REBOOT DETAIL=" + detail);
    Serial.flush();
    delay(500);
    ESP.restart();
    return true;
}

void OtaManager::service(bool safeToUpdate) {
    if (!automatic_ || !connected() || !safeToUpdate) return;
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextAutomaticCheckMs_) < 0) return;
    String version;
    String detail;
    Serial.println("OTA AUTO STATUS=CHECKING");
    if (!checkForUpdate(version, detail)) {
        scheduleAfterFailure();
        Serial.println("OTA AUTO STATUS=ERROR DETAIL=" + detail);
        return;
    }
    if (compareVersions(version, CURRENT_VERSION) <= 0) {
        scheduleAfterSuccess();
        Serial.println("OTA AUTO STATUS=UP_TO_DATE VERSION=" + version);
        return;
    }
    Serial.println("OTA AUTO STATUS=INSTALLING VERSION=" + version);
    if (!installUpdate(detail)) {
        scheduleAfterFailure();
        Serial.println("OTA AUTO STATUS=ERROR DETAIL=" + detail);
    }
}
