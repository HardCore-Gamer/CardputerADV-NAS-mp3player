#include <Arduino.h>
#include <M5Cardputer.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <time.h>

#include <AudioFileSource.h>
#include <AudioFileSourceBuffer.h>
#include <AudioFileSourceID3.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutput.h>

#include <algorithm>
#include <vector>

namespace {

constexpr int SCREEN_W = 240;
constexpr int SCREEN_H = 135;
constexpr int HEADER_H = 18;
constexpr int FOOTER_H = 16;
constexpr int ROW_H = 14;
constexpr size_t MAX_LIST_BODY = 96 * 1024;
constexpr size_t MAX_ENTRIES = 120;
constexpr uint32_t RESUME_SAVE_INTERVAL_MS = 3000;
constexpr uint32_t SEEK_STEP_MIN_BYTES = 96 * 1024;
constexpr uint32_t POWER_SAVE_IDLE_MS = 15000;
constexpr uint32_t POWER_SAVE_SCREEN_OFF_MS = 35000;
constexpr uint32_t BAD_PLAYBACK_TIME_MS = 2000;
constexpr uint32_t BAD_PLAYBACK_POS_BYTES = 32 * 1024;
constexpr uint8_t DISPLAY_BRIGHTNESS_ACTIVE = 128;
constexpr uint8_t DISPLAY_BRIGHTNESS_DIM = 18;
constexpr uint8_t DISPLAY_BRIGHTNESS_OFF = 0;

constexpr uint16_t C_BG = 0x0000;
constexpr uint16_t C_PANEL = 0x18C3;
constexpr uint16_t C_PANEL_ALT = 0x2124;
constexpr uint16_t C_TEXT = 0xFFFF;
constexpr uint16_t C_DIM = 0xBDF7;
constexpr uint16_t C_ACCENT = 0x07FF;
constexpr uint16_t C_SELECT = 0x03EF;
constexpr uint16_t C_WARN = 0xFBE0;
constexpr uint16_t C_ERR = 0xF800;
constexpr uint16_t C_SOFT = 0x39C7;
constexpr uint16_t C_GOOD = 0x87F0;
constexpr uint16_t C_EDGE = 0x31A6;
constexpr uint16_t C_TRACK = 0x10A2;

struct ParsedUrl {
    bool ok = false;
    String scheme = "http";
    String user;
    String pass;
    String host;
    uint16_t port = 80;
    String path = "/";
};

struct FileEntry {
    String name;
    String url;
    bool dir = false;
    bool parent = false;
};

struct WifiItem {
    String ssid;
    int32_t rssi = 0;
    uint8_t enc = WIFI_AUTH_OPEN;
    bool saved = false;
    bool manual = false;
};

struct KeyEvent {
    bool pressed = false;
    bool enter = false;
    bool del = false;
    bool tab = false;
    bool space = false;
    bool fn = false;
    std::vector<char> chars;
};

enum class Screen {
    WifiList,
    TextInput,
    FileList,
    Player,
    TimerMenu,
    Message,
};

enum class InputMode {
    None,
    WifiPassword,
    ManualSsid,
    NasUrl,
    FileSearch,
};

Preferences prefs;
Screen screen = Screen::Message;
Screen returnAfterMessage = Screen::WifiList;
Screen wifiReturnScreen = Screen::Message;
InputMode inputMode = InputMode::None;

std::vector<WifiItem> wifiItems;
std::vector<FileEntry> allEntries;
std::vector<FileEntry> entries;

String savedSsid;
String savedPass;
String savedNas;
String savedResumeUrl;
String pendingSsid;
String inputText;
String inputTitle;
String fileSearchQuery;
bool inputSecret = false;

String currentUrl;
String currentTrackName;
String currentTrackUrl;
int selected = 0;
int scrollTop = 0;
int wifiSelected = 0;
int wifiScrollTop = 0;
int timerSelected = 0;
int volume = 180;
uint32_t savedResumePos = 0;
bool powerSaveEnabled = true;
bool playerDimmed = false;
bool playerScreenOff = false;
uint32_t lastUserActionAt = 0;

uint32_t messageUntil = 0;
String messageTitle;
String messageBody;
uint32_t shutdownAt = 0;
uint32_t lastPlayerRedraw = 0;
uint32_t lastResumeSaveAt = 0;
uint32_t playbackStartedAt = 0;
bool needsRedraw = true;
bool showHelpOverlay = false;
bool bootAutoStartPending = false;
bool skipPlaybackLoopOnce = false;
esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;
bool timeSyncRequested = false;

int lastHeaderWifiBars = -2;
bool lastHeaderWifiConnected = false;
int lastHeaderBattery = -2;
int lastHeaderCharging = -2;
String lastHeaderClockShown;
uint32_t lastPlayerPosShown = UINT32_MAX;
uint32_t lastPlayerSizeShown = UINT32_MAX;
int lastPlayerVolumeShown = -1;
String lastPlayerTimerShown;
String lastPlayerStateShown;

AudioGeneratorMP3 *mp3 = nullptr;
AudioFileSource *httpSource = nullptr;
AudioFileSourceBuffer *buffSource = nullptr;
AudioFileSourceID3 *id3Source = nullptr;

String normalizeNasUrl(const String &raw);
bool loadDirectory(const String &url);
void startPlayback(int index, uint32_t offset = 0, bool fallbackToZero = true);
void stopPlayback(bool keepResume = true);
void drawFileList();
void drawPlayer();
void drawPlayerDynamic(bool force = false);
bool tryAutoStart();
void wakePlayerDisplay();
void markUserActivity();
void applyFileSearch();
void drawHelpOverlay();
void scanWifi();
bool isAbnormalPlaybackReset(esp_reset_reason_t reason);
void drawBusyScreen(const String &title, const String &body);
void prepareSingleTrackContext(const String &url);
bool ensurePlaybackFolderLoaded(const String &failNextScreenText);
void syncClockIfNeeded();
String headerClockText();

String toLowerCopy(String value) {
    value.toLowerCase();
    return value;
}

void removeLastUtf8(String &value) {
    if (value.isEmpty()) {
        return;
    }
    int i = value.length() - 1;
    while (i > 0 && ((static_cast<uint8_t>(value[i]) & 0xC0) == 0x80)) {
        --i;
    }
    value.remove(i);
}

String xmlHtmlDecode(String value) {
    value.replace("&amp;", "&");
    value.replace("&lt;", "<");
    value.replace("&gt;", ">");
    value.replace("&quot;", "\"");
    value.replace("&#39;", "'");
    value.replace("&apos;", "'");
    return value;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

String percentDecode(const String &value) {
    String out;
    out.reserve(value.length());
    for (int i = 0; i < value.length(); ++i) {
        char c = value[i];
        if (c == '%' && i + 2 < value.length()) {
            int hi = hexValue(value[i + 1]);
            int lo = hexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (c == '+') {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

String trimCopy(String value) {
    value.trim();
    return value;
}

bool containsIgnoreCase(const String &text, const String &needle) {
    if (needle.isEmpty()) {
        return true;
    }
    String hay = toLowerCopy(text);
    String ndl = toLowerCopy(needle);
    return hay.indexOf(ndl) >= 0;
}

ParsedUrl parseUrl(String raw) {
    ParsedUrl out;
    raw = trimCopy(raw);
    if (raw.isEmpty()) {
        return out;
    }
    if (raw.indexOf("://") < 0) {
        raw = "http://" + raw;
    }

    int schemeEnd = raw.indexOf("://");
    if (schemeEnd <= 0) {
        return out;
    }
    out.scheme = raw.substring(0, schemeEnd);
    out.scheme.toLowerCase();
    if (out.scheme != "http") {
        return out;
    }

    int authorityStart = schemeEnd + 3;
    int pathStart = raw.indexOf('/', authorityStart);
    String authority = pathStart >= 0 ? raw.substring(authorityStart, pathStart) : raw.substring(authorityStart);
    out.path = pathStart >= 0 ? raw.substring(pathStart) : "/";
    if (out.path.isEmpty()) {
        out.path = "/";
    }

    int at = authority.lastIndexOf('@');
    if (at >= 0) {
        String userInfo = authority.substring(0, at);
        authority = authority.substring(at + 1);
        int colon = userInfo.indexOf(':');
        if (colon >= 0) {
            out.user = percentDecode(userInfo.substring(0, colon));
            out.pass = percentDecode(userInfo.substring(colon + 1));
        } else {
            out.user = percentDecode(userInfo);
        }
    }

    int portSep = authority.lastIndexOf(':');
    if (portSep > 0) {
        out.host = authority.substring(0, portSep);
        int parsedPort = authority.substring(portSep + 1).toInt();
        if (parsedPort > 0 && parsedPort <= 65535) {
            out.port = static_cast<uint16_t>(parsedPort);
        }
    } else {
        out.host = authority;
    }

    out.host.trim();
    out.ok = !out.host.isEmpty();
    return out;
}

String buildOrigin(const ParsedUrl &url, bool includeAuth) {
    String out = url.scheme + "://";
    if (includeAuth && !url.user.isEmpty()) {
        out += url.user;
        if (!url.pass.isEmpty()) {
            out += ":";
            out += url.pass;
        }
        out += "@";
    }
    out += url.host;
    if (url.port != 80) {
        out += ":";
        out += String(url.port);
    }
    return out;
}

String ensureDirectoryUrl(String url) {
    String low = toLowerCopy(url);
    int q = low.indexOf('?');
    String pathOnly = q >= 0 ? low.substring(0, q) : low;
    if (!pathOnly.endsWith(".mp3") && !url.endsWith("/")) {
        url += "/";
    }
    return url;
}

String normalizeNasUrl(const String &raw) {
    String url = trimCopy(raw);
    if (url.indexOf("://") < 0) {
        url = "http://" + url;
    }
    ParsedUrl parsed = parseUrl(url);
    if (!parsed.ok) {
        return url;
    }
    url = buildOrigin(parsed, true) + parsed.path;
    return ensureDirectoryUrl(url);
}

String joinUrl(const String &baseUrl, String href) {
    href = xmlHtmlDecode(trimCopy(href));
    if (href.startsWith("//")) {
        return "http:" + href;
    }
    if (href.startsWith("http://") || href.startsWith("https://")) {
        ParsedUrl base = parseUrl(baseUrl);
        ParsedUrl target = parseUrl(href);
        if (base.ok && target.ok && !base.user.isEmpty() && base.host == target.host && base.port == target.port) {
            return buildOrigin(base, true) + target.path;
        }
        return href;
    }

    ParsedUrl base = parseUrl(baseUrl);
    if (!base.ok) {
        return href;
    }

    String path;
    if (href.startsWith("/")) {
        path = href;
    } else {
        path = base.path;
        int query = path.indexOf('?');
        if (query >= 0) {
            path.remove(query);
        }
        if (!path.endsWith("/")) {
            int slash = path.lastIndexOf('/');
            path = slash >= 0 ? path.substring(0, slash + 1) : "/";
        }
        path += href;
    }
    return buildOrigin(base, true) + path;
}

String fileNameFromUrl(const String &url) {
    ParsedUrl parsed = parseUrl(url);
    String path = parsed.ok ? parsed.path : url;
    int query = path.indexOf('?');
    if (query >= 0) {
        path.remove(query);
    }
    if (path.endsWith("/") && path.length() > 1) {
        path.remove(path.length() - 1);
    }
    int slash = path.lastIndexOf('/');
    String name = slash >= 0 ? path.substring(slash + 1) : path;
    name = percentDecode(xmlHtmlDecode(name));
    return name.isEmpty() ? url : name;
}

String parentUrlOf(const String &url) {
    ParsedUrl parsed = parseUrl(url);
    if (!parsed.ok) {
        return url;
    }
    String path = parsed.path;
    int query = path.indexOf('?');
    if (query >= 0) {
        path.remove(query);
    }
    if (path.length() <= 1) {
        return buildOrigin(parsed, true) + "/";
    }
    if (path.endsWith("/")) {
        path.remove(path.length() - 1);
    }
    int slash = path.lastIndexOf('/');
    path = slash <= 0 ? "/" : path.substring(0, slash + 1);
    return buildOrigin(parsed, true) + path;
}

bool samePathUrl(const String &a, const String &b) {
    ParsedUrl pa = parseUrl(a);
    ParsedUrl pb = parseUrl(b);
    if (!pa.ok || !pb.ok) {
        return a == b;
    }
    String ap = pa.path;
    String bp = pb.path;
    int aq = ap.indexOf('?');
    int bq = bp.indexOf('?');
    if (aq >= 0) {
        ap.remove(aq);
    }
    if (bq >= 0) {
        bp.remove(bq);
    }
    if (!ap.endsWith("/")) {
        ap += "/";
    }
    if (!bp.endsWith("/")) {
        bp += "/";
    }
    return pa.host == pb.host && pa.port == pb.port && ap == bp;
}

bool pathIsMp3(const String &urlOrPath) {
    String low = toLowerCopy(urlOrPath);
    int hash = low.indexOf('#');
    if (hash >= 0) {
        low.remove(hash);
    }
    int query = low.indexOf('?');
    if (query >= 0) {
        low.remove(query);
    }
    return low.endsWith(".mp3");
}

String displayFit(String value, int width) {
    if (M5Cardputer.Display.textWidth(value) <= width) {
        return value;
    }
    while (!value.isEmpty() && M5Cardputer.Display.textWidth(value + "...") > width) {
        removeLastUtf8(value);
    }
    return value + "...";
}

String tailFit(String value, int width) {
    if (M5Cardputer.Display.textWidth(value) <= width) {
        return value;
    }
    while (!value.isEmpty() && M5Cardputer.Display.textWidth("..." + value) > width) {
        int i = 0;
        if (value.length() > 1) {
            i = 1;
            while (i < value.length() && ((static_cast<uint8_t>(value[i]) & 0xC0) == 0x80)) {
                ++i;
            }
        }
        value.remove(0, i);
    }
    return "..." + value;
}

uint32_t currentPlaybackPos() {
    return id3Source ? id3Source->getPos() : 0;
}

uint32_t currentPlaybackSize() {
    return id3Source ? id3Source->getSize() : 0;
}

void saveResumeState(const String &url, uint32_t pos) {
    savedResumeUrl = url;
    savedResumePos = pos;
    prefs.putString("resume_url", savedResumeUrl);
    prefs.putUInt("resume_pos", savedResumePos);
}

void clearResumeState() {
    saveResumeState("", 0);
}

void persistPlaybackState(bool force) {
    if (currentTrackUrl.isEmpty()) {
        return;
    }
    uint32_t now = millis();
    if (!force && now - lastResumeSaveAt < RESUME_SAVE_INTERVAL_MS) {
        return;
    }
    saveResumeState(currentTrackUrl, currentPlaybackPos());
    lastResumeSaveAt = now;
}

int findEntryIndexByUrl(const String &url) {
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].url == url) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

uint32_t clampResumeOffset(uint32_t offset, uint32_t size) {
    if (size == 0) {
        return offset;
    }
    if (offset >= size) {
        return 0;
    }
    return offset;
}

uint32_t computeSeekTarget(bool forward) {
    uint32_t size = currentPlaybackSize();
    uint32_t pos = currentPlaybackPos();
    uint32_t step = size > 0 ? std::max<uint32_t>(size / 20, SEEK_STEP_MIN_BYTES) : SEEK_STEP_MIN_BYTES;
    if (forward) {
        if (size > 0) {
            return std::min(pos + step, size > 1 ? size - 1 : 0U);
        }
        return pos + step;
    }
    return pos > step ? pos - step : 0;
}

bool charInEvent(const KeyEvent &key, char wanted) {
    char w = static_cast<char>(tolower(static_cast<unsigned char>(wanted)));
    for (char c : key.chars) {
        if (tolower(static_cast<unsigned char>(c)) == w) {
            return true;
        }
    }
    return false;
}

int getWifiSignalBars() {
    if (WiFi.status() != WL_CONNECTED) {
        return 0;
    }
    long rssi = WiFi.RSSI();
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

int getBatteryPercent() {
    int level = M5Cardputer.Power.getBatteryLevel();
    if (level < 0) {
        return 0;
    }
    if (level > 100) {
        return 100;
    }
    return level;
}

bool isBatteryCharging() {
    return M5Cardputer.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
}

void syncClockIfNeeded() {
    if (timeSyncRequested || WiFi.status() != WL_CONNECTED) {
        return;
    }
    configTzTime("CST-8", "pool.ntp.org", "time.nist.gov", "time.google.com");
    timeSyncRequested = true;
}

String headerClockText() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 0)) {
        return "--:--";
    }
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    return String(buf);
}

bool isAbnormalPlaybackReset(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            return true;
        default:
            return false;
    }
}

void wakePlayerDisplay() {
    if (playerDimmed || playerScreenOff) {
        M5Cardputer.Display.setBrightness(DISPLAY_BRIGHTNESS_ACTIVE);
        playerDimmed = false;
        playerScreenOff = false;
        needsRedraw = true;
    }
}

void markUserActivity() {
    lastUserActionAt = millis();
    wakePlayerDisplay();
}

KeyEvent readKeys() {
    KeyEvent out;
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) {
        return out;
    }

    auto status = M5Cardputer.Keyboard.keysState();
    out.pressed = true;
    out.enter = status.enter;
    out.del = status.del;
    out.tab = status.tab;
    out.fn = status.fn;

    for (char c : status.word) {
        if (c == ' ') {
            out.space = true;
        }
        if (c >= 32 && c <= 126) {
            out.chars.push_back(c);
        }
    }
    return out;
}

void drawHeader(const String &title) {
    int statusX = SCREEN_W - 102;
    M5Cardputer.Display.fillRect(0, 0, SCREEN_W, HEADER_H, C_BG);
    M5Cardputer.Display.fillRect(0, HEADER_H - 1, SCREEN_W, 1, C_EDGE);
    M5Cardputer.Display.fillRoundRect(4, 3, 4, 12, 2, C_ACCENT);
    M5Cardputer.Display.setTextColor(C_TEXT, C_BG);
    M5Cardputer.Display.setCursor(12, 3);
    M5Cardputer.Display.print(displayFit(title, statusX - 8));

    bool wifiConnected = WiFi.status() == WL_CONNECTED;
    int wifiBars = getWifiSignalBars();
    int battery = getBatteryPercent();
    bool charging = isBatteryCharging();
    String clock = headerClockText();

    M5Cardputer.Display.fillRoundRect(statusX - 2, 2, SCREEN_W - statusX + 2, 14, 6, C_PANEL_ALT);
    M5Cardputer.Display.drawRoundRect(statusX - 2, 2, SCREEN_W - statusX + 2, 14, 6, C_EDGE);

    M5Cardputer.Display.setTextColor(C_TEXT, C_PANEL_ALT);
    M5Cardputer.Display.setCursor(statusX + 4, 3);
    M5Cardputer.Display.print(clock);

    int wifiX = SCREEN_W - 54;
    int wifiY = 3;
    uint16_t wifiColor = wifiConnected ? C_GOOD : C_SOFT;
    for (int i = 0; i < 4; ++i) {
        int barH = 3 + i * 2;
        int x = wifiX + i * 4;
        int y = wifiY + (9 - barH);
        M5Cardputer.Display.fillRect(x, y, 3, barH, i < wifiBars ? wifiColor : C_SOFT);
    }
    if (!wifiConnected) {
        M5Cardputer.Display.drawLine(wifiX - 1, 12, wifiX + 14, 2, C_ERR);
    }

    int batX = SCREEN_W - 26;
    int batY = 3;
    int batW = 18;
    int batH = 9;
    M5Cardputer.Display.drawRect(batX, batY, batW, batH, C_DIM);
    M5Cardputer.Display.fillRect(batX + batW, batY + 2, 2, batH - 4, C_DIM);
    int fillW = ((batW - 3) * battery) / 100;
    uint16_t batColor = battery > 25 ? (charging ? C_GOOD : C_TEXT) : C_WARN;
    if (battery <= 10) {
        batColor = C_ERR;
    }
    if (fillW > 0) {
        M5Cardputer.Display.fillRect(batX + 2, batY + 2, fillW, batH - 3, batColor);
    }
    if (charging) {
        M5Cardputer.Display.drawLine(batX + 8, batY + 1, batX + 6, batY + 5, C_GOOD);
        M5Cardputer.Display.drawLine(batX + 6, batY + 5, batX + 10, batY + 5, C_GOOD);
        M5Cardputer.Display.drawLine(batX + 10, batY + 5, batX + 8, batY + 8, C_GOOD);
    }

    lastHeaderWifiBars = wifiBars;
    lastHeaderWifiConnected = wifiConnected;
    lastHeaderBattery = battery;
    lastHeaderCharging = charging ? 1 : 0;
    lastHeaderClockShown = clock;
}

void drawFooter(const String &text) {
    M5Cardputer.Display.fillRect(0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H, C_BG);
    M5Cardputer.Display.fillRect(0, SCREEN_H - FOOTER_H, SCREEN_W, 1, C_EDGE);
    M5Cardputer.Display.setTextColor(C_DIM, C_BG);
    M5Cardputer.Display.setCursor(4, SCREEN_H - FOOTER_H + 3);
    M5Cardputer.Display.print(displayFit(text, SCREEN_W - 8));
}

void drawChip(int x, int y, int w, const String &label, uint16_t border, uint16_t fg) {
    M5Cardputer.Display.fillRoundRect(x, y, w, 14, 5, C_PANEL_ALT);
    M5Cardputer.Display.drawRoundRect(x, y, w, 14, 5, border);
    M5Cardputer.Display.setTextColor(fg, C_PANEL_ALT);
    M5Cardputer.Display.setCursor(x + 5, y + 3);
    M5Cardputer.Display.print(displayFit(label, w - 10));
}

void fillPanel(int x, int y, int w, int h, uint16_t fill = C_PANEL, uint16_t border = C_EDGE, int radius = 8) {
    M5Cardputer.Display.fillRoundRect(x, y, w, h, radius, fill);
    M5Cardputer.Display.drawRoundRect(x, y, w, h, radius, border);
}

void drawMeter(int x, int y, int w, int h, int fillW, uint16_t fillColor) {
    fillPanel(x, y, w, h, C_TRACK, C_EDGE, h / 2);
    int innerW = std::max(0, std::min(fillW, w - 4));
    if (innerW > 0) {
        M5Cardputer.Display.fillRoundRect(x + 2, y + 2, innerW, h - 4, (h - 4) / 2, fillColor);
    }
}

void drawSignalGlyph(int x, int y, int bars, uint16_t active, uint16_t inactive) {
    for (int i = 0; i < 4; ++i) {
        int barH = 3 + i * 2;
        int barX = x + i * 4;
        int barY = y + (9 - barH);
        M5Cardputer.Display.fillRect(barX, barY, 3, barH, i < bars ? active : inactive);
    }
}

void showMessage(const String &title, const String &body, uint32_t ms, Screen next) {
    messageTitle = title;
    messageBody = body;
    messageUntil = millis() + ms;
    returnAfterMessage = next;
    screen = Screen::Message;
    needsRedraw = true;
}

void drawBusyScreen(const String &title, const String &body) {
    M5Cardputer.Display.fillScreen(C_BG);
    drawHeader(title);
    fillPanel(14, 34, SCREEN_W - 28, 54, C_PANEL, C_EDGE, 10);
    M5Cardputer.Display.fillRect(24, 44, 32, 3, C_ACCENT);
    M5Cardputer.Display.setTextColor(C_TEXT, C_PANEL);
    M5Cardputer.Display.setCursor(24, 56);
    M5Cardputer.Display.print(displayFit(body, SCREEN_W - 52));
    drawFooter("wait");
}

void prepareSingleTrackContext(const String &url) {
    currentUrl = parentUrlOf(url);
    entries.clear();
    allEntries.clear();
    selected = 0;
    scrollTop = 0;
    FileEntry entry;
    entry.name = fileNameFromUrl(url);
    entry.url = url;
    entry.dir = false;
    entries.push_back(entry);
    allEntries = entries;
}

void drawMessage() {
    M5Cardputer.Display.fillScreen(C_BG);
    drawHeader(messageTitle);
    fillPanel(14, 34, SCREEN_W - 28, 54, C_PANEL, C_EDGE, 10);
    M5Cardputer.Display.fillRect(24, 44, 32, 3, C_ACCENT);
    M5Cardputer.Display.setTextColor(C_TEXT, C_PANEL);
    M5Cardputer.Display.setCursor(24, 56);
    M5Cardputer.Display.print(displayFit(messageBody, SCREEN_W - 52));
    drawFooter("wait");
    needsRedraw = false;
}

void beginInput(InputMode mode, const String &title, const String &seed, bool secret) {
    wakePlayerDisplay();
    showHelpOverlay = false;
    inputMode = mode;
    inputTitle = title;
    inputText = seed;
    inputSecret = secret;
    screen = Screen::TextInput;
    needsRedraw = true;
}

void drawInput() {
    M5Cardputer.Display.fillScreen(C_BG);
    drawHeader(inputTitle);
    fillPanel(8, 24, SCREEN_W - 16, 94, C_PANEL, C_EDGE, 10);
    M5Cardputer.Display.fillRect(18, 32, 48, 3, C_ACCENT);
    M5Cardputer.Display.setTextColor(C_DIM, C_PANEL);
    M5Cardputer.Display.setCursor(18, 42);
    if (inputMode == InputMode::NasUrl) {
        M5Cardputer.Display.print("HTTP/WebDAV URL");
    } else if (inputMode == InputMode::WifiPassword) {
        M5Cardputer.Display.print(pendingSsid);
    } else if (inputMode == InputMode::FileSearch) {
        M5Cardputer.Display.print("Filter current folder");
    } else {
        M5Cardputer.Display.print("Manual SSID");
    }

    String shown = inputText;
    if (inputSecret) {
        shown = "";
        for (int i = 0; i < inputText.length(); ++i) {
            shown += '*';
        }
    }
    fillPanel(18, 56, SCREEN_W - 36, 24, C_BG, C_SOFT, 6);
    M5Cardputer.Display.setTextColor(C_TEXT, C_BG);
    M5Cardputer.Display.setCursor(24, 62);
    M5Cardputer.Display.print(tailFit(shown, SCREEN_W - 56));

    M5Cardputer.Display.setTextColor(C_DIM, C_PANEL);
    M5Cardputer.Display.setCursor(18, 91);
    if (inputMode == InputMode::NasUrl) {
        M5Cardputer.Display.print("Example: http://nas:5005/music/");
    } else if (inputMode == InputMode::FileSearch) {
        M5Cardputer.Display.print("Enter apply   Tab clear");
    } else {
        M5Cardputer.Display.print("Saved after successful connect");
    }
    drawFooter("Enter confirm   Tab back");
    needsRedraw = false;
}

class AudioOutputM5Speaker : public AudioOutput {
public:
    explicit AudioOutputM5Speaker(m5::Speaker_Class *speaker, uint8_t channel = 0)
        : speaker_(speaker), channel_(channel) {
    }

