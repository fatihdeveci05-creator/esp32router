#include "dns_engine.h"
#include <WiFi.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>

DnsEngine dnsEngine;

static void dnsTaskTrampoline(void* arg) {
    DnsEngine* engine = static_cast<DnsEngine*>(arg);
    while (true) {
        bool handled = engine->process();
        if (!handled) {
            vTaskDelay(pdMS_TO_TICKS(1)); // Sadece yeni paket yokken dinlen
        }
    }
}

DnsEngine::DnsEngine() 
    : _cache_head(0),
      _upstream_dns(FALLBACK_UPSTREAM_DNS),
      _task_handle(nullptr),
      _running(false)
{
    memset(_cache, 0, sizeof(_cache));
    strncpy(_custom_domain, LOCAL_ADMIN_DOMAIN, sizeof(_custom_domain) - 1);
    _custom_domain[sizeof(_custom_domain) - 1] = '\0';
}

bool DnsEngine::begin(const IPAddress& upstream_dns) {
    if (_running) return true;

    _upstream_dns = upstream_dns;

    // AP istemcilerinden gelen Port 53 sorgularını dinle
    if (!_local_udp.begin(DNS_SERVER_PORT)) {
        Serial.println("[DNS] HATA: Port 53 baslatilamadi!");
        return false;
    }

    _upstream_udp.begin(0); // Rastgele bosta olan port ile upstream icin baslat
    _running = true;

    // DNS gorevini Cekirdek 1 uzerinde baslat (Cekirdek 0 Wi-Fi ve NAPT'a kalir)
    xTaskCreatePinnedToCore(
        dnsTaskTrampoline,
        "dns_engine_task",
        4096,
        this,
        2, // Oncelik 2 (Gecikmeyi onlemek icin yuksek oncelik)
        &_task_handle,
        1
    );

    Serial.printf("[DNS] Sunucu aktif. Upstream: %s, Yerel Yonlendirme: %s -> %s\n",
                  _upstream_dns.toString().c_str(),
                  LOCAL_ADMIN_DOMAIN,
                  AP_LOCAL_IP.toString().c_str());

    return true;
}

void DnsEngine::stop() {
    _running = false;
    if (_task_handle) {
        vTaskDelete(_task_handle);
        _task_handle = nullptr;
    }
    _local_udp.stop();
    _upstream_udp.stop();
}

void DnsEngine::setUpstreamDns(const IPAddress& upstream_dns) {
    _upstream_dns = upstream_dns;
}

void DnsEngine::setCustomDomain(const char* domain) {
    if (domain && strlen(domain) > 2) {
        String d = String(domain);
        d.trim();
        if (d.startsWith("https://")) d = d.substring(8);
        else if (d.startsWith("http://")) d = d.substring(7);
        int slash = d.indexOf('/');
        if (slash != -1) d = d.substring(0, slash);
        int colon = d.indexOf(':');
        if (colon != -1) d = d.substring(0, colon);
        d.toLowerCase();

        strncpy(_custom_domain, d.c_str(), sizeof(_custom_domain) - 1);
        _custom_domain[sizeof(_custom_domain) - 1] = '\0';
        Serial.printf("[DNS] Ozel yonlendirme alan adi guncellendi: %s\n", _custom_domain);
    }
}

bool DnsEngine::checkCache(const char* domain, uint32_t& out_ip) {
    uint32_t now = millis();
    for (size_t i = 0; i < DNS_CACHE_SIZE; i++) {
        if (_cache[i].domain[0] != '\0' && strcasecmp(_cache[i].domain, domain) == 0) {
            if (now < _cache[i].expire_ms) {
                out_ip = _cache[i].ip;
                return true;
            } else {
                _cache[i].domain[0] = '\0';
            }
        }
    }
    return false;
}

void DnsEngine::saveCache(const char* domain, uint32_t ip, uint32_t ttl_sec) {
    if (!domain || ip == 0) return;
    if (ttl_sec < 60) ttl_sec = 60;
    if (ttl_sec > 600) ttl_sec = 600;

    size_t slot = _cache_head % DNS_CACHE_SIZE;
    _cache_head++;

    strncpy(_cache[slot].domain, domain, sizeof(_cache[slot].domain) - 1);
    _cache[slot].domain[sizeof(_cache[slot].domain) - 1] = '\0';
    _cache[slot].ip = ip;
    _cache[slot].expire_ms = millis() + (ttl_sec * 1000);
}

