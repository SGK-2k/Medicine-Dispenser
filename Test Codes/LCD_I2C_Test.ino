// ======================================================
// 16x2 I2C LCD TEST - Raspberry Pi Pico W
// Same wiring as the Medicine Dispenser sketch:
//   LCD SDA -> GP8   (pin 11)
//   LCD SCL -> GP9   (pin 12)
//   LCD VCC -> VBUS  (pin 40, 5V from USB)
//   LCD GND -> GND
// Open Serial Monitor at 115200 baud.
// ======================================================

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define LCD_SDA 8
#define LCD_SCL 9

byte lcdAddress = 0x27;            // default, replaced by the scan result
LiquidCrystal_I2C* lcd = nullptr;

// ------------------------------------------------------
// Scan the I2C bus and return the first address found
// ------------------------------------------------------
byte scanI2C()
{
    byte found = 0;
    int count = 0;

    Serial.println("Scanning I2C bus...");

    for (byte addr = 1; addr < 127; addr++)
    {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0)
        {
            Serial.print("  Device found at 0x");
            if (addr < 0x10) Serial.print("0");
            Serial.println(addr, HEX);

            if (found == 0) found = addr;
            count++;
        }
    }

    if (count == 0)
    {
        Serial.println("  No I2C devices found!");
        Serial.println("  Check: SDA->GP8, SCL->GP9, VCC, GND.");
    }

    return found;
}

void setup()
{
    Serial.begin(115200);

    // Wait up to 5 s for Serial Monitor (Pico uses native USB)
    unsigned long start = millis();
    while (!Serial && millis() - start < 5000) { }

    Serial.println("--------------------------------");
    Serial.println("16x2 I2C LCD TEST");
    Serial.println("--------------------------------");

    Wire.setSDA(LCD_SDA);
    Wire.setSCL(LCD_SCL);
    Wire.begin();

    byte addr = scanI2C();
    if (addr != 0) lcdAddress = addr;

    Serial.print("Using LCD address 0x");
    Serial.println(lcdAddress, HEX);

    lcd = new LiquidCrystal_I2C(lcdAddress, 16, 2);
    lcd->init();
    lcd->backlight();

    // Test 1: text on both lines
    lcd->setCursor(0, 0);
    lcd->print("LCD Test OK!");
    lcd->setCursor(0, 1);
    lcd->print("Addr: 0x");
    lcd->print(lcdAddress, HEX);

    Serial.println("Text written. If the screen shows only blocks");
    Serial.println("or nothing, turn the blue contrast pot on the back.");

    delay(3000);

    // Test 2: backlight blink
    for (int i = 0; i < 3; i++)
    {
        lcd->noBacklight();
        delay(300);
        lcd->backlight();
        delay(300);
    }

    // Test 3: fill every character position
    lcd->clear();
    for (int row = 0; row < 2; row++)
    {
        for (int col = 0; col < 16; col++)
        {
            lcd->setCursor(col, row);
            lcd->print((char)('A' + row * 16 + col));
            delay(60);
        }
    }

    delay(2000);
    lcd->clear();
}

void loop()
{
    // Test 4: live counter
    lcd->setCursor(0, 0);
    lcd->print("Running...      ");
    lcd->setCursor(0, 1);
    lcd->print("Time: ");
    lcd->print(millis() / 1000);
    lcd->print(" s     ");

    delay(500);
}
