#include "napt_router.h"
#include "dns_engine.h"
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_netif.h>

#define DHCPS_OFFER_DNS 0x02

#if IP_NAPT
extern "C" {
#include "lwip/lwip_napt.h"
}
#endif

NaptRouter naptRouter;
static Preferences prefs;

NaptRouter::NaptRouter()
    : _sta_connected(false),
      _napt_active(false),
      _last_sta_check_ms(0),
      _last_led_blink_ms(0),
      _led_state(false)
{
    memset(&_config, 0, sizeof(_config));
}

static String cleanDomainString(const String& input) {
    String d = input;
    d.trim();
    if (d.startsWith("https://")) d = d.substring(8);
    else if (d.startsWith("http://")) d = d.substring(7);
    int slash_idx = d.indexOf('/');
    if (slash_idx != -1) d = d.substring(0, slash_idx);
    int colon_idx = d.indexOf(':');
    if (colon_idx != -1) d = d.substring(0, colon_idx);
    d.trim();
    d.toLowerCase();
    if (d.startsWith("www.")) d = d.substring(4);
    return d;
}

void NaptRouter::loadConfig() {
    prefs.begin(NVS_NAMESPACE, false);

    String sta_ssid = prefs.getString(NVS_KEY_STA_SSID, DEFAULT_STA_SSID);
    String sta_pass = prefs.getString(NVS_KEY_STA_PASS, DEFAULT_STA_PASS);
    String ap_ssid = prefs.getString(NVS_KEY_AP_SSID, DEFAULT_AP_SSID);
    String ap_pass = prefs.getString(NVS_KEY_AP_PASS, DEFAULT_AP_PASS);
    String admin_pass = prefs.getString(NVS_KEY_ADMIN_PASS, "admin");
    String custom_domain = prefs.getString(NVS_KEY_CUSTOM_DOMAIN, LOCAL_ADMIN_DOMAIN);
    custom_domain = cleanDomainString(custom_domain);
    if (custom_domain.length() < 3) custom_domain = LOCAL_ADMIN_DOMAIN;

    strncpy(_config.sta_ssid, sta_ssid.c_str(), sizeof(_config.sta_ssid) - 1);
    strncpy(_config.sta_pass, sta_pass.c_str(), sizeof(_config.sta_pass) - 1);
    strncpy(_config.ap_ssid, ap_ssid.c_str(), sizeof(_config.ap_ssid) - 1);
    strncpy(_config.ap_pass, ap_pass.c_str(), sizeof(_config.ap_pass) - 1);
    strncpy(_config.admin_pass, admin_pass.c_str(), sizeof(_config.admin_pass) - 1);
    strncpy(_config.custom_domain, custom_domain.c_str(), sizeof(_config.custom_domain) - 1);

    _config.napt_enabled = prefs.getBool("napt_en", true);
    _config.filter_enabled = prefs.getBool("filter_en", true);

    prefs.end();

    dnsEngine.setCustomDomain(_config.custom_domain);
}

void NaptRouter::saveConfig() {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString(NVS_KEY_STA_SSID, _config.sta_ssid);
    prefs.putString(NVS_KEY_STA_PASS, _config.sta_pass);
    prefs.putString(NVS_KEY_AP_SSID, _config.ap_ssid);
    prefs.putString(NVS_KEY_AP_PASS, _config.ap_pass);
    prefs.putString(NVS_KEY_ADMIN_PASS, _config.admin_pass);
    prefs.putString(NVS_KEY_CUSTOM_DOMAIN, _config.custom_domain);
    prefs.putBool("napt_en", _config.napt_enabled);
    prefs.putBool("filter_en", _config.filter_enabled);
    prefs.end();
}

void NaptRouter::configureApDhcpDns() {
    // AP DHCP sunucusunun istemcilere DNS olarak SADECE 192.168.4.1 (ESP32) vermesini sagla
    // 8.8.8.8 verilirse Windows parallel sorguda insallah.com icin NXDOMAIN alip DNS_PROBE_STARTED verir!
    esp_netif_t* netif_ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif_ap != nullptr) {
        esp_netif_dhcps_stop(netif_ap);

        esp_netif_dns_info_t dns_info_main;
        dns_info_main.ip.type = ESP_IPADDR_TYPE_V4;
        dns_info_main.ip.u_addr.ip4.addr = static_cast<uint32_t>(AP_LOCAL_IP);

        esp_netif_dns_info_t dns_info_backup;
        dns_info_backup.ip.type = ESP_IPADDR_TYPE_V4;
        dns_info_backup.ip.u_addr.ip4.addr = static_cast<uint32_t>(AP_LOCAL_IP);

        esp_netif_set_dns_info(netif_ap, ESP_NETIF_DNS_MAIN, &dns_info_main);
        esp_netif_set_dns_info(netif_ap, ESP_NETIF_DNS_BACKUP, &dns_info_backup);

        uint8_t offer_dns = DHCPS_OFFER_DNS;
        esp_netif_dhcps_option(netif_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
        esp_netif_dhcps_start(netif_ap);
        Serial.println("[AĞ] AP DHCP istemcilerine DNS olarak yalnızca 192.168.4.1 atandı.");
    }
}

