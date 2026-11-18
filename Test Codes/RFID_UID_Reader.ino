// ======================================================
// RFID UID READER - Raspberry Pi Pico W + MFRC522
// Uses the same wiring as the Medicine Dispenser sketch.
// Open Serial Monitor at 115200 baud and scan cards.
// ======================================================

#include <SPI.h>
#include <MFRC522.h>

// Same pins as the main project
#define RFID_SS   5
#define RFID_RST  28
#define RFID_MISO 4   // RX
#define RFID_SCK  6
#define RFID_MOSI 7   // TX

MFRC522 rfid(RFID_SS, RFID_RST);

void setup()
{
    Serial.begin(115200);

    // Wait up to 5 s for Serial Monitor to open (Pico uses native USB)
    unsigned long start = millis();
    while (!Serial && millis() - start < 5000) { }

    SPI.setRX(RFID_MISO);
    SPI.setCS(RFID_SS);
    SPI.setSCK(RFID_SCK);
    SPI.setTX(RFID_MOSI);
    SPI.begin();

    rfid.PCD_Init();
    delay(50);

    Serial.println("--------------------------------");
    Serial.println("RFID UID READER");
    Serial.println("--------------------------------");

    // Check that the reader is wired correctly
    byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    Serial.print("MFRC522 firmware version: 0x");
    Serial.println(version, HEX);

    if (version == 0x00 || version == 0xFF)
    {
        Serial.println("ERROR: Reader not detected. Check wiring and 3.3V power.");
    }
    else
    {
        Serial.println("Reader OK. Scan a card...");
    }
}

void loop()
{
    if (!rfid.PICC_IsNewCardPresent()) return;
    if (!rfid.PICC_ReadCardSerial())   return;

    // Build UID in the SAME format the main sketch compares against
    String uid = "";
    for (byte i = 0; i < rfid.uid.size; i++)
    {
        if (rfid.uid.uidByte[i] < 0x10) uid += "0";
        uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();

    MFRC522::PICC_Type type = rfid.PICC_GetType(rfid.uid.sak);

    Serial.println();
    Serial.print("Card type : ");
    Serial.println(rfid.PICC_GetTypeName(type));
    Serial.print("UID       : ");
    Serial.println(uid);
    Serial.print("Paste as  : \"");
    Serial.print(uid);
    Serial.println("\",");

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    delay(1000);   // avoid reading the same card repeatedly
}
