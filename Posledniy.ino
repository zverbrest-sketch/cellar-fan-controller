/*
  Контроллер вентиляции погреба — ИСПРАВЛЕННАЯ ВЕРСИЯ
  ESP32 DevKit + два датчика SHT30/SHT31 + FastBot2 1.2.8 + реле + журнал LittleFS
  Среда разработки: Arduino IDE 1.8.19

  ПЕРЕД ЗАГРУЗКОЙ:
  1) Укажите имя и пароль Wi-Fi, а также токен Telegram-бота.
  2) Укажите числовой идентификатор своего чата Telegram
     (пока он не задан, бот в ответ на любое сообщение пришлёт ваш chat ID).
  3) Проверьте логику реле при ОТКЛЮЧЁННОМ от сети 230 В вентиляторе.
  4) Часовой пояс — московское время (UTC+3).
  5) Проверьте обозначения контактов и правильность подключения модуля реле.
  6) [OTA-REMOTE] Укажите FIRMWARE_URL — прямую HTTPS-ссылку на файл .bin
     (например, из GitHub Releases). Тогда команда /update прошьёт плату
     из любой точки мира.

  ЧТО ИСПРАВЛЕНО (метки [ИСПР. N] в коде):
  1  Защита погреба от выстуживания (минимальная температура, настраивается /set_mint).
  2  Один неудачный запрос к Telegram больше не гасит вентилятор: работает таймаут 60 с.
  3  Накопленные в очереди Telegram команды не выполняются сразу после (пере)подключения.
  4  Ошибка датчика: AUTO выключается с записью в журнал и уведомлением (очередь уведомлений).
  5  LittleFS форматируется при первом запуске (иначе журнала не было бы вообще).
  6  Журнал: файл на каждые сутки (без копирования), очистка после синхронизации времени,
     контроль свободного места, команда /log для просмотра последних записей.
  7  Датчики, не ответившие при старте, переинициализируются автоматически.
  8  Сторожевой таймер (watchdog): при зависании контроллер перезагрузится, реле будет ВЫКЛ.
  + мелочи: порядок инициализации реле, минимальное время работы/паузы, пустые поля в CSV,
    молчание в ответ на чужие сообщения, небольшая задержка в loop().
  + [OTA] Прошивка по Wi-Fi с телефона: страница http://<IP>/update (логин/пароль — OTA_USER/OTA_PASS).
    Первую прошивку с этой страницей нужно залить по USB. Файл .bin собирается на компьютере:
    Скетч → Экспортировать скомпилированный бинарный файл.
  + [OTA] ArduinoOTA: прошивка прямо из ArduinoDroid / Arduino IDE по Wi-Fi (устройство "cellar-fan",
    порт 3232, пароль OTA_PASS). Это второй способ; можно пользоваться любым из двух.
  + [OTA-REMOTE] Удалённое обновление прошивки по команде /update из Telegram (или /ota).
    Работает из любой точки мира: ESP32 сама скачивает .bin по HTTPS и перепрошивается.
    Доступ — только для авторизованного чата. Требуется FIRMWARE_URL на прямой .bin.
    При ошибке watchdog восстанавливается, вентилятор остаётся выключенным, владелец получает
    уведомление через очередь.
  + [OTA-FIX 1] Защита от цикла перезагрузок: /update теперь требует одноразовый код подтверждения
    (/update → бот присылает код → /update КОД). Код хранится только в RAM и сбрасывается при
    перезагрузке, поэтому старая команда /update из очереди Telegram ничего не прошьёт.
  + [OTA-FIX 2] Скачивание с GitHub Releases: включено следование редиректам (HTTP 302).
  + [OTA-FIX 3] Web-OTA: если клиент (телефон/браузер) отвалился посреди заливки .bin, а
    UPLOAD_FILE_ABORTED не пришёл, watchdog и otaUploadStarted восстанавливаются из otaWebLoop().
    Раньше плата оставалась без WDT до перезагрузки.
  + [FIX WiFi] Реконнект: WiFi.reconnect() вместо disconnect()+begin(), интервал 30 с,
    в setup() включён WiFi.setAutoReconnect(true). На части ядер disconnect() в цикле «застревает».
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <FastBot2.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>
#include <math.h>
#include <esp_task_wdt.h>
#include <WebServer.h>    // [OTA] страница загрузки прошивки
#include <Update.h>       // [OTA] запись прошивки во flash
#include <ArduinoOTA.h>   // [OTA] прошивка из Arduino IDE / ArduinoDroid по Wi-Fi
#include <HTTPUpdate.h>   // [OTA-REMOTE] удалённая прошивка по команде из Telegram

// ---------------- НАСТРОЙКИ ПОЛЬЗОВАТЕЛЯ ----------------
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
#define BOT_TOKEN "YOUR_TELEGRAM_BOT_TOKEN"
#define AUTHORIZED_CHAT_ID "YOUR_NUMERIC_CHAT_ID"

// [OTA] Логин и пароль страницы обновления прошивки (http://<IP-адрес>/update).
// Тот же OTA_PASS служит паролем для ArduinoOTA (прошивка из Arduino IDE / ArduinoDroid, порт 3232).
// Пароль не короче 8 символов; пока он не изменён, обновление по Wi-Fi остаётся ОТКЛЮЧЕНО,
// иначе любой в вашей сети мог бы залить свою прошивку в устройство, управляющее реле.
#define OTA_USER "admin"
#define OTA_PASS "CHANGE_ME_OTA_PASSWORD"

// [OTA-REMOTE] Прямая HTTPS-ссылка на файл .bin (например, из GitHub Releases).
// Ссылка должна вести именно на файл, а не на страницу релиза.
// Пример: "https://github.com/you/repo/releases/download/v1.0.0/firmware.bin"
// Пока значение не изменено (содержит "ВАШ_" / "YOUR_"), команда /update отвечает отказом.
#define FIRMWARE_URL "YOUR_FIRMWARE_BIN_URL"

static const int OUTDOOR_SDA = 21;
static const int OUTDOOR_SCL = 22;
static const int CELLAR_SDA  = 18;
static const int CELLAR_SCL  = 19;
static const uint8_t SHT_ADDR = 0x44;

static const int RELAY_PIN = 25;
// false — реле включается высоким уровнем GPIO; true — низким уровнем GPIO.
// Проверяйте полярность только при отключённом от сети вентиляторе.
static const bool RELAY_ACTIVE_LOW = false;

// Московское время: UTC+3 круглый год, без перехода на летнее время.
static const char* TZ_INFO = "MSK-3";

static const uint32_t SENSOR_INTERVAL_MS    = 60000UL;
// [ИСПР. 6] Запись журнала раз в 10 минут (было 5): 30 суток занимают ~650 КБ
// и гарантированно помещаются в раздел LittleFS (обычно 1,5 МБ).
static const uint32_t LOG_INTERVAL_MS       = 600000UL;
static const uint32_t HEARTBEAT_INTERVAL_MS = 30000UL;
static const uint32_t TELEGRAM_TIMEOUT_MS   = 60000UL;  // сколько вентилятор может работать без связи
static const uint32_t AUTO_INTERVAL_MS      = 10000UL;

// [FIX WiFi] Интервал между попытками реконнекта Wi-Fi. Было 10 с с disconnect()+begin(),
// что на части ядер ESP32 приводило к «застреванию» в состоянии реконнекта.
// Теперь 30 с и WiFi.reconnect() (без disconnect).
static const uint32_t WIFI_RECONNECT_INTERVAL_MS = 30000UL;

// [ИСПР. 1] Защита от промерзания: при T погреба ниже порога вентилятор останавливается,
// повторный пуск в AUTO — только когда температура поднимется на FREEZE_HYST_C выше порога.
static const float FREEZE_HYST_C = 1.0f;

// Длительность циклов AUTO по умолчанию (можно менять командами Telegram).
// Эти параметры задают максимальное время одного включения и паузу перед следующим запуском.
// Аварийные остановки и остановка при достаточно сухом наружном воздухе выполняются раньше.
static const uint16_t DEFAULT_AUTO_ON_MIN  = 30;
static const uint16_t DEFAULT_AUTO_OFF_MIN = 15;
static const uint16_t AUTO_DURATION_MIN = 1;
static const uint16_t AUTO_DURATION_MAX = 1440; // до 24 часов

// [ИСПР. 3] «Тихое окно» после восстановления связи: команды, меняющие состояние,
// в это время игнорируются, чтобы не исполнить устаревшие команды из очереди Telegram.
static const uint32_t COMMAND_QUIET_MS = 8000UL;

// [OTA-FIX 1] Время действия одноразового кода подтверждения /update.
static const uint32_t OTA_CONFIRM_WINDOW_MS = 60000UL;

// [ИСПР. 8] Таймаут сторожевого таймера, секунды. Должен быть больше самой долгой блокировки
// в loop() (запрос getMe до ~9 с, опрос Telegram).
// [FIX] Увеличен до 60 с: один проход loop() может включать getMe (~9 с) + bot.tick() (~21 с),
//        итого до ~30 с. 40 с — слишком плотно, возможны ложные перезагрузки при плохой связи.
static const uint32_t WDT_TIMEOUT_S = 60;

// [ИСПР. 6] Журнал: по одному файлу на сутки /log_ГГГГММДД.csv, хранится LOG_KEEP_DAYS суток.
static const int      LOG_KEEP_DAYS   = 30;
static const uint32_t LOW_SPACE_BYTES = 16384UL;
static const uint8_t  LOG_TAIL_MAX    = 10;
static const uint32_t MIN_VALID_EPOCH = 1700000000UL;  // всё, что раньше, считаем «время не задано»
static const char* LOG_HEADER =
  "epoch,local_time,out_temp_c,out_rh_pct,out_ah_gm3,cellar_temp_c,cellar_rh_pct,cellar_ah_gm3,delta_ah_gm3,mode,fan,note";

// ---------------- ОБЪЕКТЫ ----------------
Adafruit_SHT31 shtOutdoor(&Wire);
Adafruit_SHT31 shtCellar(&Wire1);
FastBot2 bot(BOT_TOKEN);
Preferences prefs;

// ---------------- РЕЖИМЫ И СОСТОЯНИЕ ----------------
enum FanMode : uint8_t { MODE_OFF = 0, MODE_ON = 1, MODE_AUTO = 2 };
FanMode fanMode = MODE_OFF;
bool relayOn = false;
bool outdoorOK = false;
bool cellarOK = false;
bool fsOK = false;
bool clockOK = false;
bool telegramHealthy = false;       // результат ПОСЛЕДНЕЙ проверки getMe
bool everTelegramHealthy = false;
bool connectionLossHandled = false; // вентилятор был остановлен из-за потери связи
bool pruneNeeded = true;            // [ИСПР. 6] очистка журнала при первой возможности

// [OTA] Состояние всех OTA-механизмов (вынесено сюда, чтобы handleCommand() мог проверять).
bool webStarted = false;            // [OTA] web-сервер запущен
bool otaUploadStarted = false;      // [OTA] идёт загрузка через /update на веб-странице
bool otaOk = false;                 // [OTA] результат web-загрузки
bool remoteOtaInProgress = false;   // [OTA-REMOTE] идёт удалённое обновление из Telegram
bool arduinoOtaStarted = false;     // [OTA] ArduinoOTA инициализирован
bool otaWdtDetached = false;        // [OTA] WDT снят на время OTA (ArduinoOTA)

// [OTA-FIX 1] Одноразовый код подтверждения удалённого обновления. Только в RAM:
// после перезагрузки код пуст, и повторно полученная старая команда /update ничего не прошьёт.
String otaConfirmCode = "";
uint32_t otaConfirmUntilMs = 0;

float onThreshold = 2.0f;
float offThreshold = 1.0f;
float minCellarT = 3.0f;            // [ИСПР. 1] минимальная температура погреба, °C
uint8_t scheduleStart = 10;
uint8_t scheduleEnd = 19;
uint16_t autoOnMinutes = DEFAULT_AUTO_ON_MIN;
uint16_t autoOffMinutes = DEFAULT_AUTO_OFF_MIN;

float outT = NAN, outRH = NAN, cellarT = NAN, cellarRH = NAN;
float outAH = NAN, cellarAH = NAN, deltaAH = NAN;
uint32_t lastSensorMs = 0;
uint32_t lastLogMs = 0;
uint32_t lastAutoMs = 0;
uint32_t lastHeartbeatMs = 0;
uint32_t lastGoodTelegramMs = 0;
uint32_t lastWiFiAttemptMs = 0;
uint32_t lastPruneMs = 0;
uint32_t lastRelayChangeMs = 0;
uint32_t commandBlockUntilMs = 0;
bool commandBlockActive = false;
String lastReason = "Запуск: безопасный режим OFF";
String pendingNotice = "";          // [ИСПР. 4] очередь уведомлений владельцу

// Предварительные объявления функций, вызываемых раньше своего определения.
void enforceSensorSafety();
void notifyOwner(const String& text);
void logEvent(const String& event);

// ---------------- ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ----------------
bool credentialsConfigured() {
  return strlen(WIFI_SSID) > 0 && strcmp(WIFI_SSID, "YOUR_WIFI_NAME") != 0 &&
         strlen(WIFI_PASS) > 0 && strcmp(WIFI_PASS, "YOUR_WIFI_PASSWORD") != 0 &&
         strlen(BOT_TOKEN) > 20 && strcmp(BOT_TOKEN, "YOUR_TELEGRAM_BOT_TOKEN") != 0;
}
bool authConfigured() {
  return strlen(AUTHORIZED_CHAT_ID) > 0 &&
         strcmp(AUTHORIZED_CHAT_ID, "YOUR_NUMERIC_CHAT_ID") != 0;
}
bool isAuthorized(const String& chatID) {
  return authConfigured() && chatID == String(AUTHORIZED_CHAT_ID);
}
String modeText() {
  if (fanMode == MODE_ON) return "РУЧНОЙ ВКЛ";
  if (fanMode == MODE_AUTO) return "АВТО";
  return "ВЫКЛ";
}
String relayText() { return relayOn ? "ВКЛЮЧЁН" : "ВЫКЛЮЧЕН"; }

void writeRelay(bool on) {
  if (on != relayOn) lastRelayChangeMs = millis();  // отсчёт длительности текущего включения или паузы
  relayOn = on;
  bool pinHigh = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(RELAY_PIN, pinHigh ? HIGH : LOW);
}
void stopFan(const String& reason) {
  bool wasOn = relayOn;
  writeRelay(false);
  lastReason = reason;
  if (wasOn) Serial.println("Вентилятор остановлен: " + reason);
}
void setMode(FanMode newMode, const String& reason) {
  fanMode = newMode;
  lastReason = reason;
  if (newMode == MODE_OFF) {
    stopFan(reason);
  } else if (newMode == MODE_ON) {
    // Все проверки (связь, температура) выполнены в handleCommand();
    // здесь — последний рубеж.
    if (telegramHealthy && WiFi.status() == WL_CONNECTED) writeRelay(true);
    else { stopFan("Нет связи: ручной запуск запрещён"); fanMode = MODE_OFF; }
  }
  // MODE_AUTO: реле включит/выключит runAutoController().
}

float absoluteHumidity(float temperature, float rh) {
  if (!isfinite(temperature) || !isfinite(rh) || rh < 0.0f || rh > 100.0f ||
      temperature < -50.0f || temperature > 80.0f) return NAN;
  const float saturation = 6.112f * expf((17.67f * temperature) / (temperature + 243.5f));
  const float vaporPressure = saturation * rh / 100.0f;
  return 216.7f * vaporPressure / (273.15f + temperature);
}
bool sensorsValid() {
  return outdoorOK && cellarOK && isfinite(outT) && isfinite(outRH) &&
         isfinite(cellarT) && isfinite(cellarRH) &&
         isfinite(outAH) && isfinite(cellarAH) && isfinite(deltaAH);
}
// Число → строка через snprintf: не зависит от перегрузок String(float, ...), которые в некоторых
// версиях ядра ESP32 дают ошибку «call to constructor of 'String' is ambiguous».
String numToStr(float v, int digits) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%.*f", digits, (double)v);
  return String(buf);
}
// Аргумента по умолчанию нет намеренно: автопрототипы ArduinoDroid копируют его
// и дают ошибку «redefinition of default argument». Вместо него — перегрузка.
String fmtFloat(float v, int digits) {
  if (!isfinite(v)) return "нет данных";
  return numToStr(v, digits);
}
String fmtFloat(float v) { return fmtFloat(v, 1); }
// [ИСПР. мелочь] Для CSV — пустое поле вместо текста «нет данных».
String csvNum(float v, int digits) {
  return isfinite(v) ? numToStr(v, digits) : String("");
}
bool updateClock() {
  struct tm ti;
  clockOK = getLocalTime(&ti, 20);
  return clockOK;
}
String localTimeText() {
  struct tm ti;
  if (!getLocalTime(&ti, 20)) return "время не синхронизировано";
  char b[24];
  strftime(b, sizeof(b), "%d.%m.%Y %H:%M:%S", &ti);
  return String(b);
}
bool insideSchedule() {
  struct tm ti;
  if (!getLocalTime(&ti, 20)) return false;
  int h = ti.tm_hour;
  if (scheduleStart < scheduleEnd) return h >= scheduleStart && h < scheduleEnd;
  return h >= scheduleStart || h < scheduleEnd; // Например, расписание с 22:00 до 06:00.
}

// [ИСПР. 2] «Связь жива», если последняя УСПЕШНАЯ проверка Telegram была не более
// TELEGRAM_TIMEOUT_MS назад. Один неудачный запрос вентилятор не останавливает.
// Потеря Wi-Fi тоже попадает под этот таймаут (getMe перестаёт отвечать).
// Хотите жёстче (остановка сразу при пропаже Wi-Fi) — добавьте проверку WiFi.status() здесь.
bool linkAlive() {
  return everTelegramHealthy &&
         (uint32_t)(millis() - lastGoodTelegramMs) < TELEGRAM_TIMEOUT_MS;
}

// ---------------- ДАННЫЕ ДАТЧИКОВ ----------------
void readSensors() {
  // [ИСПР. 7 + FIX] Если датчик не ответил при старте ИЛИ перестал читаться — пробуем
  // инициализировать снова. Раньше флаг outdoorOK/cellarOK не сбрасывался при отказе
  // чтений, и датчик, сломавшийся после старта, не восстанавливался без перезагрузки.
  if (!outdoorOK || !isfinite(outT)) outdoorOK = shtOutdoor.begin(SHT_ADDR);
  if (!cellarOK  || !isfinite(cellarT)) cellarOK  = shtCellar.begin(SHT_ADDR);

  float t1 = shtOutdoor.readTemperature();
  float h1 = shtOutdoor.readHumidity();
  float t2 = shtCellar.readTemperature();
  float h2 = shtCellar.readHumidity();

  if (outdoorOK && isfinite(t1) && isfinite(h1) && h1 >= 0 && h1 <= 100) {
    outT = t1; outRH = h1; outAH = absoluteHumidity(t1, h1);
  } else { outT = outRH = outAH = NAN; }

  if (cellarOK && isfinite(t2) && isfinite(h2) && h2 >= 0 && h2 <= 100) {
    cellarT = t2; cellarRH = h2; cellarAH = absoluteHumidity(t2, h2);
  } else { cellarT = cellarRH = cellarAH = NAN; }

  deltaAH = (isfinite(cellarAH) && isfinite(outAH)) ? cellarAH - outAH : NAN;

  // [ИСПР. 4] Реакция на ошибку датчика и на замерзание вынесена в отдельную функцию.
  enforceSensorSafety();
}

// ---------------- СОХРАНЕНИЕ НАСТРОЕК ----------------
void loadSettings() {
  // Открываем с правом записи (false): при первом запуске пространства имён ещё нет,
  // и в режиме «только чтение» NVS сыпал бы ошибками в Serial.
  prefs.begin("cellarCtl", false);
  onThreshold = prefs.getFloat("onAH", 2.0f);
  offThreshold = prefs.getFloat("offAH", 1.0f);
  minCellarT = prefs.getFloat("minT", 3.0f);
  scheduleStart = prefs.getUChar("start", 10);
  scheduleEnd = prefs.getUChar("end", 19);
  autoOnMinutes = prefs.getUShort("autoOnMin", DEFAULT_AUTO_ON_MIN);
  autoOffMinutes = prefs.getUShort("autoOffMin", DEFAULT_AUTO_OFF_MIN);
  prefs.end();

  if (!isfinite(onThreshold) || !isfinite(offThreshold) ||
      offThreshold < 0.0f || onThreshold <= offThreshold ||
      onThreshold > 20.0f || scheduleStart > 23 || scheduleEnd > 23 ||
      scheduleStart == scheduleEnd) {
    onThreshold = 2.0f; offThreshold = 1.0f;
    scheduleStart = 10; scheduleEnd = 19;
  }
  if (autoOnMinutes < AUTO_DURATION_MIN || autoOnMinutes > AUTO_DURATION_MAX)
    autoOnMinutes = DEFAULT_AUTO_ON_MIN;
  if (autoOffMinutes < AUTO_DURATION_MIN || autoOffMinutes > AUTO_DURATION_MAX)
    autoOffMinutes = DEFAULT_AUTO_OFF_MIN;
  if (!isfinite(minCellarT) || minCellarT < -5.0f || minCellarT > 15.0f) minCellarT = 3.0f;
}
void saveSettings() {
  prefs.begin("cellarCtl", false);
  prefs.putFloat("onAH", onThreshold);
  prefs.putFloat("offAH", offThreshold);
  prefs.putFloat("minT", minCellarT);
  prefs.putUChar("start", scheduleStart);
  prefs.putUChar("end", scheduleEnd);
  prefs.putUShort("autoOnMin", autoOnMinutes);
  prefs.putUShort("autoOffMin", autoOffMinutes);
  prefs.end();
}

// ---------------- ЖУРНАЛ CSV (ПО ФАЙЛУ НА СУТКИ) ----------------
String csvEscape(String s) {
  s.replace("\"", "\"\"");
  return "\"" + s + "\"";
}
String dateStamp(time_t t) {
  struct tm ti;
  localtime_r(&t, &ti);
  char b[12];
  strftime(b, sizeof(b), "%Y%m%d", &ti);
  return String(b);
}
String logPathForTime(time_t t) { return "/log_" + dateStamp(t) + ".csv"; }
String formatTime(time_t t) {
  struct tm ti;
  localtime_r(&t, &ti);
  char b[24];
  strftime(b, sizeof(b), "%d.%m.%Y %H:%M:%S", &ti);
  return String(b);
}

// [ИСПР. 6] Запись одной строки. Теперь:
//  - не зависит от флага clockOK (время проверяется прямо здесь);
//  - в колонку note попадает только текст события (у периодических записей она пустая,
//    раньше там повторялась последняя причина и это сбивало с толку);
//  - при нехватке места запись пропускается, а очистка запрашивается.
void logRecord(const String& note) {
  if (!fsOK) return;
  time_t now = time(nullptr);
  if (now < (time_t)MIN_VALID_EPOCH) return;   // время не синхронизировано — имя файла неизвестно

  size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (freeBytes < LOW_SPACE_BYTES) pruneNeeded = true;
  if (freeBytes < 4096) return;

  String path = logPathForTime(now);
  bool isNew = !LittleFS.exists(path);
  File f = LittleFS.open(path, FILE_APPEND);
  if (!f) return;
  if (isNew) f.println(LOG_HEADER);

  String line = String((unsigned long)now); line += ",";
  line += csvEscape(formatTime(now)); line += ",";
  line += csvNum(outT, 2); line += ",";
  line += csvNum(outRH, 2); line += ",";
  line += csvNum(outAH, 3); line += ",";
  line += csvNum(cellarT, 2); line += ",";
  line += csvNum(cellarRH, 2); line += ",";
  line += csvNum(cellarAH, 3); line += ",";
  line += csvNum(deltaAH, 3); line += ",";
  line += csvEscape(modeText()); line += ",";
  line += relayOn ? "1" : "0"; line += ",";
  line += csvEscape(note);
  f.println(line);
  f.close();
}
void logEvent(const String& event) {
  lastReason = event;
  logRecord(event);
}

// [ИСПР. 6] Удаляем файлы старше LOG_KEEP_DAYS суток. Никакого копирования файлов
// (прежняя схема «переписать во временный файл» требовала вдвое больше места и могла
// потерять журнал при сбое). Если места всё равно мало — удаляем самый старый файл.
void pruneOldLogs() {
  if (!fsOK) return;
  time_t now = time(nullptr);
  if (now < (time_t)MIN_VALID_EPOCH) return;

  String cutoff = dateStamp(now - (time_t)LOG_KEEP_DAYS * 86400L);
  String today = dateStamp(now);
  String expired[40];
  uint8_t nExpired = 0;
  String oldest = "";

  // Сначала собираем имена, потом удаляем: удалять файлы во время обхода каталога нельзя.
  File root = LittleFS.open("/");
  if (!root) return;
  File f = root.openNextFile();
  while (f) {
    String name = String(f.name());
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);  // в разных версиях ядра имя с "/" или без
    if (name.length() == 16 && name.startsWith("log_") && name.endsWith(".csv")) {
      String d = name.substring(4, 12);
      if (d < cutoff && nExpired < 40) expired[nExpired++] = name;
      if (d != today && (oldest.length() == 0 || d < oldest.substring(4, 12))) oldest = name;
    }
    f = root.openNextFile();
  }
  root.close();

  for (uint8_t i = 0; i < nExpired; i++) LittleFS.remove("/" + expired[i]);

  size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (freeBytes < LOW_SPACE_BYTES && oldest.length()) LittleFS.remove("/" + oldest);
}

// [ИСПР. 6] Последние n записей сегодняшнего журнала — для команды /log
// (раньше журнал можно было только записывать, но не прочитать).
String tailOfTodayLog(uint8_t n) {
  if (!fsOK) return "Журнал недоступен: LittleFS не смонтирована.";
  time_t now = time(nullptr);
  if (now < (time_t)MIN_VALID_EPOCH) return "Время не синхронизировано, журнал не ведётся.";
  File f = LittleFS.open(logPathForTime(now), FILE_READ);
  if (!f) return "За сегодня записей пока нет.";
  if (n < 1) n = 1;
  if (n > LOG_TAIL_MAX) n = LOG_TAIL_MAX;

  String ring[LOG_TAIL_MAX];
  uint8_t head = 0, count = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length() || line.startsWith("epoch,")) continue;
    ring[head] = line;
    head = (head + 1) % n;
    if (count < n) count++;
  }
  f.close();
  if (count == 0) return "За сегодня записей пока нет.";

  String out = "Последние записи (" + String(count) + "):\n";
  uint8_t idx = (head + n - count) % n;
  for (uint8_t i = 0; i < count; i++) {
    out += ring[idx] + "\n";
    idx = (idx + 1) % n;
  }
  return out;
}

// ---------------- ИНТЕРФЕЙС TELEGRAM ----------------
void sendText(const String& chatID, const String& text) {
  fb::Message msg(text, chatID);
  bot.sendMessage(msg);
}
void sendMainKeyboard(const String& chatID) {
  fb::Message msg("Управление вентиляцией погреба:", chatID);
  fb::Keyboard keyboard;
  keyboard.addButton("🤖 AUTO");
  keyboard.addButton("🟢 ВКЛЮЧИТЬ");
  keyboard.addButton("🔴 ВЫКЛЮЧИТЬ");
  keyboard.addButton("📊 Статус");
  keyboard.addButton("⚙️ Настройки AUTO");
  keyboard.resize = true;
  keyboard.persistent = true;
  msg.setKeyboard(&keyboard);
  bot.sendMessage(msg);
}
String statusText() {
  String s = "📊 МОНИТОРИНГ ПОГРЕБА\n\n";
  s += "УЛИЦА\nT: " + fmtFloat(outT) + " °C\nRH: " + fmtFloat(outRH) + " %\nАбсолютная влажность: " + fmtFloat(outAH, 2) + " г/м³\n\n";
  s += "ПОГРЕБ\nT: " + fmtFloat(cellarT) + " °C\nRH: " + fmtFloat(cellarRH) + " %\nАбсолютная влажность: " + fmtFloat(cellarAH, 2) + " г/м³\n\n";
  s += "Разница (погреб − улица): " + fmtFloat(deltaAH, 2) + " г/м³\n";
  s += "Режим: " + modeText() + "\nВентилятор: " + relayText() + "\n";
  s += "Расписание AUTO: " + String(scheduleStart) + ":00–" + String(scheduleEnd) + ":00\n";
  s += "Цикл AUTO: включение до " + String(autoOnMinutes) + " мин, пауза " + String(autoOffMinutes) + " мин\n";
  s += "Мин. T погреба: " + String(minCellarT, 1) + " °C\n";
  s += "Время: " + localTimeText() + "\n";
  s += "Связь Telegram: " + String(telegramHealthy ? "OK" : "НЕТ") + "\n";
  // [OTA] IP нужен для страницы обновления прошивки: http://<IP>/update
  if (WiFi.status() == WL_CONNECTED) s += "IP: " + WiFi.localIP().toString() + "\n";
  s += "Последняя причина/событие: " + lastReason;
  return s;
}
String settingsText() {
  String s = "⚙️ НАСТРОЙКИ AUTO\n";
  s += "Включение при Δ абсолютной влажности ≥ " + String(onThreshold, 2) + " г/м³\n";
  s += "Выключение при Δ абсолютной влажности < " + String(offThreshold, 2) + " г/м³\n";
  s += "Расписание: " + String(scheduleStart) + ":00–" + String(scheduleEnd) + ":00\n";
  s += "Время работы за цикл: " + String(autoOnMinutes) + " мин\n";
  s += "Пауза между циклами: " + String(autoOffMinutes) + " мин\n";
  s += "Мин. температура погреба: " + String(minCellarT, 1) + " °C\n\n";
  s += "Команды:\n/set_on 2.5\n/set_off 1.2\n/set_start 9\n/set_end 20\n/set_mint 3\n/set_on_min 30\n/set_off_min 15\n/log 10\n/update — прошить прошивку удалённо (с кодом подтверждения)\n/settings";
  return s;
}

// [ИСПР. 4] Все уведомления идут через очередь: если связи нет, сообщение не теряется,
// а уходит, как только Telegram снова станет доступен (см. flushNotices()).
void notifyOwner(const String& text) {
  if (!authConfigured()) return;
  if (pendingNotice.length() > 1000) pendingNotice = "(часть уведомлений пропущена)";
  if (pendingNotice.length()) pendingNotice += "\n\n";
  pendingNotice += text;
}
void flushNotices() {
  if (!pendingNotice.length()) return;
  if (!authConfigured() || !telegramHealthy || WiFi.status() != WL_CONNECTED) return;
  sendText(String(AUTHORIZED_CHAT_ID), pendingNotice);
  pendingNotice = "";
}

// ---------------- [OTA-REMOTE] УДАЛЁННОЕ ОБНОВЛЕНИЕ ПРОШИВКИ ЧЕРЕЗ TELEGRAM ----------------
// Вызывается из handleCommand() по команде /update КОД от авторизованного чата
// (код подтверждения выдаётся в ответ на /update без аргумента).
// ESP32 сама скачивает .bin по HTTPS и перепрошивается — работает из любой точки мира,
// потому что исходящее соединение инициирует сама плата (как и для Telegram).
//
// Поведение:
//  • выключает реле и переводит режим в OFF;
//  • снимает WDT на время скачивания и записи (может занять минуты);
//  • при успехе ESP32 перезагружается сама (rebootOnUpdate);
//  • при ошибке восстанавливает WDT, пишет событие в журнал и уведомляет владельца
//    через notifyOwner() — сообщение уйдёт, как только связь восстановится.
void performRemoteOTA(const String& chatID) {
  // Защита от повторного запуска (например, если пользователь дважды нажал /update,
  // пока бот ещё отвечал, или если параллельно идёт web-загрузка).
  if (remoteOtaInProgress || otaUploadStarted) {
    sendText(chatID, "⏳ Обновление уже запрошено или идёт. Дождитесь перезагрузки.");
    return;
  }

  // Проверка, что URL задан и это не заглушка.
  String url = String(FIRMWARE_URL);
  if (url.length() < 10 ||
      url.indexOf("YOUR_") >= 0 || url.indexOf("ВАШ_") >= 0 ||
      !url.startsWith("http")) {
    sendText(chatID,
      "❌ FIRMWARE_URL не задан или некорректен.\n"
      "Впишите в скетч прямую HTTPS-ссылку на .bin (например, GitHub Releases) "
      "и перепрошейте по USB один раз.");
    return;
  }

  remoteOtaInProgress = true;

  // Останавливаем вентилятор и фиксируем причину.
  writeRelay(false);
  fanMode = MODE_OFF;
  lastReason = "Остановлено: удалённое обновление прошивки";
  logEvent(lastReason);

  sendText(chatID,
    "🔄 Начинаю удалённое обновление.\n"
    "Вентилятор выключен. Скачивание и запись могут занять 1–3 минуты.\n"
    "Устройство перезагрузится автоматически. Не отключайте питание.");

  // Снимаем watchdog: скачивание и запись флеша могут занять > WDT_TIMEOUT_S.
  esp_task_wdt_delete(NULL);

  WiFiClientSecure client;
  // Для совместимости (как и в checkTelegramAPI) отключаем проверку сертификата.
  // Для максимальной безопасности замените на client.setCACert(...) с корневым CA хостинга.
  client.setInsecure();
  // Ограничиваем время TLS-рукопожатия, чтобы не зависнуть навсегда при плохой сети.
  client.setHandshakeTimeout(15);  // секунды

  httpUpdate.rebootOnUpdate(true);  // успешная прошивка → автоматическая перезагрузка
  // [OTA-FIX 2] GitHub Releases отдаёт файл через редирект (HTTP 302) на другой хост —
  // без этой строки скачивание заканчивалось бы ошибкой.
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  t_httpUpdate_return ret = httpUpdate.update(client, url);

  // Сюда управление доходит ТОЛЬКО при ошибке (при успехе плата уже перезагружена).
  String err;
  switch (ret) {
    case HTTP_UPDATE_FAILED:
      err = "HTTP_UPDATE_FAILED (код " + String(httpUpdate.getLastError()) + "): " +
            httpUpdate.getLastErrorString();
      break;
    case HTTP_UPDATE_NO_UPDATES:
      err = "HTTP_UPDATE_NO_UPDATES: сервер не отдал файл";
      break;
    case HTTP_UPDATE_OK:
      // Теоретически недостижимо при rebootOnUpdate(true), но пусть будет.
      err = "HTTP_UPDATE_OK, но перезагрузка не произошла";
      break;
    default:
      err = "HTTP_UPDATE неизвестный результат";
      break;
  }

  // Восстанавливаем watchdog и состояние.
  esp_task_wdt_add(NULL);
  remoteOtaInProgress = false;
  lastReason = "Ошибка удалённого обновления: " + err;
  logEvent(lastReason);
  // Уведомление — через очередь: если связи сейчас нет, уйдёт позже.
  notifyOwner("❌ Удалённое обновление не удалось.\n" + err +
              "\nВентилятор остаётся выключенным. Проверьте FIRMWARE_URL, доступность сервера и связь.");
}

// ---------------- ОБРАБОТКА КОМАНД ----------------
bool parseFloatArg(String s, float& value) {
  s.trim(); s.replace(',', '.');
  if (!s.length()) return false;
  char* endptr = nullptr;
  value = strtof(s.c_str(), &endptr);
  return endptr != s.c_str() && *endptr == '\0' && isfinite(value);
}
bool parseHourArg(String s, int& value) {
  s.trim();
  if (!s.length()) return false;
  char* endptr = nullptr;
  long v = strtol(s.c_str(), &endptr, 10);
  if (endptr == s.c_str() || *endptr != '\0' || v < 0 || v > 23) return false;
  value = (int)v;
  return true;
}

// [ИСПР. 3] Идёт ли «тихое окно» после восстановления связи.
// Флаг commandBlockActive исключает ошибку сравнения millis() после 24,8 суток работы.
bool commandsQuiet() {
  if (!commandBlockActive) return false;
  if ((int32_t)(commandBlockUntilMs - millis()) > 0) return true;
  commandBlockActive = false;
  return false;
}
bool isStateChangingCommand(const String& t) {
  return t == "🟢 ВКЛЮЧИТЬ" || t == "/on" || t == "🤖 AUTO" || t == "/auto" || t.startsWith("/set_");
}

// [ИСПР. 1] Причина, по которой нельзя запустить вентилятор вручную ("" — можно).
// Если датчик погреба неисправен, ручной запуск разрешён (решение за человеком),
// но защита от промерзания в этом случае работать не сможет.
String manualStartBlocker() {
  if (!telegramHealthy || WiFi.status() != WL_CONNECTED)
    return "связь с Telegram/Wi-Fi не подтверждена";
  if (cellarOK && isfinite(cellarT) && cellarT < minCellarT)
    return "в погребе " + fmtFloat(cellarT) + " °C — ниже минимума " + fmtFloat(minCellarT) + " °C (защита от промерзания)";
  return "";
}

void handleCommand(String text, const String& chatID) {
  text.trim();

  if (!isAuthorized(chatID)) {
    // [ИСПР. мелочь] Чужим не отвечаем (раньше бот выдавал подсказку про настройку).
    // Исключение — режим первоначальной настройки: пока chat ID не задан, сообщаем его владельцу.
    if (!authConfigured()) sendText(chatID, "Ваш chat ID: " + chatID + "\nВпишите его в AUTHORIZED_CHAT_ID и перепрошейте контроллер.");
    Serial.println("Команда от неавторизованного чата: " + chatID);
    return;
  }

  if (text == "/start") { sendMainKeyboard(chatID); sendText(chatID, statusText()); return; }
  if (text == "/status" || text == "📊 Статус") { sendText(chatID, statusText()); return; }
  if (text == "/settings" || text == "⚙️ Настройки AUTO") { sendText(chatID, settingsText()); return; }

  if (text == "/log" || text.startsWith("/log ")) {
    long v = (text.length() > 5) ? text.substring(5).toInt() : 0;
    uint8_t n = (v >= 1 && v <= LOG_TAIL_MAX) ? (uint8_t)v : LOG_TAIL_MAX;
    sendText(chatID, tailOfTodayLog(n));
    return;
  }

  // Выключение разрешено всегда и без ограничений.
  if (text == "🔴 ВЫКЛЮЧИТЬ" || text == "/off") {
    setMode(MODE_OFF, "Выключено пользователем");
    logEvent(lastReason);
    sendText(chatID, "🔴 Вентилятор выключен. Режим OFF.");
    return;
  }

  // [OTA-REMOTE + OTA-FIX 1] Удалённое обновление с одноразовым кодом подтверждения.
  // /update (без кода) → бот присылает код; /update КОД → запуск обновления.
  // Код лежит только в RAM: после перезагрузки он пуст, поэтому повторно полученная
  // из очереди Telegram старая команда /update ничего не прошьёт, а лишь запросит новый код.
  // Команда вне «тихого окна» — это осознанное действие владельца.
  if (text == "/update" || text == "/ota" ||
      text.startsWith("/update ") || text.startsWith("/ota ")) {
    int sp = text.indexOf(' ');
    String arg = (sp > 0) ? text.substring(sp + 1) : String("");
    arg.trim();
    bool codeValid = otaConfirmCode.length() &&
                     (int32_t)(otaConfirmUntilMs - millis()) > 0;
    if (arg.length() && codeValid && arg == otaConfirmCode) {
      otaConfirmCode = "";          // код одноразовый
      performRemoteOTA(chatID);
      return;
    }
    otaConfirmCode = String((unsigned long)(esp_random() % 9000UL + 1000UL));
    otaConfirmUntilMs = millis() + OTA_CONFIRM_WINDOW_MS;
    sendText(chatID, "Для подтверждения обновления отправьте в течение 60 секунд:\n/update " +
                     otaConfirmCode);
    return;
  }

  // [ИСПР. 3] Команды, меняющие состояние, не выполняем в первые секунды после
  // (пере)подключения: именно тогда Telegram отдаёт накопленную очередь старых сообщений.
  if (isStateChangingCommand(text) && commandsQuiet()) {
    sendText(chatID, "Связь только что восстановлена. Чтобы не выполнить устаревшую команду, повторите её через несколько секунд.");
    return;
  }

  if (text == "🟢 ВКЛЮЧИТЬ" || text == "/on") {
    String blocker = manualStartBlocker();
    if (blocker.length()) {
      stopFan("Ручной запуск отклонён: " + blocker);
      sendText(chatID, "Не включаю: " + blocker + ".");
      return;
    }
    setMode(MODE_ON, "Ручное включение пользователем");
    logEvent(lastReason);
    // [FIX] Предупреждаем, если датчик погреба сломан: защита от промерзания не работает.
    String warn;
    if (!cellarOK || !isfinite(cellarT))
      warn = "\n\n⚠️ Датчик погреба не читается — защита от промерзания ОТКЛЮЧЕНА. "
             "Вентилятор включён под вашу ответственность. Для остановки — /off.";
    sendText(chatID, "🟢 Ручной режим ВКЛ включён." + warn);
    return;
  }
  if (text == "🤖 AUTO" || text == "/auto") {
    if (!sensorsValid()) { sendText(chatID, "AUTO недоступен: нет корректных показаний обоих датчиков."); return; }
    if (!updateClock()) { sendText(chatID, "AUTO недоступен: время ещё не синхронизировано."); return; }
    if (!telegramHealthy) { sendText(chatID, "AUTO недоступен: нет связи с Telegram."); return; }
    setMode(MODE_AUTO, "AUTO включён пользователем");
    logEvent(lastReason);
    sendText(chatID, "🤖 Режим AUTO включён. Управление по влажности и расписанию.");
    return;
  }

  if (text.startsWith("/set_on ")) {
    float v;
    if (!parseFloatArg(text.substring(8), v) || v < 0 || v > 20 || v <= offThreshold) {
      sendText(chatID, "Ошибка: порог включения должен быть 0–20 и выше порога выключения."); return;
    }
    onThreshold = v; saveSettings(); sendText(chatID, settingsText()); return;
  }
  if (text.startsWith("/set_off ")) {
    float v;
    if (!parseFloatArg(text.substring(9), v) || v < 0 || v >= onThreshold) {
      sendText(chatID, "Ошибка: порог выключения должен быть ≥ 0 и ниже порога включения."); return;
    }
    offThreshold = v; saveSettings(); sendText(chatID, settingsText()); return;
  }
  if (text.startsWith("/set_start ")) {
    int v;
    if (!parseHourArg(text.substring(11), v) || v == scheduleEnd) {
      sendText(chatID, "Ошибка: час начала должен быть 0–23 и отличаться от часа окончания."); return;
    }
    scheduleStart = (uint8_t)v; saveSettings(); sendText(chatID, settingsText()); return;
  }
  if (text.startsWith("/set_end ")) {
    int v;
    if (!parseHourArg(text.substring(9), v) || v == scheduleStart) {
      sendText(chatID, "Ошибка: час окончания должен быть 0–23 и отличаться от часа начала."); return;
    }
    scheduleEnd = (uint8_t)v; saveSettings(); sendText(chatID, settingsText()); return;
  }
  // Минимальная температура погреба.
  if (text.startsWith("/set_mint ")) {
    float v;
    if (!parseFloatArg(text.substring(10), v) || v < -5 || v > 15) {
      sendText(chatID, "Ошибка: минимальная температура погреба должна быть от -5 до 15 °C."); return;
    }
    minCellarT = v; saveSettings(); sendText(chatID, settingsText()); return;
  }
  // Длительность включения вентилятора в AUTO, минуты (1–1440).
  if (text.startsWith("/set_on_min ")) {
    String arg = text.substring(12); arg.trim();
    char* endptr = nullptr;
    long n = strtol(arg.c_str(), &endptr, 10);
    if (arg.length() == 0 || endptr == arg.c_str() || *endptr != '\0' ||
        n < AUTO_DURATION_MIN || n > AUTO_DURATION_MAX) {
      sendText(chatID, "Ошибка: длительность включения должна быть целым числом от 1 до 1440 минут."); return;
    }
    autoOnMinutes = (uint16_t)n; saveSettings(); sendText(chatID, settingsText()); return;
  }
  // Длительность паузы в AUTO, минуты (1–1440).
  if (text.startsWith("/set_off_min ")) {
    String arg = text.substring(13); arg.trim();
    char* endptr = nullptr;
    long n = strtol(arg.c_str(), &endptr, 10);
    if (arg.length() == 0 || endptr == arg.c_str() || *endptr != '\0' ||
        n < AUTO_DURATION_MIN || n > AUTO_DURATION_MAX) {
      sendText(chatID, "Ошибка: пауза должна быть целым числом от 1 до 1440 минут."); return;
    }
    autoOffMinutes = (uint16_t)n; saveSettings(); sendText(chatID, settingsText()); return;
  }
  sendText(chatID, "Команда не распознана. Используйте кнопки или /status, /settings, /auto, /on, /off, /log, /update.");
}

void updateHandler(fb::Update& update) {
  if (!update.isMessage()) return;
  auto message = update.message();
  String text = message.text();
  String chatID = message.chat().id();
  if (text.length()) handleCommand(text, chatID);
}

// ---------------- ПРОВЕРКА ДОСТУПНОСТИ TELEGRAM ----------------
// Выполняется настоящий запрос к API Telegram (блокирует loop до ~9 с — поэтому есть watchdog
// с запасом и все проверки безопасности используют millis(), а не результат этого запроса).
// Проверка сертификата TLS отключена ради совместимости — это снижает безопасность соединения.
// Для усиления: client.setCACert(<корневой сертификат Telegram>) вместо setInsecure().
bool checkTelegramAPI() {
  if (WiFi.status() != WL_CONNECTED || !credentialsConfigured()) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  String url = String("https://api.telegram.org/bot") + BOT_TOKEN + "/getMe";
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  String body = (code == HTTP_CODE_OK) ? http.getString() : "";
  http.end();
  return code == HTTP_CODE_OK && body.indexOf("\"ok\":true") >= 0;
}

// ---------------- ПРОВЕРКИ БЕЗОПАСНОСТИ ----------------

// [ИСПР. 2] Вызывается на КАЖДОМ проходе loop(). Вентилятор (в любом режиме) останавливается,
// только если успешной проверки Telegram не было TELEGRAM_TIMEOUT_MS. Единичный сбой запроса
// ничего не меняет. Режим после потери связи — OFF, возобновление только вручную.
void enforceLinkSafety() {
  if (fanMode == MODE_OFF && !relayOn) return;
  if (linkAlive()) return;
  stopFan("Аварийная остановка: нет связи с Telegram более 60 секунд");
  fanMode = MODE_OFF;
  connectionLossHandled = true;   // после восстановления связи сообщим владельцу
  logEvent(lastReason);
}

// [ИСПР. 1 и 4] Вызывается после каждого чтения датчиков и в цикле AUTO.
void enforceSensorSafety() {
  // Ошибка датчика в AUTO: остановка + запись в журнал + уведомление (когда появится связь).
  if (fanMode == MODE_AUTO && !sensorsValid()) {
    stopFan("AUTO остановлен: ошибка датчика");
    fanMode = MODE_OFF;
    logEvent(lastReason);
    notifyOwner("⚠️ AUTO выключен: нет корректных показаний датчиков. После восстановления включите AUTO вручную.");
    return;
  }

  // Защита от промерзания — действует и в AUTO, и в ручном режиме.
  bool cellarKnown = cellarOK && isfinite(cellarT);
  if (relayOn && cellarKnown && cellarT < minCellarT) {
    String why = "Остановлено: T погреба " + fmtFloat(cellarT) + " °C ниже минимума " + fmtFloat(minCellarT) + " °C";
    if (fanMode == MODE_ON) {
      stopFan(why);
      fanMode = MODE_OFF;              // ручной режим сбрасываем: человек должен включить осознанно
    } else {
      stopFan(why);                    // AUTO остаётся включённым и сам продолжит после прогрева
    }
    logEvent(lastReason);
    notifyOwner("❄️ " + why + ".");
  }
}

// ---------------- АВТОМАТИЧЕСКОЕ УПРАВЛЕНИЕ ----------------
void runAutoController() {
  if (fanMode != MODE_AUTO) return;
  // Связь проверяет enforceLinkSafety() — здесь отдельно не дублируем.

  if (!updateClock()) {
    stopFan("AUTO остановлен: время не синхронизировано");
    fanMode = MODE_OFF;
    logEvent(lastReason);
    notifyOwner("⚠️ AUTO выключен: время не синхронизировано. После восстановления включите AUTO вручную.");
    return;
  }
  if (!sensorsValid()) return;   // обработано в enforceSensorSafety()

  if (!insideSchedule()) {
    if (relayOn) {
      stopFan("AUTO: вне расписания");
      logEvent(lastReason);
      notifyOwner("🔴 Вентилятор выключен: вне расписания AUTO.");
    }
    return;
  }

  // millis() берём заново: nowMs в loop() мог устареть из-за блокирующих запросов.
  uint32_t sinceChange = (uint32_t)(millis() - lastRelayChangeMs);

  if (!relayOn) {
    // Пуск: влажность выше порога, погреб достаточно тёплый и пауза завершена.
    bool warmEnough = cellarT >= minCellarT + FREEZE_HYST_C;
    uint32_t requiredPauseMs = (uint32_t)autoOffMinutes * 60000UL;
    if (deltaAH >= onThreshold && warmEnough && sinceChange >= requiredPauseMs) {
      writeRelay(true);
      logEvent("AUTO: начат цикл включения");
      notifyOwner("🟢 Вентилятор включён автоматически.\nΔ абсолютной влажности: " + String(deltaAH, 2) +
                  " г/м³. Время работы этого цикла — до " + String(autoOnMinutes) + " мин.");
    }
  } else {
    // Завершаем цикл по таймеру. Если воздух уже достаточно сухой, выключаем раньше.
    uint32_t requiredOnMs = (uint32_t)autoOnMinutes * 60000UL;
    if (sinceChange >= requiredOnMs) {
      stopFan("AUTO: завершена заданная длительность включения");
      logEvent(lastReason);
      notifyOwner("⏸ Вентилятор выключен: цикл работы завершён (" + String(autoOnMinutes) +
                  " мин). Пауза перед следующим запуском — " + String(autoOffMinutes) + " мин.");
    } else if (deltaAH < offThreshold) {
      stopFan("AUTO: разница абсолютной влажности ниже порога выключения");
      logEvent(lastReason);
      notifyOwner("🔴 Вентилятор выключен автоматически: Δ абсолютной влажности ниже порога выключения.");
    }
  }
}

// ---------------- [OTA] ОБНОВЛЕНИЕ ПРОШИВКИ ПО Wi-Fi (С ТЕЛЕФОНА) ----------------
// Откройте в браузере http://<IP-адрес ESP32>/update (телефон — в той же Wi-Fi сети),
// введите логин/пароль, выберите файл .bin и нажмите «Прошить».
// Авторизация идёт по HTTP без шифрования: не пробрасывайте порт 80 из роутера в интернет.
WebServer web(80);

bool otaConfigured() {
  return strlen(OTA_PASS) >= 8 && strcmp(OTA_PASS, "CHANGE_ME_OTA_PASSWORD") != 0;
}
bool otaAuthOK() { return web.authenticate(OTA_USER, OTA_PASS); }

void otaWebSetup() {
  // Страница с формой загрузки.
  web.on("/update", HTTP_GET, []() {
    if (!otaAuthOK()) return web.requestAuthentication();
    web.send(200, "text/html",
      "<meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
      "<h3>Обновление прошивки</h3>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='fw' accept='.bin'><br><br>"
      "<input type='submit' value='Прошить'></form>");
  });

  // Ответ после загрузки + сама загрузка (второй обработчик вызывается по частям).
  web.on("/update", HTTP_POST, []() {
    if (!otaAuthOK()) return web.requestAuthentication();
    if (!otaUploadStarted) { web.send(400, "text/plain", "Файл не получен"); return; }
    web.send(200, "text/plain", otaOk ? "Готово. Перезагрузка..." : "Ошибка прошивки. Перезагрузка...");
    delay(500);
    ESP.restart();   // и при успехе, и при ошибке: после перезагрузки реле гарантированно ВЫКЛ
  }, []() {
    if (!otaAuthOK()) return;
    HTTPUpload& up = web.upload();
    if (up.status == UPLOAD_FILE_START) {
      otaUploadStarted = true;
      otaOk = false;
      // На время прошивки вентилятор выключаем: loop() не выполняется, пока идёт загрузка.
      writeRelay(false);
      fanMode = MODE_OFF;
      lastReason = "Остановлено: идёт обновление прошивки";
      esp_task_wdt_delete(NULL);   // иначе watchdog перезагрузит плату посреди загрузки
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_END) {
      otaOk = Update.end(true);    // true — принять даже если размер неизвестен заранее
      if (!otaOk) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_ABORTED) {
      Update.abort();
      otaUploadStarted = false;
      esp_task_wdt_add(NULL);      // загрузка оборвана — возвращаем сторожевой таймер
    }
  });

  web.begin();
}

// Запуск сервера, когда Wi-Fi уже подключён, и обработка запросов.
void otaWebLoop() {
  if (!otaConfigured()) return;
  if (!webStarted && WiFi.status() == WL_CONNECTED) {
    otaWebSetup();
    webStarted = true;
    Serial.println("OTA: http://" + WiFi.localIP().toString() + "/update");
  }
  if (webStarted) web.handleClient();

  // [OTA-FIX 3] Если загрузка «зависла» без обратного вызова UPLOAD_FILE_ABORTED
  // (например, телефон отключился посреди заливки .bin), otaUploadStarted остаётся true,
  // а watchdog — снятым до перезагрузки. Возвращаем WDT и сбрасываем состояние.
  // Условие безопасно: если handleClient() уже вернул управление, а клиент отключён —
  // загрузка действительно прервана (POST-обработчик, если бы он был, вызвал ESP.restart()).
  if (otaUploadStarted && !web.client()) {
    otaUploadStarted = false;
    Update.abort();
    esp_task_wdt_add(NULL);
    lastReason = "Web-OTA прервана (клиент отключился)";
    logEvent(lastReason);
    writeRelay(false);   // на всякий случай: реле не должно остаться включённым
  }
}

// ---------------- [OTA] ARDUINOOTA (ПРОШИВКА ПРЯМО ИЗ ARDUINODROID / ARDUINO IDE) ----------------
// После первой прошивки по USB плата появляется как сетевое устройство "cellar-fan"
// (порт 3232, пароль — OTA_PASS). Загрузка идёт внутри ArduinoOTA.handle() и блокирует loop().

void arduinoOtaSetup() {
  ArduinoOTA.setHostname("cellar-fan");
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.onStart([]() {
    // На время прошивки вентилятор выключаем и снимаем сторожевой таймер.
    writeRelay(false);
    fanMode = MODE_OFF;
    lastReason = "Остановлено: идёт обновление прошивки";
    esp_task_wdt_delete(NULL);
    otaWdtDetached = true;
  });
  ArduinoOTA.onError([](ota_error_t error) {
    // Загрузка не удалась — возвращаем сторожевой таймер и продолжаем работу.
    if (otaWdtDetached) {
      esp_task_wdt_add(NULL);
      otaWdtDetached = false;
    }
  });
  ArduinoOTA.begin();
}

void arduinoOtaLoop() {
  if (!otaConfigured()) return;
  if (!arduinoOtaStarted && WiFi.status() == WL_CONNECTED) {
    arduinoOtaSetup();
    arduinoOtaStarted = true;
    Serial.println("ArduinoOTA: " + WiFi.localIP().toString() + ", порт 3232");
  }
  if (arduinoOtaStarted) ArduinoOTA.handle();
}

// ---------------- НАСТРОЙКА ПРИ ЗАПУСКЕ ----------------
void setup() {
  // [ИСПР. мелочь] Сначала записываем уровень «выключено» в выходной регистр и только потом
  // включаем пин как выход. Иначе на ESP32 короткий импульс LOW при pinMode(OUTPUT)
  // мог бы на миг включить реле с активным низким уровнем.
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(RELAY_PIN, OUTPUT);
  writeRelay(false);
  fanMode = MODE_OFF;

  Serial.begin(115200);
  delay(500);

  Wire.begin(OUTDOOR_SDA, OUTDOOR_SCL);
  Wire1.begin(CELLAR_SDA, CELLAR_SCL);
  delay(100);
  outdoorOK = shtOutdoor.begin(SHT_ADDR);
  cellarOK = shtCellar.begin(SHT_ADDR);

  loadSettings();

  // [ИСПР. 5] true — при неудачном монтировании раздел форматируется. На чистом ESP32
  // раздел не отформатирован, и с false журнал не заработал бы никогда.
  // Риск потери данных нет: если раздел не монтируется, читать в нём всё равно нечего.
  fsOK = LittleFS.begin(true);
  if (!fsOK) {
    Serial.println("LittleFS не смонтирована; журнал отключён, управление продолжит работу.");
  }

  // Синхронизация времени нужна и для AUTO, и для журнала, независимо от LittleFS.
  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov", "time.google.com");

  if (!credentialsConfigured()) {
    Serial.println("Заполните Wi-Fi и BOT_TOKEN в начале скетча.");
  } else {
    WiFi.mode(WIFI_STA);
    // [FIX WiFi] Автоматический реконнект на уровне стека. С ним ручной вызов
    // WiFi.reconnect() из loop() нужен только как «страховка» на редкий затяжной случай.
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  bot.setPollMode(fb::Poll::Long, 21000);
  bot.onUpdate(updateHandler);
  bot.setOnline(false);

  readSensors();
  lastSensorMs = millis();
  lastLogMs = millis();
  lastHeartbeatMs = millis() - HEARTBEAT_INTERVAL_MS; // Скоро после запуска выполнить первую проверку связи.
  lastRelayChangeMs = millis() - (uint32_t)autoOffMinutes * 60000UL; // после старта AUTO не ждёт искусственную паузу
  lastReason = "Перезапуск: безопасный режим OFF";

  Serial.println("Старт завершён. Режим OFF.");
  Serial.printf("Уличный датчик: %s; датчик погреба: %s\n",
                outdoorOK ? "OK" : "ОШИБКА", cellarOK ? "OK" : "ОШИБКА");
  Serial.printf("Порог ON %.2f, OFF %.2f г/м3; расписание %u–%u; мин. T погреба %.1f\n",
                onThreshold, offThreshold, scheduleStart, scheduleEnd, minCellarT);

  if (!otaConfigured()) {
    Serial.println("OTA по Wi-Fi отключено: задайте OTA_PASS (не короче 8 символов).");
  }
  // [OTA-REMOTE] Сообщаем, настроено ли удалённое обновление.
  {
    String u = String(FIRMWARE_URL);
    if (u.length() > 10 && u.indexOf("YOUR_") < 0 && u.indexOf("ВАШ_") < 0 && u.startsWith("http"))
      Serial.println("OTA-REMOTE: включено, команда /update (с кодом подтверждения)");
    else
      Serial.println("OTA-REMOTE: отключено (FIRMWARE_URL не задан)");
  }

  // [ИСПР. 8] Сторожевой таймер включаем в самом конце setup(), когда долгие операции позади.
  // Это единственное место, зависящее от версии ядра ESP32. Если не компилируется —
  // удалите блок между #if и #endif вместе с esp_task_wdt_reset() в loop().
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtCfg;
  wdtCfg.timeout_ms = WDT_TIMEOUT_S * 1000;
  wdtCfg.idle_core_mask = 0;
  wdtCfg.trigger_panic = true;
  esp_task_wdt_reconfigure(&wdtCfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
}

// ---------------- ГЛАВНЫЙ ЦИКЛ ----------------
void loop() {
  esp_task_wdt_reset();
  otaWebLoop();     // [OTA] страница обновления прошивки (во время загрузки .bin loop блокируется)
  arduinoOtaLoop(); // [OTA] прошивка из Arduino IDE / ArduinoDroid
  uint32_t nowMs = millis();

  // [FIX WiFi] Реконнект: WiFi.reconnect() вместо disconnect()+begin(). На части ядер ESP32
  // повторный disconnect() в цикле приводил к «застреванию» в состоянии реконнекта;
  // интервал увеличен с 10 до 30 с, плюс включён WiFi.setAutoReconnect(true) в setup().
  if (WiFi.status() != WL_CONNECTED && credentialsConfigured() &&
      nowMs - lastWiFiAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
    lastWiFiAttemptMs = nowMs;
    WiFi.reconnect();
  }

  if (nowMs - lastSensorMs >= SENSOR_INTERVAL_MS) {
    lastSensorMs = nowMs;
    readSensors();   // внутри вызывает enforceSensorSafety()
  }

  if (nowMs - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = nowMs;
    bool wasHealthy = telegramHealthy;
    telegramHealthy = checkTelegramAPI();   // может блокировать до ~9 с

    if (telegramHealthy) {
      lastGoodTelegramMs = nowMs;           // время ДО запроса — оценка в безопасную сторону
      everTelegramHealthy = true;

      if (!wasHealthy) {
        // [ИСПР. 3] Связь появилась (или восстановилась): открываем «тихое окно»,
        // пока из очереди Telegram не пришли старые команды.
        commandBlockUntilMs = millis() + COMMAND_QUIET_MS;
        commandBlockActive = true;
      }
      if (!wasHealthy && connectionLossHandled) {
        connectionLossHandled = false;
        notifyOwner("📡 Связь с Telegram восстановлена. Вентилятор оставлен выключенным. Для возобновления выберите ВКЛЮЧИТЬ или AUTO.");
      }
    }
    // [ИСПР. 2] Ветка «связь потеряна» убрана отсюда: остановку по таймауту выполняет
    // enforceLinkSafety() на каждом проходе цикла, а единичный сбой запроса ни на что не влияет.

    bot.setOnline(telegramHealthy);
    nowMs = millis();   // запрос выше мог занять секунды — обновляем «сейчас»
  }

  enforceLinkSafety();

  if (nowMs - lastAutoMs >= AUTO_INTERVAL_MS) {
    lastAutoMs = nowMs;
    updateClock();
    enforceSensorSafety();   // защита от промерзания действует и между чтениями датчиков
    runAutoController();
  }

  // Ручной режим: пока режим ON и все проверки выше пройдены — удерживаем реле включённым.
  // Если какая-то проверка перевела режим в OFF, эта ветка не выполняется.
  if (fanMode == MODE_ON) {
    writeRelay(true);
  }

  if (nowMs - lastLogMs >= LOG_INTERVAL_MS) {
    lastLogMs = nowMs;
    logRecord("");   // периодическая запись без текста события
  }

  // [ИСПР. 6] Очистка журнала: первый раз — как только синхронизировано время (а не через
  // 24 часа аптайма), далее раз в сутки; а также по запросу при нехватке места.
  if (clockOK && (pruneNeeded || nowMs - lastPruneMs >= 86400000UL)) {
    pruneNeeded = false;
    lastPruneMs = nowMs;
    pruneOldLogs();
  }

  flushNotices();

  if (WiFi.status() == WL_CONNECTED && telegramHealthy) {
    bot.tick();
  }

  delay(5);   // [ИСПР. мелочь] отдать время системным задачам Wi-Fi/FreeRTOS
}