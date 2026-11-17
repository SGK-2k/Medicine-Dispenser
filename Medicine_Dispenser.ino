// ======================================================
// MEDICINE DISPENSER - Raspberry Pi Pico W  (Version 2)
//
// Flow:
//   1. Select the box on the keypad (1 = General, 2 = Severe, 3 = High)
//   2. Authenticate with an RFID card OR a 4-digit PIN + '#'
//   3. If the user's role is allowed for that box, the box opens
//
// Keys:
//   1-3  select box (on the selection screen)
//   1-9  PIN digits (on the login screen)
//   #    submit PIN
//   *    clear PIN
//   0    exit to box selection (so PINs must not contain 0)
//
// A wrong PIN / unknown card keeps you on the login screen.
// After 3 wrong tries the keypad locks for 10 seconds, then you can retry.
//
// Board package: "Raspberry Pi Pico/RP2040" by Earle Philhower
// Libraries: MFRC522 (GithubCommunity), LiquidCrystal I2C (Frank de Brabander)
// Note: the Keypad library is NOT used any more. A custom scanner is used
// because the Keypad library reads too fast on the RP2040 and produces
// phantom key presses.
// ======================================================

#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Servo.h>
#include <LiquidCrystal_I2C.h>

// ======================================================
// 1. PIN DEFINITIONS
// ======================================================

// RFID - SPI0
#define RFID_MISO     4
#define RFID_SS       5
#define RFID_SCK      6
#define RFID_MOSI     7
#define RFID_RST      28

// LCD - I2C0
#define LCD_SDA       8
#define LCD_SCL       9

// Servos
#define SERVO_GENERAL 16
#define SERVO_SEVERE  17
#define SERVO_HIGH    18

// LEDs
#define GREEN_LED     19
#define YELLOW_LED    20
#define RED_LED       21

// Buzzer
#define BUZZER        22
#define BUZZER_ENABLED 1

// Servo positions (degrees)
#define SERVO_CLOSED  0
#define SERVO_OPEN    180

// Pulse range for a full 0-180 sweep on SG90 / MG90S / MG996R servos.
// If your servo buzzes or strains at the ends, try 600 and 2400.
#define SERVO_MIN_US  500
#define SERVO_MAX_US  2500

// ======================================================
// 2. SETTINGS
// ======================================================

const unsigned long LOGIN_TIMEOUT = 30000;  // ms of inactivity before reset
const int           MAX_ATTEMPTS  = 3;      // wrong tries before a lockout
const unsigned long LOCKOUT_TIME  = 10000;  // ms the keypad is locked
const unsigned long BOX_OPEN_TIME = 3000;   // ms the box stays open

// ======================================================
// 3. LCD
// ======================================================

LiquidCrystal_I2C lcd(0x27, 16, 2);   // change to 0x3F if your LCD uses it

void showLCD(String line1, String line2)
{
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(line1.substring(0, 16));
    lcd.setCursor(0, 1);
    lcd.print(line2.substring(0, 16));
}

// ======================================================
// 4. RFID
// ======================================================

MFRC522 rfid(RFID_SS, RFID_RST);

// Replace with the real UIDs printed by RFID_UID_Reader.ino
// (each one is exactly 8 characters for a normal 4-byte card)
const char* rfidUIDs[] =
{
    "AABBCCDD",   // Patient
    "55667788",   // Nurse
    "11223344",   // Head Nurse
    "01020304"    // Doctor
};

// ======================================================
// 5. KEYPAD (custom scanner)
// ======================================================

const byte ROWS = 4;
const byte COLS = 4;

const char keys[ROWS][COLS] =
{
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}
};

// Keypad connector, left to right: R1 R2 R3 R4 C1 C2 C3 C4
const byte rowPins[ROWS] = {0, 1, 2, 3};
const byte colPins[COLS] = {12, 13, 14, 15};

const unsigned long DEBOUNCE_MS = 30;

void keypadInit()
{
    for (byte r = 0; r < ROWS; r++)
    {
        pinMode(rowPins[r], INPUT_PULLUP);
    }

    // Columns idle as inputs (high impedance) so they never fight each other
    for (byte c = 0; c < COLS; c++)
    {
        pinMode(colPins[c], INPUT);
    }
}

