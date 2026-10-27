#include "PlexLogGateway.h"

#include <cstring>

PlexLogGateway::PlexLogGateway(HardwareSerial& upstream,
                               ModbusRtuMaster& downstream,
                               int8_t rxPin, int8_t txPin)
    : upstream_(upstream), downstream_(downstream), rxPin_(rxPin),
      txPin_(txPin) {}

void PlexLogGateway::begin(uint32_t baud) {
    if (baud_ == baud) return;
    baud_ = baud;
    upstream_.end();
    // PORT.C uses an automatic-direction RS485 adapter.  GPIO5 is UART TX;
    // when idle it remains high and the adapter returns to receive mode.
    upstream_.begin(baud, SERIAL_8N1, rxPin_, txPin_);
    const uint32_t charUs = (10000000UL + baud - 1) / baud;  // 10 bits/char
    frameGapMs_ = max<uint32_t>(2, (charUs * 4 + 999) / 1000);
    receiving_.length = 0;
    queueHead_ = queueTail_ = queueCount_ = 0;
}

void PlexLogGateway::end() {
    upstream_.end();
    baud_ = 0;
    receiving_.length = 0;
    queueHead_ = queueTail_ = queueCount_ = 0;
    forwarding_ = false;
    controlBusy_ = false;
}

uint16_t PlexLogGateway::crc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) ? ((crc >> 1) ^ 0xA001U) : (crc >> 1);
    }
    return crc;
}

bool PlexLogGateway::validCrc(const Frame& frame) {
    if (frame.length < 4) return false;
    const uint16_t expected = crc16(frame.bytes, frame.length - 2);
    const uint16_t actual = frame.bytes[frame.length - 2] |
                            (frame.bytes[frame.length - 1] << 8);
    return expected == actual;
}

bool PlexLogGateway::validProxyRequest(const Frame& frame, uint8_t& slave) {
    if (frame.length < 4) return false;
    slave = frame.bytes[0];
    if (slave < FIRST_PROXY_SLAVE || slave > LAST_PROXY_SLAVE) return false;
    const uint8_t function = frame.bytes[1];
    const uint16_t quantity = frame.length >= 6
        ? (uint16_t(frame.bytes[4]) << 8) | frame.bytes[5] : 0;
    switch (function) {
        // Standard fixed-length Modbus read requests.
        case 0x01:
        case 0x02:
            return frame.length == 8 && quantity > 0 && quantity <= 2000;
        case 0x03:
        case 0x04:
            return frame.length == 8 && quantity > 0 && quantity <= 125;
        // Single-coil / single-register writes.  Their replies have the same
        // shape, but replies cannot occur on the separate upstream segment
        // unless the installation is wired incorrectly.
        case 0x05:
        case 0x06:
            return frame.length == 8;
        // Multiple-coil write: byte count must exactly match the quantity.
        case 0x0F: {
            if (frame.length < 10 || quantity == 0 || quantity > 1968)
                return false;
            const uint8_t byteCount = frame.bytes[6];
            return byteCount == (quantity + 7U) / 8U &&
                   frame.length == size_t(9U + byteCount);
        }
        // Multiple-register write: byte count must be two bytes/register.
        case 0x10: {
            if (frame.length < 11 || quantity == 0 || quantity > 123)
                return false;
            const uint8_t byteCount = frame.bytes[6];
            return byteCount == quantity * 2U &&
                   frame.length == size_t(9U + byteCount);
        }
        default:
            return false;
    }
}

bool PlexLogGateway::cacheableRead(const Frame& frame, uint8_t& slave,
                                   uint8_t& function, uint16_t& address,
                                   uint16_t& quantity) {
    if (frame.length != 8) return false;
    slave = frame.bytes[0];
    function = frame.bytes[1];
    if (slave < FIRST_PROXY_SLAVE || slave > LAST_PROXY_SLAVE ||
        (function != 0x03 && function != 0x04))
        return false;
    address = (uint16_t(frame.bytes[2]) << 8) | frame.bytes[3];
    quantity = (uint16_t(frame.bytes[4]) << 8) | frame.bytes[5];
    return quantity > 0 && quantity <= 125;
}

