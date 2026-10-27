
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ==================================================
// 1. WIFI AND GOOGLE SHEETS SETTINGS
// ==================================================

const char* WIFI_SSID = "Xiaomi13T";
const char* WIFI_PASSWORD = "11122233";

// Change only this deployment ID when needed.
const char* DEPLOY MENT_ID = "AKfycbx5zdUTIjGWBYaL9d58mSMaqkg1wMVwN1biZ9r851O9jMHloTFt6tHnL2N_ekTDPOwOug";

// Name recorded in Google Sheets
const char* PERSON_NAME = "Hammad";  

// Your authorized tag number from the working Arduino code
const unsigned long AUTHORIZED_TAG = 5903419UL;

// ==================================================
// 2. HARDWARE PINS
// ==================================================

// RDM6300 TX -> ESP32 GPIO16
HardwareSerial RFIDSerial(2);
const int RFID_RX_PIN = 16; 

// OLED: SDA GPIO21, SCL GPIO22
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

Adafruit_SSD1306 display(
  SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET
);

bool oledAvailable = false;

// RDM6300 frame settings
const int BUFFER_SIZE = 14;
const int DATA_SIZE = 10;
const int DATA_TAG_SIZE = 8;
const int CHECKSUM_SIZE = 2;

uint8_t buffer[BUFFER_SIZE];
int buffer_index = 0;

// Prevent repeated scans while the same card remains nearby
unsigned long lastReadTime = 0;
const unsigned long CARD_COOLDOWN = 2500;
unsigned long lastWiFiAttempt = 0;

// ==================================================
// 3. GOOGLE APPS SCRIPT URL
// ==================================================

String getScriptURL() {
  return String("https://script.google.com/macros/s/") +
         DEPLOYMENT_ID + "/exec";
}

// ==================================================
// 4. URL ENCODING
// ==================================================

String urlEncode(const String& input) {
  String encoded = "";
  const char hexChars[] = "0123456789ABCDEF";

  for (unsigned int i = 0; i < input.length(); i++) {
    uint8_t c = (uint8_t)input[i];

    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' ||
        c == '.' || c == '~') {
      encoded += (char)c;
    } else {
      encoded += '%';
      encoded += hexChars[c >> 4];
      encoded += hexChars[c & 0x0F];
    }
  }

  return encoded;
}

// ==================================================
// 5. OLED DISPLAY FUNCTIONS
// ==================================================

void centerText(const String& text, int y, int size = 1) {
  if (!oledAvailable) return;

  display.setTextSize(size);
  display.setTextColor(SSD1306_WHITE);

  int16_t x1, y1;
  uint16_t w, h;

  display.getTextBounds(
    text.c_str(), 0, y, &x1, &y1, &w, &h
  );

  display.setCursor((SCREEN_WIDTH - w) / 2, y);
  display.println(text);
}

void showMessage(
  const String& line1,
  const String& line2 = "",
  const String& line3 = ""
) {
  if (!oledAvailable) return;

  display.clearDisplay();
  centerText(line1, 8);
  centerText(line2, 27);
  centerText(line3, 45);
  display.display();
}

void showWelcome() {
  if (!oledAvailable) return;

  display.clearDisplay();
  centerText("RFID ATTENDANCE", 3);
  display.drawLine(0, 15, 127, 15, SSD1306_WHITE);
  centerText("Scan Your Card", 27);

  if (WiFi.status() == WL_CONNECTED) {
    centerText("WiFi Connected", 47);
  } else {
    centerText("WiFi Offline", 47);
  }

  display.display();
}

void showResult(
  const String& name,
  const String& action,
  const String& date,
  const String& time
) {
  if (!oledAvailable) return;

  display.clearDisplay();
  centerText(name, 0);
  display.drawLine(0, 12, 127, 12, SSD1306_WHITE);
  centerText(action, 18);
  centerText(date, 34);
  centerText(time, 45);
  centerText("Data Sent", 56);
  display.display();
}

// ==================================================
// 6. HEX STRING TO DECIMAL VALUE
// ==================================================

unsigned long hexstr_to_value(
  const char* str,
  unsigned int length
) {
  char copy[9];

  if (length > 8) return 0;

  memcpy(copy, str, length);
  copy[length] = '\0';

  return strtoul(copy, nullptr, 16);
}

// ==================================================
// 7. WIFI CONNECTION
// ==================================================

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  showMessage("Connecting WiFi", "Please wait");

  Serial.print("Connecting to WiFi");

  unsigned long startTime = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startTime < 20000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected");
    Serial.println(WiFi.localIP());
    showMessage("WiFi Connected", WiFi.localIP().toString());
    delay(1200);
  } else {
    Serial.println("WiFi connection failed");
    showMessage("WiFi Offline", "Check credentials");
    delay(1200);
  }
}

// ==================================================
// 8. READ JSON FIELD
// ==================================================

String getJsonValue(const String& json, const char* key) {
  String searchKey = "\"" + String(key) + "\"";
  int pos = json.indexOf(searchKey);

  if (pos < 0) return "";

  int colon = json.indexOf(':', pos + searchKey.length());
  if (colon < 0) return "";

  int start = colon + 1;

  while (start < (int)json.length() &&
         (json[start] == ' ' ||
          json[start] == '\n' ||
          json[start] == '\r')) {
    start++;
  }

  if (start >= (int)json.length()) return "";

  if (json[start] == '"') {
    start++;
    String result = "";

    for (int i = start; i < (int)json.length(); i++) {
      if (json[i] == '"') return result;
      if (json[i] == '\\' && i + 1 < (int)json.length()) {
        i++;
      }
      result += json[i];
    }

    return result;
  }

  int end = start;

  while (end < (int)json.length() &&
         json[end] != ',' &&
         json[end] != '}') {
    end++;
  }

  String result = json.substring(start, end);
  result.trim();
  return result;
}

