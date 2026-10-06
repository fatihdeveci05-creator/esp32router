#pragma once

#include <Arduino.h>
#include <esp_https_server.h>
#include <esp_http_server.h>
#include "config.h"

class WebServerManager {
public:
    WebServerManager();
    bool begin();
    void stop();

private:
    httpd_handle_t _https_server;
    httpd_handle_t _http_server;
    bool _running;

    static esp_err_t rootHandler(httpd_req_t *req);
    static esp_err_t adminHandler(httpd_req_t *req);
    static esp_err_t staticFileHandler(httpd_req_t *req);
    static esp_err_t apiStatusHandler(httpd_req_t *req);
    static esp_err_t apiClientsHandler(httpd_req_t *req);
    static esp_err_t apiDnsLogsHandler(httpd_req_t *req);
    static esp_err_t apiBlacklistGetHandler(httpd_req_t *req);
    static esp_err_t apiBlacklistPostHandler(httpd_req_t *req);
    static esp_err_t apiBlockClientHandler(httpd_req_t *req);
    static esp_err_t apiWifiScanHandler(httpd_req_t *req);
    static esp_err_t apiWifiConfigHandler(httpd_req_t *req);
    static esp_err_t apiCustomSiteGetHandler(httpd_req_t *req);
    static esp_err_t apiCustomSitePostHandler(httpd_req_t *req);
    static esp_err_t apiRebootHandler(httpd_req_t *req);

    void registerUriHandlers(httpd_handle_t server);
};

extern WebServerManager webServerManager;