    bool begin() override {
        return true;
    }

    bool ConsumeSample(int16_t sample[2]) override {
        if (bufPos_ + 1 >= BUF_SAMPLES) {
            flush();
            return false;
        }
        triBuf_[bufIndex_][bufPos_++] = sample[0];
        triBuf_[bufIndex_][bufPos_++] = sample[1];
        return true;
    }

    bool stop() override {
        flush();
        return true;
    }

    bool SetRate(int hz) override {
        sampleRate_ = hz;
        return true;
    }

    bool SetBitsPerSample(int bits) override {
        return bits == 16;
    }

    bool SetChannels(int channels) override {
        return channels == 2;
    }

    bool SetGain(float f) override {
        gain_ = f;
        return true;
    }

    void flush() override {
        if (bufPos_ == 0) {
            return;
        }
        speaker_->playRaw(triBuf_[bufIndex_], bufPos_, sampleRate_, true, 1, channel_);
        bufIndex_ = (bufIndex_ + 1) % 3;
        bufPos_ = 0;
    }

private:
    static constexpr size_t BUF_SAMPLES = 1536;
    m5::Speaker_Class *speaker_;
    uint8_t channel_;
    int16_t triBuf_[3][BUF_SAMPLES] = {};
    size_t bufPos_ = 0;
    uint8_t bufIndex_ = 0;
    int sampleRate_ = 44100;
    float gain_ = 1.0f;
};

AudioOutputM5Speaker out(&M5Cardputer.Speaker);

class AudioFileSourceHTTPBasic : public AudioFileSource {
public:
    AudioFileSourceHTTPBasic() = default;
    ~AudioFileSourceHTTPBasic() override {
        close();
    }

