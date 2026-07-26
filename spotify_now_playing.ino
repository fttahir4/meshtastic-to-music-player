/**
 * Heltec V4 Spotify Now Playing display
 * shows a spinning disk icon + song title (ticker scroll if long) + fixed artist name
 * supports multiple scripts (Latin, Japanese, Korean, Cyrillic, Urdu/Arabic, Devanagari, Hebrew, Greek)
 * pulls data from Spotify's Web API over wifi
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <base64.h>
#include <time.h>

// ==== FILL THESE IN YOURSELF ====
// add as many wifi networks as you want, it'll try them in order
struct WifiNetwork {
  const char* ssid;
  const char* password;
};

WifiNetwork WIFI_NETWORKS[] = {
  {"YOUR_WIFI_NAME_HERE", "YOUR_WIFI_PASSWORD_HERE"},
  {"YOUR_SECOND_WIFI_NAME_HERE", "YOUR_SECOND_WIFI_PASSWORD_HERE"},
  // add more lines like the ones above if you want
};
const int WIFI_NETWORK_COUNT = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

const char* SPOTIFY_CLIENT_ID = "YOUR_CLIENT_ID_HERE";
const char* SPOTIFY_CLIENT_SECRET = "YOUR_CLIENT_SECRET_HERE";
const char* SPOTIFY_REFRESH_TOKEN = "YOUR_REFRESH_TOKEN_HERE";
// ===================================

// OLED pins (Heltec V4, same as V3)
#define VEXT_PIN 36
#define SDA_PIN  17
#define SCL_PIN  18
#define RST_PIN  21

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, RST_PIN, SCL_PIN, SDA_PIN);

String accessToken = "";
unsigned long tokenExpiryMillis = 0;

String currentSong = "";
String currentArtist = "";
bool isPlaying = false;
bool shuffleState = false;
String repeatState = "off"; // "off", "context", or "track"

long trackProgressMs = 0;
long trackDurationMs = 0;
unsigned long lastProgressSyncMillis = 0;

int songScrollX = 0;
int artistScrollX = 0;
unsigned long lastScrollTime = 0;
unsigned long lastPollTime = 0;

// last played history, most recent first
const int HISTORY_SIZE = 3;
String historySong[HISTORY_SIZE];
String historyArtist[HISTORY_SIZE];
int historyCount = 0;

unsigned long lastHistoryCycleTime = 0;
int historyDisplayIndex = 0;
int historyScrollX = 0;
const unsigned long HISTORY_CYCLE_INTERVAL = 4000; // how long a short (non-scrolling) history entry holds before advancing

unsigned long lastBrightnessCheckTime = 0;
const unsigned long BRIGHTNESS_CHECK_INTERVAL = 60000; // recheck time of day once a minute
bool timeIsSynced = false;

float diskAngle = 0;
int spinStep = 0;
const int SPIN_STEPS_PER_ROTATION = 24; // number of steps per full spin, keeps landings exact
unsigned long lastSpinTime = 0;

unsigned long lastWifiCheckTime = 0;
const unsigned long WIFI_CHECK_INTERVAL = 5000; // check wifi status every 5 sec

const unsigned long POLL_INTERVAL = 5000;   // check spotify every 5 sec
const unsigned long SCROLL_INTERVAL = 40;   // ticker speed, lower = faster
const unsigned long SPIN_INTERVAL = 30;     // disk spin speed

const int TEXT_START_X = 45;   // where text area begins (right of the disk)
const int TEXT_END_X = 128;    // right edge of screen
const int TEXT_AREA_WIDTH = TEXT_END_X - TEXT_START_X; // available pixel width for song/artist text
const int TICKER_GAP = 20;     // gap between repeats of the ticker text

/**
 * Counts actual characters in a UTF-8 string (not bytes)
 * multi-byte characters (like Japanese, Korean, Urdu) count as 1 character each
 */
int utf8CharCount(const String &s) {
  int count = 0;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if ((c & 0xC0) != 0x80) count++; // not a continuation byte, so it's a new character
  }
  return count;
}

/**
 * Figures out how many bytes a UTF-8 character starting at this byte takes up
 */
