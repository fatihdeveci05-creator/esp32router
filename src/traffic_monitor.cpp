#include "traffic_monitor.h"
#include <esp_wifi.h>
#include <string.h>

TrafficMonitor trafficMonitor;

TrafficMonitor::TrafficMonitor() 
    : _mutex(nullptr),
      _client_count(0),
      _dns_log_head(0),
      _dns_log_count(0),
      _blacklist_count(0),
      _total_rx(0),
      _total_tx(0),
      _blocked_queries_count(0),
      _allowed_queries_count(0)
{
    memset(_clients, 0, sizeof(_clients));
    memset(_dns_logs, 0, sizeof(_dns_logs));
    memset(_blacklist, 0, sizeof(_blacklist));
}

void TrafficMonitor::begin() {
    if (_mutex == nullptr) {
        _mutex = xSemaphoreCreateMutex();
    }

    // Varsayılan popüler engellenebilir alan adları örneği
    addDomainToBlacklist("ads.example.com");
    addDomainToBlacklist("track.adservice.com");
}

void TrafficMonitor::updateClientTraffic(const char* ip_str, const char* mac_str, size_t rx_bytes, size_t tx_bytes) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int idx = findClientIndexByMac(mac_str);
        if (idx == -1 && _client_count < MAX_TRACKED_CLIENTS) {
            idx = _client_count++;
            strncpy(_clients[idx].mac, mac_str, sizeof(_clients[idx].mac) - 1);
            strncpy(_clients[idx].ip, ip_str, sizeof(_clients[idx].ip) - 1);
            _clients[idx].bytes_rx = 0;
            _clients[idx].bytes_tx = 0;
            _clients[idx].is_blocked = false;
            _clients[idx].active = true;
        }

        if (idx != -1) {
            _clients[idx].bytes_rx += rx_bytes;
            _clients[idx].bytes_tx += tx_bytes;
            _clients[idx].last_seen_sec = millis() / 1000;
            _clients[idx].active = true;
            strncpy(_clients[idx].ip, ip_str, sizeof(_clients[idx].ip) - 1);
        }

        _total_rx += rx_bytes;
        _total_tx += tx_bytes;

        xSemaphoreGive(_mutex);
    }
}

void TrafficMonitor::touchClient(const IPAddress& ip) {
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);

    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        int idx = findClientIndexByIp(ip_str);
        if (idx != -1) {
            _clients[idx].last_seen_sec = millis() / 1000;
        }
        xSemaphoreGive(_mutex);
    }
}

bool TrafficMonitor::isClientBlocked(const IPAddress& ip) {
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);

    bool blocked = false;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        int idx = findClientIndexByIp(ip_str);
        if (idx != -1) {
            blocked = _clients[idx].is_blocked;
        }
        xSemaphoreGive(_mutex);
    }
    return blocked;
}

bool TrafficMonitor::toggleClientBlock(const char* mac_str, bool block_state) {
    bool found = false;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int idx = findClientIndexByMac(mac_str);
        if (idx != -1) {
            _clients[idx].is_blocked = block_state;
            found = true;
        }
        xSemaphoreGive(_mutex);
    }
    return found;
}

void TrafficMonitor::logDnsQuery(const IPAddress& client_ip, const char* domain, DnsActionType action) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        size_t idx = _dns_log_head;
        _dns_logs[idx].timestamp_sec = millis() / 1000;
        snprintf(_dns_logs[idx].client_ip, sizeof(_dns_logs[idx].client_ip), "%u.%u.%u.%u", 
                 client_ip[0], client_ip[1], client_ip[2], client_ip[3]);
        strncpy(_dns_logs[idx].domain, domain, sizeof(_dns_logs[idx].domain) - 1);
        _dns_logs[idx].action = action;

        _dns_log_head = (_dns_log_head + 1) % MAX_DNS_LOG_ENTRIES;
        if (_dns_log_count < MAX_DNS_LOG_ENTRIES) {
            _dns_log_count++;
        }

        if (action == DNS_ACTION_BLOCKED) {
            _blocked_queries_count++;
        } else {
            _allowed_queries_count++;
        }

        xSemaphoreGive(_mutex);
    }
}

bool TrafficMonitor::isDomainBlocked(const char* domain) {
    if (!domain || strlen(domain) == 0) return false;

    bool blocked = false;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        for (size_t i = 0; i < _blacklist_count; i++) {
            // Tam eşleşme veya alt alan adı kontrolü (.domain.com)
            if (strcasecmp(domain, _blacklist[i]) == 0 || strstr(domain, _blacklist[i]) != nullptr) {
                blocked = true;
                break;
            }
        }
        xSemaphoreGive(_mutex);
    }
    return blocked;
}

