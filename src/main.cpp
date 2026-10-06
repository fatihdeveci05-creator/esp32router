#include <Arduino.h>
#include "config.h"
#include "traffic_monitor.h"
#include "dns_engine.h"
#include "napt_router.h"
#include "web_server.h"

static uint32_t button_press_start_ms = 0;
static bool button_pressed = false;

void checkResetButton() {
    // GPIO 0 BOOT Butonu: LOW ise basili demektir
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
        if (!button_pressed) {
            button_pressed = true;
            button_press_start_ms = millis();
        } else {
            // 5 saniyeden fazla basili tutulursa fabrika ayarlarina don
            if (millis() - button_press_start_ms > 5000) {
                Serial.println("\n[SİSTEM] Fabrika ayarlarına dönüş butonu algılandı!");
                naptRouter.factoryReset();
            }
        }
    } else {
        button_pressed = false;
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n==================================================");
    Serial.println("   ESP32 GÜVENLİ YÖNLENDİRİCİ, DNS SINKHOLE &    ");
    Serial.println("              HTTPS WEB SUNUCUSU                  ");
    Serial.println("==================================================");

    // 1. Donanim ve Reset Butonu Yapilandirmasi
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

    // 2. Trafik ve Istatistik Takip Motoru
    Serial.println("[1/4] Trafik izleme motoru baslatiliyor...");
    trafficMonitor.begin();

    // 3. Wi-Fi AP + STA ve NAPT Yonlendirici
    Serial.println("[2/4] Wi-Fi ve NAPT yonlendirici baslatiliyor...");
    naptRouter.begin();

    // 4. DNS Sunucusu ve Guvenlik Duvari (Port 53)
    Serial.println("[3/4] Port 53 DNS Sinkhole ve Alan Adi Yonlendirici baslatiliyor...");
    dnsEngine.begin(FALLBACK_UPSTREAM_DNS);

    // 5. HTTPS (Port 443) ve HTTP (Port 80) Web Sunuculari
    Serial.println("[4/4] HTTPS/HTTP Web Sunuculari ve API servisleri baslatiliyor...");
    webServerManager.begin();

    Serial.println("\n[HAZIR] Sistem basariyla calisiyor!");
    Serial.printf(" Yerel Yonetim Adresi: https://%s (veya https://192.168.4.1)\n", LOCAL_ADMIN_DOMAIN);
    Serial.println("==================================================\n");
}

void loop() {
    // NAPT durumu, LED kontrolu ve modem baglantisini guncelle
    naptRouter.update();

    // BOOT butonu ile fabrika ayarlarina sifirlama kontrolu
    checkResetButton();

    vTaskDelay(pdMS_TO_TICKS(10));
}
