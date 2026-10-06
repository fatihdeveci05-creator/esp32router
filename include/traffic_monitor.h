#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <ArduinoJson.h>
#include "config.h"

enum DnsActionType {
    DNS_ACTION_ALLOWED = 0,
    DNS_ACTION_BLOCKED = 1,
    DNS_ACTION_LOCAL_ADMIN = 2
};

struct DnsLogEntry {
    uint32_t timestamp_sec;
    char client_ip[16];
    char domain[64];
    DnsActionType action;
};

struct TrackedClient {
    char mac[18];
    char ip[16];
    uint64_t bytes_rx; // İndirilen (Download)
    uint64_t bytes_tx; // Yüklenen (Upload)
    uint32_t last_seen_sec;
    bool is_blocked;
    bool active;
};

class TrafficMonitor {
public:
    TrafficMonitor();
    void begin();

    // İstemci ve Trafik Takibi
    void updateClientTraffic(const char* ip_str, const char* mac_str, size_t rx_bytes, size_t tx_bytes);
    void touchClient(const IPAddress& ip);
    bool isClientBlocked(const IPAddress& ip);
    bool toggleClientBlock(const char* mac_str, bool block_state);

    // DNS Sorgu Günlüğü (Ring Buffer)
    void logDnsQuery(const IPAddress& client_ip, const char* domain, DnsActionType action);

    // Kara Liste (Blacklist) Yönetimi
    bool isDomainBlocked(const char* domain);
    bool addDomainToBlacklist(const char* domain);
    bool removeDomainFromBlacklist(const char* domain);
    void clearBlacklist();

    // İstatistik ve JSON Çıktıları
    void getStatusJson(JsonObject& doc);
    void getClientsJson(JsonArray& array);
    void getDnsLogsJson(JsonArray& array);
    void getBlacklistJson(JsonArray& array);

    uint64_t getTotalRx() const { return _total_rx; }
    uint64_t getTotalTx() const { return _total_tx; }

private:
    SemaphoreHandle_t _mutex;

    // İstemci listesi
    TrackedClient _clients[MAX_TRACKED_CLIENTS];
    size_t _client_count;

    // Halka Log Belleği
    DnsLogEntry _dns_logs[MAX_DNS_LOG_ENTRIES];
    size_t _dns_log_head;
    size_t _dns_log_count;

    // Kara Liste
    char _blacklist[MAX_BLACKLIST_DOMAINS][64];
    size_t _blacklist_count;

    // Kümülatif Sayaçlar
    uint64_t _total_rx;
    uint64_t _total_tx;
    uint32_t _blocked_queries_count;
    uint32_t _allowed_queries_count;

    int findClientIndexByMac(const char* mac_str);
    int findClientIndexByIp(const char* ip_str);
};

extern TrafficMonitor trafficMonitor;
