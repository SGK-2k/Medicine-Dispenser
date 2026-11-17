// ======================================================
// MEDICINE DISPENSER - Raspberry Pi Pico W  (Version 4)
//
// Same behaviour as version 3 and the same USB protocol, so the
// MediDispense GUI works unchanged. Version 4 restructures the code
// around data structures and object-oriented design.
//
// ------------------------------------------------------
// RUBRIC MAP: Data Structures / OOP
// ------------------------------------------------------
// DATA STRUCTURES (pointers, dynamic structs, stacks/queues/lists)
//   PinStack        Stack built from dynamically allocated nodes (new/delete).
//                   Holds the PIN digits; 'B' on the keypad pops the last one.
//   Queue<T>        Template FIFO queue (singly linked list, head/tail pointers).
//                   Keypad, RFID and USB inputs are queued as InputEvent structs
//                   and processed in order.
//   UserRegistry    Singly linked list of dynamically created User structs.
//                   Searched by PIN or card UID, returns a User* (or nullptr).
//   AccessHistory   Doubly linked list (prev/next pointers) of AccessRecord
//                   structs, capped at 20 entries; oldest node is deleted.
//                   Send "HISTORY" over USB to print it newest first.
//   EventBus        Linked list of EventListener* (observer list).
//   Pointer use     BoxServo* array, Device* array, InputSource* array,
//                   DispenserState* current state, User* lookups.
//
// OOP (classes, inheritance, design patterns)
//   Inheritance / polymorphism (abstract base classes with virtual functions):
//     Device        -> Led, Buzzer, BoxServo, LcdDisplay
//     InputSource   -> KeypadInput, RfidInput, SerialInput
//     Authenticator -> PinAuthenticator, RfidAuthenticator
//     EventListener -> SerialReporter
//     DispenserState-> SelectBoxState, AuthenticateState
//   Design patterns:
//     Singleton  Dispenser::instance()      - exactly one dispenser object
//     State      DispenserState subclasses  - behaviour depends on screen
//     Observer   EventBus + EventListener   - devices publish, reporter sends USB
//     Factory    UserFactory::create()      - builds User structs on the heap
//     Facade     Dispenser                  - one simple interface over all devices
//     Strategy   Authenticator subclasses   - PIN or RFID check, same call
//
// ------------------------------------------------------
// FLOW
// ------------------------------------------------------
//   1. Select the box (keypad 1-3, or click a box in the GUI)
//   2. Authenticate with an RFID card OR a 4-digit PIN + '#'
//   3. If the user's role is allowed for that box, the box opens
//
// Keys:  1-3 select box | 1-9 PIN digits | # submit | * clear
//        B delete last digit | 0 exit to box selection
// A wrong PIN / unknown card keeps you on the login screen.
// After 3 wrong tries the login locks for 10 seconds.
//
// ------------------------------------------------------
// USB SERIAL PROTOCOL (115200 baud, one command per line)
// ------------------------------------------------------
// PC -> Pico: HELLO, STATUS, PING, HISTORY, SELECT <1-3>, PIN <dddd>, CLEAR, EXIT
// Pico -> PC: lines starting with "EVT " (READY, BOX, ROLE, STATUS, STATE, LCD,
//   LED, KEY, PINLEN, CARD, AUTH, FAIL, DENIED, LOCK, UNLOCK, OPEN, CLOSE,
//   TIMEOUT, HIST, ERR, PONG). Any other line is a human-readable log.
//
// Board package: "Raspberry Pi Pico/RP2040" by Earle Philhower
// Libraries: MFRC522 (GithubCommunity), LiquidCrystal I2C (Frank de Brabander)
// ======================================================

#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Servo.h>
#include <LiquidCrystal_I2C.h>

#define FIRMWARE_VERSION "4.0"

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
#define LCD_ADDRESS   0x27      // change to 0x3F if your LCD uses it

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

// Servo positions (degrees) and pulse range for a full 0-180 sweep
#define SERVO_CLOSED  0
#define SERVO_OPEN    180
#define SERVO_MIN_US  500
#define SERVO_MAX_US  2500

// ======================================================
// 2. SETTINGS
// ======================================================

const unsigned long LOGIN_TIMEOUT = 30000;  // ms of inactivity before reset
const int           MAX_ATTEMPTS  = 3;      // wrong tries before a lockout
const unsigned long LOCKOUT_TIME  = 10000;  // ms the login is locked
const unsigned long BOX_OPEN_TIME = 3000;   // ms the box stays open
const uint8_t       PIN_LENGTH    = 4;
const size_t        HISTORY_SIZE  = 20;     // records kept in AccessHistory
const size_t        QUEUE_SIZE    = 16;     // max pending input events
const int           BOX_COUNT     = 3;

const char* const BOX_NAMES[] = { "", "General", "Severe", "High Sev" };

// ------------------------------------------------------
// USER TABLE - one row per user. Replace the card UIDs with the
// ones printed by RFID_UID_Reader.ino.
// level = highest box the user may open (1 General, 2 Severe, 3 High)
// ------------------------------------------------------
struct UserConfig
{
    const char* name;
    const char* pin;
    const char* uid;
    int         level;
};

