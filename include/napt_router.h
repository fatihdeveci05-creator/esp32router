#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <IPAddress.h>
#include "config.h"

class NaptRouter {
public:
    NaptRouter();
    bool begin();
    void update();

    // Durum Fonksiyonlari
    bool isStaConnected() const { return _sta_connected; }
    bool isNaptActive() const { return _napt_active; }
    int getStaRssi() const;
    IPAddress getStaIp() const;
    String getStaSsid() const;
    
    // Wi-Fi Yapilandirma
    void connectToRemoteAp(const char* ssid, const char* pass);
    void updateApSettings(const char* ssid, const char* pass);
    void factoryReset();

private:
    RouterConfig _config;
    bool _sta_connected;
    bool _napt_active;
    uint32_t _last_sta_check_ms;
    uint32_t _last_led_blink_ms;
    bool _led_state;

    void loadConfig();
    void saveConfig();
    void configureApDhcpDns();
    void enableNapt();
    void disableNapt();
    void handleLed();
};

extern NaptRouter naptRouter;