void PlexLogGateway::collect() {
    while (upstream_.available()) {
        const int value = upstream_.read();
        if (value < 0) break;
        lastByteAt_ = millis();
        if (receiving_.length < MAX_FRAME) {
            receiving_.bytes[receiving_.length++] = uint8_t(value);
        } else {
            receiving_.length = 0;
            ++invalidFrames_;
        }
    }
}

void PlexLogGateway::sendUpstream(const uint8_t* data, size_t length) {
    upstream_.write(data, length);
    upstream_.flush();
}

bool PlexLogGateway::replyFromCache(const Frame& request) {
    uint8_t slave = 0, function = 0;
    uint16_t address = 0, quantity = 0;
    if (!cacheableRead(request, slave, function, address, quantity)) return false;
    const uint32_t now = millis();
    const uint32_t allowedAge = controlBusy_
        ? CONTROL_CACHE_TTL_MS : CACHE_TTL_MS;
    for (const auto& entry : cache_) {
        if (!entry.valid || entry.slave != slave ||
            entry.function != function || entry.address != address ||
            entry.quantity != quantity ||
            uint32_t(now - entry.updatedAt) > allowedAge)
            continue;
        sendUpstream(entry.response, entry.responseLength);
        ++cachedReplies_;
        ++cachedById_[slave - FIRST_PROXY_SLAVE];
        if (controlBusy_) ++controlCacheReplies_;
        return true;
    }
    return false;
}

void PlexLogGateway::remember(const Frame& request, const uint8_t* response,
                              size_t responseLength) {
    uint8_t slave = 0, function = 0;
    uint16_t address = 0, quantity = 0;
    if (!cacheableRead(request, slave, function, address, quantity) ||
        responseLength < 5 || responseLength > MAX_FRAME ||
        response[0] != slave || response[1] != function)
        return;
    CacheEntry* target = nullptr;
    for (auto& entry : cache_) {
        if (entry.valid && entry.slave == slave &&
            entry.function == function && entry.address == address &&
            entry.quantity == quantity) {
            target = &entry;
            break;
        }
    }
    if (!target) {
        target = &cache_[nextCache_];
        nextCache_ = (nextCache_ + 1) % CACHE_ENTRIES;
    }
    target->valid = true;
    target->slave = slave;
    target->function = function;
    target->address = address;
    target->quantity = quantity;
    target->updatedAt = millis();
    target->responseLength = responseLength;
    memcpy(target->response, response, responseLength);
}

bool PlexLogGateway::enqueue(const Frame& frame) {
    // A master normally has one outstanding request.  Coalesce an identical
    // retry instead of letting it fill the queue while RSE owns the bus.
    for (uint8_t i = 0, pos = queueHead_; i < queueCount_; ++i,
                 pos = (pos + 1) % QUEUE_DEPTH) {
        if (queue_[pos].length == frame.length &&
            memcmp(queue_[pos].bytes, frame.bytes, frame.length) == 0)
            return true;
    }
    if (queueCount_ >= QUEUE_DEPTH) {
        ++queueDrops_;
        return false;
    }
    queue_[queueTail_] = frame;
    queueTail_ = (queueTail_ + 1) % QUEUE_DEPTH;
    ++queueCount_;
    if (queueCount_ > maxQueueDepth_) maxQueueDepth_ = queueCount_;
    return true;
}

bool PlexLogGateway::dequeue(Frame& frame) {
    if (!queueCount_) return false;
    frame = queue_[queueHead_];
    queueHead_ = (queueHead_ + 1) % QUEUE_DEPTH;
    --queueCount_;
    return true;
}