const UserConfig USER_TABLE[] =
{
    { "Patient",    "1111", "9DB9BE21", 1 },
    { "Head Nurse", "3333", "F3A0EEE3", 2 },
    { "Doctor",     "4444", "CFDE401C", 3 }
};

const int USER_TABLE_SIZE = sizeof(USER_TABLE) / sizeof(USER_TABLE[0]);


// ######################################################
//                  DATA STRUCTURES
// ######################################################

// ======================================================
// 3. STACK - PIN digits (linked nodes on the heap)
// ======================================================
class PinStack
{
public:
    explicit PinStack(uint8_t capacity) : capacity_(capacity) {}
    ~PinStack() { clear(); }

    // Push a digit on top. Returns false if the stack is full.
    bool push(char digit)
    {
        if (size_ >= capacity_) return false;
        Node* node = new Node{ digit, top_ };   // dynamic struct
        top_ = node;
        size_++;
        return true;
    }

    // Pop the top digit. Returns false if the stack is empty.
    bool pop(char* out = nullptr)
    {
        if (top_ == nullptr) return false;
        Node* node = top_;
        if (out != nullptr) *out = node->digit;
        top_ = node->next;
        delete node;                            // free the node
        size_--;
        return true;
    }

    void clear()
    {
        while (pop()) { }
    }

    uint8_t size()  const { return size_; }
    bool    full()  const { return size_ >= capacity_; }
    bool    empty() const { return size_ == 0; }

    // Digits in the order they were typed (bottom of the stack first)
    String toString() const
    {
        char buf[17];
        uint8_t i = size_;
        buf[i] = '\0';
        for (Node* n = top_; n != nullptr; n = n->next)
        {
            buf[--i] = n->digit;
        }
        return String(buf);
    }

private:
    struct Node
    {
        char  digit;
        Node* next;
    };

    Node*         top_  = nullptr;
    uint8_t       size_ = 0;
    const uint8_t capacity_;
};

// ======================================================
// 4. QUEUE - generic FIFO (singly linked list)
// ======================================================
template <typename T>
class Queue
{
public:
    explicit Queue(size_t capacity) : capacity_(capacity) {}

    ~Queue()
    {
        T discard;
        while (pop(discard)) { }
    }

    // Add to the back. Returns false if the queue is full.
    bool push(const T& value)
    {
        if (size_ >= capacity_) return false;
        Node* node = new Node{ value, nullptr };
        if (tail_ != nullptr) tail_->next = node;
        else                  head_ = node;
        tail_ = node;
        size_++;
        return true;
    }

    // Remove from the front. Returns false if the queue is empty.
    bool pop(T& out)
    {
        if (head_ == nullptr) return false;
        Node* node = head_;
        out   = node->value;
        head_ = node->next;
        if (head_ == nullptr) tail_ = nullptr;
        delete node;
        size_--;
        return true;
    }

    bool   empty() const { return size_ == 0; }
    size_t size()  const { return size_; }

private:
    struct Node
    {
        T     value;
        Node* next;
    };

    Node*        head_ = nullptr;
    Node*        tail_ = nullptr;
    size_t       size_ = 0;
    const size_t capacity_;
};

// ======================================================
// 5. INPUT EVENT - what goes through the queue
// ======================================================
enum class InputType : uint8_t
{
    Key,        // key pressed on the keypad   (key)
    Card,       // RFID card scanned           (text = UID)
    SelectBox,  // SELECT n from the PC        (box)
    Pin,        // PIN dddd from the PC        (text = PIN)
    Clear,      // CLEAR from the PC
    Exit,       // EXIT from the PC
    Hello,      // HELLO from the PC
    Status,     // STATUS from the PC
    Ping,       // PING from the PC
    History,    // HISTORY from the PC
    Unknown     // anything else               (text = command)
};

struct InputEvent
{
    InputType type = InputType::Unknown;
    char      key  = 0;
    int       box  = 0;
    String    text = "";
};

// ======================================================
// 6. USER REGISTRY - singly linked list of User structs
// ======================================================
struct User
{
    String name;
    String pin;
    String uid;
    int    level;
    User*  next;
};

// Factory pattern: the only place where User structs are created
class UserFactory
{
public:
    static User* create(const UserConfig& cfg)
    {
        return new User{ cfg.name, cfg.pin, cfg.uid, cfg.level, nullptr };
    }
};

class UserRegistry
{
public:
    ~UserRegistry()
    {
        while (head_ != nullptr)
        {
            User* next = head_->next;
            delete head_;
            head_ = next;
        }
    }

    // Append to the end of the list (keeps the table order)
    void add(User* user)
    {
        if (user == nullptr) return;
        user->next = nullptr;
        if (head_ == nullptr)
        {
            head_ = user;
        }
        else
        {
            User* cur = head_;
            while (cur->next != nullptr) cur = cur->next;
            cur->next = user;
        }
        count_++;
    }

    User* findByPin(const String& pin) const
    {
        for (User* u = head_; u != nullptr; u = u->next)
        {
            if (u->pin == pin) return u;
        }
        return nullptr;
    }

    User* findByUid(const String& uid) const
    {
        for (User* u = head_; u != nullptr; u = u->next)
        {
            if (u->uid == uid) return u;
        }
        return nullptr;
    }