// Returns the key currently held, or 0 if none / more than one
char keypadScanRaw()
{
    char found = 0;
    int  count = 0;

    for (byte c = 0; c < COLS; c++)
    {
        // Drive this column LOW
        pinMode(colPins[c], OUTPUT);
        digitalWrite(colPins[c], LOW);
        delayMicroseconds(20);           // let the line settle (fixes ghost keys)

        for (byte r = 0; r < ROWS; r++)
        {
            if (digitalRead(rowPins[r]) == LOW)
            {
                found = keys[r][c];
                count++;
            }
        }

        // Release the column
        pinMode(colPins[c], INPUT);
        delayMicroseconds(20);
    }

    return (count == 1) ? found : 0;
}

// Returns a key ONCE per press (debounced), otherwise 0
char getKey()
{
    static char lastRaw = 0;
    static char stableKey = 0;
    static unsigned long lastChange = 0;

    char raw = keypadScanRaw();
    unsigned long now = millis();

    if (raw != lastRaw)
    {
        lastRaw = raw;
        lastChange = now;
    }

    if (now - lastChange >= DEBOUNCE_MS && raw != stableKey)
    {
        stableKey = raw;
        if (stableKey != 0)
        {
            return stableKey;             // new press
        }
    }

    return 0;
}

// ======================================================
// 6. SERVOS
// ======================================================

Servo generalServo;   // Box 1 - General medicine
Servo severeServo;    // Box 2 - Severe medicine
Servo highServo;      // Box 3 - High severity medicine

// Returns the servo for the selected box, or nullptr if the box number is invalid
Servo* boxServo(int box)
{
    switch (box)
    {
        case 1:  return &generalServo;   // Box 1 - General
        case 2:  return &severeServo;    // Box 2 - Severe
        case 3:  return &highServo;      // Box 3 - High severity
        default: return nullptr;         // anything else - no box
    }
}

// ======================================================
// 7. USER DATABASE
// ======================================================

// 0 = Patient / Attender, 1 = Nurse, 2 = Head Nurse, 3 = Doctor
const char* userPINs[]  = { "1111", "2222", "3333", "4444" };
const char* roleNames[] = { "Patient", "Nurse", "Head Nurse", "Doctor" };

// Highest box each role may open (1 = General, 2 = Severe, 3 = High)
const int accessLevel[] = { 1, 2, 3, 3 };

const int USER_COUNT = 4;

const char* boxNames[] = { "", "General", "Severe", "High Sev" };

// ======================================================
// 8. STATE
// ======================================================

enum State
{
    SELECT_BOX,
    AUTHENTICATE
};

State state = SELECT_BOX;

int           selectedBox  = 0;
String        enteredPIN   = "";
int           attempts     = 0;
unsigned long lastActivity = 0;

// ======================================================
// 9. LED AND BUZZER
// ======================================================

void allLEDsOff()
{
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(YELLOW_LED, LOW);
    digitalWrite(RED_LED, LOW);
}

void beep(int frequency, int duration)
{
#if BUZZER_ENABLED
    tone(BUZZER, frequency, duration);
#endif
}

void keyClick()
{
    beep(2500, 30);
}

void successSignal()
{
    digitalWrite(GREEN_LED, HIGH);
    beep(1500, 150);
    delay(200);
    digitalWrite(GREEN_LED, LOW);
}

void deniedSignal()
{
    digitalWrite(RED_LED, HIGH);
    beep(400, 500);
    delay(500);
    digitalWrite(RED_LED, LOW);
}

// ======================================================
// 10. SCREENS
// ======================================================

void showSelectScreen()
{
    showLCD("Select Box 1-3", "1Low 2Mid 3High");
}

void showLoginScreen()
{
    String masked = "";
    for (unsigned int i = 0; i < enteredPIN.length(); i++)
    {
        masked += "*";
    }

    String line1 = "Box" + String(selectedBox) + ":" + String(boxNames[selectedBox]);

    if (enteredPIN.length() == 0)
    {
        showLCD(line1, "Card/PIN# 0=Exit");
    }
    else
    {
        showLCD(line1, "PIN: " + masked);
    }
}

// ======================================================
// 11. STATE CHANGES
// ======================================================

void goToSelectBox()
{
    state       = SELECT_BOX;
    selectedBox = 0;
    enteredPIN  = "";
    attempts    = 0;

    allLEDsOff();
    showSelectScreen();

    Serial.println();
    Serial.println("Select box: 1 = General, 2 = Severe, 3 = High severity");
}