    bool open(const char *filename) override {
        return openAt(filename, 0);
    }

    bool openAt(const char *filename, uint32_t offset) {
        close();
        ParsedUrl parsed = parseUrl(String(filename));
        if (!parsed.ok) {
            return false;
        }
        requestUrl_ = filename;
        cleanUrl_ = buildOrigin(parsed, false) + parsed.path;
        client_.setTimeout(7000);
        if (!http_.begin(client_, cleanUrl_)) {
            return false;
        }
#ifdef HTTPC_FORCE_FOLLOW_REDIRECTS
        http_.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
#endif
        http_.setTimeout(12000);
        http_.setReuse(false);
        http_.useHTTP10(true);
        http_.addHeader("User-Agent", "CardputerAdvMP3/1.0");
        const char *headerKeys[] = {"Content-Range"};
        http_.collectHeaders(headerKeys, 1);
        if (!parsed.user.isEmpty()) {
            http_.setAuthorization(parsed.user.c_str(), parsed.pass.c_str());
        }
        if (offset > 0) {
            http_.addHeader("Range", "bytes=" + String(offset) + "-");
        }
        int code = http_.GET();
        if (code != HTTP_CODE_OK && code != HTTP_CODE_PARTIAL_CONTENT) {
            http_.end();
            return false;
        }
        if (offset > 0 && code != HTTP_CODE_PARTIAL_CONTENT) {
            http_.end();
            return false;
        }
        stream_ = http_.getStreamPtr();
        startOffset_ = code == HTTP_CODE_PARTIAL_CONTENT ? offset : 0;
        size_ = http_.getSize();
        if (code == HTTP_CODE_PARTIAL_CONTENT) {
            String contentRange = http_.header("Content-Range");
            int slash = contentRange.lastIndexOf('/');
            if (slash >= 0) {
                int total = contentRange.substring(slash + 1).toInt();
                if (total > 0) {
                    size_ = total;
                } else if (size_ > 0) {
                    size_ += static_cast<int>(startOffset_);
                }
            } else if (size_ > 0) {
                size_ += static_cast<int>(startOffset_);
            }
        }
        pos_ = startOffset_;
        opened_ = true;
        return true;
    }