    User* first() const { return head_; }
    int   count() const { return count_; }

private:
    User* head_  = nullptr;
    int   count_ = 0;
};

// ======================================================
// 7. ACCESS HISTORY - doubly linked list, newest at the tail
// ======================================================
struct AccessRecord
{
    unsigned long timeMs;
    int           box;
    String        who;
    String        method;
    String        result;     // OPENED, DENIED, FAILED, LOCKED, TIMEOUT
    AccessRecord* prev;
    AccessRecord* next;
};

class AccessHistory
{
public:
    explicit AccessHistory(size_t capacity) : capacity_(capacity) {}

    ~AccessHistory()
    {
        while (head_ != nullptr) removeOldest();
    }

    void add(int box, const String& who, const String& method, const String& result)
    {
        AccessRecord* rec = new AccessRecord{ millis(), box, who, method, result, tail_, nullptr };

        if (tail_ != nullptr) tail_->next = rec;
        else                  head_ = rec;
        tail_ = rec;
        size_++;

        if (size_ > capacity_) removeOldest();
    }

    // Walk from newest to oldest using the prev pointers
    template <typename Visitor>
    void forEachNewestFirst(Visitor visit) const
    {
        for (AccessRecord* r = tail_; r != nullptr; r = r->prev)
        {
            visit(*r);
        }
    }

    size_t size() const { return size_; }

private:
    void removeOldest()
    {
        if (head_ == nullptr) return;
        AccessRecord* old = head_;
        head_ = old->next;
        if (head_ != nullptr) head_->prev = nullptr;
        else                  tail_ = nullptr;
        delete old;
        size_--;
    }

    AccessRecord* head_ = nullptr;
    AccessRecord* tail_ = nullptr;
    size_t        size_ = 0;
    const size_t  capacity_;
};


// ######################################################
//              OBSERVER: EVENTS FOR THE GUI
// ######################################################

// ======================================================
// 8. EVENT LISTENER (interface) + EVENT BUS (subject)
// ======================================================
class EventListener
{
public:
    virtual ~EventListener() {}
    virtual void onEvent(const String& event) = 0;
};

class EventBus
{
public:
    ~EventBus()
    {
        while (head_ != nullptr)
        {
            Node* next = head_->next;
            delete head_;
            head_ = next;
        }
    }

    void subscribe(EventListener* listener)
    {
        head_ = new Node{ listener, head_ };
    }

    void publish(const String& event) const
    {
        for (Node* n = head_; n != nullptr; n = n->next)
        {
            n->listener->onEvent(event);
        }
    }

private:
    struct Node
    {
        EventListener* listener;
        Node*          next;
    };

    Node* head_ = nullptr;
};

// Sends every event to the PC as "EVT ..." (read by the GUI)
class SerialReporter : public EventListener
{
public:
    void onEvent(const String& event) override
    {
        Serial.print("EVT ");
        Serial.println(event);
    }
};


// ######################################################
//              DEVICES (inheritance from Device)
// ######################################################

// ======================================================
// 9. DEVICE BASE CLASS
// ======================================================
class Device
{
public:
    Device(const char* name, EventBus* bus) : name_(name), bus_(bus) {}
    virtual ~Device() {}

    virtual void begin() = 0;                 // set up the hardware
    const char* name() const { return name_; }

protected:
    void publish(const String& event) const
    {
        if (bus_ != nullptr) bus_->publish(event);
    }

    const char* name_;
    EventBus*   bus_;
};

// ======================================================
// 10. LED
// ======================================================
class Led : public Device
{
public:
    Led(const char* name, uint8_t pin, char code, EventBus* bus)
        : Device(name, bus), pin_(pin), code_(code) {}

    void begin() override
    {
        pinMode(pin_, OUTPUT);
        set(false);
    }

    void set(bool on)
    {
        on_ = on;
        digitalWrite(pin_, on ? HIGH : LOW);
        publish(String("LED ") + code_ + (on ? " 1" : " 0"));
    }

    bool isOn() const { return on_; }
    char code() const { return code_; }

private:
    uint8_t pin_;
    char    code_;
    bool    on_ = false;
};

// ======================================================
// 11. BUZZER
// ======================================================
class Buzzer : public Device
{
public:
    Buzzer(const char* name, uint8_t pin) : Device(name, nullptr), pin_(pin) {}

    void begin() override { pinMode(pin_, OUTPUT); }

    void beep(unsigned int frequency, unsigned long duration)
    {
        tone(pin_, frequency, duration);
    }

    void click()   { beep(2500, 30);  }
    void success() { beep(1500, 150); }
    void denied()  { beep(400, 500);  }
    void open()    { beep(1800, 200); }

private:
    uint8_t pin_;
};

// ======================================================
// 12. BOX SERVO
// ======================================================
class BoxServo : public Device
{
public:
    BoxServo(const char* name, uint8_t pin, int box, EventBus* bus)
        : Device(name, bus), pin_(pin), box_(box) {}

    void begin() override
    {
        servo_.attach(pin_, SERVO_MIN_US, SERVO_MAX_US);
        servo_.write(SERVO_CLOSED);
    }

    void open()
    {
        servo_.write(SERVO_OPEN);
        open_ = true;
        publish("OPEN " + String(box_));
    }