// ==================================================
// 9. SEND ATTENDANCE TO GOOGLE SHEETS
// ==================================================

bool sendAttendance(
  unsigned long tag,
  String& action,
  String& date,
  String& time
) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return false;
  }

  WiFiClientSecure client;

  // Testing convenience only: certificate verification is disabled.
  // Use certificate validation for a production deployment.
  client.setInsecure();

  HTTPClient http;

  String url = getScriptURL() +
               "?uid=" + String(tag) +
               "&name=" + urlEncode(String(PERSON_NAME));

  Serial.println("Sending attendance to Google Sheets...");

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  if (!http.begin(client, url)) {
    Serial.println("Could not start HTTPS request");
    return false;
  }

  int httpCode = http.GET();
  String response = "";

  if (httpCode > 0) {
    response = http.getString();

    Serial.print("HTTP code: ");
    Serial.println(httpCode);
    Serial.println(response);
  } else {
    Serial.println(http.errorToString(httpCode));
  }

  http.end();

  if (httpCode != HTTP_CODE_OK) return false;

  if (!getJsonValue(response, "success").equalsIgnoreCase("true")) {
    Serial.println("Google Sheets did not confirm success");
    return false;
  }

  action = getJsonValue(response, "action");
  date = getJsonValue(response, "date");
  time = getJsonValue(response, "time");

  return true;
}

// ==================================================
// 10. PROCESS AUTHORIZED TAG
// ==================================================

void processTag(unsigned long tag) {
  Serial.print("Tag Detected: ");
  Serial.println(tag);

  if (tag != AUTHORIZED_TAG) {
    Serial.println("Access Denied");
    showMessage("Access Denied", "Unknown Card");
    delay(1500);
    showWelcome();
    return;
  }

  Serial.println("Access Granted");
  Serial.println("Name: " + String(PERSON_NAME));

  showMessage("Access Granted", PERSON_NAME, "Sending data...");

  String action = "";
  String date = "";
  String time = "";

  if (sendAttendance(tag, action, date, time)) {
    Serial.println("Attendance recorded successfully");
    Serial.println("Action: " + action);
    Serial.println("Date: " + date);
    Serial.println("Time: " + time);

    showResult(PERSON_NAME, action, date, time);
    delay(3000);
  } else {
    Serial.println("Attendance upload failed");
    showMessage("Upload Failed", PERSON_NAME, "Check WiFi/Script");
    delay(2000);
  }

  showWelcome();
}

// ==================================================
// 11. EXTRACT AND VALIDATE RFID FRAME
// Uses the same frame layout as your working Arduino code.
// ==================================================

void extract_tag() {
  uint8_t* msg_data = buffer + 1;
  uint8_t* msg_data_tag = msg_data + 2;
  uint8_t* msg_checksum = buffer + 11;

  unsigned long tag =
    hexstr_to_value((const char*)msg_data_tag, DATA_TAG_SIZE);

  unsigned long checksum = 0;

  for (int i = 0; i < DATA_SIZE; i += CHECKSUM_SIZE) {
    checksum ^= hexstr_to_value(
      (const char*)(msg_data + i), CHECKSUM_SIZE
    );
  }

  unsigned long receivedChecksum =
    hexstr_to_value((const char*)msg_checksum, CHECKSUM_SIZE);

  if (checksum != receivedChecksum) {
    Serial.println("Checksum Error");
    buffer_index = 0;
    return;
  }

  Serial.print("Tag Detected: ");
  Serial.println(tag);

  if (tag == AUTHORIZED_TAG) {
    if (millis() - lastReadTime > CARD_COOLDOWN) {
      lastReadTime = millis();
      processTag(tag);
    }
  } else {
    Serial.println("Access Denied");
    showMessage("Access Denied", "Unknown Card");
    delay(1200);
    showWelcome();
  }

  buffer_index = 0;
}

// ==================================================
// 12. SETUP
// ==================================================

void setup() {
  Serial.begin(115200);

  // Initialize OLED if one is connected.
  Wire.begin(21, 22);

  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledAvailable = true;
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.display();
    showMessage("RFID ATTENDANCE", "Starting...");
  } else {
    Serial.println("OLED not detected; continuing without OLED");
  }

  // UART2 RX is GPIO16; TX is unused.
  RFIDSerial.begin(9600, SERIAL_8N1, RFID_RX_PIN, -1);

  Serial.println("==============================");
  Serial.println("ESP32 RFID Attendance System");
  Serial.println("Scan your authorized tag");
  Serial.println("==============================");

  connectWiFi();
  showWelcome();
}

// ==================================================
// 13. MAIN LOOP
// ==================================================

void loop() {
  if (WiFi.status() != WL_CONNECTED &&
      millis() - lastWiFiAttempt >= 10000) {
    lastWiFiAttempt = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  while (RFIDSerial.available()) {
    int ssvalue = RFIDSerial.read();

    if (ssvalue == 2) {
      buffer_index = 0;
    }

    if (buffer_index >= BUFFER_SIZE) {
      buffer_index = 0;
    }

    buffer[buffer_index++] = (uint8_t)ssvalue;

    if (ssvalue == 3) {
      if (buffer_index == BUFFER_SIZE) {
        extract_tag();
      } else {
        buffer_index = 0;
      }
    }
  }
}
