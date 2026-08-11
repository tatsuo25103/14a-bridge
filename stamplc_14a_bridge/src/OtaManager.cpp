#include "OtaManager.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>
#include <time.h>

extern const uint8_t githubRootsStart[] asm("_binary_certs_github_roots_pem_start");

namespace {
constexpr char CURRENT_VERSION[] = "1.0.3";
constexpr char MANIFEST_URL[] =
    "https://raw.githubusercontent.com/tatsuo25103/14a-bridge/main/"
    "stamplc_14a_bridge/release/ota_manifest.json";
constexpr char ALLOWED_DOWNLOAD_PREFIX[] =
    "https://raw.githubusercontent.com/tatsuo25103/14a-bridge/main/"
    "stamplc_14a_bridge/release/";
constexpr uint32_t FIRST_AUTO_CHECK_MS = 60000;
constexpr uint32_t AUTO_CHECK_INTERVAL_MS = 24UL * 60UL * 60UL * 1000UL;
constexpr size_t MAX_MANIFEST_BYTES = 4096;

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
}  // namespace

OtaManager otaManager;

void OtaManager::reportProgress(const String& stage, int percent) {
    if (progressCallback_) progressCallback_(stage, percent);
}

void OtaManager::begin() {
    load();
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

bool OtaManager::fetchManifest(Manifest& manifest, String& detail) {
    if (!connected()) {
        detail = "Wi-Fi is not connected";
        return false;
    }
    if (time(nullptr) < 1704067200) {
        configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
        const uint32_t deadline = millis() + 6000;
        while (time(nullptr) < 1704067200 && static_cast<int32_t>(millis() - deadline) < 0)
            delay(50);
        if (time(nullptr) < 1704067200) {
            detail = "network time is unavailable; HTTPS validation cannot start";
            return false;
        }
    }
    WiFiClientSecure client;
    client.setCACert(reinterpret_cast<const char*>(githubRootsStart));
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(15000);
    http.useHTTP10(true);
    if (!http.begin(client, MANIFEST_URL)) {
        detail = "could not open update manifest URL";
        return false;
    }
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        detail = "manifest HTTP " + String(code);
        http.end();
        return false;
    }
    const int length = http.getSize();
    if (length <= 0 || length > static_cast<int>(MAX_MANIFEST_BYTES)) {
        detail = "manifest size is invalid";
        http.end();
        return false;
    }
    const String json = http.getString();
    http.end();
    manifest.version = jsonString(json, "version");
    manifest.url = jsonString(json, "url");
    manifest.sha256 = jsonString(json, "sha256");
    manifest.sha256.toLowerCase();
    if (manifest.version.isEmpty() ||
        !manifest.url.startsWith(ALLOWED_DOWNLOAD_PREFIX) ||
        !validSha256(manifest.sha256)) {
        detail = "manifest fields are missing or invalid";
        return false;
    }
    detail = compareVersions(manifest.version, CURRENT_VERSION) > 0
        ? "update available" : "already up to date";
    return true;
}

bool OtaManager::checkForUpdate(String& availableVersion, String& detail) {
    reportProgress("CHECKING");
    Manifest manifest;
    if (!fetchManifest(manifest, detail)) {
        reportProgress("ERROR");
        return false;
    }
    availableVersion = manifest.version;
    reportProgress(compareVersions(manifest.version, CURRENT_VERSION) > 0
                       ? "UPDATE FOUND" : "UP TO DATE", 100);
    return true;
}

bool OtaManager::downloadAndInstall(const Manifest& manifest, String& detail) {
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
    if (!http.begin(client, manifest.url)) {
        detail = "could not open firmware URL";
        return false;
    }
    const int code = http.GET();
    const int length = http.getSize();
    if (code != HTTP_CODE_OK || length <= 0) {
        detail = "firmware HTTP " + String(code) + " or missing content length";
        http.end();
        return false;
    }
    if (!Update.begin(static_cast<size_t>(length), U_FLASH)) {
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
    while (remaining > 0) {
        if (safetyCheck_ && !safetyCheck_()) {
            ok = false;
            detail = "RSE changed during download; update cancelled before activation";
            break;
        }
        const size_t available = stream->available();
        if (available == 0) {
            if (!http.connected()) { ok = false; break; }
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
    if (!Update.end(true)) {
        detail = "firmware image rejected: " + String(Update.errorString());
        return false;
    }
    reportProgress("INSTALLING", 100);
    detail = "verified; rebooting into " + manifest.version;
    return true;
}

bool OtaManager::installUpdate(String& detail) {
    reportProgress("CHECKING");
    Manifest manifest;
    if (!fetchManifest(manifest, detail)) {
        reportProgress("ERROR");
        return false;
    }
    if (compareVersions(manifest.version, CURRENT_VERSION) <= 0) {
        detail = "already running the latest version " + String(CURRENT_VERSION);
        reportProgress("UP TO DATE", 100);
        return false;
    }
    if (!downloadAndInstall(manifest, detail)) {
        reportProgress("ERROR");
        return false;
    }
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
    nextAutomaticCheckMs_ = now + AUTO_CHECK_INTERVAL_MS;
    String version;
    String detail;
    Serial.println("OTA AUTO STATUS=CHECKING");
    if (!checkForUpdate(version, detail)) {
        Serial.println("OTA AUTO STATUS=ERROR DETAIL=" + detail);
        return;
    }
    if (compareVersions(version, CURRENT_VERSION) <= 0) {
        Serial.println("OTA AUTO STATUS=UP_TO_DATE VERSION=" + version);
        return;
    }
    Serial.println("OTA AUTO STATUS=INSTALLING VERSION=" + version);
    installUpdate(detail);
}