    void close()
    {
        servo_.write(SERVO_CLOSED);
        open_ = false;
        publish("CLOSE " + String(box_));
    }

    bool isOpen() const { return open_; }
    int  box()    const { return box_; }

private:
    Servo   servo_;
    uint8_t pin_;
    int     box_;
    bool    open_ = false;
};

// ======================================================
// 13. LCD DISPLAY
// ======================================================
class LcdDisplay : public Device
{
public:
    LcdDisplay(const char* name, uint8_t address, EventBus* bus)
        : Device(name, bus), lcd_(address, 16, 2) {}

    void begin() override
    {
        Wire.setSDA(LCD_SDA);
        Wire.setSCL(LCD_SCL);
        Wire.begin();
        lcd_.init();
        lcd_.backlight();
    }

    void show(const String& line1, const String& line2)
    {
        line1_ = line1.substring(0, 16);
        line2_ = line2.substring(0, 16);

        lcd_.clear();
        lcd_.setCursor(0, 0);
        lcd_.print(line1_);
        lcd_.setCursor(0, 1);
        lcd_.print(line2_);

        publish("LCD " + line1_ + "|" + line2_);
    }

    // Re-send the current screen (used by STATUS)
    void republish() const { publish("LCD " + line1_ + "|" + line2_); }

private:
    LiquidCrystal_I2C lcd_;
    String            line1_ = "";
    String            line2_ = "";
};


// ######################################################
//            INPUT SOURCES (inheritance from InputSource)
// ######################################################

// ======================================================
// 14. INPUT SOURCE BASE CLASS
// ======================================================
class InputSource
{
public:
    virtual ~InputSource() {}
    virtual void begin() {}
    virtual void poll(Queue<InputEvent>& queue) = 0;   // add new events to the queue
};

// ======================================================
// 15. KEYPAD INPUT (custom scanner, no Keypad library)
// ======================================================
class KeypadInput : public InputSource
{
public:
    void begin() override
    {
        for (byte r = 0; r < ROWS; r++) pinMode(ROW_PINS[r], INPUT_PULLUP);
        for (byte c = 0; c < COLS; c++) pinMode(COL_PINS[c], INPUT);   // idle = high impedance
    }

    void poll(Queue<InputEvent>& queue) override
    {
        char raw = scanRaw();
        unsigned long now = millis();

        if (raw != lastRaw_)
        {
            lastRaw_    = raw;
            lastChange_ = now;
        }

        if (now - lastChange_ >= DEBOUNCE_MS && raw != stableKey_)
        {
            stableKey_ = raw;
            if (stableKey_ != 0)
            {
                InputEvent e;
                e.type = InputType::Key;
                e.key  = stableKey_;
                queue.push(e);
            }
        }
    }

    // Returns the key currently held, or 0 if none / more than one
    char scanRaw() const
    {
        char found = 0;
        int  count = 0;

        for (byte c = 0; c < COLS; c++)
        {
            pinMode(COL_PINS[c], OUTPUT);
            digitalWrite(COL_PINS[c], LOW);
            delayMicroseconds(20);                 // let the line settle

            for (byte r = 0; r < ROWS; r++)
            {
                if (digitalRead(ROW_PINS[r]) == LOW)
                {
                    found = KEYS[r][c];
                    count++;
                }
            }

            pinMode(COL_PINS[c], INPUT);
            delayMicroseconds(20);
        }

        return (count == 1) ? found : 0;
    }

private:
    static const byte ROWS = 4;
    static const byte COLS = 4;
    static constexpr unsigned long DEBOUNCE_MS = 30;

    // Keypad connector, left to right: R1 R2 R3 R4 C1 C2 C3 C4
    static constexpr byte ROW_PINS[ROWS] = { 0, 1, 2, 3 };
    static constexpr byte COL_PINS[COLS] = { 12, 13, 14, 15 };
    static constexpr char KEYS[ROWS][COLS] =
    {
        { '1', '2', '3', 'A' },
        { '4', '5', '6', 'B' },
        { '7', '8', '9', 'C' },
        { '*', '0', '#', 'D' }
    };

    char          lastRaw_    = 0;
    char          stableKey_  = 0;
    unsigned long lastChange_ = 0;
};

// ======================================================
// 16. RFID INPUT
// ======================================================
class RfidInput : public InputSource
{
public:
    explicit RfidInput(MFRC522* reader) : reader_(reader) {}

    void begin() override
    {
        SPI.setRX(RFID_MISO);
        SPI.setCS(RFID_SS);
        SPI.setSCK(RFID_SCK);
        SPI.setTX(RFID_MOSI);
        SPI.begin();
        reader_->PCD_Init();
    }

    // The reader is only checked on the login screen
    void setEnabled(bool enabled) { enabled_ = enabled; }

    bool detected() const
    {
        byte v = reader_->PCD_ReadRegister(MFRC522::VersionReg);
        return !(v == 0x00 || v == 0xFF);
    }