bool TrafficMonitor::addDomainToBlacklist(const char* domain) {
    if (!domain || strlen(domain) < 3) return false;

    bool success = false;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Zaten var mı kontrolü
        bool exists = false;
        for (size_t i = 0; i < _blacklist_count; i++) {
            if (strcasecmp(_blacklist[i], domain) == 0) {
                exists = true;
                break;
            }
        }

        if (!exists && _blacklist_count < MAX_BLACKLIST_DOMAINS) {
            strncpy(_blacklist[_blacklist_count], domain, sizeof(_blacklist[_blacklist_count]) - 1);
            _blacklist_count++;
            success = true;
        }
        xSemaphoreGive(_mutex);
    }
    return success;
}

bool TrafficMonitor::removeDomainFromBlacklist(const char* domain) {
    if (!domain) return false;

    bool success = false;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (size_t i = 0; i < _blacklist_count; i++) {
            if (strcasecmp(_blacklist[i], domain) == 0) {
                for (size_t j = i; j < _blacklist_count - 1; j++) {
                    strncpy(_blacklist[j], _blacklist[j + 1], sizeof(_blacklist[j]));
                }
                _blacklist_count--;
                success = true;
                break;
            }
        }
        xSemaphoreGive(_mutex);
    }
    return success;
}

void TrafficMonitor::clearBlacklist() {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _blacklist_count = 0;
        xSemaphoreGive(_mutex);
    }
}

void TrafficMonitor::getStatusJson(JsonObject& doc) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        doc["uptime_sec"] = millis() / 1000;
        doc["free_heap"] = esp_get_free_heap_size();
        doc["min_free_heap"] = esp_get_minimum_free_heap_size();
        doc["total_rx"] = _total_rx;
        doc["total_tx"] = _total_tx;
        doc["allowed_queries"] = _allowed_queries_count;
        doc["blocked_queries"] = _blocked_queries_count;
        doc["blacklist_count"] = _blacklist_count;
        doc["connected_clients"] = _client_count;
        xSemaphoreGive(_mutex);
    }
}

void TrafficMonitor::getClientsJson(JsonArray& array) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint32_t now = millis() / 1000;
        for (size_t i = 0; i < _client_count; i++) {
            JsonObject clientObj = array.add<JsonObject>();
            clientObj["ip"] = _clients[i].ip;
            clientObj["mac"] = _clients[i].mac;
            clientObj["rx"] = _clients[i].bytes_rx;
            clientObj["tx"] = _clients[i].bytes_tx;
            clientObj["is_blocked"] = _clients[i].is_blocked;
            clientObj["active"] = (now - _clients[i].last_seen_sec < 180); // Son 3 dk içinde aktif mi?
        }
        xSemaphoreGive(_mutex);
    }
}

void TrafficMonitor::getDnsLogsJson(JsonArray& array) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // En son logdan geriye doğru ekle
        size_t count = _dns_log_count;
        size_t idx = (_dns_log_head == 0) ? (MAX_DNS_LOG_ENTRIES - 1) : (_dns_log_head - 1);

        for (size_t i = 0; i < count && i < 30; i++) { // Son 30 log
            JsonObject logObj = array.add<JsonObject>();
            logObj["time"] = _dns_logs[idx].timestamp_sec;
            logObj["ip"] = _dns_logs[idx].client_ip;
            logObj["domain"] = _dns_logs[idx].domain;
            logObj["action"] = (int)_dns_logs[idx].action;

            idx = (idx == 0) ? (MAX_DNS_LOG_ENTRIES - 1) : (idx - 1);
        }
        xSemaphoreGive(_mutex);
    }
}

void TrafficMonitor::getBlacklistJson(JsonArray& array) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (size_t i = 0; i < _blacklist_count; i++) {
            array.add(_blacklist[i]);
        }
        xSemaphoreGive(_mutex);
    }
}

int TrafficMonitor::findClientIndexByMac(const char* mac_str) {
    for (size_t i = 0; i < _client_count; i++) {
        if (strcasecmp(_clients[i].mac, mac_str) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int TrafficMonitor::findClientIndexByIp(const char* ip_str) {
    for (size_t i = 0; i < _client_count; i++) {
        if (strcmp(_clients[i].ip, ip_str) == 0) {
            return (int)i;
        }
    }
    return -1;
}
