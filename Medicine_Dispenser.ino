#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Servo.h>
#include <LiquidCrystal_I2C.h>

#define FIRMWARE_VERSION "6.2"

// ======================================================
// 1. PIN DEFINITIONS
// ======================================================
// WIRING SUMMARY (Pico W GP number -> physical pin)
//   Keypad rows R1-R4    GP0, GP1, GP2, GP3      (pins 1, 2, 4, 5)
//   Keypad cols C1-C4    GP12, GP13, GP14, GP15  (pins 16, 17, 19, 20)
//   RFID  MISO/SDA/SCK/MOSI  GP4, GP5, GP6, GP7  (pins 6, 7, 9, 10)
//   RFID  RST            GP28                    (pin 34)   RFID VCC = 3V3 only
//   LCD   SDA / SCL      GP8, GP9                (pins 11, 12)
//   Servo 1 / 2 / 3      GP16, GP17, GP18        (pins 21, 22, 24)
//   IR    1 / 2 / 3      GP19, GP20, GP21        (pins 25, 26, 27)

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

// IR sensors (one per box)
#define IR_BOX1       19
#define IR_BOX2       20
#define IR_BOX3       21
#define IR_ACTIVE_LOW 0        // 1 = output goes LOW when something is detected
                                //     (FC-51 style modules). Set 0 if yours goes HIGH.

// Servos
#define SERVO_GENERAL 16
#define SERVO_SEVERE  17
#define SERVO_HIGH    18

// Servo positions (degrees) and pulse range for a full 0-180 sweep
#define SERVO_CLOSED  0
#define SERVO_OPEN    180
#define SERVO_MIN_US  500
#define SERVO_MAX_US  2500

// ======================================================
// 2. SETTINGS
// ======================================================

const char*         REFILL_CODE      = "1234";  // refill / reset passcode
const unsigned long CLOSE_DELAY      = 5000;    // ms after IR change before a box closes
const unsigned long COLLECT_TIMEOUT  = 30000;   // ms to take the medicine
const unsigned long EMPTY_CONFIRM    = 3000;    // ms all boxes must read empty in reset
const unsigned long LOGIN_TIMEOUT    = 30000;   // ms of inactivity on the login screen
const unsigned long DOCTOR_SESSION   = 900000;  // 15 min doctor session on the GUI
const int           MAX_ATTEMPTS     = 3;       // wrong tries before a lockout
const unsigned long LOCKOUT_TIME     = 10000;   // ms locked after too many tries
const unsigned long IR_DEBOUNCE      = 80;      // ms an IR reading must be stable
const uint8_t       PIN_LENGTH       = 4;
const size_t        HISTORY_SIZE     = 20;
const size_t        QUEUE_SIZE       = 16;
const int           BOX_COUNT        = 3;

const char* const BOX_NAMES[] = { "", "General", "Severe", "High Sev" };

// ------------------------------------------------------
// USER TABLE - one row per user.
// level       = highest box the user may open (1 General, 2 Severe, 3 High)
// canPrescribe = may log in on the GUI and send prescriptions
// ------------------------------------------------------
struct UserConfig
{
    const char* name;
    const char* pin;
    const char* uid;
    int         level;
    bool        canPrescribe;
};

const UserConfig USER_TABLE[] =
{
    { "Patient",    "1111", "9DB9BE21", 1, false },
    { "Head Nurse", "3333", "F3A0EEE3", 2, false },
    { "Doctor",     "4444", "CFDE401C", 3, true  }
};

const int USER_TABLE_SIZE = sizeof(USER_TABLE) / sizeof(USER_TABLE[0]);


// ######################################################
//                  DATA STRUCTURES
// ######################################################

// ======================================================
// 3. STACK - code digits (linked nodes on the heap)
// ======================================================
class PinStack
{
public:
    explicit PinStack(uint8_t capacity) : capacity_(capacity) {}
    ~PinStack() { clear(); }

    bool push(char digit)
    {
        if (size_ >= capacity_) return false;
        top_ = new Node{ digit, top_ };
        size_++;
        return true;
    }

    bool pop(char* out = nullptr)
    {
        if (top_ == nullptr) return false;
        Node* node = top_;
        if (out != nullptr) *out = node->digit;
        top_ = node->next;
        delete node;
        size_--;
        return true;
    }

    void clear() { while (pop()) { } }

    uint8_t size()  const { return size_; }
    bool    full()  const { return size_ >= capacity_; }
    bool    empty() const { return size_ == 0; }

    // Digits in typing order (bottom of the stack first)
    String toString() const
    {
        char buf[17];
        uint8_t i = size_;
        buf[i] = '\0';
        for (Node* n = top_; n != nullptr; n = n->next) buf[--i] = n->digit;
        return String(buf);
    }

private:
    struct Node { char digit; Node* next; };

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
    ~Queue() { clear(); }

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

    void clear()
    {
        T discard;
        while (pop(discard)) { }
    }

    bool   empty() const { return size_ == 0; }
    size_t size()  const { return size_; }

private:
    struct Node { T value; Node* next; };

    Node*        head_ = nullptr;
    Node*        tail_ = nullptr;
    size_t       size_ = 0;
    const size_t capacity_;
};

// ======================================================
// 5. INPUT EVENT - what goes through the input queue
// ======================================================
enum class InputType : uint8_t
{
    Key, Card,                                  // from the dispenser
    SelectBox, Pin, Clear, Exit,                // optional, from the PC
    Hello, Status, Ping, History,               // info requests
    DocLogin, DocLogout, Prescription,          // doctor actions
    Unknown
};

struct InputEvent
{
    InputType type = InputType::Unknown;
    char      key  = 0;
    int       box  = 0;
    String    text = "";
};

// ======================================================
// 6. USERS - singly linked list, created by a factory
// ======================================================
struct User
{
    String name;
    String pin;
    String uid;
    int    level;
    bool   canPrescribe;
    User*  next;
};