    void poll(Queue<InputEvent>& queue) override
    {
        if (!enabled_) return;
        if (!reader_->PICC_IsNewCardPresent()) return;
        if (!reader_->PICC_ReadCardSerial())   return;

        String uid = "";
        for (byte i = 0; i < reader_->uid.size; i++)
        {
            if (reader_->uid.uidByte[i] < 0x10) uid += "0";
            uid += String(reader_->uid.uidByte[i], HEX);
        }
        uid.toUpperCase();

        reader_->PICC_HaltA();
        reader_->PCD_StopCrypto1();

        InputEvent e;
        e.type = InputType::Card;
        e.text = uid;
        queue.push(e);
    }

private:
    MFRC522* reader_;
    bool     enabled_ = false;
};

// ======================================================
// 17. SERIAL INPUT (commands from the GUI)
// ======================================================
class SerialInput : public InputSource
{
public:
    void poll(Queue<InputEvent>& queue) override
    {
        while (Serial.available() > 0)
        {
            char c = (char)Serial.read();

            if (c == '\n' || c == '\r')
            {
                if (line_.length() > 0)
                {
                    parse(line_, queue);
                    line_ = "";
                }
            }
            else if (line_.length() < 64)
            {
                line_ += c;
            }
        }
    }

private:
    void parse(String cmd, Queue<InputEvent>& queue)
    {
        cmd.trim();
        if (cmd.length() == 0) return;

        String upper = cmd;
        upper.toUpperCase();

        InputEvent e;
        e.text = cmd;

        if      (upper == "HELLO")   e.type = InputType::Hello;
        else if (upper == "STATUS")  e.type = InputType::Status;
        else if (upper == "PING")    e.type = InputType::Ping;
        else if (upper == "HISTORY") e.type = InputType::History;
        else if (upper == "CLEAR")   e.type = InputType::Clear;
        else if (upper == "EXIT")    e.type = InputType::Exit;
        else if (upper.startsWith("SELECT "))
        {
            e.type = InputType::SelectBox;
            e.box  = upper.substring(7).toInt();
        }
        else if (upper.startsWith("PIN "))
        {
            e.type = InputType::Pin;
            e.text = upper.substring(4);
            e.text.trim();
        }
        else
        {
            e.type = InputType::Unknown;
        }

        queue.push(e);
    }

    String line_ = "";
};


// ######################################################
//        STRATEGY: AUTHENTICATORS (inheritance)
// ######################################################

// ======================================================
// 18. AUTHENTICATOR BASE CLASS + PIN / RFID
// ======================================================
class Authenticator
{
public:
    explicit Authenticator(const UserRegistry* users) : users_(users) {}
    virtual ~Authenticator() {}

    // Returns the matching user, or nullptr if the credential is wrong
    virtual User*       authenticate(const String& credential) const = 0;
    virtual const char* method()   const = 0;     // PIN or CARD
    virtual const char* failText() const = 0;     // shown on the LCD

protected:
    const UserRegistry* users_;
};

class PinAuthenticator : public Authenticator
{
public:
    explicit PinAuthenticator(const UserRegistry* users) : Authenticator(users) {}

    User* authenticate(const String& pin) const override
    {
        if (pin.length() != PIN_LENGTH) return nullptr;
        return users_->findByPin(pin);
    }

    const char* method()   const override { return "PIN"; }
    const char* failText() const override { return "Invalid PIN"; }
};

class RfidAuthenticator : public Authenticator
{
public:
    explicit RfidAuthenticator(const UserRegistry* users) : Authenticator(users) {}

    User* authenticate(const String& uid) const override
    {
        return users_->findByUid(uid);
    }

    const char* method()   const override { return "CARD"; }
    const char* failText() const override { return "Unknown Card"; }
};


// ######################################################
//          STATE PATTERN + SINGLETON / FACADE
// ######################################################

class Dispenser;   // forward declaration (states need a reference to it)

// ======================================================
// 19. STATE BASE CLASS + THE TWO SCREENS
// ======================================================
class DispenserState
{
public:
    virtual ~DispenserState() {}
    virtual const char* name() const = 0;
    virtual void enter(Dispenser& d) = 0;
    virtual void handle(Dispenser& d, const InputEvent& e) = 0;
    virtual void update(Dispenser& d) { (void)d; }
};

class SelectBoxState : public DispenserState
{
public:
    const char* name() const override { return "SELECT"; }
    void enter(Dispenser& d) override;
    void handle(Dispenser& d, const InputEvent& e) override;
};

class AuthenticateState : public DispenserState
{
public:
    const char* name() const override { return "AUTH"; }
    void enter(Dispenser& d) override;
    void handle(Dispenser& d, const InputEvent& e) override;
    void update(Dispenser& d) override;
};

// ======================================================
// 20. DISPENSER - Singleton + Facade
// ======================================================
class Dispenser
{
public:
    // Singleton: the one and only dispenser
    static Dispenser& instance()
    {
        static Dispenser theDispenser;
        return theDispenser;
    }

    Dispenser(const Dispenser&) = delete;
    Dispenser& operator=(const Dispenser&) = delete;

    void begin();
    void loop();

    // ---------- facade used by the states ----------
    void changeState(DispenserState* next)
    {
        state_ = next;
        state_->enter(*this);
    }

    void goToSelect()          { changeState(&selectState_); }
    void selectBox(int box)    { selectedBox_ = box; changeState(&authState_); }

    void showSelectScreen()    { lcd_.show("Select Box 1-3", "1Low 2Mid 3High"); }
    void showLoginScreen();

