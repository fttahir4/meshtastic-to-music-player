/**
 * Heltec V4 hello world test
 * confirms the board is running YOUR code instead of Meshtastic
 * turns on OLED power (Vext) and prints a message
 */

#include <Wire.h>
#include <U8g2lib.h>

#define VEXT_PIN 36
#define SDA_PIN  17
#define SCL_PIN  18
#define RST_PIN  21

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, RST_PIN, SCL_PIN, SDA_PIN);

void setup() {
  Serial.begin(115200);

  // turn on power to the OLED (Vext active low)
  pinMode(VEXT_PIN, OUTPUT);
  digitalWrite(VEXT_PIN, LOW);
  delay(100);

  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 20, "this node is mine now");
  u8g2.drawStr(0, 40, "meshtastic is gone");
  u8g2.sendBuffer();

  Serial.println("custom firmware running, not meshtastic");
}

void loop() {
  // nothing yet
}