int utf8CharByteLength(const String &s, int index) {
  uint8_t c = (uint8_t)s[index];
  if ((c & 0x80) == 0) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1; // invalid byte, treat as 1 to avoid getting stuck
}

/**
 * Decodes the Unicode codepoint of the character starting at this byte
 */
uint32_t decodeUTF8At(const String &s, int index, int charLen) {
  uint8_t c = (uint8_t)s[index];
  uint32_t codepoint = 0;

  if (charLen == 1) {
    codepoint = c;
  } else if (charLen == 2) {
    codepoint = c & 0x1F;
  } else if (charLen == 3) {
    codepoint = c & 0x0F;
  } else if (charLen == 4) {
    codepoint = c & 0x07;
  }

  for (int j = 1; j < charLen && (index + j) < (int)s.length(); j++) {
    codepoint = (codepoint << 6) | ((uint8_t)s[index + j] & 0x3F);
  }
  return codepoint;
}

/**
 * Checks if a single codepoint belongs to Arabic or Urdu, which read right-to-left
 */
bool isRTLCodepoint(uint32_t codepoint) {
  return (codepoint >= 0x0600 && codepoint <= 0x06FF) ||
         (codepoint >= 0x0750 && codepoint <= 0x077F) ||
         (codepoint >= 0xFB50 && codepoint <= 0xFDFF) ||
         (codepoint >= 0xFE70 && codepoint <= 0xFEFF);
}

/**
 * Reverses the order of characters (not bytes) in a UTF-8 string
 * used so right-to-left runs read in the correct direction
 */
String reverseUTF8(const String &s) {
  String result = "";
  result.reserve(s.length());

  int i = s.length();
  while (i > 0) {
    int j = i - 1;
    while (j > 0 && ((uint8_t)s[j] & 0xC0) == 0x80) {
      j--; // walk back to the start byte of this character
    }
    result += s.substring(j, i);
    i = j;
  }
  return result;
}

/**
 * Prepares text for display: English/Latin stays in normal reading order,
 * but any Arabic or Urdu run within the same string gets reversed in place
 * so it reads right-to-left, same as real bidirectional text rendering
 */
String getDisplayText(const String &s) {
  String result = "";
  int len = s.length();
  int i = 0;

  while (i < len) {
    int charLen = utf8CharByteLength(s, i);
    uint32_t codepoint = decodeUTF8At(s, i, charLen);
    bool rtl = isRTLCodepoint(codepoint);

    int runStart = i;
    int runEnd = i + charLen;

    // extend the run while consecutive characters share the same direction
    while (runEnd < len) {
      int nextLen = utf8CharByteLength(s, runEnd);
      uint32_t nextCodepoint = decodeUTF8At(s, runEnd, nextLen);
      if (isRTLCodepoint(nextCodepoint) != rtl) break;
      runEnd += nextLen;
    }

    String run = s.substring(runStart, runEnd);
    result += rtl ? reverseUTF8(run) : run;
    i = runEnd;
  }

  return result;
}

/**
 * Scans a UTF-8 string and picks the best matching font based on the first
 * non-ASCII character it finds. u8g2 doesn't have one universal font, each
 * script needs its own, so we detect and switch.
 */