    void allLedsOff()          { for (int i = 0; i < 3; i++) leds_[i]->set(false); }
    void successSignal()       { green_.set(true); buzzer_.success(); delay(200); green_.set(false); }
    void deniedSignal()        { red_.set(true);   buzzer_.denied();  delay(500); red_.set(false);   }

    void resetSession()        { selectedBox_ = 0; attempts_ = 0; pin_.clear(); }
    void touch()               { lastActivity_ = millis(); }
    bool timedOut() const      { return millis() - lastActivity_ > LOGIN_TIMEOUT; }

    void tryLogin(const Authenticator& auth, const String& credential, const char* method);
    void handleTimeout();
    void emit(const String& event) const { bus_.publish(event); }

    // accessors
    LcdDisplay&               lcd()      { return lcd_; }
    Buzzer&                   buzzer()   { return buzzer_; }
    Led&                      yellow()   { return yellow_; }
    RfidInput&                rfid()     { return rfidInput_; }
    PinStack&                 pin()      { return pin_; }
    const PinAuthenticator&   pinAuth()  { return pinAuth_; }
    const RfidAuthenticator&  rfidAuth() { return rfidAuth_; }
    int                       selectedBox() const { return selectedBox_; }

private:
    Dispenser();

    void dispatch(const InputEvent& e);
    void sendHello();
    void sendStatus();
    void sendHistory();
    void handleUser(User* user, const char* method);
    void handleFailedAttempt(const char* text, const char* method);
    void openBox(int box);
    BoxServo* servoFor(int box) const;

    // Observer
    EventBus       bus_;
    SerialReporter reporter_;

    // Devices
    LcdDisplay lcd_;
    Led        green_;
    Led        yellow_;
    Led        red_;
    Buzzer     buzzer_;
    BoxServo*  boxes_[BOX_COUNT];      // created with new in the constructor
    Led*       leds_[3];
    Device*    devices_[5 + BOX_COUNT];
    int        deviceCount_ = 0;

    // Inputs
    MFRC522      reader_;
    KeypadInput  keypadInput_;
    RfidInput    rfidInput_;
    SerialInput  serialInput_;
    InputSource* inputs_[3];
    Queue<InputEvent> inputQueue_;

    // Users and authentication
    UserRegistry      users_;
    PinAuthenticator  pinAuth_;
    RfidAuthenticator rfidAuth_;

    // History
    AccessHistory history_;

    // States
    SelectBoxState     selectState_;
    AuthenticateState  authState_;
    DispenserState*    state_ = nullptr;

    // Session data
    int           selectedBox_  = 0;
    PinStack      pin_;
    int           attempts_     = 0;
    unsigned long lastActivity_ = 0;
    bool          locked_       = false;
    bool          busy_         = false;
};

// ------------------------------------------------------
// Dispenser: construction (no hardware access here)
// ------------------------------------------------------
Dispenser::Dispenser()
    : lcd_("LCD", LCD_ADDRESS, &bus_),
      green_("Green LED", GREEN_LED, 'G', &bus_),
      yellow_("Yellow LED", YELLOW_LED, 'Y', &bus_),
      red_("Red LED", RED_LED, 'R', &bus_),
      buzzer_("Buzzer", BUZZER),
      reader_(RFID_SS, RFID_RST),
      rfidInput_(&reader_),
      inputQueue_(QUEUE_SIZE),
      pinAuth_(&users_),
      rfidAuth_(&users_),
      history_(HISTORY_SIZE),
      pin_(PIN_LENGTH)
{
    bus_.subscribe(&reporter_);

    // Box servos are allocated dynamically and kept as pointers
    boxes_[0] = new BoxServo("Box 1 servo", SERVO_GENERAL, 1, &bus_);
    boxes_[1] = new BoxServo("Box 2 servo", SERVO_SEVERE,  2, &bus_);
    boxes_[2] = new BoxServo("Box 3 servo", SERVO_HIGH,    3, &bus_);

    leds_[0] = &green_;
    leds_[1] = &yellow_;
    leds_[2] = &red_;

    // Every device in one array of base-class pointers (polymorphism)
    devices_[deviceCount_++] = &green_;
    devices_[deviceCount_++] = &yellow_;
    devices_[deviceCount_++] = &red_;
    devices_[deviceCount_++] = &buzzer_;
    devices_[deviceCount_++] = &lcd_;
    for (int i = 0; i < BOX_COUNT; i++) devices_[deviceCount_++] = boxes_[i];

    inputs_[0] = &keypadInput_;
    inputs_[1] = &serialInput_;
    inputs_[2] = &rfidInput_;
}

// ------------------------------------------------------
// Dispenser: start-up
// ------------------------------------------------------
void Dispenser::begin()
{
    for (int i = 0; i < deviceCount_; i++) devices_[i]->begin();   // virtual call
    lcd_.show("MED DISPENSER", "Starting...");

    for (int i = 0; i < 3; i++) inputs_[i]->begin();               // virtual call

    for (int i = 0; i < USER_TABLE_SIZE; i++)
    {
        users_.add(UserFactory::create(USER_TABLE[i]));
    }

    delay(1500);
    Serial.println("MEDICINE DISPENSER VERSION 4");

    if (!rfidInput_.detected())
    {
        emit("ERR RFID reader not detected - check wiring and 3.3V");
    }

    char stuck = keypadInput_.scanRaw();
    if (stuck != 0)
    {
        emit(String("ERR key ") + stuck + " reads as pressed at start-up - check keypad wiring");
    }

    sendHello();
    goToSelect();
}