class UserFactory
{
public:
    static User* create(const UserConfig& cfg)
    {
        return new User{ cfg.name, cfg.pin, cfg.uid, cfg.level, cfg.canPrescribe, nullptr };
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

    void add(User* user)
    {
        if (user == nullptr) return;
        user->next = nullptr;
        if (head_ == nullptr) { head_ = user; }
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
        for (User* u = head_; u != nullptr; u = u->next) if (u->pin == pin) return u;
        return nullptr;
    }

    User* findByUid(const String& uid) const
    {
        for (User* u = head_; u != nullptr; u = u->next) if (u->uid == uid) return u;
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
    String        result;   // OPENED, DENIED, FAILED, LOCKED, TIMEOUT, DISPENSED,
                            // NOTTAKEN, LOADED, EMPTIED
    AccessRecord* prev;
    AccessRecord* next;
};

class AccessHistory
{
public:
    explicit AccessHistory(size_t capacity) : capacity_(capacity) {}
    ~AccessHistory() { while (head_ != nullptr) removeOldest(); }

    void add(int box, const String& who, const String& method, const String& result)
    {
        AccessRecord* rec = new AccessRecord{ millis(), box, who, method, result, tail_, nullptr };
        if (tail_ != nullptr) tail_->next = rec;
        else                  head_ = rec;
        tail_ = rec;
        size_++;
        if (size_ > capacity_) removeOldest();
    }

    template <typename Visitor>
    void forEachNewestFirst(Visitor visit) const
    {
        for (AccessRecord* r = tail_; r != nullptr; r = r->prev) visit(*r);
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

// ======================================================
// 8. PRESCRIPTION AND BOX SLOTS
// ======================================================
enum class SlotStatus : uint8_t { Empty, Await, Loaded, Dispensed };

struct BoxSlot
{
    int        box        = 0;
    String     medicine   = "-";
    bool       prescribed = false;
    SlotStatus status     = SlotStatus::Empty;

    const char* statusName() const
    {
        switch (status)
        {
            case SlotStatus::Await:     return "AWAIT";
            case SlotStatus::Loaded:    return "LOADED";
            case SlotStatus::Dispensed: return "DISPENSED";
            default:                    return "EMPTY";
        }
    }
};

struct Prescription
{
    String patient = "";
    String medicine[BOX_COUNT + 1];          // index 1..3, "-" = none
    bool   active  = false;

    bool hasBox(int box) const { return medicine[box].length() > 0 && medicine[box] != "-"; }
};


// ######################################################
//              OBSERVER: EVENTS FOR THE GUI
// ######################################################

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

    void subscribe(EventListener* listener) { head_ = new Node{ listener, head_ }; }

    void publish(const String& event) const
    {
        for (Node* n = head_; n != nullptr; n = n->next) n->listener->onEvent(event);
    }

private:
    struct Node { EventListener* listener; Node* next; };
    Node* head_ = nullptr;
};

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

class Device
{
public:
    Device(const char* name, EventBus* bus) : name_(name), bus_(bus) {}
    virtual ~Device() {}
    virtual void begin() = 0;
    const char* name() const { return name_; }

protected:
    void publish(const String& event) const { if (bus_ != nullptr) bus_->publish(event); }

    const char* name_;
    EventBus*   bus_;
};

// ---------- Box servo ----------
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

    // Small movement used by the start-up self-check
    void wiggle()
    {
        servo_.write(45);
        delay(350);
        servo_.write(SERVO_CLOSED);
        delay(350);
    }

    bool isOpen() const { return open_; }
    int  box()    const { return box_; }

private:
    Servo   servo_;
    uint8_t pin_;
    int     box_;
    bool    open_ = false;
};

// ---------- IR sensor (one per box) ----------
class IrSensor : public Device
{
public:
    IrSensor(const char* name, uint8_t pin, int box, EventBus* bus)
        : Device(name, bus), pin_(pin), box_(box) {}

    void begin() override
    {
        pinMode(pin_, INPUT_PULLUP);
        stable_ = readRaw();
        candidate_ = stable_;
    }

    bool readRaw() const
    {
        bool level = digitalRead(pin_) == HIGH;
        return IR_ACTIVE_LOW ? !level : level;
    }

    // Debounce: a new reading must stay the same for IR_DEBOUNCE ms
    void update()
    {
        bool raw = readRaw();
        unsigned long now = millis();

        if (raw != candidate_)
        {
            candidate_ = raw;
            changedAt_ = now;
        }
        else if (raw != stable_ && now - changedAt_ >= IR_DEBOUNCE)
        {
            stable_ = raw;
            publish("IR " + String(box_) + (stable_ ? " 1" : " 0"));
        }
    }

    bool present() const { return stable_; }   // true = medicine detected
    int  box()     const { return box_; }

private:
    uint8_t       pin_;
    int           box_;
    bool          stable_    = false;
    bool          candidate_ = false;
    unsigned long changedAt_ = 0;
};

// ---------- LCD ----------
class LcdDisplay : public Device
{
public:
    LcdDisplay(const char* name, uint8_t address, EventBus* bus)
        : Device(name, bus), address_(address), lcd_(address, 16, 2) {}

    void begin() override
    {
        Wire.setSDA(LCD_SDA);
        Wire.setSCL(LCD_SCL);
        Wire.begin();
        lcd_.init();
        lcd_.backlight();
    }

    bool detected() const
    {
        Wire.beginTransmission(address_);
        return Wire.endTransmission() == 0;
    }

    void show(const String& line1, const String& line2)
    {
        String a = line1.substring(0, 16);
        String b = line2.substring(0, 16);
        if (a == line1_ && b == line2_) return;    // nothing changed
        line1_ = a;
        line2_ = b;

        lcd_.clear();
        lcd_.setCursor(0, 0);
        lcd_.print(line1_);
        lcd_.setCursor(0, 1);
        lcd_.print(line2_);

        publish("LCD " + line1_ + "|" + line2_);
    }

    void republish() const { publish("LCD " + line1_ + "|" + line2_); }

private:
    uint8_t           address_;
    LiquidCrystal_I2C lcd_;
    String            line1_ = "";
    String            line2_ = "";
};


// ######################################################
//            INPUT SOURCES (inheritance from InputSource)
// ######################################################

class InputSource
{
public:
    virtual ~InputSource() {}
    virtual void begin() {}
    virtual void poll(Queue<InputEvent>& queue) = 0;
};

// ---------- Keypad (custom scanner) ----------
class KeypadInput : public InputSource
{
public:
    void begin() override
    {
        for (byte r = 0; r < ROWS; r++) pinMode(ROW_PINS[r], INPUT_PULLUP);
        for (byte c = 0; c < COLS; c++) pinMode(COL_PINS[c], INPUT);
    }

    void poll(Queue<InputEvent>& queue) override
    {
        char raw = scanRaw();
        unsigned long now = millis();

        if (raw != lastRaw_) { lastRaw_ = raw; lastChange_ = now; }

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

    char scanRaw() const
    {
        char found = 0;
        int  count = 0;

        for (byte c = 0; c < COLS; c++)
        {
            pinMode(COL_PINS[c], OUTPUT);
            digitalWrite(COL_PINS[c], LOW);
            delayMicroseconds(20);

            for (byte r = 0; r < ROWS; r++)
            {
                if (digitalRead(ROW_PINS[r]) == LOW) { found = KEYS[r][c]; count++; }
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

// ---------- RFID ----------
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

// ---------- USB serial commands ----------
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
                if (line_.length() > 0) { parse(line_, queue); line_ = ""; }
            }
            else if (line_.length() < 120)
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

        if      (upper == "HELLO")     e.type = InputType::Hello;
        else if (upper == "STATUS")    e.type = InputType::Status;
        else if (upper == "PING")      e.type = InputType::Ping;
        else if (upper == "HISTORY")   e.type = InputType::History;
        else if (upper == "CLEAR")     e.type = InputType::Clear;
        else if (upper == "EXIT")      e.type = InputType::Exit;
        else if (upper == "DOCLOGOUT") e.type = InputType::DocLogout;
        else if (upper.startsWith("DOCLOGIN "))
        {
            e.type = InputType::DocLogin;
            e.text = cmd.substring(9);
            e.text.trim();
        }
        else if (upper.startsWith("RX "))
        {
            e.type = InputType::Prescription;
            e.text = cmd.substring(3);          // keep original letter case
            e.text.trim();
        }
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

class Authenticator
{
public:
    explicit Authenticator(const UserRegistry* users) : users_(users) {}
    virtual ~Authenticator() {}
    virtual User*       authenticate(const String& credential) const = 0;
    virtual const char* failText() const = 0;

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

    const char* failText() const override { return "Invalid PIN"; }
};

class RfidAuthenticator : public Authenticator
{
public:
    explicit RfidAuthenticator(const UserRegistry* users) : Authenticator(users) {}

    User* authenticate(const String& uid) const override { return users_->findByUid(uid); }
    const char* failText() const override { return "Unknown Card"; }
};


// ######################################################
//              STATE PATTERN - one class per phase
// ######################################################

class Dispenser;

class DispenserState
{
public:
    virtual ~DispenserState() {}
    virtual const char* mode() const = 0;                       // sent to the GUI
    virtual void enter(Dispenser& d) = 0;                       // phase starts
    virtual void handle(Dispenser& d, const InputEvent& e) = 0; // key / card / PC input
    virtual void update(Dispenser& d) { (void)d; }              // called every loop
    virtual void redraw(Dispenser& d) { (void)d; }              // repaint the LCD
    virtual bool moving() const { return false; }               // a box is open
};

// ---------- 1. Idle: waiting for a prescription ----------
class IdleState : public DispenserState
{
public:
    const char* mode() const override { return "IDLE"; }
    void enter(Dispenser& d) override;
    void handle(Dispenser& d, const InputEvent& e) override;
    void redraw(Dispenser& d) override;
};

// ---------- Code entry (base for refill / reset passcodes) ----------
// Template Method: handle() collects the digits and checks the code,
// subclasses decide what happens when the code is correct.
class CodeEntryState : public DispenserState
{
public:
    void enter(Dispenser& d) override;
    void handle(Dispenser& d, const InputEvent& e) override;
    void redraw(Dispenser& d) override;

protected:
    virtual const char* title() const = 0;          // LCD line 1
    virtual void onCodeAccepted(Dispenser& d) = 0;  // what to do next
};

class RefillStartState : public CodeEntryState
{
public:
    const char* mode() const override { return "REFILL_AUTH"; }
protected:
    const char* title() const override { return "REFILLING"; }
    void onCodeAccepted(Dispenser& d) override;
};

class RefillFinishState : public CodeEntryState
{
public:
    const char* mode() const override { return "REFILL_DONE"; }
protected:
    const char* title() const override { return "Refill complete"; }
    void onCodeAccepted(Dispenser& d) override;
};

class ResetStartState : public CodeEntryState
{
public:
    const char* mode() const override { return "RESET_AUTH"; }
protected:
    const char* title() const override { return "RESET MODE"; }
    void onCodeAccepted(Dispenser& d) override;
};

// ---------- 5. Complete: every prescribed medicine has been dispensed ----------
// No box can be selected. The code refills the same prescription;
// a new prescription from the doctor also leaves this state.
class CompleteState : public CodeEntryState
{
public:
    const char* mode() const override { return "COMPLETE"; }
    void handle(Dispenser& d, const InputEvent& e) override;
    void redraw(Dispenser& d) override;
protected:
    const char* title() const override { return "All dispensed"; }
    void onCodeAccepted(Dispenser& d) override;
};

// ---------- 3. Refill: open prescribed boxes one by one ----------
class RefillState : public DispenserState
{
public:
    const char* mode() const override { return "REFILL"; }
    void enter(Dispenser& d) override;
    void handle(Dispenser& d, const InputEvent& e) override { (void)d; (void)e; }
    void update(Dispenser& d) override;
    void redraw(Dispenser& d) override;
    bool moving() const override { return true; }

private:
    void nextBox(Dispenser& d);

    int           box_        = 0;
    unsigned long detectedAt_ = 0;
};

// ---------- 4. Ready: choose a box ----------
class ReadyState : public DispenserState
{
public:
    const char* mode() const override { return "READY"; }
    void enter(Dispenser& d) override;
    void handle(Dispenser& d, const InputEvent& e) override;
    void redraw(Dispenser& d) override;

private:
    void choose(Dispenser& d, int box);
};

// ---------- 4b. Log in for the chosen box ----------
class AuthenticateState : public DispenserState
{
public:
    const char* mode() const override { return "AUTH"; }
    void enter(Dispenser& d) override;
    void handle(Dispenser& d, const InputEvent& e) override;
    void update(Dispenser& d) override;
    void redraw(Dispenser& d) override;
};

// ---------- 4c. Dispense: box open until the medicine is taken ----------
class DispenseState : public DispenserState
{
public:
    const char* mode() const override { return "DISPENSE"; }
    void enter(Dispenser& d) override;
    void handle(Dispenser& d, const InputEvent& e) override { (void)d; (void)e; }
    void update(Dispenser& d) override;
    void redraw(Dispenser& d) override;
    bool moving() const override { return true; }

private:
    unsigned long openedAt_ = 0;
    unsigned long takenAt_  = 0;
};

// ---------- 6. Reset: all boxes open until empty ----------
class ResetState : public DispenserState
{
public:
    const char* mode() const override { return "RESET"; }
    void enter(Dispenser& d) override;
    void handle(Dispenser& d, const InputEvent& e) override { (void)d; (void)e; }
    void update(Dispenser& d) override;
    bool moving() const override { return true; }

private:
    unsigned long emptySince_ = 0;
};


// ######################################################
//          DISPENSER - Singleton + Facade
// ######################################################

class Dispenser
{
public:
    static Dispenser& instance()
    {
        static Dispenser theDispenser;
        return theDispenser;
    }

    Dispenser(const Dispenser&) = delete;
    Dispenser& operator=(const Dispenser&) = delete;

    void begin();
    void loop();

    // ---------- state changes ----------
    void changeState(DispenserState* next)
    {
        state_ = next;
        emit(String("MODE ") + state_->mode() + " " + String(selectedBox_));
        state_->enter(*this);
    }

    void goIdle(const char* line2)  { idleMessage_ = line2; changeState(&idleState_); }
    void goReady()                  { changeState(&readyState_); }
    void goRefillStart()            { changeState(&refillStartState_); }
    void goRefill()                 { changeState(&refillState_); }
    void goRefillFinish()           { changeState(&refillFinishState_); }
    void goReset()                  { changeState(&resetState_); }
    void goComplete()               { changeState(&completeState_); }
    void selectBox(int box)         { selectedBox_ = box; changeState(&authState_); }
    void startDispense()            { changeState(&dispenseState_); }

    void emit(const String& event) const { bus_.publish(event); }

    // ---------- session helpers ----------
    void resetSession()   { selectedBox_ = 0; attempts_ = 0; pin_.clear(); }
    void touch()          { lastActivity_ = millis(); }
    bool timedOut() const { return millis() - lastActivity_ > LOGIN_TIMEOUT; }
    void clearAttempts()  { attempts_ = 0; }

    // ---------- actions used by the states ----------
    void tryLogin(const Authenticator& auth, const String& credential, const char* method);
    void handleFailedAttempt(const char* text, const char* method);
    void handleTimeout();
    void showCodeScreen(const char* title);
    void showLoginScreen();

    void activatePendingPrescription();
    void markRefilled();
    void finishDispense(bool taken);
    void finishReset();
    void refillSamePrescription();
    void checkCompletion();

    bool anyPresent() const
    {
        for (int i = 0; i < BOX_COUNT; i++) if (irs_[i]->present()) return true;
        return false;
    }

    bool allDispensed() const
    {
        bool any = false;
        for (int b = 1; b <= BOX_COUNT; b++)
        {
            const BoxSlot& s = slots_[b];
            if (!s.prescribed) continue;
            any = true;
            if (s.status != SlotStatus::Dispensed) return false;
        }
        return any;
    }

    void setSlot(int box, SlotStatus status)
    {
        slots_[box].status = status;
        emitSlot(box);
    }

    void emitSlot(int box) const
    {
        const BoxSlot& s = slots_[box];
        emit("SLOT " + String(box) + " " + s.statusName() + " " + s.medicine);
    }

    void logHistory(int box, const String& who, const String& method, const String& result)
    {
        history_.add(box, who, method, result);
    }

    // ---------- accessors ----------
    LcdDisplay&              lcd()               { return lcd_; }
    RfidInput&               rfid()              { return rfidInput_; }
    PinStack&                pin()               { return pin_; }
    const PinAuthenticator&  pinAuth()           { return pinAuth_; }
    const RfidAuthenticator& rfidAuth()          { return rfidAuth_; }
    BoxServo*                servo(int box)      { return (box >= 1 && box <= BOX_COUNT) ? boxes_[box - 1] : nullptr; }
    IrSensor*                ir(int box)         { return (box >= 1 && box <= BOX_COUNT) ? irs_[box - 1] : nullptr; }
    BoxSlot*                 slot(int box)       { return (box >= 1 && box <= BOX_COUNT) ? &slots_[box] : nullptr; }
    Queue<int>&              refillQueue()       { return refillQueue_; }
    const Prescription&      prescription() const { return rx_; }
    int                      selectedBox() const { return selectedBox_; }
    const String&            idleMessage() const { return idleMessage_; }
    bool                     hasPending()  const { return hasPending_; }
    User*                    lastUser()    const { return lastUser_; }
    const String&            lastMethod()  const { return lastMethod_; }

private:
    Dispenser();

    void runSelfCheck();
    void dispatch(const InputEvent& e);
    void sendHello();
    void sendStatus();
    void sendHistory();
    void sendPrescription() const;
    void handleUser(User* user, const char* method);
    void doctorLogin(const String& pin);
    void receivePrescription(const String& body);
    bool parsePrescription(const String& body, Prescription& out) const;

    // Observer
    EventBus       bus_;
    SerialReporter reporter_;

    // Devices
    LcdDisplay lcd_;
    BoxServo*  boxes_[BOX_COUNT];
    IrSensor*  irs_[BOX_COUNT];
    Device*    devices_[1 + 2 * BOX_COUNT];
    int        deviceCount_ = 0;

    // Inputs
    MFRC522           reader_;
    KeypadInput       keypadInput_;
    RfidInput         rfidInput_;
    SerialInput       serialInput_;
    InputSource*      inputs_[3];
    Queue<InputEvent> inputQueue_;

    // Users
    UserRegistry      users_;
    PinAuthenticator  pinAuth_;
    RfidAuthenticator rfidAuth_;
    AccessHistory     history_;

    // Prescription data
    Prescription rx_;
    Prescription pending_;
    bool         hasPending_ = false;
    BoxSlot      slots_[BOX_COUNT + 1];      // index 1..3
    Queue<int>   refillQueue_;

    // Doctor session (GUI)
    User*         doctor_       = nullptr;
    unsigned long doctorSince_  = 0;

    // States
    IdleState          idleState_;
    RefillStartState   refillStartState_;
    RefillState        refillState_;
    RefillFinishState  refillFinishState_;
    ReadyState         readyState_;
    AuthenticateState  authState_;
    DispenseState      dispenseState_;
    ResetStartState    resetStartState_;
    ResetState         resetState_;
    CompleteState      completeState_;
    DispenserState*    state_ = nullptr;

    // Session
    int           selectedBox_  = 0;
    PinStack      pin_;
    int           attempts_     = 0;
    unsigned long lastActivity_ = 0;
    bool          locked_       = false;
    String        idleMessage_  = "Waiting for Rx";
    User*         lastUser_     = nullptr;
    String        lastMethod_   = "";
};

// ------------------------------------------------------
// Construction (no hardware access here)
// ------------------------------------------------------
Dispenser::Dispenser()
    : lcd_("LCD", LCD_ADDRESS, &bus_),
      reader_(RFID_SS, RFID_RST),
      rfidInput_(&reader_),
      inputQueue_(QUEUE_SIZE),
      pinAuth_(&users_),
      rfidAuth_(&users_),
      history_(HISTORY_SIZE),
      refillQueue_(BOX_COUNT),
      pin_(PIN_LENGTH)
{
    bus_.subscribe(&reporter_);

    boxes_[0] = new BoxServo("Servo 1", SERVO_GENERAL, 1, &bus_);
    boxes_[1] = new BoxServo("Servo 2", SERVO_SEVERE,  2, &bus_);
    boxes_[2] = new BoxServo("Servo 3", SERVO_HIGH,    3, &bus_);

    irs_[0] = new IrSensor("IR 1", IR_BOX1, 1, &bus_);
    irs_[1] = new IrSensor("IR 2", IR_BOX2, 2, &bus_);
    irs_[2] = new IrSensor("IR 3", IR_BOX3, 3, &bus_);

    for (int b = 1; b <= BOX_COUNT; b++) slots_[b].box = b;

    devices_[deviceCount_++] = &lcd_;
    for (int i = 0; i < BOX_COUNT; i++) devices_[deviceCount_++] = boxes_[i];
    for (int i = 0; i < BOX_COUNT; i++) devices_[deviceCount_++] = irs_[i];

    inputs_[0] = &keypadInput_;
    inputs_[1] = &serialInput_;
    inputs_[2] = &rfidInput_;
}

// ------------------------------------------------------
// Start-up: self-check, then initialise
// ------------------------------------------------------
void Dispenser::begin()
{
    for (int i = 0; i < deviceCount_; i++) devices_[i]->begin();   // virtual
    for (int i = 0; i < 3; i++)            inputs_[i]->begin();    // virtual
    for (int i = 0; i < USER_TABLE_SIZE; i++) users_.add(UserFactory::create(USER_TABLE[i]));

    lcd_.show("MED DISPENSER", "Self-check...");
    Serial.println("MEDICINE DISPENSER VERSION 6.2");
    sendHello();
    runSelfCheck();

    lcd_.show("Initialising", "dispenser...");
    delay(800);

    // The boxes are empty at start-up, so always begin with the
    // first prescription (no reset here).
    goIdle("Waiting for Rx");
}

void Dispenser::runSelfCheck()
{
    int failures = 0;

    // LCD
    if (lcd_.detected())
    {
        emit("CHECK LCD OK found at 0x" + String(LCD_ADDRESS, HEX));
    }
    else
    {
        emit("CHECK LCD FAIL not found on I2C - check SDA GP8 / SCL GP9");
        failures++;
    }

    // RFID
    lcd_.show("Check: RFID", "");
    if (rfidInput_.detected())
    {
        emit("CHECK RFID OK reader found");
    }
    else
    {
        emit("CHECK RFID FAIL reader not found - check SPI wiring and 3.3V");
        failures++;
    }
    delay(200);

    // Keypad
    lcd_.show("Check: Keypad", "");
    char stuck = keypadInput_.scanRaw();
    if (stuck != 0)
    {
        emit(String("CHECK KEYPAD FAIL key ") + stuck + " reads as pressed - check wiring");
        failures++;
    }
    else
    {
        emit("CHECK KEYPAD OK no stuck keys");
    }
    delay(200);

    // IR sensors
    lcd_.show("Check: IR", "");
    for (int i = 0; i < BOX_COUNT; i++)
    {
        int  box     = i + 1;
        bool present = irs_[i]->readRaw();
        emit("IR " + String(box) + (present ? " 1" : " 0"));
        if (present) emit("CHECK IR" + String(box) + " WARN reads 'item inside' but boxes should be empty - calibrate the sensor or check IR_ACTIVE_LOW");
        else         emit("CHECK IR" + String(box) + " OK box " + String(box) + " empty");
    }
    delay(200);

    // Servos (no feedback - each one moves a little so you can watch)
    for (int i = 0; i < BOX_COUNT; i++)
    {
        lcd_.show("Check: Servo " + String(i + 1), "");
        boxes_[i]->wiggle();
        emit("CHECK SERVO" + String(i + 1) + " OK moved");
    }

    emit("CHECK DONE " + String(failures));

    if (failures == 0)
    {
        lcd_.show("Self-check OK", "All parts ready");
        delay(1200);
    }
    else
    {
        lcd_.show("Self-check", String(failures) + " part(s) FAIL");
        delay(2500);
    }
}

// ------------------------------------------------------
// Main loop
// ------------------------------------------------------
void Dispenser::loop()
{
    for (int i = 0; i < BOX_COUNT; i++) irs_[i]->update();
    for (int i = 0; i < 3; i++) inputs_[i]->poll(inputQueue_);

    InputEvent e;
    while (inputQueue_.pop(e)) dispatch(e);

    // End the doctor session after DOCTOR_SESSION ms
    if (doctor_ != nullptr && millis() - doctorSince_ > DOCTOR_SESSION)
    {
        doctor_ = nullptr;
        emit("DOC OUT");
    }

    state_->update(*this);
    checkCompletion();
}

void Dispenser::dispatch(const InputEvent& e)
{
    switch (e.type)
    {
        case InputType::Hello:        sendHello(); sendStatus();      return;
        case InputType::Status:       sendStatus();                   return;
        case InputType::Ping:         emit("PONG");                   return;
        case InputType::History:      sendHistory();                  return;
        case InputType::DocLogin:     doctorLogin(e.text);            return;
        case InputType::DocLogout:    doctor_ = nullptr; emit("DOC OUT"); return;
        case InputType::Prescription: receivePrescription(e.text);    return;
        case InputType::Unknown:      emit("ERR unknown command: " + e.text); return;
        case InputType::Key:          emit(String("KEY ") + e.key);   break;
        default: break;
    }

    state_->handle(*this, e);
}

// ------------------------------------------------------
// Reports to the GUI
// ------------------------------------------------------
void Dispenser::sendHello()
{
    emit(String("READY ") + FIRMWARE_VERSION);
    for (int b = 1; b <= BOX_COUNT; b++) emit("BOX " + String(b) + " " + BOX_NAMES[b]);
    for (User* u = users_.first(); u != nullptr; u = u->next)
    {
        emit("ROLE " + String(u->level) + " " + u->name);
    }
}

void Dispenser::sendPrescription() const
{
    if (!rx_.active) { emit("RX NONE"); return; }
    emit("RX " + rx_.patient + "|" + rx_.medicine[1] + "|" + rx_.medicine[2] + "|" + rx_.medicine[3]);
}

void Dispenser::sendStatus()
{
    const char* m = locked_ ? "LOCKED" : (state_ ? state_->mode() : "BOOT");
    emit(String("MODE ") + m + " " + String(selectedBox_));
    emit(String("STATUS ") + m + " " + String(selectedBox_) + " " + String(attempts_) + " " + String(MAX_ATTEMPTS));
    if (doctor_ != nullptr) emit("DOC OK " + doctor_->name);
    else                    emit("DOC OUT");
    sendPrescription();
    for (int b = 1; b <= BOX_COUNT; b++)
    {
        emitSlot(b);
        emit("IR " + String(b) + (irs_[b - 1]->present() ? " 1" : " 0"));
    }
    lcd_.republish();
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
// Doctor and prescription (from the GUI)
// ------------------------------------------------------
void Dispenser::doctorLogin(const String& pin)
{
    User* u = pinAuth_.authenticate(pin);
    if (u != nullptr && u->canPrescribe)
    {
        doctor_      = u;
        doctorSince_ = millis();
        logHistory(0, u->name, "PC", "DOCLOGIN");
        emit("DOC OK " + u->name);
    }
    else
    {
        logHistory(0, "-", "PC", "DOCFAIL");
        emit("DOC FAIL");
    }
}

bool Dispenser::parsePrescription(const String& body, Prescription& out) const
{
    // <patient>|<m1>|<m2>|<m3>
    int p1 = body.indexOf('|');
    int p2 = body.indexOf('|', p1 + 1);
    int p3 = body.indexOf('|', p2 + 1);
    if (p1 <= 0 || p2 < 0 || p3 < 0) return false;

    out.patient     = body.substring(0, p1);
    out.medicine[0] = "";
    out.medicine[1] = body.substring(p1 + 1, p2);
    out.medicine[2] = body.substring(p2 + 1, p3);
    out.medicine[3] = body.substring(p3 + 1);

    bool any = false;
    for (int b = 1; b <= BOX_COUNT; b++)
    {
        out.medicine[b].trim();
        if (out.medicine[b].length() == 0) out.medicine[b] = "-";
        if (out.hasBox(b)) any = true;
    }
    out.patient.trim();
    out.active = true;
    return any && out.patient.length() > 0;
}

void Dispenser::receivePrescription(const String& body)
{
    if (doctor_ == nullptr)       { emit("ERR doctor not logged in"); return; }
    if (state_->moving() || locked_) { emit("ERR dispenser busy - try again when the boxes are closed"); return; }

    Prescription rx;
    if (!parsePrescription(body, rx)) { emit("ERR bad prescription"); return; }

    doctorSince_ = millis();     // keep the doctor session alive
    pending_     = rx;
    hasPending_  = true;
    logHistory(0, doctor_->name, "PC", "RX " + rx.patient);

    // Reset only when a previous patient's medicine is still loaded in a box.
    // First prescription after start-up, or after everything was dispensed,
    // goes straight to the refill.
    bool needReset = false;
    if (rx_.active)
    {
        for (int b = 1; b <= BOX_COUNT; b++)
        {
            if (slots_[b].status == SlotStatus::Loaded) needReset = true;
        }
    }

    if (needReset)
    {
        emit("NEEDRESET");
        changeState(&resetStartState_);
    }
    else
    {
        activatePendingPrescription();
        goRefillStart();
    }
}

void Dispenser::activatePendingPrescription()
{
    if (!hasPending_) return;
    rx_ = pending_;
    hasPending_ = false;

    for (int b = 1; b <= BOX_COUNT; b++)
    {
        slots_[b].medicine   = rx_.medicine[b];
        slots_[b].prescribed = rx_.hasBox(b);
        slots_[b].status     = slots_[b].prescribed ? SlotStatus::Await : SlotStatus::Empty;
    }

    sendPrescription();
    for (int b = 1; b <= BOX_COUNT; b++) emitSlot(b);
}

// ------------------------------------------------------
// Screens
// ------------------------------------------------------
void Dispenser::showCodeScreen(const char* title)
{
    String line2;
    if (pin_.empty())
    {
        line2 = "Code + #";
    }
    else
    {
        line2 = "Code: ";
        for (uint8_t i = 0; i < pin_.size(); i++) line2 += "*";
    }
    lcd_.show(title, line2);
    emit("PINLEN " + String(pin_.size()));
}

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

// ------------------------------------------------------
// Login, failed tries, lockout
// ------------------------------------------------------
void Dispenser::tryLogin(const Authenticator& auth, const String& credential, const char* method)
{
    touch();
    User* user = auth.authenticate(credential);
    pin_.clear();

    if (user != nullptr) handleUser(user, method);
    else                 handleFailedAttempt(auth.failText(), method);
}

void Dispenser::handleUser(User* user, const char* method)
{
    emit(String("AUTH ") + method + " " + user->name);
    lcd_.show("Welcome!", user->name);
    delay(1000);

    if (selectedBox_ <= user->level)
    {
        lastUser_   = user;
        lastMethod_ = method;
        logHistory(selectedBox_, user->name, method, "OPENED");
        startDispense();
    }
    else
    {
        logHistory(selectedBox_, user->name, method, "DENIED");
        emit("DENIED " + String(selectedBox_) + " " + user->name);
        lcd_.show("Access Denied", "Not Authorised");
        delay(500);
        delay(1500);

        touch();
        showLoginScreen();
    }
}

void Dispenser::handleFailedAttempt(const char* text, const char* method)
{
    attempts_++;
    logHistory(selectedBox_, "-", method, "FAILED");
    emit(String("FAIL ") + method + " " + String(attempts_) + " " + String(MAX_ATTEMPTS));

    lcd_.show(text, "Access Denied");
    delay(500);
    delay(1200);

    if (attempts_ >= MAX_ATTEMPTS)
    {
        locked_ = true;
        logHistory(selectedBox_, "-", method, "LOCKED");
        emit("LOCK " + String(LOCKOUT_TIME / 1000));
        for (int s = LOCKOUT_TIME / 1000; s > 0; s--)
        {
            lcd_.show("Too Many Tries", "Wait " + String(s) + " s");
            delay(1000);
        }
        attempts_ = 0;
        locked_   = false;
        emit("UNLOCK");
    }

    pin_.clear();
    touch();
    state_->redraw(*this);       // back to the same screen
}

void Dispenser::handleTimeout()
{
    logHistory(selectedBox_, "-", "-", "TIMEOUT");
    emit("TIMEOUT");
    lcd_.show("Session Timeout", "Start Again");
    delay(500);
    delay(1000);
    goReady();
}

// ------------------------------------------------------
// Refill / dispense / reset results
// ------------------------------------------------------
void Dispenser::markRefilled()
{
    emit("REFILLED");
    goRefillFinish();
}

void Dispenser::finishDispense(bool taken)
{
    int box = selectedBox_;
    BoxSlot* s = slot(box);

    if (taken)
    {
        setSlot(box, SlotStatus::Dispensed);
        emit("DISPENSED " + String(box) + " " + s->medicine);
        logHistory(box, lastUser_ ? lastUser_->name : "-", lastMethod_, "DISPENSED");
        lcd_.show("Dispensed", s->medicine);
    }
    else
    {
        emit("NOTTAKEN " + String(box));
        logHistory(box, lastUser_ ? lastUser_->name : "-", lastMethod_, "NOTTAKEN");
        lcd_.show("Not collected", "Box " + String(box) + " closed");
        delay(500);
    }
    delay(1500);

    if (allDispensed())
    {
        emit("RXCOMPLETE " + rx_.patient);
        logHistory(0, rx_.patient, "-", "RXCOMPLETE");
        goComplete();
    }
    else
    {
        goReady();
    }
}

// Safety net, called every loop: once every prescribed medicine has been
// dispensed, the dispenser must leave the dispensing screens.
void Dispenser::checkCompletion()
{
    bool dispensingScreen = (state_ == &readyState_) || (state_ == &authState_);
    if (dispensingScreen && rx_.active && allDispensed())
    {
        emit("RXCOMPLETE " + rx_.patient);
        goComplete();
    }
}

// Refill the same prescription again (code entered on the COMPLETE screen)
void Dispenser::refillSamePrescription()
{
    for (int b = 1; b <= BOX_COUNT; b++)
    {
        if (slots_[b].prescribed) slots_[b].status = SlotStatus::Await;
        emitSlot(b);
    }
    logHistory(0, rx_.patient, "CODE", "REFILL AGAIN");
    goRefill();
}

void Dispenser::finishReset()
{
    for (int i = 0; i < BOX_COUNT; i++) boxes_[i]->close();

    for (int b = 1; b <= BOX_COUNT; b++)
    {
        slots_[b].status     = SlotStatus::Empty;
        slots_[b].prescribed = false;
        slots_[b].medicine   = "-";
    }
    rx_.active = false;

    emit("EMPTIED");
    logHistory(0, "-", "CODE", "EMPTIED");
    lcd_.show("Emptied", "");
    delay(2000);

    if (hasPending_)
    {
        activatePendingPrescription();
        goRefillStart();
    }
    else
    {
        sendPrescription();
        for (int b = 1; b <= BOX_COUNT; b++) emitSlot(b);
        goIdle("Waiting for Rx");
    }
}


// ######################################################
//                  STATE IMPLEMENTATIONS
// ######################################################

// ---------- Idle ----------
void IdleState::enter(Dispenser& d)
{
    d.resetSession();
    d.rfid().setEnabled(false);
    redraw(d);
}

void IdleState::redraw(Dispenser& d)
{
    d.lcd().show("System Ready", d.idleMessage());
}

void IdleState::handle(Dispenser& d, const InputEvent& e)
{
    if (e.type == InputType::Key || e.type == InputType::SelectBox)
    {
        d.lcd().show("No prescription", "Ask the doctor");
        delay(1200);
        redraw(d);
    }
}

// ---------- Code entry (shared by refill start/finish and reset) ----------
void CodeEntryState::enter(Dispenser& d)
{
    d.pin().clear();
    d.clearAttempts();
    d.rfid().setEnabled(false);
    redraw(d);
}

void CodeEntryState::redraw(Dispenser& d)
{
    d.showCodeScreen(title());
}

void CodeEntryState::handle(Dispenser& d, const InputEvent& e)
{
    if (e.type != InputType::Key) return;   // the code must be typed on the dispenser
    char k = e.key;

    if (k >= '0' && k <= '9')
    {
        if (d.pin().push(k)) { redraw(d); }
    }
    else if (k == 'B')
    {
        if (d.pin().pop()) { redraw(d); }
    }
    else if (k == '*')
    {
        d.pin().clear();
        redraw(d);
    }
    else if (k == '#')
    {
        String code = d.pin().toString();
        d.pin().clear();

        if (code == REFILL_CODE)
        {
            d.emit("CODE OK");
            onCodeAccepted(d);                 // subclass decides what comes next
        }
        else
        {
            d.handleFailedAttempt("Wrong code", "CODE");
        }
    }
}

void RefillStartState::onCodeAccepted(Dispenser& d)  { d.goRefill(); }

void RefillFinishState::onCodeAccepted(Dispenser& d)
{
    d.lcd().show("Medicine", "Dispenser Ready");
    delay(2000);
    d.goReady();
}

void ResetStartState::onCodeAccepted(Dispenser& d)   { d.goReset(); }

// ---------- Complete ----------
void CompleteState::redraw(Dispenser& d)
{
    if (d.pin().empty()) d.lcd().show("All dispensed", "Refill/Prescribe");
    else                 d.showCodeScreen("Refill: code");
}

void CompleteState::handle(Dispenser& d, const InputEvent& e)
{
    if (e.type == InputType::SelectBox || e.type == InputType::Pin)
    {
        d.emit("ERR all medicine dispensed - refill or prescribe");
        return;
    }
    CodeEntryState::handle(d, e);      // keypad: code + # refills the same prescription
}

void CompleteState::onCodeAccepted(Dispenser& d) { d.refillSamePrescription(); }

// ---------- Refill: one prescribed box at a time ----------
void RefillState::enter(Dispenser& d)
{

    // Queue the prescribed boxes in order 1, 2, 3
    d.refillQueue().clear();
    for (int b = 1; b <= BOX_COUNT; b++)
    {
        if (d.slot(b)->prescribed) d.refillQueue().push(b);
    }
    nextBox(d);
}

void RefillState::nextBox(Dispenser& d)
{
    int b;
    if (!d.refillQueue().pop(b))
    {
        box_ = 0;
        d.markRefilled();
        return;
    }

    box_        = b;
    detectedAt_ = 0;
    d.setSlot(b, SlotStatus::Await);
    d.emit("REFILL " + String(b));
    d.servo(b)->open();
    redraw(d);
}

void RefillState::redraw(Dispenser& d)
{
    if (box_ == 0) return;
    d.lcd().show("REFILLING Box " + String(box_), d.slot(box_)->medicine);
}

void RefillState::update(Dispenser& d)
{
    if (box_ == 0) return;

    if (!d.ir(box_)->present())
    {
        if (detectedAt_ != 0) { detectedAt_ = 0; redraw(d); }   // taken out again
        return;
    }

    if (detectedAt_ == 0) detectedAt_ = millis();
    unsigned long elapsed = millis() - detectedAt_;

    if (elapsed >= CLOSE_DELAY)
    {
        d.servo(box_)->close();
        d.setSlot(box_, SlotStatus::Loaded);
        d.emit("LOADED " + String(box_));
        d.logHistory(box_, d.slot(box_)->medicine, "IR", "LOADED");
        delay(600);
        nextBox(d);
    }
    else
    {
        unsigned long left = (CLOSE_DELAY - elapsed + 999) / 1000;
        d.lcd().show("Box " + String(box_) + " detected", "Closing in " + String(left) + " s");
    }
}

// ---------- Ready: choose a box ----------
void ReadyState::enter(Dispenser& d)
{
    d.resetSession();
    d.rfid().setEnabled(false);
    redraw(d);
}

void ReadyState::redraw(Dispenser& d)
{
    d.lcd().show("Dispenser Ready", "Select Box 1-3");
}

void ReadyState::choose(Dispenser& d, int box)
{
    BoxSlot* s = d.slot(box);
    if (s == nullptr) { d.emit("ERR box must be 1, 2 or 3"); return; }

    if (s->status == SlotStatus::Loaded)
    {
        d.selectBox(box);
        return;
    }

    const char* why = !s->prescribed                         ? "Not prescribed"
                    : (s->status == SlotStatus::Dispensed)    ? "Already given"
                                                              : "Empty";
    d.emit("ERR box " + String(box) + ": " + why);
    d.lcd().show("Box " + String(box), why);
    delay(500);
    delay(1200);
    redraw(d);
}

void ReadyState::handle(Dispenser& d, const InputEvent& e)
{
    if (e.type == InputType::Key && e.key >= '1' && e.key <= '3') choose(d, e.key - '0');
    else if (e.type == InputType::SelectBox)                         choose(d, e.box);
    else if (e.type == InputType::Pin)                               d.emit("ERR select a box first");
}

// ---------- Authenticate ----------
void AuthenticateState::enter(Dispenser& d)
{
    d.pin().clear();
    d.touch();
    d.emit("STATE AUTH " + String(d.selectedBox()));
    redraw(d);
    d.rfid().setEnabled(true);
}

void AuthenticateState::redraw(Dispenser& d) { d.showLoginScreen(); }

void AuthenticateState::handle(Dispenser& d, const InputEvent& e)
{
    switch (e.type)
    {
        case InputType::Key:
        {
            d.touch();
            char k = e.key;
            if (k == '0')
            {
                d.lcd().show("Exiting...", "");
                delay(500);
                d.goReady();
            }
            else if (k >= '1' && k <= '9') { if (d.pin().push(k)) { redraw(d); } }
            else if (k == 'B')             { if (d.pin().pop())   { redraw(d); } }
            else if (k == '*')             { d.pin().clear(); redraw(d); }
            else if (k == '#')
            {
                if (d.pin().full()) d.tryLogin(d.pinAuth(), d.pin().toString(), "PIN");
                else
                {
                    d.lcd().show("Enter 4 Digit", "PIN First");
                    delay(1000);
                    redraw(d);
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
            for (unsigned int i = 0; i < e.text.length(); i++) if (e.text[i] < '1' || e.text[i] > '9') valid = false;
            if (!valid) d.emit("ERR PIN must be 4 digits from 1 to 9");
            else        d.tryLogin(d.pinAuth(), e.text, "PC");
            break;
        }

        case InputType::Clear: d.pin().clear(); redraw(d); break;
        case InputType::Exit:  d.lcd().show("Exiting...", ""); delay(300); d.goReady(); break;
        case InputType::SelectBox: d.emit("ERR exit the current login first"); break;
        default: break;
    }
}

void AuthenticateState::update(Dispenser& d)
{
    if (d.timedOut()) d.handleTimeout();
}

// ---------- Dispense ----------
void DispenseState::enter(Dispenser& d)
{
    d.rfid().setEnabled(false);
    d.servo(d.selectedBox())->open();
    openedAt_ = millis();
    takenAt_  = 0;
    redraw(d);
}

void DispenseState::redraw(Dispenser& d)
{
    d.lcd().show("Take medicine", d.slot(d.selectedBox())->medicine);
}

void DispenseState::update(Dispenser& d)
{
    int box = d.selectedBox();
    unsigned long now = millis();

    if (!d.ir(box)->present())
    {
        // Medicine taken: close 5 s later
        if (takenAt_ == 0) takenAt_ = now;
        unsigned long elapsed = now - takenAt_;

        if (elapsed >= CLOSE_DELAY)
        {
            d.servo(box)->close();
            d.finishDispense(true);
            return;
        }
        unsigned long left = (CLOSE_DELAY - elapsed + 999) / 1000;
        d.lcd().show("Medicine taken", "Closing in " + String(left) + " s");
        return;
    }

    // Still inside (or put back)
    if (takenAt_ != 0) { takenAt_ = 0; redraw(d); }

    if (now - openedAt_ >= COLLECT_TIMEOUT)
    {
        d.servo(box)->close();
        d.finishDispense(false);
        return;
    }

    unsigned long left = (COLLECT_TIMEOUT - (now - openedAt_) + 999) / 1000;
    d.lcd().show("Take med: " + String(left) + " s", d.slot(box)->medicine);
}

// ---------- Reset: open all, wait until every box is empty ----------
void ResetState::enter(Dispenser& d)
{
    for (int b = 1; b <= BOX_COUNT; b++) d.servo(b)->open();
    emptySince_ = 0;
}

void ResetState::update(Dispenser& d)
{
    String left = "";
    for (int b = 1; b <= BOX_COUNT; b++)
    {
        if (d.ir(b)->present()) left += (left.length() ? " " : "") + String(b);
    }

    if (left.length() > 0)
    {
        emptySince_ = 0;
        d.lcd().show("RESET: empty box", "Remove: " + left);
        return;
    }

    if (emptySince_ == 0) emptySince_ = millis();
    if (millis() - emptySince_ >= EMPTY_CONFIRM)
    {
        d.finishReset();
        return;
    }
    d.lcd().show("All boxes empty", "Checking...");
}


// ######################################################
//                  ARDUINO ENTRY POINTS
// ######################################################

void setup()
{
    Serial.begin(115200);
    delay(300);                       // give the USB port a moment
    Dispenser::instance().begin();
}

void loop()
{
    Dispenser::instance().loop();
}
