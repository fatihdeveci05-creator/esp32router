#include "web_server.h"
#include "cert.h"
#include "traffic_monitor.h"
#include "napt_router.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>

WebServerManager webServerManager;

// Kullanicinin ozel web sitesi icin varsayilan HTML sablonu
static const char FALLBACK_CUSTOM_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="tr">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Özel Web Siteniz - ESP32</title>
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #0f172a; color: #f8fafc; text-align: center; padding: 60px 20px; margin: 0; }
        .box { max-width: 650px; margin: auto; background: #1e293b; padding: 40px; border-radius: 16px; box-shadow: 0 10px 30px rgba(0,0,0,0.5); }
        h1 { color: #38bdf8; font-size: 26px; margin-bottom: 12px; }
        p { color: #94a3b8; font-size: 15px; line-height: 1.6; }
        .info { background: #0b0f19; padding: 16px; border-radius: 10px; margin: 24px 0; font-family: monospace; color: #38bdf8; font-size: 13px; }
        .btn { display: inline-block; background: #38bdf8; color: #0b0f19; padding: 12px 24px; border-radius: 8px; text-decoration: none; font-weight: bold; font-size: 14px; }
    </style>
</head>
<body>
    <div class="box">
        <h1>🌐 Özel Web Siteniz Yayında!</h1>
        <p>Bu web sayfası doğrudan <strong>ESP32 DevKit V1</strong> dahili belleğinden HTTPS üzerinden sunulmaktadır.</p>
        <div class="info">Kendi HTML/CSS kodlarınızı yüklemek veya URL alan adını değiştirmek için yönetim paneline geçebilirsiniz.</div>
        <a href="/admin" class="btn">⚙️ Yönetim Paneline Git (/admin)</a>
    </div>
</body>
</html>
)rawhtml";

// LittleFS baglanamazsa acil durum admin paneli
static const char FALLBACK_ADMIN_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="tr">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Router & Güvenlik Paneli</title>
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #0f172a; color: #f8fafc; margin: 0; padding: 20px; }
        .card { background: #1e293b; border-radius: 12px; padding: 24px; max-width: 600px; margin: 40px auto; box-shadow: 0 10px 25px rgba(0,0,0,0.5); }
        h1 { color: #38bdf8; font-size: 22px; margin-top: 0; }
        p { color: #94a3b8; line-height: 1.6; }
        .badge { background: #0284c7; padding: 4px 10px; border-radius: 6px; font-size: 13px; font-weight: bold; }
    </style>
</head>
<body>
    <div class="card">
        <h1>🚀 ESP32 Yönetim Portalı</h1>
        <p>Yönlendirici ve API servisleri arka planda aktiftir. Tam arayüz için LittleFS dosyalarını yükleyebilirsiniz.</p>
        <p><span class="badge">Sistem Durumu: Çevrimiçi</span></p>
    </div>
</body>
</html>
)rawhtml";

WebServerManager::WebServerManager()
    : _https_server(nullptr),
      _http_server(nullptr),
      _running(false)
{
}

bool WebServerManager::begin() {
    if (_running) return true;

    // LittleFS dosya sistemini baslat
    if (!LittleFS.begin(true)) {
        Serial.println("[WEB] LittleFS baslatilamadi, yedek dahili arayuz kullanilacak.");
    } else {
        Serial.println("[WEB] LittleFS basariyla baglandi.");
    }

    // ==========================================
    // 1. HTTPS SUNUCUSU (PORT 443) - mbedTLS
    // ==========================================
    httpd_ssl_config_t https_conf = HTTPD_SSL_CONFIG_DEFAULT();
    https_conf.cacert_pem = (const uint8_t*)SERVER_CERT_PEM;
    https_conf.cacert_len = strlen(SERVER_CERT_PEM) + 1;
    https_conf.prvtkey_pem = (const uint8_t*)SERVER_KEY_PEM;
    https_conf.prvtkey_len = strlen(SERVER_KEY_PEM) + 1;
    https_conf.httpd.port = HTTPS_SERVER_PORT;
    https_conf.httpd.stack_size = 10240;
    https_conf.httpd.max_uri_handlers = 16;
    https_conf.httpd.lru_purge_enable = true;

    esp_err_t https_ret = httpd_ssl_start(&_https_server, &https_conf);
    if (https_ret == ESP_OK) {
        Serial.println("[WEB] HTTPS Sunucusu Port 443 uzerinde basariyla baslatildi.");
        registerUriHandlers(_https_server);
    } else {
        Serial.printf("[WEB] HATA: HTTPS sunucusu baslatilamadi! Hata Kodu: %d\n", https_ret);
    }

    // ==========================================
    // 2. HTTP SUNUCUSU (PORT 80)
    // ==========================================
    httpd_config_t http_conf = HTTPD_DEFAULT_CONFIG();
    http_conf.server_port = HTTP_SERVER_PORT;
    http_conf.stack_size = 6144;
    http_conf.max_uri_handlers = 16;
    http_conf.lru_purge_enable = true;

    esp_err_t http_ret = httpd_start(&_http_server, &http_conf);
    if (http_ret == ESP_OK) {
        Serial.println("[WEB] HTTP Sunucusu Port 80 uzerinde basariyla baslatildi.");
        registerUriHandlers(_http_server);
    } else {
        Serial.printf("[WEB] HATA: HTTP sunucusu baslatilamadi! Hata Kodu: %d\n", http_ret);
    }

    _running = (https_ret == ESP_OK || http_ret == ESP_OK);
    return _running;
}

void WebServerManager::stop() {
    if (_https_server) {
        httpd_ssl_stop(_https_server);
        _https_server = nullptr;
    }
    if (_http_server) {
        httpd_stop(_http_server);
        _http_server = nullptr;
    }
    _running = false;
}

void WebServerManager::registerUriHandlers(httpd_handle_t server) {
    if (!server) return;

    // Kok Dizin (/)
    httpd_uri_t uri_root = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = rootHandler,
        .user_ctx  = nullptr
    };
    httpd_register_uri_handler(server, &uri_root);

    // Statik Dosyalar (/style.css, /app.js, favicon.ico vb.)
    httpd_uri_t uri_style = {
        .uri       = "/style.css",
        .method    = HTTP_GET,
        .handler   = staticFileHandler,
        .user_ctx  = (void*)"text/css"
    };
    httpd_register_uri_handler(server, &uri_style);

    httpd_uri_t uri_js = {
        .uri       = "/app.js",
        .method    = HTTP_GET,
        .handler   = staticFileHandler,
        .user_ctx  = (void*)"application/javascript"
    };
    httpd_register_uri_handler(server, &uri_js);

    // API Uç Noktaları
    httpd_uri_t uri_api_status = {
        .uri       = "/api/status",
        .method    = HTTP_GET,
        .handler   = apiStatusHandler,
        .user_ctx  = nullptr
    };
    httpd_register_uri_handler(server, &uri_api_status);

    httpd_uri_t uri_api_clients = {
        .uri       = "/api/clients",
        .method    = HTTP_GET,
        .handler   = apiClientsHandler,
        .user_ctx  = nullptr
    };
    httpd_register_uri_handler(server, &uri_api_clients);

    httpd_uri_t uri_api_dns = {
        .uri       = "/api/dns-logs",
        .method    = HTTP_GET,
        .handler   = apiDnsLogsHandler,
        .user_ctx  = nullptr
    };
    httpd_register_uri_handler(server, &uri_api_dns);

    httpd_uri_t uri_api_bl_get = {
        .uri       = "/api/blacklist",
        .method    = HTTP_GET,
        .handler   = apiBlacklistGetHandler,
        .user_ctx  = nullptr
    };
    httpd_register_uri_handler(server, &uri_api_bl_get);

    httpd_uri_t uri_api_bl_post = {
        .uri       = "/api/blacklist",
        .method    = HTTP_POST,
        .handler   = apiBlacklistPostHandler,
        .user_ctx  = nullptr
    };
    httpd_register_uri_handler(server, &uri_api_bl_post);

    httpd_uri_t uri_api_block_client = {
        .uri       = "/api/block-client",
        .method    = HTTP_POST,
        .handler   = apiBlockClientHandler,
        .user_ctx  = nullptr
    };
    httpd_register_uri_handler(server, &uri_api_block_client);

    httpd_uri_t uri_api_wifi_scan = {
        .uri       = "/api/wifi-scan",
        .method    = HTTP_GET,
        .handler   = apiWifiScanHandler,
        .user_ctx  = nullptr
    };
    httpd_register_uri_handler(server, &uri_api_wifi_scan);

    httpd_uri_t uri_api_wifi_cfg = {
        .uri       = "/api/wifi-config",
        .method    = HTTP_POST,
        .handler   = apiWifiConfigHandler,
        .user_ctx  = nullptr
    };
    httpd_register_uri_handler(server, &uri_api_wifi_cfg);

    // Yonetim Portali (/admin)
    httpd_uri_t uri_admin = {
        .uri       = "/admin",
        .method    = HTTP_GET,
        .handler   = adminHandler,
        .user_ctx  = nullptr
    };
    httpd_register_uri_handler(server, &uri_admin);

    // Ozel Site Ayarlari ve HTML Duzenleyici API
    httpd_uri_t uri_api_site_get = {
        .uri       = "/api/custom-site",
        .method    = HTTP_GET,
        .handler   = apiCustomSiteGetHandler,
        .user_ctx  = nullptr
    };
    httpd_register_uri_handler(server, &uri_api_site_get);

    httpd_uri_t uri_api_site_post = {
        .uri       = "/api/custom-site",
        .method    = HTTP_POST,
        .handler   = apiCustomSitePostHandler,
        .user_ctx  = nullptr
    };
    httpd_register_uri_handler(server, &uri_api_site_post);

    httpd_uri_t uri_api_reboot = {
        .uri       = "/api/reboot",
        .method    = HTTP_POST,
        .handler   = apiRebootHandler,
        .user_ctx  = nullptr
    };
    httpd_register_uri_handler(server, &uri_api_reboot);
}

esp_err_t WebServerManager::rootHandler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    // Kullanicinin ozel HTML sayfasi LittleFS'de varsa onu gonder
    if (LittleFS.exists("/site/index.html")) {
        File file = LittleFS.open("/site/index.html", "r");
        if (file) {
            char chunk[512];
            while (file.available()) {
                size_t read_bytes = file.readBytes(chunk, sizeof(chunk));
                httpd_resp_send_chunk(req, chunk, read_bytes);
            }
            file.close();
            httpd_resp_send_chunk(req, nullptr, 0);
            return ESP_OK;
        }
    }

    // LittleFS'de ozel dosya yoksa varsayilan ozel site karsilama ekranini gonder
    httpd_resp_send(req, FALLBACK_CUSTOM_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t WebServerManager::adminHandler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    // Yonetim Portali HTML dosyasi
    const char* admin_paths[] = {"/admin/index.html", "/admin.html", "/index.html"};
    for (const char* path : admin_paths) {
        if (LittleFS.exists(path)) {
            File file = LittleFS.open(path, "r");
            if (file) {
                char chunk[512];
                while (file.available()) {
                    size_t read_bytes = file.readBytes(chunk, sizeof(chunk));
                    httpd_resp_send_chunk(req, chunk, read_bytes);
                }
                file.close();
                httpd_resp_send_chunk(req, nullptr, 0);
                return ESP_OK;
            }
        }
    }

    // LittleFS baglanamadiysa acil durum admin paneli
    httpd_resp_send(req, FALLBACK_ADMIN_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t WebServerManager::staticFileHandler(httpd_req_t *req) {
    const char* content_type = (const char*)req->user_ctx;
    if (content_type) {
        httpd_resp_set_type(req, content_type);
    }

    String path = req->uri;
    if (LittleFS.exists(path)) {
        File file = LittleFS.open(path, "r");
        if (file) {
            char chunk[512];
            while (file.available()) {
                size_t read_bytes = file.readBytes(chunk, sizeof(chunk));
                httpd_resp_send_chunk(req, chunk, read_bytes);
            }
            file.close();
            httpd_resp_send_chunk(req, nullptr, 0);
            return ESP_OK;
        }
    }

    httpd_resp_send_404(req);
    return ESP_FAIL;
}

esp_err_t WebServerManager::apiStatusHandler(httpd_req_t *req) {
    JsonDocument doc;
    JsonObject obj = doc.to<JsonObject>();
    trafficMonitor.getStatusJson(obj);

    obj["sta_connected"] = naptRouter.isStaConnected();
    obj["napt_active"] = naptRouter.isNaptActive();
    obj["sta_rssi"] = naptRouter.getStaRssi();
    obj["sta_ip"] = naptRouter.getStaIp().toString();
    obj["sta_ssid"] = naptRouter.getStaSsid();
    obj["admin_domain"] = LOCAL_ADMIN_DOMAIN;

    String response;
    serializeJson(doc, response);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, response.c_str(), response.length());
    return ESP_OK;
}

esp_err_t WebServerManager::apiClientsHandler(httpd_req_t *req) {
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();
    trafficMonitor.getClientsJson(array);

    String response;
    serializeJson(doc, response);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, response.c_str(), response.length());
    return ESP_OK;
}

esp_err_t WebServerManager::apiDnsLogsHandler(httpd_req_t *req) {
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();
    trafficMonitor.getDnsLogsJson(array);

    String response;
    serializeJson(doc, response);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, response.c_str(), response.length());
    return ESP_OK;
}

esp_err_t WebServerManager::apiBlacklistGetHandler(httpd_req_t *req) {
    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();
    trafficMonitor.getBlacklistJson(array);

    String response;
    serializeJson(doc, response);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, response.c_str(), response.length());
    return ESP_OK;
}

esp_err_t WebServerManager::apiBlacklistPostHandler(httpd_req_t *req) {
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf);
    if (err) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Gecersiz JSON");
        return ESP_FAIL;
    }

    const char* action = doc["action"];
    const char* domain = doc["domain"];

    if (!action || !domain) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Eksik parametre");
        return ESP_FAIL;
    }

    bool ok = false;
    if (strcmp(action, "add") == 0) {
        ok = trafficMonitor.addDomainToBlacklist(domain);
    } else if (strcmp(action, "remove") == 0) {
        ok = trafficMonitor.removeDomainFromBlacklist(domain);
    }

    httpd_resp_set_type(req, "application/json");
    if (ok) {
        httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Islem basarisiz");
    }
    return ESP_OK;
}

esp_err_t WebServerManager::apiBlockClientHandler(httpd_req_t *req) {
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    JsonDocument doc;
    deserializeJson(doc, buf);
    const char* mac = doc["mac"];
    bool block = doc["block"];

    if (!mac) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Eksik MAC");
        return ESP_FAIL;
    }

    bool success = trafficMonitor.toggleClientBlock(mac, block);
    httpd_resp_set_type(req, "application/json");
    if (success) {
        httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Cihaz bulunamadi");
    }
    return ESP_OK;
}