    uint32_t read(void *data, uint32_t len) override {
        return readInternal(data, len, false);
    }

    uint32_t readNonBlock(void *data, uint32_t len) override {
        return readInternal(data, len, true);
    }

    bool seek(int32_t pos, int dir) override {
        uint32_t target = 0;
        if (dir == SEEK_CUR) {
            int64_t candidate = static_cast<int64_t>(pos_) + pos;
            if (candidate < 0) {
                candidate = 0;
            }
            target = static_cast<uint32_t>(candidate);
        } else if (dir == SEEK_END) {
            if (size_ <= 0) {
                return false;
            }
            int64_t candidate = static_cast<int64_t>(size_) + pos;
            if (candidate < 0) {
                candidate = 0;
            }
            target = static_cast<uint32_t>(candidate);
        } else {
            if (pos < 0) {
                return false;
            }
            target = static_cast<uint32_t>(pos);
        }
        if (size_ > 0 && target >= static_cast<uint32_t>(size_)) {
            target = static_cast<uint32_t>(size_ - 1);
        }
        if (requestUrl_.isEmpty()) {
            return false;
        }
        return openAt(requestUrl_.c_str(), target);
    }

    bool close() override {
        if (opened_) {
            http_.end();
        }
        stream_ = nullptr;
        opened_ = false;
        size_ = -1;
        pos_ = 0;
        startOffset_ = 0;
        return true;
    }

    bool isOpen() override {
        return opened_;
    }

    uint32_t getSize() override {
        return size_ > 0 ? static_cast<uint32_t>(size_) : 0;
    }

    uint32_t getPos() override {
        return pos_;
    }

private:
    uint32_t readInternal(void *data, uint32_t len, bool nonBlock) {
        if (!opened_ || stream_ == nullptr || data == nullptr || len == 0) {
            return 0;
        }
        if (size_ > 0 && pos_ >= static_cast<uint32_t>(size_)) {
            return 0;
        }
        if (size_ > 0 && len > static_cast<uint32_t>(size_) - pos_) {
            len = static_cast<uint32_t>(size_) - pos_;
        }

        uint32_t start = millis();
        while (stream_->available() <= 0) {
            if (!http_.connected()) {
                return 0;
            }
            if (nonBlock || millis() - start > 1000) {
                return 0;
            }
            delay(1);
        }
        int avail = stream_->available();
        if (avail <= 0) {
            return 0;
        }
        if (len > static_cast<uint32_t>(avail)) {
            len = static_cast<uint32_t>(avail);
        }
        int got = stream_->readBytes(static_cast<uint8_t *>(data), len);
        if (got > 0) {
            pos_ += got;
            return got;
        }
        return 0;
    }

    WiFiClient client_;
    HTTPClient http_;
    Client *stream_ = nullptr;
    String requestUrl_;
    String cleanUrl_;
    int size_ = -1;
    uint32_t pos_ = 0;
    uint32_t startOffset_ = 0;
    bool opened_ = false;
};

bool beginHttp(HTTPClient &http, WiFiClient &client, const String &url) {
    ParsedUrl parsed = parseUrl(url);
    if (!parsed.ok) {
        return false;
    }
    client.setTimeout(10000);
    String clean = buildOrigin(parsed, false) + parsed.path;
    if (!http.begin(client, clean)) {
        return false;
    }
#ifdef HTTPC_FORCE_FOLLOW_REDIRECTS
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
#endif
    http.setTimeout(15000);
    http.setReuse(false);
    http.addHeader("User-Agent", "CardputerAdvMP3/1.0");
    if (!parsed.user.isEmpty()) {
        http.setAuthorization(parsed.user.c_str(), parsed.pass.c_str());
    }
    return true;
}

String readHttpBody(HTTPClient &http, size_t maxBytes) {
    String body;
    int total = http.getSize();
    if (total > 0) {
        body.reserve(std::min(static_cast<size_t>(total), maxBytes) + 1);
    }
    Client *stream = http.getStreamPtr();
    if (!stream) {
        return body;
    }

    char tmp[513];
    uint32_t lastData = millis();
    while (http.connected() && body.length() < maxBytes) {
        size_t avail = stream->available();
        if (avail > 0) {
            size_t want = std::min(avail, sizeof(tmp) - 1);
            want = std::min(want, maxBytes - static_cast<size_t>(body.length()));
            int got = stream->readBytes(reinterpret_cast<uint8_t *>(tmp), want);
            if (got > 0) {
                tmp[got] = '\0';
                body += tmp;
                lastData = millis();
            }
        } else {
            if (total >= 0 && body.length() >= total) {
                break;
            }
            if (millis() - lastData > 12000) {
                break;
            }
            delay(1);
        }
        yield();
    }
    return body;
}

void addEntryIfUseful(const String &baseUrl, String href, bool forceDir) {
    href = xmlHtmlDecode(trimCopy(href));
    if (href.isEmpty()) {
        return;
    }
    String lowHref = toLowerCopy(href);
    if (lowHref.startsWith("#") || lowHref.startsWith("?") || lowHref.startsWith("javascript:") ||
        lowHref.startsWith("mailto:") || href == "../" || href == "..") {
        return;
    }

    String joined = joinUrl(baseUrl, href);
    bool dir = forceDir || joined.endsWith("/");
    bool mp3File = pathIsMp3(joined);
    if (!dir && !mp3File) {
        return;
    }
    if (dir && samePathUrl(joined, baseUrl)) {
        return;
    }
    if (entries.size() >= MAX_ENTRIES) {
        return;
    }

    for (const auto &entry : entries) {
        if (entry.url == joined) {
            return;
        }
    }

    FileEntry entry;
    entry.url = dir ? ensureDirectoryUrl(joined) : joined;
    entry.dir = dir;
    entry.name = fileNameFromUrl(entry.url);
    if (entry.name.isEmpty()) {
        entry.name = dir ? "[folder]" : "[mp3]";
    }
    entries.push_back(entry);
}

void parseHtmlListing(const String &baseUrl, const String &body) {
    String lower = toLowerCopy(body);
    int pos = 0;
    while (pos >= 0 && entries.size() < MAX_ENTRIES) {
        int href = lower.indexOf("href", pos);
        if (href < 0) {
            break;
        }
        int eq = lower.indexOf('=', href + 4);
        if (eq < 0) {
            break;
        }
        int begin = eq + 1;
        while (begin < body.length() && isspace(static_cast<unsigned char>(body[begin]))) {
            ++begin;
        }
        if (begin >= body.length()) {
            break;
        }
        char quote = body[begin];
        int end = -1;
        if (quote == '"' || quote == '\'') {
            ++begin;
            end = body.indexOf(quote, begin);
        } else {
            end = begin;
            while (end < body.length() && !isspace(static_cast<unsigned char>(body[end])) && body[end] != '>') {
                ++end;
            }
        }
        if (end > begin) {
            addEntryIfUseful(baseUrl, body.substring(begin, end), false);
        }
        pos = end > href ? end + 1 : href + 4;
    }
}

String extractHrefTag(const String &body, const String &lower, int tagStart, int &tagEndOut) {
    int gt = lower.indexOf('>', tagStart);
    if (gt < 0) {
        tagEndOut = tagStart + 1;
        return "";
    }
    int close = lower.indexOf("</", gt + 1);
    if (close < 0) {
        tagEndOut = gt + 1;
        return "";
    }
    tagEndOut = close + 2;
    return body.substring(gt + 1, close);
}

void parseWebDavListing(const String &baseUrl, const String &body) {
    String lower = toLowerCopy(body);
    int pos = 0;
    while (entries.size() < MAX_ENTRIES) {
        int tag = lower.indexOf("href", pos);
        if (tag < 0) {
            break;
        }
        int lt = lower.lastIndexOf('<', tag);
        int gt = lower.indexOf('>', tag);
        if (lt < 0 || gt < 0 || lt > tag) {
            pos = tag + 4;
            continue;
        }
        String tagName = lower.substring(lt + 1, gt);
        tagName.trim();
        if (!tagName.endsWith("href")) {
            pos = tag + 4;
            continue;
        }
        int afterHref = 0;
        String href = extractHrefTag(body, lower, lt, afterHref);
        int nextHref = lower.indexOf("href", afterHref);
        int blockEnd = nextHref >= 0 ? nextHref : std::min<int>(afterHref + 700, body.length());
        String block = lower.substring(lt, blockEnd);
        bool isDir = block.indexOf("collection") >= 0 || href.endsWith("/");
        addEntryIfUseful(baseUrl, href, isDir);
        pos = afterHref;
    }
}

void sortEntries() {
    std::sort(entries.begin(), entries.end(), [](const FileEntry &a, const FileEntry &b) {
        if (a.parent != b.parent) {
            return a.parent;
        }
        if (a.dir != b.dir) {
            return a.dir && !b.dir;
        }
        String an = toLowerCopy(a.name);
        String bn = toLowerCopy(b.name);
        return an < bn;
    });
}

bool fetchHttpListing(const String &url) {
    WiFiClient client;
    HTTPClient http;
    if (!beginHttp(http, client, url)) {
        return false;
    }
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
        String body = readHttpBody(http, MAX_LIST_BODY);
        parseHtmlListing(url, body);
        http.end();
        return !entries.empty();
    }
    http.end();
    return false;
}

