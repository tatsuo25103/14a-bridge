#include "ModbusRtuMaster.h"

namespace {
constexpr uint8_t FC_READ_HOLDING = 0x03;
constexpr uint8_t FC_WRITE_MULTIPLE = 0x10;
}

ModbusRtuMaster::ModbusRtuMaster(HardwareSerial& serial, int8_t rxPin,
                                 int8_t txPin, int8_t dirPin)
    : serial_(serial), rxPin_(rxPin), txPin_(txPin), dirPin_(dirPin) {}

void ModbusRtuMaster::begin(uint32_t baud) {
    if (baud_ == baud) return;
    baud_ = baud;
    pinMode(dirPin_, OUTPUT);
    digitalWrite(dirPin_, LOW);
    serial_.end();
    serial_.begin(baud, SERIAL_8N1, rxPin_, txPin_);
}

uint16_t ModbusRtuMaster::crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
        }
    }
    return crc;
}

void ModbusRtuMaster::transmit(const uint8_t* data, size_t length) {
    while (serial_.available()) serial_.read();
    digitalWrite(dirPin_, HIGH);
    delayMicroseconds(100);
    serial_.write(data, length);
    serial_.flush();
    delayMicroseconds(100);
    digitalWrite(dirPin_, LOW);
}

void ModbusRtuMaster::responsiveDelay(uint32_t durationMs) {
    const uint32_t started = millis();
    while (millis() - started < durationMs) {
        if (idleCallback_) idleCallback_();
        delay(1);
    }
}

bool ModbusRtuMaster::receive(uint8_t slave, uint8_t function,
                              uint8_t* response, size_t capacity,
                              size_t& length, uint32_t timeoutMs,
                              String& detail) {
    length = 0;
    const uint32_t started = millis();
    uint32_t lastByte = started;
    while (millis() - started < timeoutMs) {
        while (serial_.available() && length < capacity) {
            response[length++] = static_cast<uint8_t>(serial_.read());
            lastByte = millis();
        }
        if (length >= 5 && millis() - lastByte >= 4) break;
        if (idleCallback_) idleCallback_();
        delay(1);
    }
    if (length < 5) {
        detail = "timeout/incomplete response";
        return false;
    }
    const uint16_t expected = crc16(response, length - 2);
    const uint16_t received = response[length - 2] | (response[length - 1] << 8);
    if (expected != received) {
        detail = "CRC mismatch";
        return false;
    }
    if (response[0] != slave) {
        detail = "unexpected slave id";
        return false;
    }
    if (response[1] != function && response[1] != (function | 0x80)) {
        detail = "unexpected function";
        return false;
    }
    return true;
}

ModbusResult ModbusRtuMaster::readRaw(uint8_t slave, uint16_t address,
                                      uint8_t quantity, uint32_t timeoutMs) {
    ModbusResult result;
    if (quantity != 1 && quantity != 2) {
        result.detail = "quantity must be 1 or 2";
        return result;
    }
    uint8_t request[8] = {slave, FC_READ_HOLDING,
                          static_cast<uint8_t>(address >> 8),
                          static_cast<uint8_t>(address), 0, quantity, 0, 0};
    const uint16_t crc = crc16(request, 6);
    request[6] = crc & 0xFF;
    request[7] = crc >> 8;
    transmit(request, sizeof(request));

    uint8_t response[16];
    size_t length = 0;
    if (!receive(slave, FC_READ_HOLDING, response, sizeof(response), length,
                 timeoutMs, result.detail)) return result;
    result.communicationOk = true;
    if (response[1] & 0x80) {
        result.exceptionCode = response[2];
        result.detail = "Modbus exception " + String(result.exceptionCode);
        return result;
    }
    const uint8_t byteCount = quantity * 2;
    if (length != static_cast<size_t>(byteCount + 5) || response[2] != byteCount) {
        result.detail = "invalid FC03 response length";
        return result;
    }
    result.value = quantity == 1
        ? (static_cast<uint32_t>(response[3]) << 8) | response[4]
        : (static_cast<uint32_t>(response[3]) << 24) |
          (static_cast<uint32_t>(response[4]) << 16) |
          (static_cast<uint32_t>(response[5]) << 8) | response[6];
    result.ok = true;
    result.detail = "readback verified";
    return result;
}

ModbusResult ModbusRtuMaster::writeAndVerify(uint8_t slave, uint16_t address,
                                             uint8_t quantity, uint32_t value,
                                             uint32_t timeoutMs, uint8_t retries) {
    ModbusResult result;
    if (quantity != 1 && quantity != 2) {
        result.detail = "quantity must be 1 or 2";
        return result;
    }
    uint8_t request[13] = {slave, FC_WRITE_MULTIPLE,
                           static_cast<uint8_t>(address >> 8),
                           static_cast<uint8_t>(address), 0, quantity,
                           static_cast<uint8_t>(quantity * 2), 0, 0, 0, 0, 0, 0};
    size_t payloadLength = quantity == 1 ? 9 : 11;
    if (quantity == 1) {
        request[7] = value >> 8;
        request[8] = value;
    } else {
        request[7] = value >> 24;
        request[8] = value >> 16;
        request[9] = value >> 8;
        request[10] = value;
    }
    const uint16_t crc = crc16(request, payloadLength);
    request[payloadLength] = crc & 0xFF;
    request[payloadLength + 1] = crc >> 8;
    if (retries == 0) retries = 1;
    String lastDetail = "no write attempt completed";
    bool anyCommunication = false;
    for (uint8_t attempt = 0; attempt < retries; ++attempt) {
        transmit(request, payloadLength + 2);

        uint8_t response[16];
        size_t length = 0;
        String ackDetail;
        // Readback is authoritative. Do not spend the full FC03 timeout on a
        // missing/non-standard FC16 acknowledgement.
        const uint32_t ackTimeoutMs = timeoutMs < 400 ? timeoutMs : 400;
        receive(slave, FC_WRITE_MULTIPLE, response, sizeof(response), length,
                ackTimeoutMs, ackDetail);

        responsiveDelay(300 + attempt * 250);
        result = readRaw(slave, address, quantity, timeoutMs);
        anyCommunication = anyCommunication || result.communicationOk;
        if (result.ok && result.value == value) {
            result.detail = "write and readback verified on attempt " +
                            String(attempt + 1) + "/" + String(retries);
            return result;
        }
        if (result.ok) {
            lastDetail = "readback " + String(result.value) +
                         " did not match target " + String(value);
        } else {
            lastDetail = result.detail;
            if (!ackDetail.isEmpty()) lastDetail += "; ACK: " + ackDetail;
        }
        if (attempt + 1 < retries) responsiveDelay(500 + attempt * 250);
    }
    result.ok = false;
    result.communicationOk = anyCommunication;
    result.detail = "write not verified after " + String(retries) +
                    " attempts: " + lastDetail;
    return result;
}