esp_err_t WebServerManager::apiWifiScanHandler(httpd_req_t *req) {
    int n = WiFi.scanComplete();
    if (n == -2) {
        WiFi.scanNetworks(true); // Asenkron tarama baslat
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"scanning\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    JsonDocument doc;
    JsonArray array = doc.to<JsonArray>();

    for (int i = 0; i < n; i++) {
        JsonObject net = array.add<JsonObject>();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
        net["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }

    WiFi.scanDelete();
    WiFi.scanNetworks(true); // Bir sonraki istek icin tekrar tara

    String response;
    serializeJson(doc, response);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response.c_str(), response.length());
    return ESP_OK;
}

esp_err_t WebServerManager::apiWifiConfigHandler(httpd_req_t *req) {
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    JsonDocument doc;
    deserializeJson(doc, buf);
    const char* ssid = doc["ssid"];
    const char* pass = doc["pass"];

    if (!ssid || strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID bos olamaz");
        return ESP_FAIL;
    }

    naptRouter.connectToRemoteAp(ssid, pass);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"connecting\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t WebServerManager::apiCustomSiteGetHandler(httpd_req_t *req) {
    JsonDocument doc;
    doc["domain"] = naptRouter.getCustomDomain();

    String html_content = "";
    if (LittleFS.exists("/site/index.html")) {
        File f = LittleFS.open("/site/index.html", "r");
        if (f) {
            html_content = f.readString();
            f.close();
        }
    }
    doc["html"] = html_content;

    String response;
    serializeJson(doc, response);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, response.c_str(), response.length());
    return ESP_OK;
}

esp_err_t WebServerManager::apiCustomSitePostHandler(httpd_req_t *req) {
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 16384) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Gecersiz veya asiri buyuk veri");
        return ESP_FAIL;
    }

    char* buf = (char*)malloc(total_len + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int cur = 0;
    while (cur < total_len) {
        int ret = httpd_req_recv(req, buf + cur, total_len - cur);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        cur += ret;
    }
    buf[total_len] = '\0';

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, buf);
    free(buf);

    if (err) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON formati gecersiz");
        return ESP_FAIL;
    }

    if (doc["domain"].is<const char*>()) {
        const char* new_domain = doc["domain"];
        if (new_domain && strlen(new_domain) > 2) {
            naptRouter.updateCustomDomain(new_domain);
        }
    }

    if (doc["html"].is<const char*>()) {
        const char* new_html = doc["html"];
        if (new_html) {
            File f = LittleFS.open("/site/index.html", "w");
            if (f) {
                f.print(new_html);
                f.close();
                Serial.println("[WEB] Yeni ozel site HTML kodu LittleFS'e yazildi.");
            }
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t WebServerManager::apiRebootHandler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"rebooting\"}", HTTPD_RESP_USE_STRLEN);

    delay(500);
    ESP.restart();
    return ESP_OK;
}