bool NaptRouter::begin() {
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);

    loadConfig();

    // 1. Wi-Fi Olay Dinleyicileri (Event-driven connection & NAPT)
    WiFi.onEvent([this](WiFiEvent_t event, WiFiEventInfo_t info) {
        if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            _sta_connected = true;
            Serial.printf("\n[AĞ] >>> MODEME BAĞLANDI! <<<\n");
            Serial.printf("  IP: %s\n  Ağ Geçidi (Gateway): %s\n  DNS: %s\n  Sinyal (RSSI): %d dBm\n",
                          WiFi.localIP().toString().c_str(),
                          WiFi.gatewayIP().toString().c_str(),
                          WiFi.dnsIP().toString().c_str(),
                          WiFi.RSSI());

            // DNS motoruna upstream olarak modemin DNS adresini ver
            dnsEngine.setUpstreamDns(WiFi.dnsIP());

            // NAPT yonlendiriciyi AP uzerinde baslat
            enableNapt();
        } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            uint8_t reason = info.wifi_sta_disconnected.reason;
            Serial.printf("[AĞ] Modem bağlantısı kesildi / başarısız (Neden Kodu: %d)\n", reason);
            if (_sta_connected) {
                _sta_connected = false;
                disableNapt();
            }
        } else if (event == ARDUINO_EVENT_WIFI_AP_START) {
            Serial.println("[AĞ] SoftAP Başlatıldı/Kanal Ayarlandı.");
            configureApDhcpDns();
            if (_sta_connected) {
                enableNapt();
            }
        }
    });

    // 2. Hibrit Mod: Hem AP (Kendi yayini) hem STA (Modem istemcisi)
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    esp_wifi_set_ps(WIFI_PS_NONE); // Minimum ping ve maksimum aktarim hizi

    // 3. SoftAP Agini Baslat (192.168.4.1)
    WiFi.softAPConfig(AP_LOCAL_IP, AP_GATEWAY, AP_SUBNET);
    bool ap_ok = WiFi.softAP(_config.ap_ssid, _config.ap_pass, DEFAULT_AP_CHANNEL, 0, DEFAULT_AP_MAX_CLIENT);

    if (ap_ok) {
        Serial.printf("[AĞ] Erişim Noktası Açıldı: SSID='%s', IP=%s\n", 
                      _config.ap_ssid, AP_LOCAL_IP.toString().c_str());
        configureApDhcpDns();
    } else {
        Serial.println("[AĞ] HATA: SoftAP baslatilamadi!");
    }

    // 4. Uzak modeme baglanmayi baslat
    if (strlen(_config.sta_ssid) > 0) {
        Serial.printf("[AĞ] Uzak modeme baglaniliyor: %s\n", _config.sta_ssid);
        WiFi.begin(_config.sta_ssid, _config.sta_pass);
    } else {
        Serial.println("[AĞ] Henuz tanimli bir uzak modem yok. Arayuzden yapilandirin.");
    }

    return true;
}

void NaptRouter::enableNapt() {
#if IP_NAPT
    // lwIP NAPT KURALI: NAPT SADECE ic yerel ag (AP) IP adresi uzerinde aktif edilmelidir!
    // STA interface uzerinde aktif edilirse lwIP disari giden paketleri NAT'lamayi durdurur.
    ip_napt_enable(static_cast<uint32_t>(AP_LOCAL_IP), 1);
    _napt_active = true;
    Serial.println("[NAPT] lwIP NAPT Yönlendirici AKTİF! AP (192.168.4.1) istemcileri artık internete çıkabilir.");
#else
    Serial.println("[NAPT] UYARI: IP_NAPT derleme bayrağı tanımlı değil!");
#endif
}

void NaptRouter::disableNapt() {
#if IP_NAPT
    if (_napt_active) {
        ip_napt_enable(static_cast<uint32_t>(AP_LOCAL_IP), 0);
        _napt_active = false;
        Serial.println("[NAPT] NAPT durduruldu.");
    }
#endif
}