// ------------------------------------------------------
// Dispenser: main loop - poll inputs, then empty the queue
// ------------------------------------------------------
void Dispenser::loop()
{
    for (int i = 0; i < 3; i++) inputs_[i]->poll(inputQueue_);     // virtual call

    InputEvent e;
    while (inputQueue_.pop(e))
    {
        dispatch(e);
    }

    state_->update(*this);
}

void Dispenser::dispatch(const InputEvent& e)
{
    switch (e.type)
    {
        case InputType::Hello:   sendHello(); sendStatus(); return;
        case InputType::Status:  sendStatus();              return;
        case InputType::Ping:    emit("PONG");              return;
        case InputType::History: sendHistory();             return;
        case InputType::Unknown: emit("ERR unknown command: " + e.text); return;
        case InputType::Key:     emit(String("KEY ") + e.key); break;
        default: break;
    }

    state_->handle(*this, e);    // the current state decides what the input means
}

// ------------------------------------------------------
// Dispenser: screens and USB reports
// ------------------------------------------------------
void Dispenser::showLoginScreen()
{
    String line1 = "Box" + String(selectedBox_) + ":" + BOX_NAMES[selectedBox_];

    if (pin_.empty())
    {
        lcd_.show(line1, "Card/PIN# 0=Exit");
    }
    else
    {
        String masked = "";
        for (uint8_t i = 0; i < pin_.size(); i++) masked += "*";
        lcd_.show(line1, "PIN: " + masked);
    }

    emit("PINLEN " + String(pin_.size()));
}

void Dispenser::sendHello()
{
    emit(String("READY ") + FIRMWARE_VERSION);

    for (int b = 1; b <= BOX_COUNT; b++)
    {
        emit("BOX " + String(b) + " " + BOX_NAMES[b]);
    }

    for (User* u = users_.first(); u != nullptr; u = u->next)
    {
        emit("ROLE " + String(u->level) + " " + u->name);
    }
}

void Dispenser::sendStatus()
{
    const char* st = busy_ ? "BUSY" : locked_ ? "LOCKED" : state_->name();

    emit(String("STATUS ") + st + " " + String(selectedBox_) + " " +
         String(attempts_) + " " + String(MAX_ATTEMPTS));

    lcd_.republish();
    for (int i = 0; i < 3; i++)
    {
        emit(String("LED ") + leds_[i]->code() + (leds_[i]->isOn() ? " 1" : " 0"));
    }
    emit("PINLEN " + String(pin_.size()));
}

void Dispenser::sendHistory()
{
    unsigned long now = millis();

    history_.forEachNewestFirst([&](const AccessRecord& r)
    {
        emit("HIST " + String((now - r.timeMs) / 1000) + " " + String(r.box) + " " +
             r.method + " " + r.result + " " + r.who);
    });

    emit("HIST END " + String(history_.size()));
}

// ------------------------------------------------------
// Dispenser: authentication and opening boxes
// ------------------------------------------------------
BoxServo* Dispenser::servoFor(int box) const
{
    if (box < 1 || box > BOX_COUNT) return nullptr;
    return boxes_[box - 1];
}

void Dispenser::tryLogin(const Authenticator& auth, const String& credential, const char* method)
{
    touch();
    User* user = auth.authenticate(credential);   // Strategy: PIN or RFID
    pin_.clear();

    if (user != nullptr) handleUser(user, method);
    else                 handleFailedAttempt(auth.failText(), method);
}

void Dispenser::handleUser(User* user, const char* method)
{
    emit(String("AUTH ") + method + " " + user->name);

    lcd_.show("Welcome!", user->name);
    successSignal();
    delay(1000);

    if (selectedBox_ <= user->level)
    {
        history_.add(selectedBox_, user->name, method, "OPENED");
        openBox(selectedBox_);
    }
    else
    {
        history_.add(selectedBox_, user->name, method, "DENIED");
        emit("DENIED " + String(selectedBox_) + " " + user->name);

        lcd_.show("Access Denied", "Not Authorised");
        deniedSignal();
        delay(1500);

        // Stay on the login screen so another user can log in
        touch();
        yellow_.set(true);
        showLoginScreen();
    }
}

void Dispenser::handleFailedAttempt(const char* text, const char* method)
{
    attempts_++;
    history_.add(selectedBox_, "-", method, "FAILED");
    emit(String("FAIL ") + method + " " + String(attempts_) + " " + String(MAX_ATTEMPTS));

    lcd_.show(text, "Access Denied");
    deniedSignal();
    delay(1200);

    if (attempts_ >= MAX_ATTEMPTS)
    {
        locked_ = true;
        history_.add(selectedBox_, "-", method, "LOCKED");
        emit("LOCK " + String(LOCKOUT_TIME / 1000));
        red_.set(true);

        for (int s = LOCKOUT_TIME / 1000; s > 0; s--)
        {
            lcd_.show("Too Many Tries", "Wait " + String(s) + " s");
            delay(1000);
        }

        red_.set(false);
        attempts_ = 0;
        locked_   = false;
        emit("UNLOCK");
    }

    // Stay on the login screen for the SAME box
    touch();
    yellow_.set(true);
    showLoginScreen();
}

