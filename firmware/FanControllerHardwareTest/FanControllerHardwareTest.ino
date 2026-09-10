/*
  Dual-GPU fan-controller hardware test
  Target: classic Arduino Nano / ATmega328P

  Commands in Serial Monitor:
    1 50    Set PWM group 1 to 50%
    2 65    Set PWM group 2 to 65%
    a 100   Set both groups to 100%
    ?       Show help

  Set line ending to "Newline" and baud rate to 115200.
*/

#include <Arduino.h>
#include <Wire.h>
#include <avr/interrupt.h>

constexpr uint8_t PWM1_PIN = 9;   // OC1A
constexpr uint8_t PWM2_PIN = 10;  // OC1B

constexpr uint8_t TACH1_PIN = 2;
constexpr uint8_t TACH2_PIN = 3;
constexpr uint8_t TACH3_PIN = 4;
constexpr uint8_t TACH4_PIN = 5;

constexpr uint8_t TACH_MASK =
    _BV(PD2) | _BV(PD3) | _BV(PD4) | _BV(PD5);

constexpr uint8_t PULSES_PER_REVOLUTION = 2;

constexpr uint8_t INA3221_ADDRESS_1 = 0x40;
constexpr uint8_t INA3221_ADDRESS_2 = 0x41;

// The breakout boards use shunts marked R100: 0.100 ohm.
constexpr float INA3221_SHUNT_OHMS = 0.100f;

constexpr uint8_t INA3221_CONFIG_REGISTER = 0x00;
constexpr uint8_t INA3221_MANUFACTURER_ID_REGISTER = 0xFE;
constexpr uint8_t INA3221_DIE_ID_REGISTER = 0xFF;
constexpr uint16_t INA3221_MANUFACTURER_ID = 0x5449;
constexpr uint16_t INA3221_DIE_ID = 0x3220;
constexpr uint32_t I2C_TIMEOUT_MICROSECONDS = 25000UL;

// All three channels enabled, 16-sample averaging, 1.1 ms bus and shunt
// conversion times, continuous shunt-and-bus conversion mode.
constexpr uint16_t INA3221_CONFIGURATION = 0x7527;

// 16 MHz / (1 × 640) = 25 kHz
constexpr uint16_t PWM_TOP = 639;

volatile uint32_t tachPulses[4] = {};
volatile uint8_t previousPortD;

uint8_t pwm1Duty = 100;
uint8_t pwm2Duty = 100;

bool ina3221Present[2] = {};
bool lastI2cTransactionTimedOut = false;

char commandBuffer[24];
uint8_t commandLength = 0;

// --------------------------------------------------------------------------
// INA3221 current and voltage monitoring
// --------------------------------------------------------------------------

bool i2cAddressResponds(uint8_t address)
{
    Wire.clearWireTimeoutFlag();
    Wire.beginTransmission(address);
    const uint8_t result = Wire.endTransmission();
    lastI2cTransactionTimedOut = Wire.getWireTimeoutFlag();
    Wire.clearWireTimeoutFlag();
    return result == 0 && !lastI2cTransactionTimedOut;
}

bool readIna3221Register(uint8_t address, uint8_t registerAddress,
                         uint16_t& value)
{
    Wire.clearWireTimeoutFlag();
    Wire.beginTransmission(address);
    Wire.write(registerAddress);

    if (Wire.endTransmission(false) != 0 || Wire.getWireTimeoutFlag()) {
        lastI2cTransactionTimedOut = Wire.getWireTimeoutFlag();
        Wire.clearWireTimeoutFlag();
        return false;
    }

    if (Wire.requestFrom(address, static_cast<uint8_t>(2)) != 2) {
        lastI2cTransactionTimedOut = Wire.getWireTimeoutFlag();
        Wire.clearWireTimeoutFlag();

        while (Wire.available() > 0) {
            Wire.read();
        }

        return false;
    }

    lastI2cTransactionTimedOut = Wire.getWireTimeoutFlag();
    Wire.clearWireTimeoutFlag();

    if (lastI2cTransactionTimedOut) {
        return false;
    }

    value = static_cast<uint16_t>(Wire.read()) << 8;
    value |= static_cast<uint8_t>(Wire.read());
    return true;
}