bool PlexLogGateway::forward(const Frame& request, uint32_t timeoutMs) {
    if (forwarding_) return false;
    forwarding_ = true;
    uint8_t response[MAX_FRAME];
    size_t responseLength = 0;
    String detail;
    const uint8_t slave = request.bytes[0];
    const uint32_t startedAt = millis();
    const bool ok = downstream_.transactRaw(
        request.bytes, request.length, response, sizeof(response),
        responseLength, timeoutMs, detail);
    lastForwardMs_ = uint32_t(millis() - startedAt);
    if (lastForwardMs_ > maxForwardMs_) maxForwardMs_ = lastForwardMs_;
    const uint8_t idIndex = slave - FIRST_PROXY_SLAVE;
    if (ok) {
        sendUpstream(response, responseLength);
        remember(request, response, responseLength);
        ++forwarded_;
        ++forwardedById_[idIndex];
    } else {
        ++downstreamFailures_;
        ++failedById_[idIndex];
    }
    forwarding_ = false;
    return ok;
}

void PlexLogGateway::finishReceiving(bool downstreamAvailable,
                                     uint32_t timeoutMs) {
    if (!receiving_.length) return;
    Frame frame = receiving_;
    receiving_.length = 0;
    frame.receivedAt = millis();
    ++received_;
    lastRequestAt_ = millis();
    if (!validCrc(frame)) {
        ++invalidFrames_;
        return;
    }
    uint8_t slave = 0;
    if (!validProxyRequest(frame, slave)) {
        ++rejectedRequests_;
        return;
    }
    ++receivedById_[slave - FIRST_PROXY_SLAVE];
    if (downstreamAvailable && !forwarding_) {
        forward(frame, timeoutMs);
        return;
    }
    if (!replyFromCache(frame)) enqueue(frame);
}

void PlexLogGateway::service(bool downstreamAvailable,
                             uint32_t downstreamTimeoutMs) {
    if (!baud_) return;
    collect();
    if (receiving_.length &&
        uint32_t(millis() - lastByteAt_) >= frameGapMs_)
        finishReceiving(downstreamAvailable, downstreamTimeoutMs);
    if (downstreamAvailable && !forwarding_)
        serviceOneQueued(downstreamTimeoutMs);
}

bool PlexLogGateway::serviceOneQueued(uint32_t downstreamTimeoutMs) {
    if (forwarding_) return false;
    Frame frame;
    while (dequeue(frame)) {
        const uint32_t ageMs = uint32_t(millis() - frame.receivedAt);
        // Never send a late response after PlexLog has already abandoned the
        // request; it could be mistaken for the next transaction.  Common
        // reads should have been answered from cache before reaching here.
        if (ageMs <= QUEUE_MAX_AGE_MS) {
            // The requested register may have been verified and published by
            // SmartPLC while this frame was waiting.  Re-check the cache so a
            // newly safe value is returned without another downstream bus
            // transaction.
            if (replyFromCache(frame)) {
                ++queueCacheRescues_;
                return true;
            }
            return forward(frame, downstreamTimeoutMs);
        }
        lastExpiredSlave_ = frame.length > 0 ? frame.bytes[0] : 0;
        lastExpiredFunction_ = frame.length > 1 ? frame.bytes[1] : 0;
        lastExpiredAddress_ = frame.length > 3
            ? (uint16_t(frame.bytes[2]) << 8) | frame.bytes[3] : 0;
        lastExpiredQuantity_ = frame.length > 5
            ? (uint16_t(frame.bytes[4]) << 8) | frame.bytes[5] : 0;
        lastExpiredAgeMs_ = ageMs;
        ++queueExpired_;
    }
    return false;
}

bool PlexLogGateway::upstreamQuietFor(uint32_t durationMs) const {
    return !receiving_.length && !queueCount_ &&
           uint32_t(millis() - max(lastByteAt_, lastRequestAt_)) >= durationMs;
}