void Dispenser::openBox(int box)
{
    BoxServo* servo = servoFor(box);

    if (servo == nullptr)
    {
        emit("ERR invalid box " + String(box));
        lcd_.show("Invalid Box", "Start Again");
        deniedSignal();
        delay(1500);
        goToSelect();
        return;
    }

    busy_ = true;

    allLedsOff();
    green_.set(true);
    lcd_.show("Access Granted", "Opening Box " + String(box));
    buzzer_.open();

    servo->open();
    delay(BOX_OPEN_TIME);
    servo->close();

    lcd_.show("Box Closed", "Session Ended");
    delay(1500);

    busy_ = false;
    goToSelect();
}

void Dispenser::handleTimeout()
{
    history_.add(selectedBox_, "-", "-", "TIMEOUT");
    emit("TIMEOUT");
    lcd_.show("Session Timeout", "Start Again");
    deniedSignal();
    delay(1000);
    goToSelect();
}

// ======================================================
// 21. STATE IMPLEMENTATIONS
// ======================================================

// ---------- Screen 1: choose a box ----------
void SelectBoxState::enter(Dispenser& d)
{
    d.resetSession();
    d.rfid().setEnabled(false);
    d.allLedsOff();
    d.showSelectScreen();
    d.emit("STATE SELECT");
    Serial.println("Select box: 1 = General, 2 = Severe, 3 = High severity");
}

void SelectBoxState::handle(Dispenser& d, const InputEvent& e)
{
    switch (e.type)
    {
        case InputType::Key:
            if (e.key >= '1' && e.key <= '3')
            {
                d.buzzer().click();
                d.selectBox(e.key - '0');
            }
            break;

        case InputType::SelectBox:
            if (e.box < 1 || e.box > BOX_COUNT) d.emit("ERR box must be 1, 2 or 3");
            else                                d.selectBox(e.box);
            break;

        case InputType::Pin:
            d.emit("ERR select a box first");
            break;

        default:
            break;   // cards, CLEAR and EXIT do nothing here
    }
}

// ---------- Screen 2: log in for the selected box ----------
void AuthenticateState::enter(Dispenser& d)
{
    d.pin().clear();
    d.touch();
    d.allLedsOff();
    d.yellow().set(true);
    d.emit("STATE AUTH " + String(d.selectedBox()));
    d.showLoginScreen();
    d.rfid().setEnabled(true);

    Serial.print("Box selected: ");
    Serial.print(d.selectedBox());
    Serial.print(" (");
    Serial.print(BOX_NAMES[d.selectedBox()]);
    Serial.println(")");
}

void AuthenticateState::handle(Dispenser& d, const InputEvent& e)
{
    switch (e.type)
    {
        case InputType::Key:
        {
            d.touch();
            char k = e.key;

            if (k == '0')                               // exit
            {
                d.buzzer().click();
                d.lcd().show("Exiting...", "");
                delay(500);
                d.goToSelect();
            }
            else if (k >= '1' && k <= '9')              // push a digit
            {
                if (d.pin().push(k))
                {
                    d.buzzer().click();
                    d.showLoginScreen();
                }
            }
            else if (k == 'B')                          // pop the last digit
            {
                if (d.pin().pop())
                {
                    d.buzzer().click();
                    d.showLoginScreen();
                }
            }
            else if (k == '*')                          // clear the stack
            {
                d.buzzer().click();
                d.pin().clear();
                d.showLoginScreen();
            }
            else if (k == '#')                          // submit
            {
                d.buzzer().click();
                if (d.pin().full())
                {
                    d.tryLogin(d.pinAuth(), d.pin().toString(), "PIN");
                }
                else
                {
                    d.lcd().show("Enter 4 Digit", "PIN First");
                    delay(1000);
                    d.showLoginScreen();
                }
            }
            break;
        }

        case InputType::Card:
            d.emit("CARD " + e.text);
            d.tryLogin(d.rfidAuth(), e.text, "CARD");
            break;

        case InputType::Pin:
        {
            bool valid = (e.text.length() == PIN_LENGTH);
            for (unsigned int i = 0; i < e.text.length(); i++)
            {
                if (e.text[i] < '1' || e.text[i] > '9') valid = false;
            }

            if (!valid) d.emit("ERR PIN must be 4 digits from 1 to 9");
            else        d.tryLogin(d.pinAuth(), e.text, "PC");
            break;
        }

        case InputType::SelectBox:
            d.emit("ERR exit the current login first");
            break;

        case InputType::Clear:
            d.pin().clear();
            d.showLoginScreen();
            break;

        case InputType::Exit:
            d.lcd().show("Exiting...", "");
            delay(300);
            d.goToSelect();
            break;

        default:
            break;
    }
}

void AuthenticateState::update(Dispenser& d)
{
    if (d.timedOut()) d.handleTimeout();
}


// ######################################################
//                  ARDUINO ENTRY POINTS
// ######################################################

void setup()
{
    Serial.begin(115200);
    Dispenser::instance().begin();
}

void loop()
{
    Dispenser::instance().loop();
}