bool writeIna3221Register(uint8_t address, uint8_t registerAddress,
                          uint16_t value)
{
    Wire.clearWireTimeoutFlag();
    Wire.beginTransmission(address);
    Wire.write(registerAddress);
    Wire.write(static_cast<uint8_t>(value >> 8));
    Wire.write(static_cast<uint8_t>(value));
    const uint8_t result = Wire.endTransmission();
    lastI2cTransactionTimedOut = Wire.getWireTimeoutFlag();
    Wire.clearWireTimeoutFlag();
    return result == 0 && !lastI2cTransactionTimedOut;
}

void printHexAddress(uint8_t address)
{
    Serial.print(F("0x"));

    if (address < 0x10) {
        Serial.print('0');
    }

    Serial.print(address, HEX);
}

void scanI2cBus()
{
    Serial.println(F("I2C scan (INA3221 range 0x40-0x43):"));
    bool foundAny = false;

    for (uint8_t address = 0x40; address <= 0x43; ++address) {
        Serial.print(F("  Trying "));
        printHexAddress(address);
        Serial.print(F("... "));
        Serial.flush();

        if (i2cAddressResponds(address)) {
            Serial.println(F("found"));
            foundAny = true;
        } else if (lastI2cTransactionTimedOut) {
            Serial.println(F("TIMEOUT (SDA or SCL may be stuck LOW)"));
        } else {
            Serial.println(F("no response"));
        }
    }

    if (!foundAny) {
        Serial.println(F("No INA3221 address responded."));
    }
}

bool initializeIna3221(uint8_t address)
{
    Serial.print(F("INA3221 "));
    printHexAddress(address);
    Serial.print(F(": "));

    if (!i2cAddressResponds(address)) {
        Serial.println(F("not found"));
        return false;
    }

    uint16_t manufacturerId;
    uint16_t dieId;

    if (!readIna3221Register(address, INA3221_MANUFACTURER_ID_REGISTER,
                             manufacturerId) ||
        !readIna3221Register(address, INA3221_DIE_ID_REGISTER, dieId)) {
        Serial.println(F("register read failed"));
        return false;
    }

    if (manufacturerId != INA3221_MANUFACTURER_ID ||
        dieId != INA3221_DIE_ID) {
        Serial.print(F("unexpected IDs (manufacturer=0x"));
        Serial.print(manufacturerId, HEX);
        Serial.print(F(", die=0x"));
        Serial.print(dieId, HEX);
        Serial.println(')');
        return false;
    }

    if (!writeIna3221Register(address, INA3221_CONFIG_REGISTER,
                              INA3221_CONFIGURATION)) {
        Serial.println(F("configuration failed"));
        return false;
    }

    Serial.println(F("ready"));
    return true;
}

bool readIna3221Channel(uint8_t address, uint8_t channel,
                        float& busVolts, float& currentAmps)
{
    // Channels 1-3 use shunt registers 0x01, 0x03, 0x05 and bus
    // registers 0x02, 0x04, 0x06.
    const uint8_t shuntRegister = 1 + (channel - 1) * 2;
    const uint8_t busRegister = shuntRegister + 1;

    uint16_t shuntRegisterValue;
    uint16_t busRegisterValue;

    if (!readIna3221Register(address, shuntRegister,
                             shuntRegisterValue) ||
        !readIna3221Register(address, busRegister, busRegisterValue)) {
        return false;
    }

    // Data occupies bits 15:3. The shunt LSB is 40 uV and the bus
    // voltage LSB is 8 mV. Shunt voltage is signed for bidirectional
    // measurements; negative current indicates reversed IN+/IN- wiring.
    const int16_t shuntCounts =
        static_cast<int16_t>(shuntRegisterValue) / 8;
    const uint16_t busCounts = busRegisterValue / 8;

    const float shuntVolts = shuntCounts * 0.000040f;
    busVolts = busCounts * 0.008f;
    currentAmps = shuntVolts / INA3221_SHUNT_OHMS;
    return true;
}

void reportIna3221(uint8_t address, bool present)
{
    Serial.print(F("INA "));
    printHexAddress(address);

    if (!present) {
        Serial.println(F(": NOT FOUND"));
        return;
    }

    for (uint8_t channel = 1; channel <= 3; ++channel) {
        float busVolts;
        float currentAmps;

        Serial.print(channel == 1 ? F(": ") : F(" | "));
        Serial.print(F("CH"));
        Serial.print(channel);
        Serial.print('=');

        if (!readIna3221Channel(address, channel, busVolts,
                                currentAmps)) {
            Serial.print(F("READ_ERR"));
            continue;
        }

        Serial.print(busVolts, 3);
        Serial.print(F("V "));
        Serial.print(currentAmps, 3);
        Serial.print('A');
    }

    Serial.println();
}