bool fetchWebDavListing(const String &url) {
    WiFiClient client;
    HTTPClient http;
    if (!beginHttp(http, client, url)) {
        return false;
    }
    http.addHeader("Depth", "1");
    http.addHeader("Content-Type", "application/xml; charset=utf-8");
    const char *payload =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<propfind xmlns=\"DAV:\">"
        "<prop><displayname/><resourcetype/><getcontentlength/><getcontenttype/></prop>"
        "</propfind>";
    int code = http.sendRequest("PROPFIND", reinterpret_cast<uint8_t *>(const_cast<char *>(payload)), strlen(payload));
    if (code == 207 || code == HTTP_CODE_OK) {
        String body = readHttpBody(http, MAX_LIST_BODY);
        parseWebDavListing(url, body);
        http.end();
        return !entries.empty();
    }
    http.end();
    return false;
}

void addParentEntry(const String &url) {
    String parent = parentUrlOf(url);
    if (parent == url || samePathUrl(parent, url)) {
        return;
    }
    FileEntry up;
    up.name = "[..]";
    up.url = parent;
    up.dir = true;
    up.parent = true;
    entries.push_back(up);
}

bool loadDirectory(const String &rawUrl) {
    String url = normalizeNasUrl(rawUrl);
    currentUrl = url;
    prefs.putString("nas", currentUrl);
    selected = 0;
    scrollTop = 0;
    fileSearchQuery = "";
    allEntries.clear();
    entries.clear();

    if (pathIsMp3(url)) {
        FileEntry one;
        one.name = fileNameFromUrl(url);
        one.url = url;
        one.dir = false;
        entries.push_back(one);
        allEntries = entries;
        return true;
    }

    addParentEntry(url);
    bool ok = fetchHttpListing(url);
    if (!ok || entries.size() <= 1) {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const FileEntry &entry) {
                          return !entry.parent;
                      }),
                      entries.end());
        ok = fetchWebDavListing(url);
    }
    sortEntries();
    allEntries = entries;
    applyFileSearch();
    return !entries.empty();
}

void drawWifiList() {
    M5Cardputer.Display.fillScreen(C_BG);
    drawHeader("WiFi");
    fillPanel(8, 24, SCREEN_W - 16, 94, C_PANEL, C_EDGE, 10);
    drawChip(16, 30, 66, savedSsid.isEmpty() ? "No saved" : "Saved", savedSsid.isEmpty() ? C_SOFT : C_GOOD,
             savedSsid.isEmpty() ? C_DIM : C_GOOD);
    drawChip(88, 30, 72, String(wifiItems.size()) + " nets", C_SOFT, C_TEXT);
    drawChip(166, 30, 58, "r scan", C_ACCENT, C_ACCENT);
    M5Cardputer.Display.setTextColor(C_DIM, C_PANEL);
    M5Cardputer.Display.setCursor(16, 48);
    M5Cardputer.Display.print(savedSsid.isEmpty() ? "Select a network" : displayFit(savedSsid, SCREEN_W - 32));

    int visible = 4;
    if (wifiSelected < wifiScrollTop) {
        wifiScrollTop = wifiSelected;
    }
    if (wifiSelected >= wifiScrollTop + visible) {
        wifiScrollTop = wifiSelected - visible + 1;
    }

    for (int row = 0; row < visible; ++row) {
        int idx = wifiScrollTop + row;
        if (idx >= static_cast<int>(wifiItems.size())) {
            break;
        }
        int y = 58 + row * 14;
        bool sel = idx == wifiSelected;
        fillPanel(14, y, SCREEN_W - 28, 12, sel ? C_SELECT : C_PANEL_ALT, sel ? C_SELECT : C_EDGE, 5);
        uint16_t rowBg = sel ? C_SELECT : C_PANEL_ALT;
        uint16_t fg = sel ? C_BG : C_TEXT;
        String label;
        if (wifiItems[idx].manual) {
            label = "Manual SSID";
        } else if (wifiItems[idx].saved) {
            label = wifiItems[idx].ssid;
        } else {
            label = wifiItems[idx].ssid;
        }
        M5Cardputer.Display.setTextColor(fg, rowBg);
        M5Cardputer.Display.setCursor(20, y + 2);
        M5Cardputer.Display.print(displayFit(label, SCREEN_W - 110));
        if (wifiItems[idx].manual) {
            M5Cardputer.Display.setTextColor(sel ? C_BG : C_ACCENT, rowBg);
            M5Cardputer.Display.setCursor(172, y + 2);
            M5Cardputer.Display.print("INPUT");
        } else if (wifiItems[idx].saved) {
            M5Cardputer.Display.setTextColor(sel ? C_BG : C_GOOD, rowBg);
            M5Cardputer.Display.setCursor(172, y + 2);
            M5Cardputer.Display.print("SAVED");
        } else {
            int bars = wifiItems[idx].rssi >= -55 ? 4 : wifiItems[idx].rssi >= -67 ? 3
                      : wifiItems[idx].rssi >= -75 ? 2
                      : wifiItems[idx].rssi >= -85 ? 1
                                                  : 0;
            drawSignalGlyph(188, y + 2, bars, sel ? C_BG : C_ACCENT, sel ? C_SOFT : C_TRACK);
        }
    }
    drawFooter("Enter connect   r rescan   Tab back");
    needsRedraw = false;
}

void scanWifi() {
    wakePlayerDisplay();
    M5Cardputer.Display.fillScreen(C_BG);
    drawHeader("WiFi");
    fillPanel(20, 40, SCREEN_W - 40, 34, C_PANEL, C_EDGE, 10);
    M5Cardputer.Display.setTextColor(C_TEXT, C_PANEL);
    M5Cardputer.Display.setCursor(30, 52);
    M5Cardputer.Display.print("Scanning...");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    delay(100);

    wifiItems.clear();
    if (!savedSsid.isEmpty()) {
        WifiItem item;
        item.ssid = savedSsid;
        item.saved = true;
        item.enc = WIFI_AUTH_WPA2_PSK;
        wifiItems.push_back(item);
    }

    int count = WiFi.scanNetworks(false, true);
    for (int i = 0; i < count; ++i) {
        String ssid = WiFi.SSID(i);
        if (ssid.isEmpty()) {
            continue;
        }
        bool exists = false;
        for (const auto &item : wifiItems) {
            if (item.ssid == ssid && !item.manual) {
                exists = true;
                break;
            }
        }
        if (exists) {
            continue;
        }
        WifiItem item;
        item.ssid = ssid;
        item.rssi = WiFi.RSSI(i);
        item.enc = WiFi.encryptionType(i);
        wifiItems.push_back(item);
    }
    std::sort(wifiItems.begin(), wifiItems.end(), [](const WifiItem &a, const WifiItem &b) {
        if (a.saved != b.saved) {
            return a.saved;
        }
        if (a.manual != b.manual) {
            return !a.manual;
        }
        return a.rssi > b.rssi;
    });

    WifiItem manual;
    manual.manual = true;
    manual.ssid = "Manual SSID";
    wifiItems.push_back(manual);
    wifiSelected = 0;
    wifiScrollTop = 0;
    screen = Screen::WifiList;
    needsRedraw = true;
}

void afterWifiConnected() {
    savedSsid = pendingSsid;
    savedPass = inputText;
    prefs.putString("ssid", savedSsid);
    prefs.putString("pass", savedPass);
    beginInput(InputMode::NasUrl, "NAS address", savedNas.isEmpty() ? "http://" : savedNas, false);
}

bool connectWifi(const String &ssid, const String &pass) {
    wakePlayerDisplay();
    M5Cardputer.Display.fillScreen(C_BG);
    drawHeader("WiFi");
    fillPanel(16, 30, SCREEN_W - 32, 72, C_PANEL, C_EDGE, 10);
    M5Cardputer.Display.setTextColor(C_DIM, C_PANEL);
    M5Cardputer.Display.setCursor(24, 42);
    M5Cardputer.Display.print("Connecting:");
    M5Cardputer.Display.setTextColor(C_TEXT, C_PANEL);
    M5Cardputer.Display.setCursor(24, 56);
    M5Cardputer.Display.print(displayFit(ssid, SCREEN_W - 48));
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t start = millis();
    int dots = 0;
    while (WiFi.status() != WL_CONNECTED && millis() - start < 22000) {
        M5Cardputer.update();
        if ((millis() / 350) % 4 != dots) {
            dots = (millis() / 350) % 4;
            M5Cardputer.Display.fillRect(24, 76, 80, 10, C_PANEL);
            M5Cardputer.Display.setTextColor(C_ACCENT, C_PANEL);
            M5Cardputer.Display.setCursor(24, 76);
            for (int i = 0; i < dots; ++i) {
                M5Cardputer.Display.print(".");
            }
        }
        delay(20);
    }

    if (WiFi.status() == WL_CONNECTED) {
        syncClockIfNeeded();
        M5Cardputer.Display.setTextColor(C_GOOD, C_PANEL);
        M5Cardputer.Display.setCursor(24, 88);
        M5Cardputer.Display.print(WiFi.localIP().toString());
        delay(600);
        return true;
    }
    WiFi.disconnect(false, false);
    return false;
}

bool tryAutoStart() {
    if (savedSsid.isEmpty()) {
        return false;
    }
    if (isAbnormalPlaybackReset(bootResetReason)) {
        clearResumeState();
        currentTrackUrl = "";
        currentTrackName = "";
        return false;
    }
    pendingSsid = savedSsid;
    inputText = savedPass;
    if (!connectWifi(savedSsid, savedPass)) {
        return false;
    }

    if (!savedResumeUrl.isEmpty()) {
        drawBusyScreen("NAS", "Resuming track...");
        prepareSingleTrackContext(savedResumeUrl);
        currentTrackName = entries[0].name;
        currentTrackUrl = savedResumeUrl;
        screen = Screen::Player;
        needsRedraw = true;
        return true;
    }

    drawBusyScreen("NAS", "Loading files...");
    if (!savedNas.isEmpty() && loadDirectory(savedNas)) {
        screen = Screen::FileList;
        needsRedraw = true;
        return true;
    }

    beginInput(InputMode::NasUrl, "NAS address", savedNas.isEmpty() ? "http://" : savedNas, false);
    return true;
}