bool DnsEngine::parseQName(const uint8_t* buffer, size_t len, size_t& offset, char* out_domain, size_t max_out) {
    if (offset >= len) return false;

    size_t out_idx = 0;
    size_t cur = offset;

    while (cur < len) {
        uint8_t label_len = buffer[cur++];
        if (label_len == 0) {
            // Domain sonu
            if (out_idx > 0 && out_domain[out_idx - 1] == '.') {
                out_domain[out_idx - 1] = '\0';
            } else {
                out_domain[out_idx] = '\0';
            }
            offset = cur;
            return true;
        }

        // Pointer compression kontrolu (0xC0)
        if ((label_len & 0xC0) == 0xC0) {
            cur++; // 2 baytlik isaretci
            offset = cur;
            if (out_idx > 0 && out_domain[out_idx - 1] == '.') {
                out_domain[out_idx - 1] = '\0';
            } else {
                out_domain[out_idx] = '\0';
            }
            return true;
        }

        if (cur + label_len > len) return false;

        for (uint8_t i = 0; i < label_len; i++) {
            if (out_idx < max_out - 2) {
                out_domain[out_idx++] = (char)buffer[cur++];
            } else {
                cur++;
            }
        }

        if (out_idx < max_out - 1) {
            out_domain[out_idx++] = '.';
        }
    }

    return false;
}

void DnsEngine::buildDnsResponse(uint8_t* query_buf, size_t question_end, uint16_t qtype, const IPAddress& resolved_ip, uint8_t* resp_buf, size_t& resp_len) {
    // DNS Basligi ve Soru bolumunu kopyala
    memcpy(resp_buf, query_buf, question_end);

    // Flags: Standart Yanit, No Error (0x8180)
    resp_buf[2] = 0x81;
    resp_buf[3] = 0x80;

    // QDCOUNT = 1
    resp_buf[4] = 0x00;
    resp_buf[5] = 0x01;

    // Sadece IPv4 (A / Type 1) sorgularina A kaydi dondur
    if (qtype == 1) {
        // ANCOUNT = 1 (1 Yanit Var)
        resp_buf[6] = 0x00;
        resp_buf[7] = 0x01;

        // NSCOUNT = 0, ARCOUNT = 0
        resp_buf[8] = 0x00;
        resp_buf[9] = 0x00;
        resp_buf[10] = 0x00;
        resp_buf[11] = 0x00;

        size_t cur = question_end;

        // Answer Resource Record:
        // Name: Pointer to question domain (0xC00C)
        resp_buf[cur++] = 0xC0;
        resp_buf[cur++] = 0x0C;

        // Type: A (Host Address 0x0001)
        resp_buf[cur++] = 0x00;
        resp_buf[cur++] = 0x01;

        // Class: IN (0x0001)
        resp_buf[cur++] = 0x00;
        resp_buf[cur++] = 0x01;

        // TTL: 60 saniye (0x0000003C)
        resp_buf[cur++] = 0x00;
        resp_buf[cur++] = 0x00;
        resp_buf[cur++] = 0x00;
        resp_buf[cur++] = 0x3C;

        // Data Length: 4 bayt IPv4
        resp_buf[cur++] = 0x00;
        resp_buf[cur++] = 0x04;

        // IPv4 Adresi (RDATA)
        resp_buf[cur++] = resolved_ip[0];
        resp_buf[cur++] = resolved_ip[1];
        resp_buf[cur++] = resolved_ip[2];
        resp_buf[cur++] = resolved_ip[3];

        resp_len = cur;
    } else {
        // AAAA (IPv6) veya Type 65 (HTTPS) gibi sorgularda NOERROR ve 0 Cevap (NODATA)
        // Istemci beklemeden hemen IPv4 A kaydini kullanir
        resp_buf[6] = 0x00;
        resp_buf[7] = 0x00;
        resp_buf[8] = 0x00;
        resp_buf[9] = 0x00;
        resp_buf[10] = 0x00;
        resp_buf[11] = 0x00;
        resp_len = question_end;
    }
}

void DnsEngine::forwardUpstreamAndReply(uint8_t* query_buf, size_t query_len, const char* domain, const IPAddress& client_ip, uint16_t client_port) {
    if (!WiFi.isConnected()) return;

    // Upstream DNS sunucusu: Modemin DNS'i veya 8.8.8.8
    IPAddress target_dns = _upstream_dns;
    if (target_dns == IPAddress(0, 0, 0, 0) || target_dns == AP_LOCAL_IP) {
        target_dns = IPAddress(8, 8, 8, 8); // Genel hiz icin Google DNS
    }

    _upstream_udp.beginPacket(target_dns, 53);
    _upstream_udp.write(query_buf, query_len);
    _upstream_udp.endPacket();

    uint32_t start_ms = millis();
    while (millis() - start_ms < 1500) {
        int resp_packet_size = _upstream_udp.parsePacket();
        if (resp_packet_size > 0) {
            uint8_t reply_buf[512];
            int read_len = _upstream_udp.read(reply_buf, sizeof(reply_buf));
            if (read_len > 12) {
                _local_udp.beginPacket(client_ip, client_port);
                _local_udp.write(reply_buf, read_len);
                _local_udp.endPacket();

                // Cevapta A kaydi varsa IP'yi onbellege (RAM Cache) ekle
                uint16_t ancount = (reply_buf[6] << 8) | reply_buf[7];
                if (ancount > 0 && read_len >= 16) {
                    uint32_t resolved_ip = (reply_buf[read_len - 4]) |
                                           (reply_buf[read_len - 3] << 8) |
                                           (reply_buf[read_len - 2] << 16) |
                                           (reply_buf[read_len - 1] << 24);
                    if (resolved_ip != 0) {
                        saveCache(domain, resolved_ip, 300);
                    }
                }
                return;
            }
        }
        delayMicroseconds(500);
    }
}