// --------------------------------------------------------------------------
// Tachometer interrupt
// --------------------------------------------------------------------------

ISR(PCINT2_vect)
{
    const uint8_t currentPortD = PIND;

    // Count only falling edges.
    const uint8_t fallingEdges =
        previousPortD & ~currentPortD & TACH_MASK;

    if (fallingEdges & _BV(PD2)) {
        ++tachPulses[0];
    }

    if (fallingEdges & _BV(PD3)) {
        ++tachPulses[1];
    }

    if (fallingEdges & _BV(PD4)) {
        ++tachPulses[2];
    }

    if (fallingEdges & _BV(PD5)) {
        ++tachPulses[3];
    }

    previousPortD = currentPortD;
}

// --------------------------------------------------------------------------
// PWM
// --------------------------------------------------------------------------

void configurePwm()
{
    // LOW on the Nano output turns the NPN transistor off.
    // That leaves the fan PWM input open/high, requesting full speed.
    pinMode(PWM1_PIN, OUTPUT);
    pinMode(PWM2_PIN, OUTPUT);

    digitalWrite(PWM1_PIN, LOW);
    digitalWrite(PWM2_PIN, LOW);

    // Timer1 Fast PWM, TOP = ICR1, clock prescaler = 1.
    TCCR1A = _BV(WGM11);
    TCCR1B = _BV(WGM13) | _BV(WGM12) | _BV(CS10);

    ICR1 = PWM_TOP;
    TCNT1 = 0;
}

/*
  The 2N3904 stages invert the Nano outputs:

    Nano LOW  -> transistor off -> fan PWM open/high
    Nano HIGH -> transistor on  -> fan PWM pulled low

  Therefore, the Nano waveform is the inverse of the desired fan duty.
*/
void setFanDuty(uint8_t channel, uint8_t duty)
{
    if (duty > 100) duty = 100;

    const uint8_t savedSreg = SREG;
    cli();

    if (channel == 1) {
        // Temporarily disconnect Timer1 from D9.
        TCCR1A &= ~(_BV(COM1A1) | _BV(COM1A0));

        if (duty == 100) {
            // Transistor off: fan PWM remains open/high.
            PORTB &= ~_BV(PB1);
        } else if (duty == 0) {
            // Transistor continuously on: fan PWM held low.
            PORTB |= _BV(PB1);
        } else {
            const uint16_t invertedCounts =
                ((uint32_t)(PWM_TOP + 1) * (100 - duty)) / 100;

            OCR1A = invertedCounts;

            // Non-inverting Nano waveform; the transistor inverts it.
            TCCR1A |= _BV(COM1A1);
            TCCR1A &= ~_BV(COM1A0);
        }

        pwm1Duty = duty;
    } else {
        // Temporarily disconnect Timer1 from D10.
        TCCR1A &= ~(_BV(COM1B1) | _BV(COM1B0));

        if (duty == 100) {
            PORTB &= ~_BV(PB2);
        } else if (duty == 0) {
            PORTB |= _BV(PB2);
        } else {
            const uint16_t invertedCounts =
                ((uint32_t)(PWM_TOP + 1) * (100 - duty)) / 100;

            OCR1B = invertedCounts;

            TCCR1A |= _BV(COM1B1);
            TCCR1A &= ~_BV(COM1B0);
        }

        pwm2Duty = duty;
    }

    SREG = savedSreg;
}

// --------------------------------------------------------------------------
// Serial interface
// --------------------------------------------------------------------------

void printHelp()
{
    Serial.println(F(""));
    Serial.println(F("Commands:"));
    Serial.println(F("  1 <0-100>  Set PWM group 1"));
    Serial.println(F("  2 <0-100>  Set PWM group 2"));
    Serial.println(F("  a <0-100>  Set both groups"));
    Serial.println(F("  ?          Show this help"));
    Serial.println(F(""));
}