void applyFileSearch() {
    selected = 0;
    scrollTop = 0;
    if (fileSearchQuery.isEmpty()) {
        entries = allEntries;
        return;
    }

    entries.clear();
    for (const auto &entry : allEntries) {
        if (entry.parent || containsIgnoreCase(entry.name, fileSearchQuery)) {
            entries.push_back(entry);
        }
    }
}

void drawFileList() {
    wakePlayerDisplay();
    M5Cardputer.Display.fillScreen(C_BG);
    drawHeader("Library");
    fillPanel(8, 24, SCREEN_W - 16, 94, C_PANEL, C_EDGE, 10);
    int listTop = 34;
    if (!fileSearchQuery.isEmpty()) {
        M5Cardputer.Display.setTextColor(C_TEXT, C_PANEL);
        M5Cardputer.Display.setCursor(16, 34);
        M5Cardputer.Display.print("Search: " + displayFit(fileSearchQuery, SCREEN_W - 36));
        listTop = 46;
    }

    int visible = fileSearchQuery.isEmpty() ? 6 : 5;
    if (selected < scrollTop) {
        scrollTop = selected;
    }
    if (selected >= scrollTop + visible) {
        scrollTop = selected - visible + 1;
    }

    for (int row = 0; row < visible; ++row) {
        int idx = scrollTop + row;
        if (idx >= static_cast<int>(entries.size())) {
            break;
        }
        int y = listTop + row * 12;
        bool sel = idx == selected;
        fillPanel(14, y, SCREEN_W - 28, 10, sel ? C_SELECT : C_PANEL_ALT, sel ? C_SELECT : C_EDGE, 5);
        uint16_t rowBg = sel ? C_SELECT : C_PANEL_ALT;
        uint16_t fg = sel ? C_BG : (entries[idx].dir ? C_ACCENT : C_TEXT);
        String kind = entries[idx].dir ? "DIR" : "MP3";
        if (entries[idx].parent) {
            kind = "UP";
        }
        M5Cardputer.Display.setTextColor(fg, rowBg);
        M5Cardputer.Display.setCursor(20, y + 1);
        M5Cardputer.Display.print(kind);
        M5Cardputer.Display.setCursor(52, y + 1);
        M5Cardputer.Display.print(displayFit(entries[idx].name, SCREEN_W - 94));
    }

    if (entries.empty()) {
        M5Cardputer.Display.setTextColor(C_WARN, C_PANEL);
        M5Cardputer.Display.setCursor(16, 76);
        M5Cardputer.Display.print("No matching files");
    }

    drawFooter("Enter open   f search   h help");
    needsRedraw = false;
}

int nextPlayableIndex(int from, int dir) {
    if (entries.empty()) {
        return -1;
    }
    int idx = from;
    for (size_t step = 0; step < entries.size(); ++step) {
        idx += dir;
        if (idx < 0) {
            idx = entries.size() - 1;
        }
        if (idx >= static_cast<int>(entries.size())) {
            idx = 0;
        }
        if (!entries[idx].dir && pathIsMp3(entries[idx].url)) {
            return idx;
        }
    }
    return -1;
}

bool ensurePlaybackFolderLoaded(const String &failNextScreenText) {
    if (entries.size() > 1) {
        return true;
    }
    if (currentUrl.isEmpty()) {
        return false;
    }
    drawBusyScreen("NAS", "Loading files...");
    if (!loadDirectory(currentUrl)) {
        showMessage("NAS failed", failNextScreenText, 1400, Screen::FileList);
        return false;
    }
    int currentIndex = findEntryIndexByUrl(currentTrackUrl);
    if (currentIndex >= 0) {
        selected = currentIndex;
    }
    return true;
}

void stopPlayback(bool keepResume) {
    wakePlayerDisplay();
    if (keepResume) {
        persistPlaybackState(true);
    }
    playbackStartedAt = 0;
    skipPlaybackLoopOnce = false;
    if (mp3) {
        mp3->stop();
        delete mp3;
        mp3 = nullptr;
    }
    if (id3Source) {
        delete id3Source;
        id3Source = nullptr;
    }
    if (buffSource) {
        delete buffSource;
        buffSource = nullptr;
    }
    if (httpSource) {
        delete httpSource;
        httpSource = nullptr;
    }
    out.flush();
    M5Cardputer.Speaker.stop();
}

void startPlayback(int index, uint32_t offset, bool fallbackToZero) {
    if (index < 0 || index >= static_cast<int>(entries.size()) || entries[index].dir) {
        return;
    }
    stopPlayback(false);
    selected = index;
    currentTrackName = entries[index].name;
    currentTrackUrl = entries[index].url;
    httpSource = new AudioFileSourceHTTPBasic();
    AudioFileSourceHTTPBasic *basicSource = static_cast<AudioFileSourceHTTPBasic *>(httpSource);
    bool opened = basicSource->openAt(entries[index].url.c_str(), offset);
    if (!opened && offset > 0 && fallbackToZero) {
        opened = basicSource->open(entries[index].url.c_str());
        offset = 0;
    }
    if (!opened) {
        delete httpSource;
        httpSource = nullptr;
        currentTrackUrl = "";
        if (fallbackToZero) {
            showMessage("Audio error", "Cannot open stream", 1400, Screen::FileList);
        }
        return;
    }
    buffSource = new AudioFileSourceBuffer(httpSource, 8192);
    id3Source = new AudioFileSourceID3(buffSource);
    mp3 = new AudioGeneratorMP3();
    M5Cardputer.Speaker.setVolume(volume);
    out.SetGain(1.0f);
    if (!mp3->begin(id3Source, &out)) {
        stopPlayback(false);
        currentTrackUrl = "";
        if (fallbackToZero) {
            showMessage("Audio error", "MP3 decoder failed", 1400, Screen::FileList);
        }
        return;
    }
    saveResumeState(currentTrackUrl, clampResumeOffset(offset, currentPlaybackSize()));
    lastResumeSaveAt = millis();
    playbackStartedAt = millis();
    lastUserActionAt = millis();
    skipPlaybackLoopOnce = true;
    wakePlayerDisplay();
    screen = Screen::Player;
    needsRedraw = true;
}

String timerText() {
    if (shutdownAt == 0) {
        return "timer off";
    }
    int32_t left = static_cast<int32_t>(shutdownAt - millis());
    if (left <= 0) {
        return "shutting down";
    }
    uint32_t sec = left / 1000;
    return String(sec / 60) + "m left";
}

void drawPlayer() {
    M5Cardputer.Display.fillScreen(C_BG);
    drawHeader("Playing");
    fillPanel(8, 24, SCREEN_W - 16, 28, C_PANEL, C_EDGE, 10);
    M5Cardputer.Display.setTextColor(C_TEXT, C_PANEL);
    M5Cardputer.Display.setCursor(16, 33);
    M5Cardputer.Display.print(displayFit(currentTrackName, SCREEN_W - 72));
    M5Cardputer.Display.setTextColor(powerSaveEnabled ? C_GOOD : C_SOFT, C_PANEL);
    M5Cardputer.Display.setCursor(186, 33);
    M5Cardputer.Display.print(powerSaveEnabled ? "ECO" : "ECO-");

    fillPanel(8, 58, SCREEN_W - 16, 30, C_PANEL, C_EDGE, 10);
    fillPanel(8, 92, SCREEN_W - 16, 20, C_PANEL_ALT, C_EDGE, 8);
    M5Cardputer.Display.setTextColor(C_DIM, C_PANEL_ALT);
    M5Cardputer.Display.setCursor(16, 98);
    M5Cardputer.Display.print("VOL");

    drawFooter("h help");

    lastPlayerPosShown = UINT32_MAX;
    lastPlayerSizeShown = UINT32_MAX;
    lastPlayerVolumeShown = -1;
    lastPlayerTimerShown = "";
    lastPlayerStateShown = "";
    drawPlayerDynamic(true);
    needsRedraw = false;
    lastPlayerRedraw = millis();
}

void drawPlayerDynamic(bool force) {
    if (showHelpOverlay) {
        return;
    }
    uint32_t pos = currentPlaybackPos();
    uint32_t size = currentPlaybackSize();
    String timer = timerText();
    String stateText = mp3 && mp3->isRunning() ? "PLAY" : (currentTrackUrl.isEmpty() ? "STOP" : "READY");
    String progressText;
    String progressPct = "--";
    if (size > 0) {
        uint32_t posSec = pos / 16000UL;
        uint32_t sizeSec = size / 16000UL;
        char buf[32];
        snprintf(buf, sizeof(buf), "%02lu:%02lu/%02lu:%02lu",
                 static_cast<unsigned long>(posSec / 60), static_cast<unsigned long>(posSec % 60),
                 static_cast<unsigned long>(sizeSec / 60), static_cast<unsigned long>(sizeSec % 60));
        progressText = String(buf);
        progressPct = String((pos * 100UL) / size) + "%";
    } else {
        progressText = "LIVE STREAM";
    }

    if (force || pos != lastPlayerPosShown || size != lastPlayerSizeShown) {
        drawMeter(16, 76, SCREEN_W - 32, 6, 0, C_ACCENT);
        if (size > 0) {
            int fill = static_cast<int>((static_cast<uint64_t>(SCREEN_W - 36) * pos) / size);
            drawMeter(16, 76, SCREEN_W - 32, 6, fill, C_ACCENT);
        }
        M5Cardputer.Display.fillRect(62, 66, 74, 8, C_PANEL);
        M5Cardputer.Display.setTextColor(C_DIM, C_PANEL);
        M5Cardputer.Display.setCursor(62, 66);
        M5Cardputer.Display.print(displayFit(progressText, 72));
        M5Cardputer.Display.fillRect(140, 66, 30, 8, C_PANEL);
        M5Cardputer.Display.setTextColor(C_TEXT, C_PANEL);
        M5Cardputer.Display.setCursor(140, 66);
        M5Cardputer.Display.print(displayFit(progressPct, 28));
        lastPlayerPosShown = pos;
        lastPlayerSizeShown = size;
    }

    if (force || volume != lastPlayerVolumeShown) {
        int volFill = (94 * volume) / 255;
        drawMeter(44, 99, 94, 6, volFill, C_SELECT);
        M5Cardputer.Display.fillRect(146, 97, 28, 8, C_PANEL_ALT);
        M5Cardputer.Display.setTextColor(C_TEXT, C_PANEL_ALT);
        M5Cardputer.Display.setCursor(146, 97);
        M5Cardputer.Display.print(String((volume * 100) / 255) + "%");
        lastPlayerVolumeShown = volume;
    }

    if (force || stateText != lastPlayerStateShown) {
        M5Cardputer.Display.fillRect(16, 66, 40, 8, C_PANEL);
        M5Cardputer.Display.setTextColor(stateText == "PLAY" ? C_GOOD : (stateText == "READY" ? C_ACCENT : C_WARN),
                                         C_PANEL);
        M5Cardputer.Display.setCursor(16, 66);
        M5Cardputer.Display.print(stateText);
        lastPlayerStateShown = stateText;
    }

    if (force || timer != lastPlayerTimerShown) {
        M5Cardputer.Display.fillRect(176, 66, 48, 8, C_PANEL);
        M5Cardputer.Display.setTextColor(C_TEXT, C_PANEL);
        M5Cardputer.Display.setCursor(176, 66);
        M5Cardputer.Display.print(displayFit(timer, 46));
        lastPlayerTimerShown = timer;
    }

    static int lastEcoShown = -1;
    int ecoState = powerSaveEnabled ? (playerDimmed ? 2 : 1) : 0;
    if (force || ecoState != lastEcoShown) {
        M5Cardputer.Display.fillRect(186, 33, 38, 8, C_PANEL);
        M5Cardputer.Display.setTextColor(
            ecoState == 2 ? C_WARN : (ecoState == 1 ? C_GOOD : C_SOFT), C_PANEL);
        M5Cardputer.Display.setCursor(186, 33);
        M5Cardputer.Display.print(ecoState == 0 ? "ECO-" : (ecoState == 2 ? "ECOZ" : "ECO"));
        lastEcoShown = ecoState;
    }

    bool wifiConnected = WiFi.status() == WL_CONNECTED;
    int wifiBars = getWifiSignalBars();
    int battery = getBatteryPercent();
    int charging = isBatteryCharging() ? 1 : 0;
    String clock = headerClockText();
    if (force || wifiBars != lastHeaderWifiBars || wifiConnected != lastHeaderWifiConnected
        || battery != lastHeaderBattery || charging != lastHeaderCharging || clock != lastHeaderClockShown) {
        drawHeader("Playing");
    }
    lastPlayerRedraw = millis();
}

