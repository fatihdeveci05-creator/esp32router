#include "dns_engine.h"
#include <lwip/sockets.h>
#include <lwip/netdb.h>

DnsEngine dnsEngine;

static void dnsTaskTrampoline(void* arg) {
    DnsEngine* engine = static_cast<DnsEngine*>(arg);
    while (true) {
        engine->process();
        vTaskDelay(pdMS_TO_TICKS(5)); // CPU'yu boğmamak için kısa dinlenme
    }
}

DnsEngine::DnsEngine() 
    : _upstream_dns(FALLBACK_UPSTREAM_DNS),
      _task_handle(nullptr),
      _running(false)
{
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
        1,
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
        strncpy(_custom_domain, domain, sizeof(_custom_domain) - 1);
        _custom_domain[sizeof(_custom_domain) - 1] = '\0';
        Serial.printf("[DNS] Ozel yonlendirme alan adi guncellendi: %s\n", _custom_domain);
    }
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

void DnsEngine::buildDnsResponse(uint8_t* query_buf, size_t query_len, const char* domain, const IPAddress& resolved_ip, uint8_t* resp_buf, size_t& resp_len) {
    // Orijinal sorguyu kopyala
    memcpy(resp_buf, query_buf, query_len);

    // Flags: Standart Yanit, No Error (0x8180)
    resp_buf[2] = 0x81;
    resp_buf[3] = 0x80;

    // QDCOUNT = 1
    resp_buf[4] = 0x00;
    resp_buf[5] = 0x01;

    // ANCOUNT = 1 (1 Yanit Var)
    resp_buf[6] = 0x00;
    resp_buf[7] = 0x01;

    // NSCOUNT = 0, ARCOUNT = 0
    resp_buf[8] = 0x00;
    resp_buf[9] = 0x00;
    resp_buf[10] = 0x00;
    resp_buf[11] = 0x00;

    size_t cur = query_len;

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
}

void DnsEngine::forwardUpstreamAndReply(uint8_t* query_buf, size_t query_len, const IPAddress& client_ip, uint16_t client_port) {
    // Upstream DNS sunucusuna paketi ilet
    _upstream_udp.beginPacket(_upstream_dns, 53);
    _upstream_udp.write(query_buf, query_len);
    _upstream_udp.endPacket();

    // Upstream yanitini bekle (Maksimum 600 ms)
    uint32_t start_ms = millis();
    while (millis() - start_ms < 600) {
        int resp_packet_size = _upstream_udp.parsePacket();
        if (resp_packet_size > 0) {
            uint8_t reply_buf[1024];
            int read_len = _upstream_udp.read(reply_buf, sizeof(reply_buf));
            if (read_len > 0) {
                // Istemciye geri ilet
                _local_udp.beginPacket(client_ip, client_port);
                _local_udp.write(reply_buf, read_len);
                _local_udp.endPacket();

                // Istemcinin veri sayacini guncelle
                trafficMonitor.updateClientTraffic(client_ip.toString().c_str(), "", read_len + query_len, 0);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void DnsEngine::process() {
    int packet_size = _local_udp.parsePacket();
    if (packet_size <= 12 || packet_size > 512) return; // Gecersiz veya basliksiz paket

    IPAddress client_ip = _local_udp.remoteIP();
    uint16_t client_port = _local_udp.remotePort();

    // Eger cihaz tamamen engellendiyse sorguyu yok say
    if (trafficMonitor.isClientBlocked(client_ip)) {
        return;
    }

    uint8_t query_buf[512];
    int len = _local_udp.read(query_buf, sizeof(query_buf));
    if (len <= 12) return;

    size_t offset = 12; // DNS Basligi 12 bayt
    char domain[64] = {0};

    if (!parseQName(query_buf, len, offset, domain, sizeof(domain))) {
        return;
    }

    // 1. DURUM: Kullanici ozel alan adi veya "denemesitem.com" girdi -> Kendi yerel IP'mizi don (192.168.4.1)
    bool is_custom_site = (strcasecmp(domain, _custom_domain) == 0 ||
                           (strlen(domain) > 4 && strncasecmp(domain, "www.", 4) == 0 && strcasecmp(domain + 4, _custom_domain) == 0) ||
                           strcasecmp(domain, LOCAL_ADMIN_DOMAIN) == 0 ||
                           (strlen(domain) > 4 && strncasecmp(domain, "www.", 4) == 0 && strcasecmp(domain + 4, LOCAL_ADMIN_DOMAIN) == 0) ||
                           strcasecmp(domain, "admin.local") == 0);

    if (is_custom_site) 
    {
        uint8_t resp_buf[512];
        size_t resp_len = 0;
        buildDnsResponse(query_buf, len, domain, AP_LOCAL_IP, resp_buf, resp_len);

        _local_udp.beginPacket(client_ip, client_port);
        _local_udp.write(resp_buf, resp_len);
        _local_udp.endPacket();

        trafficMonitor.logDnsQuery(client_ip, domain, DNS_ACTION_LOCAL_ADMIN);
        trafficMonitor.touchClient(client_ip);
        return;
    }

    // 2. DURUM: Alan adi Kara Listede (Blacklist) -> 0.0.0.0 (Sinkhole) don
    if (trafficMonitor.isDomainBlocked(domain)) {
        uint8_t resp_buf[512];
        size_t resp_len = 0;
        buildDnsResponse(query_buf, len, domain, IPAddress(0, 0, 0, 0), resp_buf, resp_len);

        _local_udp.beginPacket(client_ip, client_port);
        _local_udp.write(resp_buf, resp_len);
        _local_udp.endPacket();

        trafficMonitor.logDnsQuery(client_ip, domain, DNS_ACTION_BLOCKED);
        trafficMonitor.touchClient(client_ip);
        return;
    }

    // 3. DURUM: Normal Internet Sitesi -> Upstream DNS'e sor ve ilet
    trafficMonitor.logDnsQuery(client_ip, domain, DNS_ACTION_ALLOWED);
    trafficMonitor.touchClient(client_ip);
    forwardUpstreamAndReply(query_buf, len, client_ip, client_port);
}