bool DnsEngine::process() {
    int packet_size = _local_udp.parsePacket();
    if (packet_size <= 12 || packet_size > 512) return false;

    IPAddress client_ip = _local_udp.remoteIP();
    uint16_t client_port = _local_udp.remotePort();

    // Eger cihaz tamamen engellendiyse sorguyu yok say
    if (trafficMonitor.isClientBlocked(client_ip)) {
        return true;
    }

    uint8_t query_buf[512];
    int len = _local_udp.read(query_buf, sizeof(query_buf));
    if (len <= 12) return true;

    size_t offset = 12; // DNS Basligi 12 bayt
    char domain[64] = {0};

    if (!parseQName(query_buf, len, offset, domain, sizeof(domain))) {
        return true;
    }

    if (offset + 4 > (size_t)len) return true;
    uint16_t qtype = (query_buf[offset] << 8) | query_buf[offset + 1];
    size_t question_end = offset + 4;

    // 1. DURUM: Kullanici ozel alan adi veya "insallah.com" girdi -> 192.168.4.1
    String dom = String(domain);
    dom.toLowerCase();
    String cdom = String(_custom_domain);
    cdom.toLowerCase();

    bool is_custom_site = (dom == cdom ||
                           dom == "www." + cdom ||
                           cdom == "www." + dom ||
                           dom == "insallah.com" ||
                           dom == "www.insallah.com" ||
                           dom.endsWith(".insallah.com") ||
                           dom == "denemesitem.com" ||
                           dom == "www.denemesitem.com" ||
                           dom == "admin.local" ||
                           dom == "esp32.local" ||
                           dom == "router.local");

    if (is_custom_site) 
    {
        Serial.printf("[DNS-YEREL] %s sorgulandi (Tip: %d) -> 192.168.4.1 yaniti iletildi.\n", domain, qtype);
        uint8_t resp_buf[512];
        size_t resp_len = 0;
        buildDnsResponse(query_buf, question_end, qtype, AP_LOCAL_IP, resp_buf, resp_len);

        _local_udp.beginPacket(client_ip, client_port);
        _local_udp.write(resp_buf, resp_len);
        _local_udp.endPacket();

        trafficMonitor.logDnsQuery(client_ip, domain, DNS_ACTION_LOCAL_ADMIN);
        trafficMonitor.touchClient(client_ip);
        return true;
    }

    // 2. DURUM: Alan adi Kara Listede (Blacklist) -> 0.0.0.0 (Sinkhole)
    if (trafficMonitor.isDomainBlocked(domain)) {
        uint8_t resp_buf[512];
        size_t resp_len = 0;
        buildDnsResponse(query_buf, question_end, qtype, IPAddress(0, 0, 0, 0), resp_buf, resp_len);

        _local_udp.beginPacket(client_ip, client_port);
        _local_udp.write(resp_buf, resp_len);
        _local_udp.endPacket();

        trafficMonitor.logDnsQuery(client_ip, domain, DNS_ACTION_BLOCKED);
        trafficMonitor.touchClient(client_ip);
        return true;
    }

    // 3. DURUM: DNS RAM Cache (0 ms Yanit!)
    uint32_t cached_ip = 0;
    if (qtype == 1 && checkCache(domain, cached_ip)) {
        uint8_t resp_buf[512];
        size_t resp_len = 0;
        buildDnsResponse(query_buf, question_end, qtype, IPAddress(cached_ip), resp_buf, resp_len);

        _local_udp.beginPacket(client_ip, client_port);
        _local_udp.write(resp_buf, resp_len);
        _local_udp.endPacket();

        trafficMonitor.logDnsQuery(client_ip, domain, DNS_ACTION_ALLOWED);
        trafficMonitor.touchClient(client_ip);
        return true;
    }

    // 4. DURUM: Normal Internet Sitesi -> Upstream DNS'e sor ve ilet
    trafficMonitor.logDnsQuery(client_ip, domain, DNS_ACTION_ALLOWED);
    trafficMonitor.touchClient(client_ip);
    forwardUpstreamAndReply(query_buf, len, domain, client_ip, client_port);
    return true;
}