const uint8_t* pickFontForText(const String &s) {
  size_t i = 0;
  while (i < s.length()) {
    uint8_t c = (uint8_t)s[i];
    uint32_t codepoint = 0;
    int extraBytes = 0;

    if (c < 0x80) {
      i++;
      continue; // plain ascii, keep scanning for something else
    } else if ((c & 0xE0) == 0xC0) {
      codepoint = c & 0x1F;
      extraBytes = 1;
    } else if ((c & 0xF0) == 0xE0) {
      codepoint = c & 0x0F;
      extraBytes = 2;
    } else if ((c & 0xF8) == 0xF0) {
      codepoint = c & 0x07;
      extraBytes = 3;
    } else {
      i++;
      continue; // invalid byte, skip it
    }

    for (int j = 1; j <= extraBytes && (i + j) < s.length(); j++) {
      codepoint = (codepoint << 6) | ((uint8_t)s[i + j] & 0x3F);
    }
    i += extraBytes + 1;

    if (codepoint >= 0x3040 && codepoint <= 0x30FF) return u8g2_font_unifont_t_japanese2; // hiragana/katakana
    if (codepoint >= 0x4E00 && codepoint <= 0x9FFF) return u8g2_font_unifont_t_japanese2; // kanji
    if (codepoint >= 0xAC00 && codepoint <= 0xD7A3) return u8g2_font_unifont_t_korean2;   // hangul
    if (codepoint >= 0x0400 && codepoint <= 0x04FF) return u8g2_font_unifont_t_cyrillic;
    if (codepoint >= 0x0600 && codepoint <= 0x06FF) return u8g2_font_unifont_t_arabic; // standard arabic
    if ((codepoint >= 0x0750 && codepoint <= 0x077F) ||
        (codepoint >= 0xFB50 && codepoint <= 0xFDFF) ||
        (codepoint >= 0xFE70 && codepoint <= 0xFEFF)) return u8g2_font_unifont_t_urdu; // urdu-specific extended letters
    if (codepoint >= 0x0900 && codepoint <= 0x097F) return u8g2_font_unifont_t_devanagari;
    if (codepoint >= 0x0590 && codepoint <= 0x05FF) return u8g2_font_unifont_t_hebrew;
    if (codepoint >= 0x0370 && codepoint <= 0x03FF) return u8g2_font_unifont_t_greek;
    if (codepoint >= 0x1E00 && codepoint <= 0x1EFF) return u8g2_font_unifont_t_vietnamese2; // vietnamese stacked diacritics
    if (codepoint >= 0x00C0 && codepoint <= 0x024F) return u8g2_font_unifont_t_extended; // accented latin: spanish, french, german, portuguese, etc
  }

  return u8g2_font_unifont_tr; // pure ascii, same unifont family as every other language for a consistent look
}

/**
 * Draws a vinyl disk icon with a rotating spoke line, always spinning
 */
void drawDiskIcon(int centerX, int centerY, int radius, float angle) {
  u8g2.drawDisc(centerX, centerY, radius);
  u8g2.setDrawColor(0);
  u8g2.drawDisc(centerX, centerY, radius / 4);
  u8g2.setDrawColor(1);
  u8g2.drawCircle(centerX, centerY, radius / 4 + 2);

  int spokeX = centerX + cos(angle) * (radius - 3);
  int spokeY = centerY + sin(angle) * (radius - 3);
  u8g2.setDrawColor(0);
  u8g2.drawLine(centerX, centerY, spokeX, spokeY);
  u8g2.setDrawColor(1);
}

/**
 * Tries one wifi network, waits up to timeoutMs for it to connect
 * returns true if connected
 */
bool tryWifiNetwork(const char* ssid, const char* password, unsigned long timeoutMs) {
  WiFi.disconnect();
  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < timeoutMs) {
    delay(500);
    Serial.print(".");
  }

  return WiFi.status() == WL_CONNECTED;
}

/**
 * Connects to wifi, tries each network in WIFI_NETWORKS in order until one works
 * blocks until connected (retries the whole list forever if none work)
 */
void connectWifi() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 30, "connecting to wifi...");
  u8g2.sendBuffer();

  bool connected = false;
  while (!connected) {
    for (int i = 0; i < WIFI_NETWORK_COUNT; i++) {
      Serial.print("trying network: ");
      Serial.println(WIFI_NETWORKS[i].ssid);

      if (tryWifiNetwork(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].password, 8000)) {
        connected = true;
        break;
      }
    }
  }

  Serial.println("\nwifi connected");
}

/**
 * Checks wifi status and reconnects if it dropped
 * tries each known network in order
 */
void reconnectWifiIfNeeded() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("wifi dropped, reconnecting...");

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 30, "wifi lost, reconnecting");
    u8g2.sendBuffer();

    bool connected = false;
    for (int i = 0; i < WIFI_NETWORK_COUNT; i++) {
      Serial.print("trying network: ");
      Serial.println(WIFI_NETWORKS[i].ssid);

      if (tryWifiNetwork(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].password, 8000)) {
        connected = true;
        break;
      }
    }

    if (connected) {
      Serial.println("\nwifi reconnected");
    } else {
      Serial.println("\nreconnect attempt timed out, will retry later");
    }
  }
}

