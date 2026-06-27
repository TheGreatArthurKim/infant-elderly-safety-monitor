#include <SparkFun_MS5803_I2C.h>
#include <Wire.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <time.h>

// WiFi settings
const char* WIFI_SSID = "YOUR_WIFI_SSID"
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD"

IPAddress local_IP(192, 168, 45, 80);
IPAddress gateway(192, 168, 45, 1);
IPAddress subnet(255, 255, 255, 0);

WiFiServer tcpServer(8080);
WiFiClient client;
bool pauseLive = false;

String commandBuffer = "";

// Sensor and file settings
MS5803 sensor(ADDRESS_HIGH);
float baseline = 0;
const char* DATA_FILE = "/data.csv";
const char* TEMP_FILE = "/data_tmp.csv";
const char* PENDING_FILE = "/pending.csv";
const char* CSV_HEADER = "pc_time,millis,temp_c,pressure_mbar,dp_mbar";

// PC time sync settings
uint64_t syncEpochMs = 0;
unsigned long syncMillis = 0;
bool timeSynced = false;
bool waitingForSyncPrinted = false;

// Set this to the PC local timezone used for CSV timestamps.
// Korea Standard Time is UTC+9.
const long TIMEZONE_OFFSET_SECONDS = 9L * 60L * 60L;

// LED
const int LED_PIN = 2;

void createDataFileWithHeader();
void ensureDataFile();
bool isUnsyncedCsvRow(const String& row);
void removeUnsyncedRowsFromDataFile();
uint64_t parseEpochMs(const String& value);
uint64_t epochMsFromSampleMillis(unsigned long sampleMillis);
uint64_t currentEpochMs();
void formatPcTime(char* out, size_t outSize, uint64_t epochMs);
void appendPendingRow(unsigned long ms, float t, float p, float dp);
void flushPendingRows();
void syncTimeFromCommand(const String& cmd);
void dumpDataFile();
void clearDataFile();
void handleCommand(const String& rawCmd);
void handleTcpCommands();

void createDataFileWithHeader() {
  File f = LittleFS.open(DATA_FILE, "w");
  if (f) {
    f.println(CSV_HEADER);
    f.close();
  }
}

void ensureDataFile() {
  if (!LittleFS.exists(DATA_FILE)) {
    createDataFileWithHeader();
    return;
  }

  File f = LittleFS.open(DATA_FILE, "r");
  if (!f) {
    createDataFileWithHeader();
    return;
  }

  String firstLine = f.readStringUntil('\n');
  firstLine.trim();
  f.close();

  if (firstLine != CSV_HEADER) {
    Serial.println("Existing data.csv has old format. Recreating with new header.");
    LittleFS.remove(DATA_FILE);
    createDataFileWithHeader();
  }
}

bool isUnsyncedCsvRow(const String& row) {
  return row.startsWith("1970-01-01 ") || row.startsWith("1970-");
}

void removeUnsyncedRowsFromDataFile() {
  if (!LittleFS.exists(DATA_FILE)) {
    createDataFileWithHeader();
    return;
  }

  File in = LittleFS.open(DATA_FILE, "r");
  if (!in) {
    createDataFileWithHeader();
    return;
  }

  File out = LittleFS.open(TEMP_FILE, "w");
  if (!out) {
    in.close();
    return;
  }

  int removed = 0;
  out.println(CSV_HEADER);

  while (in.available()) {
    String row = in.readStringUntil('\n');
    row.trim();

    if (row.length() == 0 || row == CSV_HEADER) {
      continue;
    }

    if (isUnsyncedCsvRow(row)) {
      removed++;
      continue;
    }

    out.println(row);
  }

  in.close();
  out.close();

  if (removed > 0) {
    LittleFS.remove(DATA_FILE);
    LittleFS.rename(TEMP_FILE, DATA_FILE);
    Serial.print("Removed unsynced CSV rows: ");
    Serial.println(removed);
  } else {
    LittleFS.remove(TEMP_FILE);
  }
}

uint64_t parseEpochMs(const String& value) {
  uint64_t result = 0;

  for (int i = 0; i < value.length(); i++) {
    char ch = value.charAt(i);
    if (ch < '0' || ch > '9') {
      break;
    }
    result = (result * 10ULL) + (uint64_t)(ch - '0');
  }

  return result;
}