void drawHelpOverlay() {
    fillPanel(14, 20, SCREEN_W - 28, SCREEN_H - 40, C_PANEL, C_ACCENT, 10);
    M5Cardputer.Display.setTextColor(C_TEXT, C_PANEL);
    M5Cardputer.Display.setCursor(24, 28);
    M5Cardputer.Display.print("Help");
    M5Cardputer.Display.fillRect(24, 40, 34, 2, C_ACCENT);

    int y = 50;
    auto line = [&](const String &text) {
        M5Cardputer.Display.setCursor(26, y);
        M5Cardputer.Display.print(displayFit(text, SCREEN_W - 52));
        y += 14;
    };

    switch (screen) {
        case Screen::FileList:
            line("w/s move   Enter open");
            line("f search   r reload");
            line("n NAS      q WiFi");
            line("t timer    h close");
            break;
        case Screen::Player:
            line("Space play  +/- vol");
            line("Fn+,/. seek n/p track");
            line("b list      q WiFi");
            line("m eco       t timer");
            break;
        case Screen::WifiList:
            line("w/s move   Enter join");
            line("r rescan   Tab back");
            break;
        default:
            line("Enter confirm");
            line("Tab back    Del erase");
            break;
    }

    M5Cardputer.Display.setTextColor(C_DIM, C_PANEL);
    M5Cardputer.Display.setCursor(26, SCREEN_H - 30);
    M5Cardputer.Display.print("Any key closes");
}

void drawTimerMenu() {
    static const char *labels[] = {"Off", "15 min", "30 min", "1 hour", "2 hours"};
    M5Cardputer.Display.fillScreen(C_BG);
    drawHeader("Sleep timer");
    fillPanel(22, 26, SCREEN_W - 44, 86, C_PANEL, C_EDGE, 10);
    for (int i = 0; i < 5; ++i) {
        int y = 34 + i * 14;
        bool sel = i == timerSelected;
        fillPanel(32, y, SCREEN_W - 64, 12, sel ? C_SELECT : C_PANEL_ALT, sel ? C_SELECT : C_EDGE, 5);
        M5Cardputer.Display.setTextColor(sel ? C_BG : C_TEXT, sel ? C_SELECT : C_PANEL_ALT);
        M5Cardputer.Display.setCursor(42, y + 2);
        M5Cardputer.Display.print(labels[i]);
    }
    drawFooter("Enter set   Tab back");
    needsRedraw = false;
}

void applyTimerChoice() {
    static const uint32_t seconds[] = {0, 15 * 60, 30 * 60, 60 * 60, 2 * 60 * 60};
    uint32_t sec = seconds[timerSelected];
    shutdownAt = sec == 0 ? 0 : millis() + sec * 1000UL;
    showMessage("Timer", sec == 0 ? "Sleep timer off" : (String(sec / 60) + " min set"), 800,
                mp3 ? Screen::Player : Screen::FileList);
}

void shutdownNow() {
    stopPlayback();
    M5Cardputer.Display.fillScreen(C_BG);
    drawHeader("Power");
    M5Cardputer.Display.setTextColor(C_TEXT, C_BG);
    M5Cardputer.Display.setCursor(6, 54);
    M5Cardputer.Display.print("Auto shutdown");
    delay(300);
    WiFi.disconnect(true, false);
    M5Cardputer.Speaker.stop();
    M5Cardputer.Power.powerOff();
    delay(500);
    esp_deep_sleep_start();
}

void handleInput(const KeyEvent &key) {
    markUserActivity();
    if (key.del) {
        removeLastUtf8(inputText);
        needsRedraw = true;
    }
    for (char c : key.chars) {
        if (c >= 32 && c <= 126 && inputText.length() < 180) {
            inputText += c;
            needsRedraw = true;
        }
    }
    if (key.tab) {
        if (inputMode == InputMode::FileSearch) {
            fileSearchQuery = "";
            applyFileSearch();
            screen = Screen::FileList;
        } else if (inputMode == InputMode::NasUrl) {
            screen = Screen::FileList;
        } else {
            screen = Screen::WifiList;
        }
        needsRedraw = true;
        return;
    }
    if (!key.enter) {
        return;
    }

    if (inputMode == InputMode::ManualSsid) {
        pendingSsid = trimCopy(inputText);
        if (!pendingSsid.isEmpty()) {
            beginInput(InputMode::WifiPassword, "WiFi password", "", true);
        }
    } else if (inputMode == InputMode::WifiPassword) {
        if (connectWifi(pendingSsid, inputText)) {
            afterWifiConnected();
        } else {
            showMessage("WiFi failed", "Check password or signal", 1600, Screen::WifiList);
        }
    } else if (inputMode == InputMode::FileSearch) {
        fileSearchQuery = trimCopy(inputText);
        applyFileSearch();
        screen = Screen::FileList;
        needsRedraw = true;
    } else if (inputMode == InputMode::NasUrl) {
        String url = normalizeNasUrl(inputText);
        M5Cardputer.Display.fillScreen(C_BG);
        drawHeader("NAS");
        M5Cardputer.Display.setTextColor(C_TEXT, C_BG);
        M5Cardputer.Display.setCursor(6, 52);
        M5Cardputer.Display.print("Loading files...");
        if (loadDirectory(url)) {
            savedNas = currentUrl;
            screen = Screen::FileList;
            needsRedraw = true;
        } else {
            showMessage("NAS failed", "Use HTTP/WebDAV URL", 1800, Screen::TextInput);
        }
    }
}

void handleWifiList(const KeyEvent &key) {
    markUserActivity();
    if (key.tab && (wifiReturnScreen == Screen::FileList || wifiReturnScreen == Screen::Player)) {
        screen = wifiReturnScreen == Screen::Player && mp3 ? Screen::Player : Screen::FileList;
        needsRedraw = true;
        return;
    }
    if (charInEvent(key, 'r')) {
        scanWifi();
        return;
    }
    if (charInEvent(key, 'w') && wifiSelected > 0) {
        --wifiSelected;
        needsRedraw = true;
    }
    if (charInEvent(key, 's') && wifiSelected + 1 < static_cast<int>(wifiItems.size())) {
        ++wifiSelected;
        needsRedraw = true;
    }
    if (key.enter && wifiSelected >= 0 && wifiSelected < static_cast<int>(wifiItems.size())) {
        WifiItem &item = wifiItems[wifiSelected];
        if (item.manual) {
            beginInput(InputMode::ManualSsid, "WiFi SSID", "", false);
            return;
        }
        pendingSsid = item.ssid;
        if (item.saved) {
            inputText = savedPass;
            if (connectWifi(pendingSsid, savedPass)) {
                beginInput(InputMode::NasUrl, "NAS address", savedNas.isEmpty() ? "http://" : savedNas, false);
            } else {
                showMessage("WiFi failed", "Saved password failed", 1600, Screen::WifiList);
            }
            return;
        }
        if (item.enc == WIFI_AUTH_OPEN) {
            inputText = "";
            if (connectWifi(pendingSsid, "")) {
                afterWifiConnected();
            } else {
                showMessage("WiFi failed", "Cannot connect", 1500, Screen::WifiList);
            }
            return;
        }
        beginInput(InputMode::WifiPassword, "WiFi password", "", true);
    }
}