void NaptRouter::update() {
    uint32_t now = millis();

    // Durum LED'i yonetimi
    handleLed();

    // 10 saniyede bir STA durum kontrolu ve gerekiyorsa guvenli yeniden deneme
    if (now - _last_sta_check_ms > 10000) {
        _last_sta_check_ms = now;

        if (WiFi.status() == WL_CONNECTED) {
            if (!_sta_connected) {
                _sta_connected = true;
                dnsEngine.setUpstreamDns(WiFi.dnsIP());
                enableNapt();
            }
        } else {
            if (_sta_connected) {
                _sta_connected = false;
                disableNapt();
            }

            // Eger 20 saniyeden uzun suredir bagli degilse yeniden deneme tetikle
            if (strlen(_config.sta_ssid) > 0 && WiFi.status() != WL_CONNECTED) {
                Serial.printf("[AĞ] Modem (%s) ile bağlantı kurulamadı, yeniden deneniyor...\n", _config.sta_ssid);
                WiFi.disconnect(false);
                WiFi.begin(_config.sta_ssid, _config.sta_pass);
            }
        }
    }
}

void NaptRouter::handleLed() {
    uint32_t now = millis();

    if (_sta_connected && _napt_active) {
        // Internet var ve yonlendirici aktif -> LED Surekli Acik
        digitalWrite(STATUS_LED_PIN, HIGH);
    } else if (strlen(_config.sta_ssid) > 0) {
        // Modeme baglanmaya calisiyor -> 500 ms Yanip Sonme
        if (now - _last_led_blink_ms > 500) {
            _last_led_blink_ms = now;
            _led_state = !_led_state;
            digitalWrite(STATUS_LED_PIN, _led_state ? HIGH : LOW);
        }
    } else {
        // Yalnizca AP modu (Ayar bekleniyor) -> Hizli Yanip Sonme (200 ms)
        if (now - _last_led_blink_ms > 200) {
            _last_led_blink_ms = now;
            _led_state = !_led_state;
            digitalWrite(STATUS_LED_PIN, _led_state ? HIGH : LOW);
        }
    }
}

int NaptRouter::getStaRssi() const {
    return _sta_connected ? WiFi.RSSI() : -100;
}

IPAddress NaptRouter::getStaIp() const {
    return _sta_connected ? WiFi.localIP() : IPAddress(0, 0, 0, 0);
}

String NaptRouter::getStaSsid() const {
    return String(_config.sta_ssid);
}

void NaptRouter::connectToRemoteAp(const char* ssid, const char* pass) {
    if (!ssid) return;
    strncpy(_config.sta_ssid, ssid, sizeof(_config.sta_ssid) - 1);
    if (pass) {
        strncpy(_config.sta_pass, pass, sizeof(_config.sta_pass) - 1);
    } else {
        _config.sta_pass[0] = '\0';
    }
    saveConfig();

    Serial.printf("[AĞ] Yeni modem ayarlari kaydedildi. Baglaniliyor: %s\n", _config.sta_ssid);
    WiFi.disconnect(false, false);
    delay(100);
    WiFi.begin(_config.sta_ssid, _config.sta_pass);
}

void NaptRouter::updateApSettings(const char* ssid, const char* pass) {
    if (!ssid || strlen(ssid) < 2) return;
    strncpy(_config.ap_ssid, ssid, sizeof(_config.ap_ssid) - 1);
    if (pass && strlen(pass) >= 8) {
        strncpy(_config.ap_pass, pass, sizeof(_config.ap_pass) - 1);
    }
    saveConfig();

    Serial.println("[AĞ] AP ayarlari guncellendi. SoftAP yeniden baslatiliyor...");
    WiFi.softAP(_config.ap_ssid, _config.ap_pass, DEFAULT_AP_CHANNEL, 0, DEFAULT_AP_MAX_CLIENT);
    configureApDhcpDns();
}

void NaptRouter::updateCustomDomain(const char* domain) {
    if (!domain) return;
    String cleaned = cleanDomainString(String(domain));
    if (cleaned.length() < 3) return;
    strncpy(_config.custom_domain, cleaned.c_str(), sizeof(_config.custom_domain) - 1);
    saveConfig();
    dnsEngine.setCustomDomain(_config.custom_domain);
    Serial.printf("[AĞ] Ozel yonlendirme alani kaydedildi: %s\n", _config.custom_domain);
}

void NaptRouter::factoryReset() {
    Serial.println("[AĞ] Fabrika Ayarlarina Donuluyor...");
    prefs.begin(NVS_NAMESPACE, false);
    prefs.clear();
    prefs.end();
    delay(500);
    ESP.restart();
}