uint64_t epochMsFromSampleMillis(unsigned long sampleMillis) {
  if (!timeSynced) {
    return 0;
  }

  int64_t deltaMs = (int64_t)((int32_t)(sampleMillis - syncMillis));

  if (deltaMs < 0) {
    uint64_t beforeSyncMs = (uint64_t)(-deltaMs);
    if (syncEpochMs < beforeSyncMs) {
      return 0;
    }
    return syncEpochMs - beforeSyncMs;
  }

  return syncEpochMs + (uint64_t)deltaMs;
}

uint64_t currentEpochMs() {
  if (!timeSynced) {
    return 0;
  }
  return epochMsFromSampleMillis(millis());
}

void formatPcTime(char* out, size_t outSize, uint64_t epochMs) {
  if (!timeSynced || epochMs == 0) {
    snprintf(out, outSize, "1970-01-01 00:00:00.000");
    return;
  }

  time_t seconds = (time_t)(epochMs / 1000ULL);
  unsigned int milliseconds = (unsigned int)(epochMs % 1000ULL);

  seconds += TIMEZONE_OFFSET_SECONDS;

  struct tm tmTime;
  gmtime_r(&seconds, &tmTime);

  snprintf(
    out,
    outSize,
    "%04d-%02d-%02d %02d:%02d:%02d.%03u",
    tmTime.tm_year + 1900,
    tmTime.tm_mon + 1,
    tmTime.tm_mday,
    tmTime.tm_hour,
    tmTime.tm_min,
    tmTime.tm_sec,
    milliseconds
  );
}

void appendPendingRow(unsigned long ms, float t, float p, float dp) {
  File f = LittleFS.open(PENDING_FILE, "a");
  if (!f) {
    return;
  }

  char row[96];
  snprintf(row, sizeof(row), "%lu,%.2f,%.3f,%.3f", ms, t, p, dp);
  f.println(row);
  f.close();
}

void flushPendingRows() {
  if (!timeSynced || !LittleFS.exists(PENDING_FILE)) {
    return;
  }

  File in = LittleFS.open(PENDING_FILE, "r");
  if (!in) {
    return;
  }

  File out = LittleFS.open(DATA_FILE, "a");
  if (!out) {
    in.close();
    return;
  }

  int flushed = 0;

  while (in.available()) {
    String row = in.readStringUntil('\n');
    row.trim();

    if (row.length() == 0) {
      continue;
    }

    int comma = row.indexOf(',');
    if (comma <= 0) {
      continue;
    }

    unsigned long sampleMs = strtoul(row.substring(0, comma).c_str(), NULL, 10);
    uint64_t epochMs = epochMsFromSampleMillis(sampleMs);

    if (epochMs == 0) {
      continue;
    }

    char pcTime[32];
    formatPcTime(pcTime, sizeof(pcTime), epochMs);

    out.print(pcTime);
    out.print(",");
    out.println(row);
    flushed++;
  }

  in.close();
  out.close();
  LittleFS.remove(PENDING_FILE);

  if (flushed > 0) {
    Serial.print("Flushed pending rows after SYNC: ");
    Serial.println(flushed);
  }
}

void syncTimeFromCommand(const String& cmd) {
  String value = cmd.substring(5);
  value.trim();

  if (value.length() == 0) {
    return;
  }

  syncEpochMs = parseEpochMs(value);
  syncMillis = millis();
  timeSynced = syncEpochMs > 0;
  waitingForSyncPrinted = false;

  char epochText[24];
  snprintf(epochText, sizeof(epochText), "%llu", (unsigned long long)syncEpochMs);
  Serial.print("Time synced. epoch_ms=");
  Serial.println(epochText);

  flushPendingRows();
}

void dumpDataFile() {
  pauseLive = true;
  removeUnsyncedRowsFromDataFile();

  if (client && client.connected()) {
    client.println("<<<DUMP_START>>>");

    File f = LittleFS.open(DATA_FILE, "r");
    if (f) {
      while (f.available()) {
        String row = f.readStringUntil('\n');
        row.trim();
        if (row.length() > 0) {
          client.println(row);
        }
      }
      f.close();
    }

    client.println("<<<DUMP_END>>>");
  }
}