void goToAuthenticate(int box)
{
    state        = AUTHENTICATE;
    selectedBox  = box;
    enteredPIN   = "";
    attempts     = 0;
    lastActivity = millis();

    allLEDsOff();
    digitalWrite(YELLOW_LED, HIGH);
    showLoginScreen();

    Serial.print("Box selected: ");
    Serial.print(box);
    Serial.print(" (");
    Serial.print(boxNames[box]);
    Serial.println(")");
    Serial.println("Scan RFID card or enter PIN followed by #. Press 0 to exit.");
}

// ======================================================
// 12. OPEN BOX / ACCESS CHECK
// ======================================================

void openBox(int box)
{
    Servo* s = boxServo(box);

    if (s == nullptr)
    {
        Serial.print("ERROR: invalid box number ");
        Serial.println(box);
        showLCD("Invalid Box", "Start Again");
        deniedSignal();
        delay(1500);
        goToSelectBox();
        return;
    }

    Serial.print("ACCESS GRANTED - opening box ");
    Serial.println(box);

    allLEDsOff();
    digitalWrite(GREEN_LED, HIGH);
    showLCD("Access Granted", "Opening Box " + String(box));
    beep(1800, 200);

    s->write(SERVO_OPEN);
    delay(BOX_OPEN_TIME);
    s->write(SERVO_CLOSED);

    showLCD("Box Closed", "Session Ended");
    Serial.println("Box closed.");
    delay(1500);

    goToSelectBox();
}

// Called when a valid user (role) has been identified
void handleUser(int role)
{
    Serial.print("Authenticated: ");
    Serial.println(roleNames[role]);

    showLCD("Welcome!", roleNames[role]);
    successSignal();
    delay(1000);

    if (selectedBox <= accessLevel[role])
    {
        openBox(selectedBox);
    }
    else
    {
        Serial.print("ACCESS DENIED - ");
        Serial.print(roleNames[role]);
        Serial.print(" cannot open box ");
        Serial.println(selectedBox);

        showLCD("Access Denied", "Not Authorised");
        deniedSignal();
        delay(1500);

        // Stay on the login screen so another user can log in
        enteredPIN   = "";
        lastActivity = millis();
        showLoginScreen();
    }
}

// Called on a wrong PIN or unknown card
void handleFailedAttempt(String line1)
{
    attempts++;

    Serial.print(line1);
    Serial.print(" (attempt ");
    Serial.print(attempts);
    Serial.print(" of ");
    Serial.print(MAX_ATTEMPTS);
    Serial.println(")");

    showLCD(line1, "Access Denied");
    deniedSignal();
    delay(1200);

    if (attempts >= MAX_ATTEMPTS)
    {
        Serial.println("Too many failed attempts - keypad locked.");
        digitalWrite(RED_LED, HIGH);

        for (int s = LOCKOUT_TIME / 1000; s > 0; s--)
        {
            showLCD("Too Many Tries", "Wait " + String(s) + " s");
            delay(1000);
        }

        digitalWrite(RED_LED, LOW);
        attempts = 0;
    }

    // Stay on the login screen for the SAME box
    enteredPIN   = "";
    lastActivity = millis();
    showLoginScreen();
}

// ======================================================
// 13. PIN AUTHENTICATION
// ======================================================

void checkPIN()
{
    for (int i = 0; i < USER_COUNT; i++)
    {
        if (enteredPIN == userPINs[i])
        {
            Serial.println("PIN correct.");
            enteredPIN = "";
            handleUser(i);
            return;
        }
    }

    handleFailedAttempt("Invalid PIN");
}

// ======================================================
// 14. RFID AUTHENTICATION
// ======================================================

void checkRFID()
{
    if (!rfid.PICC_IsNewCardPresent()) return;
    if (!rfid.PICC_ReadCardSerial())   return;

    String scannedUID = "";
    for (byte i = 0; i < rfid.uid.size; i++)
    {
        if (rfid.uid.uidByte[i] < 0x10) scannedUID += "0";
        scannedUID += String(rfid.uid.uidByte[i], HEX);
    }
    scannedUID.toUpperCase();

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();

    Serial.print("Scanned RFID UID: ");
    Serial.println(scannedUID);

    for (int i = 0; i < USER_COUNT; i++)
    {
        if (scannedUID == rfidUIDs[i])
        {
            Serial.println("RFID card recognised.");
            handleUser(i);
            return;
        }
    }

    handleFailedAttempt("Unknown Card");
}

