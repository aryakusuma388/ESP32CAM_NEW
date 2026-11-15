#include "esp_camera.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "esp_http_server.h"

// ==========================================
// 1. KREDENSIAL TETHERING OFFLINE (ACCESS POINT)
// ==========================================
const char *ap_ssid = "ESP32-CAM_Offline";
const char *ap_password = "12345678";    // Minimal 8 karakter
const char *mdns_hostname = "esp32cam";  // Akses via http://esp32cam.local

// Penyimpanan permanen NVS untuk Wi-Fi
Preferences preferences;
String saved_ssid = "";
String saved_pass = "";

// ==========================================
// 2. DEFINISI PIN STANDAR AI-THINKER ESP32-CAM
// ==========================================
#define FLASH_GPIO_NUM 4
#define STATUS_LED_GPIO 33  // Active LOW (LED merah kecil di belakang board)
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27

#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t camera_httpd = NULL;
bool flashState = false;

// Variabel timer non-blocking (millis)
unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 10000;

bool shouldRestart = false;
unsigned long restartTimer = 0;

// ==========================================
// 3. HALAMAN WEB ANTARMUKA (KAMERA + WIFI MANAGER)
// ==========================================
static const char *INDEX_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32-CAM Control & WiFi Setup</title>
  <style>
    body { font-family: sans-serif; text-align: center; background: #121212; color: #fff; margin: 0; padding: 15px; }
    img { width: 100%; max-width: 640px; height: auto; border-radius: 8px; border: 2px solid #333; background: #000; min-height: 220px; }
    .panel { max-width: 640px; margin: 15px auto; background: #1e1e1e; padding: 15px; border-radius: 8px; border: 1px solid #333; text-align: left; }
    .panel h4 { margin-top: 0; color: #2196F3; border-bottom: 1px solid #333; padding-bottom: 8px; }
    .row { display: flex; justify-content: space-between; align-items: center; margin: 12px 0; gap: 10px; }
    label { font-size: 14px; color: #ccc; min-width: 90px; }
    select, input[type=text], input[type=password], input[type=range] { 
      padding: 8px; border-radius: 5px; background: #2c2c2c; color: #fff; border: 1px solid #444; width: 100%; box-sizing: border-box; 
    }
    .btn-group { text-align: center; margin-bottom: 10px; }
    button { background: #2196F3; color: white; border: none; padding: 10px 16px; font-size: 14px; border-radius: 6px; cursor: pointer; margin: 4px; }
    button.active { background: #FFC107; color: #000; font-weight: bold; }
    button.green { background: #4CAF50; }
    button.gray { background: #555; }
    .status-box { background: #252525; padding: 10px; border-radius: 6px; font-size: 13px; margin-bottom: 12px; border-left: 4px solid #2196F3; }
  </style>
</head>
<body>
  <h3>ESP32-CAM Live & Wi-Fi Manager</h3>
  <img id="stream" src="/stream">
  
  <div class="panel">
    <h4>Kontrol Kamera</h4>
    <div class="btn-group">
      <button id="flashBtn" onclick="toggleFlash()">Lampu Flash: OFF</button>
      <button onclick="window.open('/capture', '_blank')">Ambil Foto</button>
    </div>

    <div class="row">
      <label for="res">Resolusi:</label>
      <select id="res" onchange="updateCam('framesize', this.value)">
        <option value="5">QVGA (320x240 - Paling Lancar)</option>
        <option value="8" selected>VGA (640x480 - Standar)</option>
        <option value="9">SVGA (800x600 - Tajam)</option>
        <option value="10">XGA (1024x768 - HD)</option>
      </select>
    </div>

    <div class="row">
      <label for="qual">Kompresi (<span id="qVal">12</span>):</label>
      <input type="range" id="qual" min="10" max="40" value="12" oninput="document.getElementById('qVal').innerText=this.value" onchange="updateCam('quality', this.value)">
    </div>
  </div>

  <div class="panel">
    <h4>Pengaturan Koneksi Wi-Fi (DHCP)</h4>
    <div class="status-box" id="wifiStatus">Memuat status jaringan...</div>

    <div class="row">
      <label for="ssidList">Pilih Wi-Fi:</label>
      <select id="ssidList" onchange="document.getElementById('ssidInput').value=this.value">
        <option value="">-- Klik Scan Wi-Fi --</option>
      </select>
      <button class="gray" style="width: auto; white-space: nowrap;" onclick="scanWiFi(this)">Scan Wi-Fi</button>
    </div>

    <div class="row">
      <label for="ssidInput">Nama Wi-Fi:</label>
      <input type="text" id="ssidInput" placeholder="Masukkan SSID Wi-Fi">
    </div>

    <div class="row">
      <label for="passInput">Password:</label>
      <input type="password" id="passInput" placeholder="Masukkan Password Wi-Fi">
    </div>

    <div class="btn-group" style="margin-top: 15px;">
      <button class="green" onclick="saveWiFi()">Simpan & Hubungkan Wi-Fi</button>
    </div>
  </div>

  <script>
    window.onload = function() {
      loadStatus();
    };

    function loadStatus() {
      fetch('/status').then(r => r.json()).then(data => {
        document.getElementById('ssidInput').value = data.saved_ssid;
        let info = "<b>Mode Offline (AP):</b> http://" + data.ap_ip + "<br>";
        if (data.connected) {
          info += "<b>Status Wi-Fi Utama:</b> <span style='color:#4CAF50'>Terhubung (" + data.sta_ssid + ")</span><br>";
          info += "<b>IP Dinamis (DHCP):</b> <a href='http://" + data.sta_ip + "' style='color:#64B5F6'>http://" + data.sta_ip + "</a>";
        } else {
          info += "<b>Status Wi-Fi Utama:</b> <span style='color:#FF9800'>Tidak Terhubung (Mode Offline)</span>";
        }
        document.getElementById('wifiStatus').innerHTML = info;
      });
    }

    function scanWiFi(btn) {
      btn.innerText = "Memindai...";
      btn.disabled = true;
      fetch('/scan').then(r => r.text()).then(rawText => {
        const select = document.getElementById('ssidList');
        select.innerHTML = "<option value=''>-- Pilih Jaringan --</option>";
        const lines = rawText.trim().split('\n');
        lines.forEach(line => {
          if (line.length > 0) {
            const parts = line.split('|');
            const opt = document.createElement('option');
            opt.value = parts[0];
            opt.innerText = parts[0] + " (" + parts[1] + " dBm)";
            select.appendChild(opt);
          }
        });
        btn.innerText = "Scan Wi-Fi";
        btn.disabled = false;
      }).catch(() => {
        btn.innerText = "Scan Wi-Fi";
        btn.disabled = false;
      });
    }

    function saveWiFi() {
      const ssid = encodeURIComponent(document.getElementById('ssidInput').value);
      const pass = encodeURIComponent(document.getElementById('passInput').value);
      if (!ssid) {
        alert("Nama Wi-Fi (SSID) tidak boleh kosong!");
        return;
      }
      document.getElementById('wifiStatus').innerHTML = "<span style='color:#FFC107'>Menyimpan pengaturan & me-restart ESP32-CAM... Tunggu 5 detik lalu muat ulang halaman.</span>";
      fetch('/savewifi?ssid=' + ssid + '&pass=' + pass);
    }

    function toggleFlash() {
      const img = document.getElementById('stream');
      img.src = "";
      fetch('/flash').then(r => r.text()).then(state => {
        const btn = document.getElementById('flashBtn');
        btn.innerText = "Lampu Flash: " + state;
        btn.className = (state === "ON") ? "active" : "";
        img.src = "/stream?t=" + new Date().getTime();
      });
    }

    function updateCam(param, val) {
      const img = document.getElementById('stream');
      img.src = "";
      fetch('/control?var=' + param + '&val=' + val).then(() => {
        img.src = "/stream?t=" + new Date().getTime();
      });
    }
  </script>
</body>
</html>
)rawliteral";

// ==========================================
// 4. FUNGSI DECODE URL PARAMETER (POINTER AMAN)
// ==========================================
void urlDecode(char *dst, const char *src) {
  char a, b;
  while (*src) {
    if ((*src == '%') && ((a = *(src + 1)) && (b = *(src + 2))) && (isxdigit(a) && isxdigit(b))) {
      if (a >= 'a') a -= 'a' - 'A';
      if (a >= 'A') a -= ('A' - 10);
      else a -= '0';
      if (b >= 'a') b -= 'a' - 'A';
      if (b >= 'A') b -= ('A' - 10);
      else b -= '0';
      *dst++ = 16 * a + b;
      src += 3;
    } else if (*src == '+') {
      *dst++ = ' ';
      src++;
    } else {
      *dst++ = *src++;
    }
  }
  *dst++ = '\0';
}

// ==========================================
// 5. HTTP HANDLERS WEB SERVER
// ==========================================
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

static esp_err_t status_handler(httpd_req_t *req) {
  String json = "{";
  json += "\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"sta_ssid\":\"" + WiFi.SSID() + "\",";
  json += "\"sta_ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\",";
  json += "\"saved_ssid\":\"" + saved_ssid + "\"";
  json += "}";
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json.c_str(), json.length());
}

static esp_err_t scan_handler(httpd_req_t *req) {
  int n = WiFi.scanNetworks();
  String result = "";
  for (int i = 0; i < n; ++i) {
    result += WiFi.SSID(i) + "|" + String(WiFi.RSSI(i)) + "\n";
  }
  WiFi.scanDelete();
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, result.c_str(), result.length());
}
static esp_err_t savewifi_handler(httpd_req_t *req) {
  size_t buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    char *buf = (char *)malloc(buf_len);
    char *raw_ssid = (char *)calloc(64, sizeof(char));
    char *raw_pass = (char *)calloc(64, sizeof(char));
    char *dec_ssid = (char *)calloc(64, sizeof(char));
    char *dec_pass = (char *)calloc(64, sizeof(char));
    if (buf && raw_ssid && raw_pass && dec_ssid && dec_pass) {
      if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
        if (httpd_query_key_value(buf, "ssid", raw_ssid, 64) == ESP_OK) {
          urlDecode(dec_ssid, raw_ssid);
          if (httpd_query_key_value(buf, "pass", raw_pass, 64) == ESP_OK) {
            urlDecode(dec_pass, raw_pass);
          }
          preferences.begin("wifi-cfg", false);
          preferences.putString("ssid", String(dec_ssid));
          preferences.putString("pass", String(dec_pass));
          preferences.end();
          Serial.printf("[WiFi Manager] Kredensial baru disimpan: %s\n", dec_ssid);
          shouldRestart = true;
          restartTimer = millis();
        }
      }
    }
    free(buf);
    free(raw_ssid);
    free(raw_pass);
    free(dec_ssid);
    free(dec_pass);
  }
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "SAVED", 5);
}
static esp_err_t flash_handler(httpd_req_t *req) {
  flashState = !flashState;
  digitalWrite(FLASH_GPIO_NUM, flashState ? HIGH : LOW);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, flashState ? "ON" : "OFF", HTTPD_RESP_USE_STRLEN);
}
static esp_err_t control_handler(httpd_req_t *req) {
  size_t buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    char *buf = (char *)malloc(buf_len);
    char *variable = (char *)calloc(32, sizeof(char));
    char *value = (char *)calloc(32, sizeof(char));
    if (buf != nullptr && variable != nullptr && value != nullptr) {
      if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
        if (httpd_query_key_value(buf, "var", variable, 32) == ESP_OK && httpd_query_key_value(buf, "val", value, 32) == ESP_OK) {
          int val = atoi(value);
          sensor_t *s = esp_camera_sensor_get();
          if (s != nullptr) {
            if (strcmp(variable, "framesize") == 0) {
              if (s->pixformat == PIXFORMAT_JPEG) s->set_framesize(s, (framesize_t)val);
            } else if (strcmp(variable, "quality") == 0) {
              s->set_quality(s, val);
            }
          }
        }
      }
    }
    free(buf);
    free(variable);
    free(value);
  }
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "OK", 2);
}
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=foto.jpg");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char *part_buf = (char *)malloc(64);
  if (!part_buf) return ESP_FAIL;
  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    free(part_buf);
    return res;
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
    } else {
      size_t hlen = snprintf(part_buf, 64, _STREAM_PART, fb->len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
      if (res == ESP_OK) {
        res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
      }
      if (res == ESP_OK) {
        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
      }
      esp_camera_fb_return(fb);
    }
    if (res != ESP_OK) break;
  }
  free(part_buf);
  return res;
}
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 10;
  httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
  httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
  httpd_uri_t flash_uri = { .uri = "/flash", .method = HTTP_GET, .handler = flash_handler, .user_ctx = NULL };
  httpd_uri_t control_uri = { .uri = "/control", .method = HTTP_GET, .handler = control_handler, .user_ctx = NULL };
  httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
  httpd_uri_t status_uri = { .uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };
  httpd_uri_t scan_uri = { .uri = "/scan", .method = HTTP_GET, .handler = scan_handler, .user_ctx = NULL };
  httpd_uri_t savewifi_uri = { .uri = "/savewifi", .method = HTTP_GET, .handler = savewifi_handler, .user_ctx = NULL };
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &stream_uri);
    httpd_register_uri_handler(camera_httpd, &flash_uri);
    httpd_register_uri_handler(camera_httpd, &control_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &scan_uri);
    httpd_register_uri_handler(camera_httpd, &savewifi_uri);
  }
}
// ==========================================
// 6. SETUP & LOOP UTAMA
// ==========================================
void setup() {
  Serial.begin(115200);
  unsigned long bootStart = millis();
  while (millis() - bootStart < 1000) {
    yield();
  }
  Serial.println("\n=== Sistem ESP32-CAM + Web Wi-Fi Manager ===");
  pinMode(FLASH_GPIO_NUM, OUTPUT);
  digitalWrite(FLASH_GPIO_NUM, LOW);
  pinMode(STATUS_LED_GPIO, OUTPUT);
  digitalWrite(STATUS_LED_GPIO, HIGH);  // Mati (Active LOW)
  // Baca kredensial Wi-Fi yang tersimpan di memori flash NVS
  preferences.begin("wifi-cfg", true);
  saved_ssid = preferences.getString("ssid", "");
  saved_pass = preferences.getString("pass", "");
  preferences.end();
  camera_config_t config;
  memset(&config, 0, sizeof(config));
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_XGA;
  config.jpeg_quality = 12;
  if (psramFound()) {
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] Kamera gagal init! Kode error: 0x%x\n", err);
    unsigned long prevBlink = 0;
    bool ledState = false;
    while (true) {
      if (millis() - prevBlink >= 150) {
        prevBlink = millis();
        ledState = !ledState;
        digitalWrite(STATUS_LED_GPIO, ledState ? LOW : HIGH);
      }
      yield();
    }
  }
  sensor_t *s = esp_camera_sensor_get();
  if (s != nullptr) {
    s->set_framesize(s, FRAMESIZE_VGA);
  }
  // Aktifkan Mode Hybrid: Access Point (Tethering) + Station (Wi-Fi Utama)
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  // Jalankan Hotspot Tethering Offline
  WiFi.softAP(ap_ssid, ap_password);
  Serial.println("[Tethering Offline] Aktif! Wi-Fi: " + String(ap_ssid));
  Serial.println("[Tethering Offline] Buka web: http://" + WiFi.softAPIP().toString());
  // Jika sudah ada Wi-Fi yang pernah disimpan lewat Web, coba hubungkan
  if (saved_ssid.length() > 0) {
    Serial.print("[WiFi Utama] Menghubungkan ke '" + saved_ssid + "'");
    WiFi.begin(saved_ssid.c_str(), saved_pass.c_str());
    unsigned long wifiStartAttempt = millis();
    unsigned long prevDotMillis = 0;
    bool ledState = false;
    while (WiFi.status() != WL_CONNECTED && (millis() - wifiStartAttempt < 8000)) {
      if (millis() - prevDotMillis >= 250) {
        prevDotMillis = millis();
        ledState = !ledState;
        digitalWrite(STATUS_LED_GPIO, ledState ? LOW : HIGH);
        Serial.print(".");
      }
      yield();
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n[WiFi Utama] Terhubung! IP Dinamis: http://" + WiFi.localIP().toString());
    } else {
      Serial.println("\n[WiFi Utama] Gagal terhubung, tetap berjalan di Mode Tethering Offline.");
    }
  } else {
    Serial.println("[WiFi Utama] Belum diatur. Silakan atur lewat halaman web tethering.");
  }
  if (MDNS.begin(mdns_hostname)) {
    Serial.printf("[mDNS] Aktif: http://%s.local\n", mdns_hostname);
  }
  startCameraServer();
  digitalWrite(STATUS_LED_GPIO, LOW);  // LED merah menyala solid tanda siap
}
void loop() {
  unsigned long currentMillis = millis();
  // Eksekusi restart non-blocking 1.5 detik setelah menyimpan Wi-Fi baru
  if (shouldRestart && (currentMillis - restartTimer >= 1500)) {
    ESP.restart();
  }
  // Pantau koneksi Wi-Fi utama secara non-blocking jika kredensial tersimpan
  if (saved_ssid.length() > 0 && (currentMillis - lastWiFiCheck >= wifiCheckInterval)) {
    lastWiFiCheck = currentMillis;
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    }
  }
}