/**
 * Exchanges the refresh token for a fresh access token
 * spotify access tokens expire after 1 hour
 */
bool refreshAccessToken() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.begin(client, "https://accounts.spotify.com/api/token");
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String authString = String(SPOTIFY_CLIENT_ID) + ":" + String(SPOTIFY_CLIENT_SECRET);
  String authBase64 = base64::encode(authString);
  https.addHeader("Authorization", "Basic " + authBase64);

  String body = "grant_type=refresh_token&refresh_token=" + String(SPOTIFY_REFRESH_TOKEN);

  int httpCode = https.POST(body);

  if (httpCode == 200) {
    String response = https.getString();
    JsonDocument doc;
    deserializeJson(doc, response);

    accessToken = doc["access_token"].as<String>();
    int expiresIn = doc["expires_in"].as<int>();
    tokenExpiryMillis = millis() + (expiresIn - 60) * 1000UL;

    https.end();
    Serial.println("access token refreshed");
    return true;
  } else {
    Serial.print("token refresh failed, code: ");
    Serial.println(httpCode);
    https.end();
    return false;
  }
}

/**
 * Polls spotify's currently-playing endpoint and updates global state
 * resets the ticker scroll position whenever the song actually changes
 */
void pollCurrentlyPlaying() {
  if (millis() > tokenExpiryMillis) {
    refreshAccessToken();
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;
  https.begin(client, "https://api.spotify.com/v1/me/player");
  https.addHeader("Authorization", "Bearer " + accessToken);
  https.useHTTP10(true); // avoids chunked transfer encoding, more reliable to parse fully
  https.setTimeout(6000); // this response is bigger than the old endpoint, give it more time

  int httpCode = https.GET();
  Serial.print("poll http code: ");
  Serial.println(httpCode);

  if (httpCode == 200) {
    String response = https.getString();
    Serial.print("response length: ");
    Serial.println(response.length());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, response);

    if (err) {
      Serial.print("json parse failed: ");
      Serial.println(err.c_str());
    } else {
      isPlaying = doc["is_playing"] | false;
      String newSong = doc["item"]["name"] | "";
      String newArtist = doc["item"]["artists"][0]["name"] | "";

      if (newSong != currentSong) {
        songScrollX = 0; // clean reset whenever the track changes
        addToHistory(currentSong, currentArtist); // save the track that just finished
      }
      if (newArtist != currentArtist) {
        artistScrollX = 0; // clean reset whenever the artist changes
      }

      currentSong = newSong;
      currentArtist = newArtist;

      trackProgressMs = doc["progress_ms"] | 0;
      trackDurationMs = doc["item"]["duration_ms"] | 0;
      lastProgressSyncMillis = millis(); // mark when we last got a real position from spotify

      shuffleState = doc["shuffle_state"] | false;
      repeatState = doc["repeat_state"] | "off";

      Serial.print("isPlaying: ");
      Serial.print(isPlaying);
      Serial.print(" | shuffle: ");
      Serial.print(shuffleState);
      Serial.print(" | repeat: ");
      Serial.println(repeatState);
    }
  } else if (httpCode == 204) {
    isPlaying = false;
    currentSong = "";
    currentArtist = "";
    songScrollX = 0;
    artistScrollX = 0;
    trackProgressMs = 0;
    trackDurationMs = 0;
    shuffleState = false;
    repeatState = "off";
  } else {
    Serial.print("poll failed, code: ");
    Serial.println(httpCode);
  }

  https.end();
}

/**
 * Formats milliseconds as M:SS, same style spotify itself uses
 */
String formatTime(long ms) {
  if (ms < 0) ms = 0;
  long totalSeconds = ms / 1000;
  long minutes = totalSeconds / 60;
  long seconds = totalSeconds % 60;

  String secondsStr = (seconds < 10) ? "0" + String(seconds) : String(seconds);
  return String(minutes) + ":" + secondsStr;
}

/**
 * Gets the current wall-clock time as a 12-hour string like "4:32 PM"
 * returns an empty string if time hasn't synced over the internet yet
 */
String getCurrentTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) {
    return "";
  }
  timeIsSynced = true;

  int hour12 = timeinfo.tm_hour % 12;
  if (hour12 == 0) hour12 = 12;

  String minuteStr = (timeinfo.tm_min < 10) ? "0" + String(timeinfo.tm_min) : String(timeinfo.tm_min);
  String ampm = (timeinfo.tm_hour < 12) ? "AM" : "PM";

  return String(hour12) + ":" + minuteStr + " " + ampm;
}

/**
 * Dims the OLED late at night, brighter during the day
 * u8g2 contrast ranges from 0 (dimmest) to 255 (brightest)
 */
void updateBrightnessForTimeOfDay() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) {
    return; // time not synced yet, leave brightness as-is
  }

  int hour = timeinfo.tm_hour;
  bool isNight = (hour >= 20 || hour < 6); // 8pm to 6am counts as night

  u8g2.setContrast(isNight ? 20 : 200);
}

/**
 * Records a finished track into the recent-history list, most recent first
 */
void addToHistory(const String &song, const String &artist) {
  if (song == "") return;

  // avoid duplicate back-to-back entries (like if we poll mid-song twice)
  if (historyCount > 0 && historySong[0] == song && historyArtist[0] == artist) {
    return;
  }

  for (int i = HISTORY_SIZE - 1; i > 0; i--) {
    historySong[i] = historySong[i - 1];
    historyArtist[i] = historyArtist[i - 1];
  }
  historySong[0] = song;
  historyArtist[0] = artist;

  if (historyCount < HISTORY_SIZE) historyCount++;
}

/**
 * Builds the "recently: Song - Artist" text for the currently shown history entry,
 * with right-to-left runs corrected same as the main song/artist display
 */
String getHistoryDisplayText() {
  if (historyCount == 0) return "";
  String song = getDisplayText(historySong[historyDisplayIndex]);
  String artist = getDisplayText(historyArtist[historyDisplayIndex]);
  return "recently: " + song + " - " + artist;
}

/**
 * Draws elapsed time, a thin progress bar, and total duration across the bottom,
 * estimating current position between polls using the board's own clock
 */
void drawProgressBar() {
  if (trackDurationMs <= 0) return;

  long estimatedProgress = trackProgressMs;
  if (isPlaying) {
    estimatedProgress += (millis() - lastProgressSyncMillis);
  }
  if (estimatedProgress > trackDurationMs) estimatedProgress = trackDurationMs;
  if (estimatedProgress < 0) estimatedProgress = 0;

  float fraction = (float)estimatedProgress / (float)trackDurationMs;

  u8g2.setFont(u8g2_font_4x6_tr); // tiny font so both times + bar fit on one row

  String elapsedStr = formatTime(estimatedProgress);
  String durationStr = formatTime(trackDurationMs);

  int elapsedWidth = u8g2.getStrWidth(elapsedStr.c_str());
  int durationWidth = u8g2.getStrWidth(durationStr.c_str());

  int textY = 63; // bottom row baseline

  u8g2.drawStr(0, textY, elapsedStr.c_str());
  u8g2.drawStr(128 - durationWidth, textY, durationStr.c_str());

  // bar sits between the two time labels
  int barX = elapsedWidth + 4;
  int barWidth = (128 - durationWidth - 4) - barX;
  int barY = 58;
  int barHeight = 3;

  u8g2.drawFrame(barX, barY, barWidth, barHeight);
  int filledWidth = (int)(fraction * (barWidth - 2));
  if (filledWidth > 0) {
    u8g2.drawBox(barX + 1, barY + 1, filledWidth, barHeight - 2);
  }
}

/**
 * Draws small status icons across the top strip: shuffle, play/pause, repeat
 * these are read-only indicators, not pressable buttons, since the board has no input
 */