// ======================================================
// 15. KEY HANDLING
// ======================================================

void handleKeySelect(char key)
{
    if (key >= '1' && key <= '3')
    {
        keyClick();
        goToAuthenticate(key - '0');
    }
    // all other keys are ignored on the selection screen
}

void handleKeyAuth(char key)
{
    lastActivity = millis();

    if (key == '0')
    {
        keyClick();
        Serial.println("Exit pressed.");
        showLCD("Exiting...", "");
        delay(500);
        goToSelectBox();
    }
    else if (key >= '1' && key <= '9')
    {
        if (enteredPIN.length() < 4)
        {
            keyClick();
            enteredPIN += key;
            showLoginScreen();
        }
    }
    else if (key == '#')
    {
        keyClick();
        if (enteredPIN.length() == 4)
        {
            checkPIN();
        }
        else
        {
            showLCD("Enter 4 Digit", "PIN First");
            delay(1000);
            showLoginScreen();
        }
    }
    else if (key == '*')
    {
        keyClick();
        enteredPIN = "";
        Serial.println("PIN cleared.");
        showLoginScreen();
    }
    // A, B, C, D are ignored
}

// ======================================================
// 16. SETUP
// ======================================================

void setup()
{
    Serial.begin(115200);

    // Outputs first so nothing floats during start-up
    pinMode(GREEN_LED, OUTPUT);
    pinMode(YELLOW_LED, OUTPUT);
    pinMode(RED_LED, OUTPUT);
#if BUZZER_ENABLED
    pinMode(BUZZER, OUTPUT);
#endif
    allLEDsOff();

    // Keypad
    keypadInit();

    // LCD - I2C0
    Wire.setSDA(LCD_SDA);
    Wire.setSCL(LCD_SCL);
    Wire.begin();
    lcd.init();
    lcd.backlight();
    showLCD("MED DISPENSER", "Starting...");

    // RFID - SPI0
    SPI.setRX(RFID_MISO);
    SPI.setCS(RFID_SS);
    SPI.setSCK(RFID_SCK);
    SPI.setTX(RFID_MOSI);
    SPI.begin();
    rfid.PCD_Init();

    // Servos
    generalServo.attach(SERVO_GENERAL, SERVO_MIN_US, SERVO_MAX_US);
    severeServo.attach(SERVO_SEVERE, SERVO_MIN_US, SERVO_MAX_US);
    highServo.attach(SERVO_HIGH, SERVO_MIN_US, SERVO_MAX_US);
    generalServo.write(SERVO_CLOSED);
    severeServo.write(SERVO_CLOSED);
    highServo.write(SERVO_CLOSED);

    delay(1500);

    Serial.println("--------------------------------");
    Serial.println("MEDICINE DISPENSER VERSION 2");
    Serial.println("--------------------------------");

    // Self-checks
    byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    if (version == 0x00 || version == 0xFF)
    {
        Serial.println("WARNING: RFID reader not detected. Check wiring / 3.3V.");
    }
    else
    {
        Serial.print("RFID reader OK (version 0x");
        Serial.print(version, HEX);
        Serial.println(")");
    }

    char stuck = keypadScanRaw();
    if (stuck != 0)
    {
        Serial.print("WARNING: key '");
        Serial.print(stuck);
        Serial.println("' reads as pressed at start-up. Check keypad wiring.");
    }

    goToSelectBox();
}

// ======================================================
// 17. MAIN LOOP
// ======================================================

void loop()
{
    char key = getKey();

    if (key)
    {
        Serial.print("Key: ");
        Serial.println(key);
    }

    if (state == SELECT_BOX)
    {
        if (key) handleKeySelect(key);
    }
    else if (state == AUTHENTICATE)
    {
        if (key) handleKeyAuth(key);

        // handleKeyAuth may have changed the state
        if (state == AUTHENTICATE)
        {
            checkRFID();
        }

        if (state == AUTHENTICATE &&
            millis() - lastActivity > LOGIN_TIMEOUT)
        {
            Serial.println("Login timed out.");
            showLCD("Session Timeout", "Start Again");
            deniedSignal();
            delay(1000);
            goToSelectBox();
        }
    }
}