void clearDataFile() {
  LittleFS.remove(DATA_FILE);
  createDataFileWithHeader();

  if (client && client.connected()) {
    client.println("<<<CLEARED>>>");
  }
}

void handleCommand(const String& rawCmd) {
  String cmd = rawCmd;
  cmd.trim();

  if (cmd.length() == 0) {
    return;
  }

  if (cmd.startsWith("SYNC,")) {
    syncTimeFromCommand(cmd);
  }
  else if (cmd == "r") {
    dumpDataFile();
  }
  else if (cmd == "s") {
    pauseLive = false;
  }
  else if (cmd == "c") {
    clearDataFile();
  }
}

void handleTcpCommands() {
  if (!(client && client.connected())) {
    return;
  }

  while (client.available()) {
    char ch = (char)client.read();

    if (ch == '\n' || ch == '\r') {
      handleCommand(commandBuffer);
      commandBuffer = "";
      continue;
    }

    commandBuffer += ch;

    if ((commandBuffer == "r") || (commandBuffer == "c") || (commandBuffer == "s")) {
      handleCommand(commandBuffer);
      commandBuffer = "";
    }

    if (commandBuffer.length() > 64) {
      commandBuffer = "";
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin();

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return;
  }

  LittleFS.remove(PENDING_FILE);

  sensor.reset();
  delay(10);
  sensor.begin();
  delay(10);
  baseline = sensor.getPressure(ADC_4096);

  Serial.println("=== Pressure Logger (WiFi) ===");
  Serial.print("Baseline: ");
  Serial.print(baseline, 3);
  Serial.println(" mbar");

  ensureDataFile();
  removeUnsyncedRowsFromDataFile();

  WiFi.mode(WIFI_STA);
  if (!WiFi.config(local_IP, gateway, subnet)) {
    Serial.println("STA config failed");
  }
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi OK. IP: ");
    Serial.println(WiFi.localIP());
    digitalWrite(LED_PIN, HIGH);
    tcpServer.begin();
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
  } else {
    Serial.println("\nWiFi FAILED. Measurement continues. CSV logging waits for SYNC.");
    digitalWrite(LED_PIN, LOW);
  }
}

void loop() {
  if (!client || !client.connected()) {
    WiFiClient newClient = tcpServer.available();
    if (newClient) {
      client = newClient;
      commandBuffer = "";
      pauseLive = false;
      Serial.println("TCP client connected.");
    }
  }

  handleTcpCommands();

  float t = sensor.getTemperature(CELSIUS, ADC_512);
  float p = sensor.getPressure(ADC_4096);
  float dp = p - baseline;
  unsigned long ms = millis();

  if (!timeSynced) {
    if (!waitingForSyncPrinted) {
      Serial.println("Waiting for SYNC. Raw pending rows are saved until PC time is received.");
      waitingForSyncPrinted = true;
    }

    appendPendingRow(ms, t, p, dp);

    Serial.print("UNSYNCED");
    Serial.print(" | ms: "); Serial.print(ms);
    Serial.print(" | T: "); Serial.print(t, 2);
    Serial.print(" | P: "); Serial.print(p, 3);
    Serial.print(" | dP: "); Serial.println(dp, 3);

    if (WiFi.status() == WL_CONNECTED) {
      digitalWrite(LED_PIN, LOW);
      delay(20);
      digitalWrite(LED_PIN, HIGH);
    }

    delay(500);
    return;
  }

  char pcTime[32];
  formatPcTime(pcTime, sizeof(pcTime), currentEpochMs());

  char line[128];
  snprintf(line, sizeof(line), "%s,%lu,%.2f,%.3f,%.3f", pcTime, ms, t, p, dp);

  File f = LittleFS.open(DATA_FILE, "a");
  if (f) {
    f.println(line);
    f.close();
  }

  if (client && client.connected() && !pauseLive) {
    client.println(line);
  }

  Serial.print("time: "); Serial.print(pcTime);
  Serial.print(" | ms: "); Serial.print(ms);
  Serial.print(" | T: "); Serial.print(t, 2);
  Serial.print(" | P: "); Serial.print(p, 3);
  Serial.print(" | dP: "); Serial.println(dp, 3);

  if (WiFi.status() == WL_CONNECTED) {
    digitalWrite(LED_PIN, LOW);
    delay(20);
    digitalWrite(LED_PIN, HIGH);
  }

  delay(500);
}