void drawStatusIcons() {
  // shuffle icon, top-left: simple crossing arrows
  int shuffleX = 4;
  int shuffleY = 6;
  u8g2.drawLine(shuffleX, shuffleY - 3, shuffleX + 8, shuffleY + 3);
  u8g2.drawLine(shuffleX, shuffleY + 3, shuffleX + 8, shuffleY - 3);
  if (shuffleState) {
    u8g2.drawDisc(shuffleX + 4, shuffleY + 7, 1); // dot underneath means active, matches spotify's own UI
  }

  // play/pause icon, top-center
  // matches spotify's own convention: pause icon shows while playing, play icon shows while paused
  int centerX = 64;
  int centerY = 6;
  if (isPlaying) {
    u8g2.drawBox(centerX - 3, centerY - 4, 2, 8);
    u8g2.drawBox(centerX + 1, centerY - 4, 2, 8);
  } else {
    u8g2.drawTriangle(centerX - 3, centerY - 4, centerX - 3, centerY + 4, centerX + 4, centerY);
  }

  // repeat icon, top-right: a loop ring plus an arrow tick
  int repeatX = 116;
  int repeatY = 6;
  u8g2.drawCircle(repeatX, repeatY, 5, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT | U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  u8g2.drawTriangle(repeatX + 4, repeatY - 5, repeatX + 8, repeatY - 5, repeatX + 6, repeatY - 2);

  if (repeatState == "context") {
    u8g2.drawDisc(repeatX, repeatY + 9, 1); // dot means repeat-all is active
  } else if (repeatState == "track") {
    u8g2.drawDisc(repeatX, repeatY + 9, 1);
    u8g2.setFont(u8g2_font_4x6_tr);
    u8g2.drawStr(repeatX - 2, repeatY + 3, "1"); // small "1" means repeat just this track
  }
}

/**
 * Draws the disk icon, song title (static if short, ticker scroll if long),
 * and a fixed artist name below it. Picks the right font per script.
 */
void drawScreen() {
  u8g2.clearBuffer();

  drawDiskIcon(20, 32, 18, diskAngle);

  if (currentSong == "") {
    u8g2.setFont(u8g2_font_unifont_tr);

    String timeStr = getCurrentTimeString();
    if (timeStr != "") {
      u8g2.drawStr(TEXT_START_X, 28, timeStr.c_str());
    } else {
      u8g2.drawStr(TEXT_START_X, 28, "nothing");
    }

    if (historyCount > 0) {
      u8g2.setClipWindow(TEXT_START_X, 0, TEXT_END_X, 64);

      const uint8_t* histFont = pickFontForText(historySong[historyDisplayIndex]);
      u8g2.setFont(histFont);

      String histText = getHistoryDisplayText();
      int histWidth = u8g2.getUTF8Width(histText.c_str());

      if (histWidth <= TEXT_AREA_WIDTH) {
        u8g2.drawUTF8(TEXT_START_X, 44, histText.c_str());
      } else {
        int loopWidth = histWidth + TICKER_GAP;
        int x1 = TEXT_START_X - historyScrollX;
        int x2 = x1 + loopWidth;
        u8g2.drawUTF8(x1, 44, histText.c_str());
        u8g2.drawUTF8(x2, 44, histText.c_str());
      }

      u8g2.setMaxClipWindow();
    } else if (getCurrentTimeString() == "") {
      u8g2.drawStr(TEXT_START_X, 44, "playing...");
    }

    u8g2.sendBuffer();
    return;
  }

  drawStatusIcons();

  u8g2.setClipWindow(TEXT_START_X, 0, TEXT_END_X, 64);

  // song title
  const uint8_t* songFont = pickFontForText(currentSong);
  u8g2.setFont(songFont);

  String songDisplay = getDisplayText(currentSong);
  int songWidth = u8g2.getUTF8Width(songDisplay.c_str());

  if (songWidth <= TEXT_AREA_WIDTH) {
    // fits fine, just draw it fixed, no scrolling
    u8g2.drawUTF8(TEXT_START_X, 28, songDisplay.c_str());
  } else {
    // too wide to fit, scroll as a looping ticker
    int loopWidth = songWidth + TICKER_GAP;

    int x1 = TEXT_START_X - songScrollX;
    int x2 = x1 + loopWidth;

    u8g2.drawUTF8(x1, 28, songDisplay.c_str());
    u8g2.drawUTF8(x2, 28, songDisplay.c_str());
  }

  // artist name, scrolls if too wide, fixed if it fits
  const uint8_t* artistFont = pickFontForText(currentArtist);
  u8g2.setFont(artistFont);

  String artistDisplay = getDisplayText(currentArtist);
  int artistWidth = u8g2.getUTF8Width(artistDisplay.c_str());

  if (artistWidth <= TEXT_AREA_WIDTH) {
    u8g2.drawUTF8(TEXT_START_X, 44, artistDisplay.c_str());
  } else {
    int loopWidth = artistWidth + TICKER_GAP;

    int x1 = TEXT_START_X - artistScrollX;
    int x2 = x1 + loopWidth;

    u8g2.drawUTF8(x1, 44, artistDisplay.c_str());
    u8g2.drawUTF8(x2, 44, artistDisplay.c_str());
  }

  u8g2.setMaxClipWindow();

  drawProgressBar();

  u8g2.sendBuffer();
}

void setup() {
  Serial.begin(115200);

  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW);
  delay(100);

  u8g2.begin();
  u8g2.enableUTF8Print();

  connectWifi();

  // sync real time over the internet, assuming US Eastern time (adjust the TZ string below if needed)
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", "EST5EDT,M3.2.0,M11.1.0/2", 1);
  tzset();

  refreshAccessToken();
  pollCurrentlyPlaying();
  updateBrightnessForTimeOfDay();
}