void handleFileList(const KeyEvent &key) {
    markUserActivity();
    if (charInEvent(key, 'q')) {
        wifiReturnScreen = Screen::FileList;
        scanWifi();
        return;
    }
    if (charInEvent(key, 'f')) {
        beginInput(InputMode::FileSearch, "Search files", fileSearchQuery, false);
        return;
    }
    if (charInEvent(key, 'n')) {
        beginInput(InputMode::NasUrl, "NAS address", currentUrl.isEmpty() ? savedNas : currentUrl, false);
        return;
    }
    if (charInEvent(key, 'r')) {
        M5Cardputer.Display.fillScreen(C_BG);
        drawHeader("NAS");
        M5Cardputer.Display.setTextColor(C_TEXT, C_BG);
        M5Cardputer.Display.setCursor(6, 52);
        M5Cardputer.Display.print("Reloading...");
        if (!loadDirectory(currentUrl)) {
            showMessage("NAS failed", "Reload failed", 1200, Screen::FileList);
        } else {
            needsRedraw = true;
        }
        return;
    }
    if (charInEvent(key, 't')) {
        screen = Screen::TimerMenu;
        needsRedraw = true;
        return;
    }
    if (charInEvent(key, 'w') && selected > 0) {
        --selected;
        needsRedraw = true;
    }
    if (charInEvent(key, 's') && selected + 1 < static_cast<int>(entries.size())) {
        ++selected;
        needsRedraw = true;
    }
    if (key.enter && selected >= 0 && selected < static_cast<int>(entries.size())) {
        if (entries[selected].dir) {
            String url = entries[selected].url;
            M5Cardputer.Display.fillScreen(C_BG);
            drawHeader("NAS");
            M5Cardputer.Display.setTextColor(C_TEXT, C_BG);
            M5Cardputer.Display.setCursor(6, 52);
            M5Cardputer.Display.print("Opening folder...");
            if (!loadDirectory(url)) {
                showMessage("NAS failed", "No MP3 files found", 1400, Screen::FileList);
            } else {
                needsRedraw = true;
            }
        } else {
            startPlayback(selected);
        }
    }
}

void handlePlayer(const KeyEvent &key) {
    markUserActivity();
    if (charInEvent(key, 'q')) {
        stopPlayback();
        wifiReturnScreen = Screen::FileList;
        scanWifi();
        return;
    }
    if (key.space) {
        if (mp3 && mp3->isRunning()) {
            stopPlayback();
            needsRedraw = true;
        } else {
            uint32_t resumeOffset = entries[selected].url == savedResumeUrl ? savedResumePos : 0;
            startPlayback(selected, resumeOffset);
        }
    }
    if (charInEvent(key, 'b')) {
        stopPlayback();
        if (entries.size() == 1 && currentUrl.length() > 0) {
            if (!ensurePlaybackFolderLoaded("Cannot load folder")) {
                return;
            }
        }
        screen = Screen::FileList;
        needsRedraw = true;
        return;
    }
    if (charInEvent(key, 't')) {
        screen = Screen::TimerMenu;
        needsRedraw = true;
        return;
    }
    if (charInEvent(key, '+') || charInEvent(key, '=')) {
        volume = std::min(255, volume + 15);
        M5Cardputer.Speaker.setVolume(volume);
        prefs.putUInt("volume", volume);
        needsRedraw = true;
    }
    if (charInEvent(key, '-') || charInEvent(key, '_')) {
        volume = std::max(0, volume - 15);
        M5Cardputer.Speaker.setVolume(volume);
        prefs.putUInt("volume", volume);
        needsRedraw = true;
    }
    bool seekBack = (key.fn && charInEvent(key, ',')) || charInEvent(key, '<');
    bool seekForward = (key.fn && charInEvent(key, '.')) || charInEvent(key, '>');
    if (seekBack || seekForward) {
        uint32_t target = computeSeekTarget(seekForward);
        startPlayback(selected, target, false);
        if (!(mp3 && mp3->isRunning())) {
            startPlayback(selected, 0, true);
            showMessage("Seek", "Range seek unsupported", 900, Screen::Player);
        }
        return;
    }
    if (charInEvent(key, 'n')) {
        if (!ensurePlaybackFolderLoaded("Cannot load folder")) {
            return;
        }
        int next = nextPlayableIndex(selected, 1);
        if (next >= 0) {
            startPlayback(next);
        }
    }
    if (charInEvent(key, 'p')) {
        if (!ensurePlaybackFolderLoaded("Cannot load folder")) {
            return;
        }
        int prev = nextPlayableIndex(selected, -1);
        if (prev >= 0) {
            startPlayback(prev);
        }
    }
    if (charInEvent(key, 'm')) {
        powerSaveEnabled = !powerSaveEnabled;
        prefs.putBool("eco", powerSaveEnabled);
        if (!powerSaveEnabled) {
            wakePlayerDisplay();
            playerDimmed = false;
            playerScreenOff = false;
        }
        drawPlayerDynamic(true);
        return;
    }
}

void handleTimerMenu(const KeyEvent &key) {
    markUserActivity();
    if (key.tab) {
        screen = mp3 ? Screen::Player : Screen::FileList;
        needsRedraw = true;
        return;
    }
    if (charInEvent(key, 'w') && timerSelected > 0) {
        --timerSelected;
        needsRedraw = true;
    }
    if (charInEvent(key, 's') && timerSelected < 4) {
        ++timerSelected;
        needsRedraw = true;
    }
    if (key.enter) {
        applyTimerChoice();
    }
}

void dispatchKeys(const KeyEvent &key) {
    if (!key.pressed) {
        return;
    }
    if (showHelpOverlay) {
        showHelpOverlay = false;
        needsRedraw = true;
        return;
    }
    if (screen == Screen::Player && (playerDimmed || playerScreenOff)) {
        wakePlayerDisplay();
        lastUserActionAt = millis();
        return;
    }
    if ((screen == Screen::FileList || screen == Screen::Player) && charInEvent(key, 'h')) {
        showHelpOverlay = true;
        needsRedraw = true;
        return;
    }
    switch (screen) {
        case Screen::WifiList:
            handleWifiList(key);
            break;
        case Screen::TextInput:
            handleInput(key);
            break;
        case Screen::FileList:
            handleFileList(key);
            break;
        case Screen::Player:
            handlePlayer(key);
            break;
        case Screen::TimerMenu:
            handleTimerMenu(key);
            break;
        case Screen::Message:
            break;
    }
}

void drawCurrentScreen() {
    if (!needsRedraw) {
        return;
    }
    switch (screen) {
        case Screen::WifiList:
            drawWifiList();
            break;
        case Screen::TextInput:
            drawInput();
            break;
        case Screen::FileList:
            drawFileList();
            break;
        case Screen::Player:
            drawPlayer();
            break;
        case Screen::TimerMenu:
            drawTimerMenu();
            break;
        case Screen::Message:
            drawMessage();
            break;
    }
    if (showHelpOverlay) {
        drawHelpOverlay();
    }
}

}  // namespace

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setTextDatum(top_left);
    M5Cardputer.Display.setTextWrap(false);
    M5Cardputer.Display.setFont(&fonts::efontCN_12);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setBrightness(DISPLAY_BRIGHTNESS_ACTIVE);
    M5Cardputer.Display.fillScreen(C_BG);
    M5Cardputer.Speaker.begin();
    bootResetReason = esp_reset_reason();

    prefs.begin("nasmp3", false);
    savedSsid = prefs.getString("ssid", "");
    savedPass = prefs.getString("pass", "");
    savedNas = prefs.getString("nas", "");
    savedResumeUrl = prefs.getString("resume_url", "");
    savedResumePos = prefs.getUInt("resume_pos", 0);
    volume = static_cast<int>(prefs.getUInt("volume", volume));
    powerSaveEnabled = prefs.getBool("eco", true);
    lastUserActionAt = millis();
    M5Cardputer.Speaker.setVolume(volume);

    if (!savedSsid.isEmpty() && !isAbnormalPlaybackReset(bootResetReason)) {
        messageTitle = "Boot";
        messageBody = "Starting...";
        messageUntil = millis() + 600000UL;
        screen = Screen::Message;
        needsRedraw = true;
        bootAutoStartPending = true;
    } else {
        scanWifi();
    }
}

void loop() {
    M5Cardputer.update();

    if (bootAutoStartPending) {
        bootAutoStartPending = false;
        if (!tryAutoStart()) {
            scanWifi();
        }
    }

    if (shutdownAt != 0 && static_cast<int32_t>(millis() - shutdownAt) >= 0) {
        shutdownNow();
    }

    if (screen == Screen::Message && static_cast<int32_t>(millis() - messageUntil) >= 0) {
        screen = returnAfterMessage;
        needsRedraw = true;
    }

    if (mp3 && mp3->isRunning()) {
        if (skipPlaybackLoopOnce) {
            skipPlaybackLoopOnce = false;
        } else {
            persistPlaybackState(false);
            if (!mp3->loop()) {
                uint32_t playedMs = playbackStartedAt == 0 ? 0 : millis() - playbackStartedAt;
                uint32_t playedPos = currentPlaybackPos();
                stopPlayback(false);
                if (playedMs < BAD_PLAYBACK_TIME_MS || playedPos < BAD_PLAYBACK_POS_BYTES) {
                    clearResumeState();
                    currentTrackUrl = "";
                    currentTrackName = "";
                    showMessage("Audio error", "Unsupported or bad MP3", 1500, Screen::FileList);
                    return;
                }
                int next = nextPlayableIndex(selected, 1);
                if (next >= 0) {
                    startPlayback(next);
                } else {
                    clearResumeState();
                    currentTrackUrl = "";
                    screen = Screen::FileList;
                    needsRedraw = true;
                }
            }
        }
    }

    if (screen == Screen::Player && powerSaveEnabled) {
        uint32_t idleMs = millis() - lastUserActionAt;
        if (!playerDimmed && idleMs > POWER_SAVE_IDLE_MS) {
            M5Cardputer.Display.setBrightness(DISPLAY_BRIGHTNESS_DIM);
            playerDimmed = true;
            playerScreenOff = false;
            drawPlayerDynamic(true);
        } else if (playerDimmed && !playerScreenOff && idleMs > POWER_SAVE_SCREEN_OFF_MS) {
            M5Cardputer.Display.setBrightness(DISPLAY_BRIGHTNESS_OFF);
            playerScreenOff = true;
        }
    }

    if (screen == Screen::Player && !playerScreenOff) {
        uint32_t refreshMs = playerDimmed ? 1500 : 250;
        if (millis() - lastPlayerRedraw > refreshMs) {
            drawPlayerDynamic(false);
        }
    }

    KeyEvent key = readKeys();
    dispatchKeys(key);
    drawCurrentScreen();
    delay(2);
}