void PlexLogGateway::publishVerifiedHolding(uint8_t slave, uint16_t address,
                                            uint8_t quantity,
                                            uint32_t value) {
    if (slave < 2 || slave > 7 || (quantity != 1 && quantity != 2)) return;
    CacheEntry* target = nullptr;
    for (auto& entry : cache_) {
        if (entry.valid && entry.slave == slave && entry.function == 0x03 &&
            entry.address == address && entry.quantity == quantity) {
            target = &entry;
            break;
        }
    }
    if (!target) {
        target = &cache_[nextCache_];
        nextCache_ = (nextCache_ + 1) % CACHE_ENTRIES;
    }
    target->valid = true;
    target->slave = slave;
    target->function = 0x03;
    target->address = address;
    target->quantity = quantity;
    target->updatedAt = millis();
    target->responseLength = quantity == 1 ? 7 : 9;
    target->response[0] = slave;
    target->response[1] = 0x03;
    target->response[2] = quantity * 2;
    if (quantity == 1) {
        target->response[3] = value >> 8;
        target->response[4] = value;
    } else {
        target->response[3] = value >> 24;
        target->response[4] = value >> 16;
        target->response[5] = value >> 8;
        target->response[6] = value;
    }
    const uint16_t crc = crc16(target->response, target->responseLength - 2);
    target->response[target->responseLength - 2] = crc;
    target->response[target->responseLength - 1] = crc >> 8;
}

void PlexLogGateway::printStatus(Print& out) const {
    out.printf("PLEXLOG GATEWAY=%s BAUD=%lu RX=%lu FORWARDED=%lu CACHE=%lu "
               "QUEUED=%u INVALID=%lu DROPPED=%lu EXPIRED=%lu DOWNFAIL=%lu "
               "TTLMS=%lu QUEUEMS=%lu CACHEENTRIES=%u CONTROLTTLMS=%lu "
               "MAXQ=%u LASTMS=%lu MAXMS=%lu REJECTED=%lu CONTROLHITS=%lu "
               "RESCUED=%lu EXP_ID=%u EXP_FC=%u EXP_ADDR=%u EXP_QTY=%u EXP_AGE=%lu\r\n",
               enabled() ? "ON" : "OFF", static_cast<unsigned long>(baud_),
               static_cast<unsigned long>(received_),
               static_cast<unsigned long>(forwarded_),
               static_cast<unsigned long>(cachedReplies_), queueCount_,
               static_cast<unsigned long>(invalidFrames_),
               static_cast<unsigned long>(queueDrops_),
               static_cast<unsigned long>(queueExpired_),
               static_cast<unsigned long>(downstreamFailures_),
               static_cast<unsigned long>(CACHE_TTL_MS),
               static_cast<unsigned long>(QUEUE_MAX_AGE_MS),
               static_cast<unsigned>(CACHE_ENTRIES),
               static_cast<unsigned long>(CONTROL_CACHE_TTL_MS),
               static_cast<unsigned>(maxQueueDepth_),
               static_cast<unsigned long>(lastForwardMs_),
               static_cast<unsigned long>(maxForwardMs_),
               static_cast<unsigned long>(rejectedRequests_),
               static_cast<unsigned long>(controlCacheReplies_),
               static_cast<unsigned long>(queueCacheRescues_),
               static_cast<unsigned>(lastExpiredSlave_),
               static_cast<unsigned>(lastExpiredFunction_),
               static_cast<unsigned>(lastExpiredAddress_),
               static_cast<unsigned>(lastExpiredQuantity_),
               static_cast<unsigned long>(lastExpiredAgeMs_));
    for (uint8_t index = 0; index < PROXY_SLAVE_COUNT; ++index) {
        out.printf("PLEXLOG ID=%u RX=%lu FWD=%lu CACHE=%lu FAIL=%lu\r\n",
                   static_cast<unsigned>(FIRST_PROXY_SLAVE + index),
                   static_cast<unsigned long>(receivedById_[index]),
                   static_cast<unsigned long>(forwardedById_[index]),
                   static_cast<unsigned long>(cachedById_[index]),
                   static_cast<unsigned long>(failedById_[index]));
    }
}
