#pragma once

#include <Arduino.h>

#include "ModbusRtuMaster.h"

// Two-port Modbus RTU store-and-forward gateway.
//
// Upstream (PlexLog) and downstream (P17) are separate RS485 bus segments.
// PlexLog remains the upstream master.  This class emulates the addressed P17
// slave long enough to forward each request through the sole downstream
// master.  During a SmartPLC-owned downstream transaction, only a recent,
// byte-for-byte matching FC03/FC04 response may be served from cache.
class PlexLogGateway {
public:
    PlexLogGateway(HardwareSerial& upstream, ModbusRtuMaster& downstream,
                   int8_t rxPin, int8_t txPin);

    void begin(uint32_t baud);
    void end();
    bool enabled() const { return baud_ != 0; }

    // Collect upstream bytes and, when downstreamAvailable is true, forward at
    // most one complete request.  Cached read replies remain available while
    // SmartPLC has reserved the downstream bus.
    void service(bool downstreamAvailable, uint32_t downstreamTimeoutMs);

    // Forward one request which had no safe cache entry while SmartPLC was
    // using the downstream bus.  Intended for bounded service slots between
    // two inverter-control operations.
    bool serviceOneQueued(uint32_t downstreamTimeoutMs);

    bool hasQueuedRequest() const { return queueCount_ != 0; }
    bool upstreamQuietFor(uint32_t durationMs) const;
    // During an RSE transition, exact read-only replies may remain usable for
    // one complete six-inverter PlexLog polling cycle.  Verified power/feed
    // values are refreshed immediately by publishVerifiedHolding().
    void setControlBusy(bool busy) { controlBusy_ = busy; }
    void publishVerifiedHolding(uint8_t slave, uint16_t address,
                                uint8_t quantity, uint32_t value);
    void printStatus(Print& out) const;

private:
    static constexpr size_t MAX_FRAME = 256;
    static constexpr uint8_t QUEUE_DEPTH = 8;
    // Today's single-inverter PlexLog trace contained about 18 requests per
    // cycle.  128 entries cover the conservative six-inverter extrapolation
    // (6 * 18 = 108) without eviction during the same cycle.
    static constexpr uint16_t CACHE_ENTRIES = 128;
    static constexpr uint32_t CACHE_TTL_MS = 10000;
    static constexpr uint32_t CONTROL_CACHE_TTL_MS = 20000;
    static constexpr uint32_t QUEUE_MAX_AGE_MS = 350;
    static constexpr uint8_t FIRST_PROXY_SLAVE = 2;
    static constexpr uint8_t LAST_PROXY_SLAVE = 7;
    static constexpr uint8_t PROXY_SLAVE_COUNT =
        LAST_PROXY_SLAVE - FIRST_PROXY_SLAVE + 1;

    struct Frame {
        uint16_t length = 0;
        uint32_t receivedAt = 0;
        uint8_t bytes[MAX_FRAME] = {};
    };

    struct CacheEntry {
        bool valid = false;
        uint8_t slave = 0;
        uint8_t function = 0;
        uint16_t address = 0;
        uint16_t quantity = 0;
        uint32_t updatedAt = 0;
        uint16_t responseLength = 0;
        uint8_t response[MAX_FRAME] = {};
    };

    HardwareSerial& upstream_;
    ModbusRtuMaster& downstream_;
    int8_t rxPin_;
    int8_t txPin_;
    uint32_t baud_ = 0;
    uint32_t lastByteAt_ = 0;
    uint32_t lastRequestAt_ = 0;
    uint32_t frameGapMs_ = 4;
    Frame receiving_;
    Frame queue_[QUEUE_DEPTH];
    uint8_t queueHead_ = 0;
    uint8_t queueTail_ = 0;
    uint8_t queueCount_ = 0;
    CacheEntry cache_[CACHE_ENTRIES];
    uint16_t nextCache_ = 0;
    bool forwarding_ = false;
    bool controlBusy_ = false;

    uint32_t received_ = 0;
    uint32_t forwarded_ = 0;
    uint32_t cachedReplies_ = 0;
    uint32_t invalidFrames_ = 0;
    uint32_t queueDrops_ = 0;
    uint32_t queueExpired_ = 0;
    uint32_t downstreamFailures_ = 0;
    uint32_t rejectedRequests_ = 0;
    uint32_t controlCacheReplies_ = 0;
    uint32_t queueCacheRescues_ = 0;
    uint8_t maxQueueDepth_ = 0;
    uint32_t lastForwardMs_ = 0;
    uint32_t maxForwardMs_ = 0;
    uint8_t lastExpiredSlave_ = 0;
    uint8_t lastExpiredFunction_ = 0;
    uint16_t lastExpiredAddress_ = 0;
    uint16_t lastExpiredQuantity_ = 0;
    uint32_t lastExpiredAgeMs_ = 0;
    uint32_t receivedById_[PROXY_SLAVE_COUNT] = {};
    uint32_t forwardedById_[PROXY_SLAVE_COUNT] = {};
    uint32_t cachedById_[PROXY_SLAVE_COUNT] = {};
    uint32_t failedById_[PROXY_SLAVE_COUNT] = {};

    static uint16_t crc16(const uint8_t* data, size_t length);
    static bool validCrc(const Frame& frame);
    static bool validProxyRequest(const Frame& frame, uint8_t& slave);
    static bool cacheableRead(const Frame& frame, uint8_t& slave,
                              uint8_t& function, uint16_t& address,
                              uint16_t& quantity);
    void collect();
    void finishReceiving(bool downstreamAvailable, uint32_t timeoutMs);
    bool enqueue(const Frame& frame);
    bool dequeue(Frame& frame);
    bool replyFromCache(const Frame& request);
    void remember(const Frame& request, const uint8_t* response,
                  size_t responseLength);
    bool forward(const Frame& request, uint32_t timeoutMs);
    void sendUpstream(const uint8_t* data, size_t length);
};
