#pragma once

#include <Arduino.h>
#include <IPAddress.h>

// ==========================================
// DONANIM PİNLERİ (DOIT ESP32 DevKit V1)
// ==========================================
#define STATUS_LED_PIN    2      // Dahili mavi LED (GPIO 2)
#define RESET_BUTTON_PIN  0      // Dahili BOOT butonu (GPIO 0 - Fabrika Ayarları)

// ==========================================
// AĞ VE YÖNLENDİRİCİ VARSAYILANLARI
// ==========================================
#define DEFAULT_AP_SSID       "ESP32-SecureRouter"
#define DEFAULT_AP_PASS       "12345678"
#define DEFAULT_AP_CHANNEL    1
#define DEFAULT_AP_MAX_CLIENT 8

// ESP32 Erişim Noktası (AP) Yerel IP Adresi
inline const IPAddress AP_LOCAL_IP(192, 168, 4, 1);
inline const IPAddress AP_GATEWAY(192, 168, 4, 1);
inline const IPAddress AP_SUBNET(255, 255, 255, 0);

// Varsayılan Yedek Upstream DNS (Google / Cloudflare)
inline const IPAddress FALLBACK_UPSTREAM_DNS(8, 8, 8, 8);

// ==========================================
// ÖZEL ALAN ADI VE PORTLAR
// ==========================================
#define LOCAL_ADMIN_DOMAIN    "insallah.com"
#define HTTP_SERVER_PORT      80
#define HTTPS_SERVER_PORT     443
#define DNS_SERVER_PORT       53

// ==========================================
// SİSTEM SINIRLARI & TAMPONLAR
// ==========================================
#define MAX_DNS_LOG_ENTRIES   80    // RAM koruması için döngüsel log kapasitesi
#define MAX_BLACKLIST_DOMAINS 64    // Yasaklanabilecek maksimum alan adı sayısı
#define MAX_TRACKED_CLIENTS   16    // Takip edilen aktif istemci kapasitesi

// NVS Yapılandırma Anahtarları
#define NVS_NAMESPACE         "esp_router"
#define NVS_KEY_STA_SSID      "sta_ssid"
#define NVS_KEY_STA_PASS      "sta_pass"
#define NVS_KEY_AP_SSID       "ap_ssid"
#define NVS_KEY_AP_PASS       "ap_pass"
#define NVS_KEY_ADMIN_PASS    "admin_pass"
#define NVS_KEY_CUSTOM_DOMAIN "cust_domain"

struct RouterConfig {
    char sta_ssid[64];
    char sta_pass[64];
    char ap_ssid[32];
    char ap_pass[64];
    char admin_pass[32];
    char custom_domain[64];
    bool napt_enabled;
    bool filter_enabled;
};
