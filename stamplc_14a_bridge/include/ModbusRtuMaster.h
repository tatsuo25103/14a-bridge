#pragma once

#include <Arduino.h>

struct ModbusResult {
    bool ok = false;
    bool communicationOk = false;
    uint8_t exceptionCode = 0;
    uint32_t value = 0;
    String detail;
};

class ModbusRtuMaster {
public:
    ModbusRtuMaster(HardwareSerial& serial, int8_t rxPin, int8_t txPin, int8_t dirPin);
    void begin(uint32_t baud);
    ModbusResult readRaw(uint8_t slave, uint16_t address, uint8_t quantity, uint32_t timeoutMs);
    ModbusResult writeAndVerify(uint8_t slave, uint16_t address, uint8_t quantity,
                               uint32_t value, uint32_t timeoutMs, uint8_t retries);

private:
    HardwareSerial& serial_;
    int8_t rxPin_;
    int8_t txPin_;
    int8_t dirPin_;
    uint32_t baud_ = 0;

    static uint16_t crc16(const uint8_t* data, size_t length);
    void transmit(const uint8_t* data, size_t length);
    bool receive(uint8_t slave, uint8_t function, uint8_t* response,
                 size_t capacity, size_t& length, uint32_t timeoutMs, String& detail);
};