void loop() {
  unsigned long now = millis();

  if (now - lastWifiCheckTime > WIFI_CHECK_INTERVAL) {
    reconnectWifiIfNeeded();
    lastWifiCheckTime = now;
  }

  if (now - lastPollTime > POLL_INTERVAL) {
    pollCurrentlyPlaying();
    lastPollTime = now;
  }

  if (now - lastScrollTime > SCROLL_INTERVAL) {
    u8g2.setFont(pickFontForText(currentSong));
    int songWidth = u8g2.getUTF8Width(getDisplayText(currentSong).c_str());
    if (songWidth > TEXT_AREA_WIDTH) {
      int loopWidth = songWidth + TICKER_GAP;

      songScrollX += 2;
      if (songScrollX >= loopWidth) {
        songScrollX = 0; // lands exactly on the first character, then keeps looping
      }
    }

    u8g2.setFont(pickFontForText(currentArtist));
    int artistWidth = u8g2.getUTF8Width(getDisplayText(currentArtist).c_str());
    if (artistWidth > TEXT_AREA_WIDTH) {
      int loopWidth = artistWidth + TICKER_GAP;

      artistScrollX += 2;
      if (artistScrollX >= loopWidth) {
        artistScrollX = 0;
      }
    }

    if (currentSong == "" && historyCount > 0) {
      u8g2.setFont(pickFontForText(historySong[historyDisplayIndex]));
      String histText = getHistoryDisplayText();
      int histWidth = u8g2.getUTF8Width(histText.c_str());

      if (histWidth > TEXT_AREA_WIDTH) {
        // long entry, scroll it, and move to the next track once the loop finishes
        int loopWidth = histWidth + TICKER_GAP;
        historyScrollX += 2;
        if (historyScrollX >= loopWidth) {
          historyScrollX = 0;
          historyDisplayIndex = (historyDisplayIndex + 1) % historyCount;
        }
      } else if (now - lastHistoryCycleTime > HISTORY_CYCLE_INTERVAL) {
        // short entry, nothing to scroll, just hold it a bit then move on
        historyDisplayIndex = (historyDisplayIndex + 1) % historyCount;
        lastHistoryCycleTime = now;
      }
    }

    lastScrollTime = now;
  }

  if (now - lastSpinTime > SPIN_INTERVAL) {
    if (isPlaying) {
      spinStep = (spinStep + 1) % SPIN_STEPS_PER_ROTATION;
      diskAngle = spinStep * (TWO_PI / SPIN_STEPS_PER_ROTATION); // step 0 = exactly 3 o'clock
    } else if (currentSong == "") {
      spinStep = 0;
      diskAngle = -HALF_PI; // nothing loaded at all, snap to 12 o'clock
    }
    // paused mid-song: don't touch diskAngle, freezes wherever the spoke currently is
    lastSpinTime = now;
  }

  if (now - lastBrightnessCheckTime > BRIGHTNESS_CHECK_INTERVAL) {
    updateBrightnessForTimeOfDay();
    lastBrightnessCheckTime = now;
  }

  drawScreen();
  delay(30);
}