void processCommand(char* command)
{
    if (command[0] == '?') {
        printHelp();
        return;
    }

    char target;
    int requestedDuty;

    if (sscanf(command, " %c %d", &target, &requestedDuty) != 2 ||
        requestedDuty < 0 ||
        requestedDuty > 100) {
        Serial.println(F("Invalid command. Enter ? for help."));
        return;
    }

    switch (target) {
        case '1':
            setFanDuty(1, requestedDuty);
            break;

        case '2':
            setFanDuty(2, requestedDuty);
            break;

        case 'a':
        case 'A':
            setFanDuty(1, requestedDuty);
            setFanDuty(2, requestedDuty);
            break;

        default:
            Serial.println(F("Unknown fan group. Enter ? for help."));
            return;
    }

    Serial.print(F("PWM1="));
    Serial.print(pwm1Duty);
    Serial.print(F("%, PWM2="));
    Serial.print(pwm2Duty);
    Serial.println(F("%"));
}

void readSerialCommands()
{
    while (Serial.available() > 0) {
        const char received = Serial.read();

        if (received == '\n' || received == '\r') {
            if (commandLength != 0) {
                commandBuffer[commandLength] = '\0';
                processCommand(commandBuffer);
                commandLength = 0;
            }
        } else if (commandLength < sizeof(commandBuffer) - 1) {
            commandBuffer[commandLength++] = received;
        }
    }
}

// --------------------------------------------------------------------------
// RPM reporting
// --------------------------------------------------------------------------

void reportRpm()
{
    static uint32_t previousCounts[4] = {};
    static uint32_t previousReportMs = 0;

    const uint32_t now = millis();
    const uint32_t elapsedMs = now - previousReportMs;

    if (elapsedMs < 1000) {
        return;
    }

    uint32_t currentCounts[4];

    noInterrupts();

    for (uint8_t i = 0; i < 4; ++i) {
        currentCounts[i] = tachPulses[i];
    }

    interrupts();

    uint32_t rpm[4];

    for (uint8_t i = 0; i < 4; ++i) {
        const uint32_t pulseDelta =
            currentCounts[i] - previousCounts[i];

        rpm[i] =
            (pulseDelta * 60000UL) /
            (PULSES_PER_REVOLUTION * elapsedMs);

        previousCounts[i] = currentCounts[i];
    }

    previousReportMs = now;

    Serial.print(F("PWM1="));
    Serial.print(pwm1Duty);
    Serial.print(F("% PWM2="));
    Serial.print(pwm2Duty);

    Serial.print(F("% | D2="));
    Serial.print(rpm[0]);
    Serial.print(F(" D3="));
    Serial.print(rpm[1]);
    Serial.print(F(" D4="));
    Serial.print(rpm[2]);
    Serial.print(F(" D5="));
    Serial.print(rpm[3]);
    Serial.println(F(" RPM"));

    reportIna3221(INA3221_ADDRESS_1, ina3221Present[0]);
    reportIna3221(INA3221_ADDRESS_2, ina3221Present[1]);
}

// --------------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);

    // The schematic already has external 10 kΩ pull-ups.
    pinMode(TACH1_PIN, INPUT);
    pinMode(TACH2_PIN, INPUT);
    pinMode(TACH3_PIN, INPUT);
    pinMode(TACH4_PIN, INPUT);

    configurePwm();

    // Explicit safe startup state.
    setFanDuty(1, 100);
    setFanDuty(2, 100);

    // Classic Nano I2C pins: A4=SDA, A5=SCL.
    Wire.begin();
    Wire.setClock(100000UL);
    Wire.setWireTimeout(I2C_TIMEOUT_MICROSECONDS, true);

    previousPortD = PIND;

    // Enable pin-change interrupts for D2 through D5.
    PCIFR = _BV(PCIF2);       // Clear any pending interrupt.
    PCMSK2 |= TACH_MASK;
    PCICR |= _BV(PCIE2);

    Serial.println(F(""));
    Serial.println(F("Dual GPU fan-controller hardware test"));
    Serial.println(F("Starting both fan groups at 100%."));
    Serial.println(F("INA3221 shunts: R100 / 0.100 ohm"));
    Serial.print(F("I2C idle levels: SDA="));
    Serial.print(digitalRead(SDA) == HIGH ? F("HIGH") : F("LOW"));
    Serial.print(F(" SCL="));
    Serial.println(digitalRead(SCL) == HIGH ? F("HIGH") : F("LOW"));

    if (digitalRead(SDA) == LOW || digitalRead(SCL) == LOW) {
        Serial.println(F("WARNING: I2C must idle HIGH; check power and wiring."));
    }

    scanI2cBus();
    ina3221Present[0] = initializeIna3221(INA3221_ADDRESS_1);
    ina3221Present[1] = initializeIna3221(INA3221_ADDRESS_2);
    printHelp();
}

void loop()
{
    readSerialCommands();
    reportRpm();
}
