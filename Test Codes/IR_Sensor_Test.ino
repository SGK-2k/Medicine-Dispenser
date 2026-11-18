// ======================================================
// IR SENSOR TEST - Raspberry Pi Pico W
// Same wiring as Medicine_Dispenser.ino v6:
//   IR 1 OUT -> GP19   IR 2 OUT -> GP20   IR 3 OUT -> GP21
//   IR VCC   -> 3V3    IR GND   -> GND
// Open Serial Monitor at 115200 baud.
//
// What to do:
//   1. Leave all boxes EMPTY and read the lines.
//   2. Put medicine in one box and read again.
// The pin level that appears when medicine is IN the box tells you
// how to set IR_ACTIVE_LOW in Medicine_Dispenser.ino.
// ======================================================

const int IR_PINS[3] = { 19, 20, 21 };

void setup()
{
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && millis() - start < 5000) { }

    for (int i = 0; i < 3; i++) pinMode(IR_PINS[i], INPUT_PULLUP);

    Serial.println("IR SENSOR TEST");
    Serial.println("Empty box  -> note the level. Medicine in -> it should change.");
}

void loop()
{
    for (int i = 0; i < 3; i++)
    {
        int level = digitalRead(IR_PINS[i]);
        Serial.print("Box ");
        Serial.print(i + 1);
        Serial.print(": ");
        Serial.print(level == HIGH ? "HIGH" : "LOW ");
        Serial.print("   ");
    }
    Serial.println();
    delay(500);
}
