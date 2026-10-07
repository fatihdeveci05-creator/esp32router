#pragma once

#include <Arduino.h>
#include <WiFiUdp.h>
#include <IPAddress.h>
#include "config.h"
#include "traffic_monitor.h"

class DnsEngine {
public:
    DnsEngine();
    bool begin(const IPAddress& upstream_dns = FALLBACK_UPSTREAM_DNS);
    void stop();
    void setUpstreamDns(const IPAddress& upstream_dns);
    void setCustomDomain(const char* domain);
    const char* getCustomDomain() const { return _custom_domain; }

    // FreeRTOS Görevi olarak arka planda çalışır (paket işlendiyse true döner)
    bool process();

private:
    struct DnsCacheEntry {
        char domain[48];
        uint32_t ip;
        uint32_t expire_ms;
    };

    static const size_t DNS_CACHE_SIZE = 64;
    DnsCacheEntry _cache[DNS_CACHE_SIZE];
    size_t _cache_head;

    WiFiUDP _local_udp;       // Port 53 dinleyicisi (AP İstemcileri)
    WiFiUDP _upstream_udp;    // Upstream DNS yönlendiricisi (İnternet)
    IPAddress _upstream_dns;
    char _custom_domain[64];
    TaskHandle_t _task_handle;
    bool _running;

    bool parseQName(const uint8_t* buffer, size_t len, size_t& offset, char* out_domain, size_t max_out);
    void buildDnsResponse(uint8_t* query_buf, size_t question_end, uint16_t qtype, const IPAddress& resolved_ip, uint8_t* resp_buf, size_t& resp_len);
    void forwardUpstreamAndReply(uint8_t* query_buf, size_t query_len, const char* domain, const IPAddress& client_ip, uint16_t client_port);
    bool checkCache(const char* domain, uint32_t& out_ip);
    void saveCache(const char* domain, uint32_t ip, uint32_t ttl_sec);
};

extern DnsEngine dnsEngine;
