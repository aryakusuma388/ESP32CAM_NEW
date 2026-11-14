#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"

// 1. KREDENSIAL WI-FI ANDA
const char *ssid = "test";
const char *password = "11112222";

// 2. PENGATURAN IP STATIS
IPAddress local_IP(10, 96, 102, 184);
IPAddress gateway(10, 96, 102, 46);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

// 3. DEFINISI PIN STANDAR AI-THINKER ESP32-CAM
#define FLASH_GPIO_NUM 4
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
unsigned long lastWiFiCheck = 0;

// Halaman Web Antarmuka dengan Kontrol Resolusi & Kualitas
static const char *INDEX_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32-CAM Control Panel</title>
  <style>
    body { font-family: sans-serif; text-align: center; background: #121212; color: #fff; margin: 0; padding: 15px; }
    img { width: 100%; max-width: 640px; height: auto; border-radius: 8px; border: 2px solid #333; background: #000; min-height: 240px; }
    .panel { max-width: 640px; margin: 15px auto; background: #1e1e1e; padding: 15px; border-radius: 8px; border: 1px solid #333; }
    .row { display: flex; justify-content: space-between; align-items: center; margin: 12px 0; gap: 10px; }
    label { font-size: 14px; color: #ccc; }
    select, input[type=range] { padding: 8px; border-radius: 5px; background: #2c2c2c; color: #fff; border: 1px solid #444; width: 60%; }
    button { background: #2196F3; color: white; border: none; padding: 12px 18px; font-size: 15px; border-radius: 6px; cursor: pointer; margin: 5px; }
    button.active { background: #FFC107; color: #000; font-weight: bold; }
  </style>
</head>
<body>
  <h3>ESP32-CAM Live Video</h3>
  <img id="stream" src="/stream">
  
  <div class="panel">
    <div>
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

  <script>
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

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

static esp_err_t flash_handler(httpd_req_t *req) {
  flashState = !flashState;
  digitalWrite(FLASH_GPIO_NUM, flashState ? HIGH : LOW);
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, flashState ? "ON" : "OFF", HTTPD_RESP_USE_STRLEN);
}

// Handler Pengaturan Resolusi & Kualitas Kamera secara Real-Time
static esp_err_t control_handler(httpd_req_t *req) {
  char *buf;
  size_t buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char *)malloc(buf_len);
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      char variable[32];
      char value[32];
      if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) == ESP_OK && httpd_query_key_value(buf, "val", value, sizeof(value)) == ESP_OK) {
        int val = atoi(value);
        sensor_t *s = esp_camera_sensor_get();
        if (strcmp(variable, "framesize") == 0) {
          if (s->pixformat == PIXFORMAT_JPEG) s->set_framesize(s, (framesize_t)val);
        } else if (strcmp(variable, "quality") == 0) {
          s->set_quality(s, val);
        }
      }
    }
    free(buf);
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
      Serial.println("Gagal mengambil frame kamera");
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
  config.max_uri_handlers = 8;

  httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
  httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
  httpd_uri_t flash_uri = { .uri = "/flash", .method = HTTP_GET, .handler = flash_handler, .user_ctx = NULL };
  httpd_uri_t control_uri = { .uri = "/control", .method = HTTP_GET, .handler = control_handler, .user_ctx = NULL };
  httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &stream_uri);
    httpd_register_uri_handler(camera_httpd, &flash_uri);
    httpd_register_uri_handler(camera_httpd, &control_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
  }
}

void setup() {
  Serial.begin(115200);

  unsigned long bootStart = millis();
  while (millis() - bootStart < 1500) {
    yield();
  }
  Serial.println("\n Sistem mulai booting...");

  pinMode(FLASH_GPIO_NUM, OUTPUT);
  digitalWrite(FLASH_GPIO_NUM, LOW);

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

  // Dialokasikan hingga XGA agar saat resolusi dinaikkan di web tidak perlu restart
  config.frame_size = FRAMESIZE_XGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  Serial.println(" Menginisialisasi sensor kamera...");
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] Kamera gagal init! Kode error: 0x%x\n", err);
    return;
  }

  // Setel resolusi awal ke VGA (640x480) agar langsung lancar saat pertama dibuka
  sensor_t *s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_VGA);
  Serial.println(" Kamera berhasil aktif!");

  WiFi.mode(WIFI_STA);
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("[ERROR] Gagal mengatur IP Statis!");
  }

  WiFi.setSleep(false);
  WiFi.begin(ssid, password);

  Serial.print(" Menghubungkan ke Wi-Fi");
  unsigned long prevDotMillis = 0;
  while (WiFi.status() != WL_CONNECTED) {
    unsigned long currentMillis = millis();
    if (currentMillis - prevDotMillis >= 500) {
      prevDotMillis = currentMillis;
      Serial.print(".");
    }
    yield();
  }
  Serial.println("\n Wi-Fi Terhubung!");
  startCameraServer();
  Serial.print(" Buka halaman kontrol & video: http://");
  Serial.println(WiFi.localIP());
}
void loop() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastWiFiCheck >= 10000) {
    lastWiFiCheck = currentMillis;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Wi-Fi terputus, menyambungkan ulang...");
      WiFi.reconnect();
    }
  }
}