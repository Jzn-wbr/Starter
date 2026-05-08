#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include "SparkFun_BMI270_Arduino_Library.h"

namespace
{
    namespace Pin
    {
        constexpr uint8_t IMU_SDA = 3;
        constexpr uint8_t IMU_SCL = 2;
        constexpr uint8_t BATTERY_ADC = 1;
        constexpr uint8_t VIBRATION_MOTOR = 0;
    }

    constexpr uint32_t SERIAL_BAUD = 115200;
    constexpr uint32_t SAMPLE_PERIOD_MS = 20;
    constexpr uint32_t BATTERY_REPORT_PERIOD_MS = 1000;

    constexpr char AP_SSID[] = "Bracelet-Test";
    constexpr char AP_PASSWORD[] = "bracelet123";
    constexpr byte DNS_PORT = 53;

    constexpr float BATTERY_DIVIDER_RATIO = 2.0F;

    struct MotionSample
    {
        float accelX = 0.0F;
        float accelY = 0.0F;
        float accelZ = 0.0F;
        float gyroX = 0.0F;
        float gyroY = 0.0F;
        float gyroZ = 0.0F;
        float batteryVoltage = 0.0F;
        bool imuReady = false;
        int8_t imuStatus = BMI2_E_NULL_PTR;
        uint32_t updatedAtMs = 0;
    };

    BMI270 imu;
    WebServer server(80);
    DNSServer dnsServer;
    MotionSample latestSample;
    uint32_t lastSampleMs = 0;
    uint32_t lastBatteryReportMs = 0;
    bool vibrationEnabled = false;

    const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="fr">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Bracelet Test</title>
  <style>
    :root {
      color-scheme: dark;
      font-family: Inter, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: #101418;
      color: #eef3f7;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      min-height: 100vh;
      background:
        radial-gradient(circle at top left, rgba(34, 197, 94, .16), transparent 34rem),
        linear-gradient(145deg, #101418 0%, #172027 58%, #11151a 100%);
    }
    main {
      width: min(980px, 100%);
      margin: 0 auto;
      padding: 18px;
    }
    header {
      display: flex;
      align-items: flex-end;
      justify-content: space-between;
      gap: 14px;
      margin-bottom: 16px;
    }
    h1 {
      margin: 0;
      font-size: 26px;
      font-weight: 760;
      letter-spacing: 0;
    }
    .subtitle {
      margin: 5px 0 0;
      color: #9dafbd;
      font-size: 14px;
    }
    .panel {
      border: 1px solid rgba(255,255,255,.09);
      background: rgba(18, 25, 31, .82);
      border-radius: 8px;
      box-shadow: 0 14px 45px rgba(0,0,0,.28);
    }
    .status-grid {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 10px;
      margin-bottom: 12px;
    }
    .metric {
      padding: 13px;
    }
    .label {
      color: #9dafbd;
      font-size: 12px;
      text-transform: uppercase;
      letter-spacing: .06em;
    }
    .value {
      margin-top: 7px;
      font-size: 24px;
      font-weight: 730;
      font-variant-numeric: tabular-nums;
    }
    .vibration-row {
      display: flex;
      align-items: center;
      gap: 10px;
      margin-top: 7px;
    }
    .led {
      width: 17px;
      height: 17px;
      border-radius: 50%;
      border: 1px solid rgba(255,255,255,.24);
      background: #36424c;
      box-shadow: inset 0 0 9px rgba(0,0,0,.45);
      flex: 0 0 auto;
    }
    .led.on {
      background: #22c55e;
      box-shadow: 0 0 18px rgba(34,197,94,.9), inset 0 0 6px rgba(255,255,255,.35);
    }
    button {
      border: 0;
      border-radius: 8px;
      padding: 12px 14px;
      background: #eef3f7;
      color: #11151a;
      font-weight: 760;
      font-size: 14px;
      min-width: 150px;
    }
    button.active {
      background: #ef4444;
      color: #fff;
    }
    .chart-panel {
      padding: 12px;
      margin-top: 12px;
    }
    .chart-title {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 10px;
      margin: 0 0 8px;
      color: #c7d2dc;
      font-size: 14px;
      font-weight: 700;
    }
    canvas {
      width: 100%;
      height: 245px;
      display: block;
      border-radius: 6px;
      background: #0c1014;
    }
    .legend {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      color: #9dafbd;
      font-size: 12px;
    }
    .chip {
      display: inline-flex;
      align-items: center;
      gap: 5px;
    }
    .swatch {
      width: 16px;
      height: 3px;
      border-radius: 999px;
    }
    @media (max-width: 680px) {
      main { padding: 13px; }
      header { align-items: flex-start; flex-direction: column; }
      .status-grid { grid-template-columns: 1fr; }
      button { width: 100%; }
      canvas { height: 210px; }
    }
  </style>
</head>
<body>
  <main>
    <header>
      <div>
        <h1>Bracelet Test</h1>
        <p class="subtitle">Connecte-toi au WiFi Bracelet-Test puis ouvre 192.168.4.1</p>
      </div>
      <button id="vibrationButton" type="button">Enable vibration</button>
    </header>

    <section class="status-grid">
      <div class="panel metric">
        <div class="label">Batterie</div>
        <div id="battery" class="value">--.-- V</div>
      </div>
      <div class="panel metric">
        <div class="label">BMI270</div>
        <div id="imuStatus" class="value">---</div>
      </div>
      <div class="panel metric">
        <div class="label">Vibration</div>
        <div class="vibration-row">
          <span id="vibrationLed" class="led"></span>
          <span id="vibrationText" class="value">Off</span>
        </div>
      </div>
    </section>

    <section class="panel chart-panel">
      <div class="chart-title">
        <span>Accel g</span>
        <span class="legend">
          <span class="chip"><span class="swatch" style="background:#38bdf8"></span>X</span>
          <span class="chip"><span class="swatch" style="background:#f97316"></span>Y</span>
          <span class="chip"><span class="swatch" style="background:#22c55e"></span>Z</span>
        </span>
      </div>
      <canvas id="accelChart"></canvas>
    </section>

    <section class="panel chart-panel">
      <div class="chart-title">
        <span>Gyro deg/s</span>
        <span class="legend">
          <span class="chip"><span class="swatch" style="background:#38bdf8"></span>X</span>
          <span class="chip"><span class="swatch" style="background:#f97316"></span>Y</span>
          <span class="chip"><span class="swatch" style="background:#22c55e"></span>Z</span>
        </span>
      </div>
      <canvas id="gyroChart"></canvas>
    </section>
  </main>

  <script>
    const maxPoints = 120;
    const accelHistory = [];
    const gyroHistory = [];
    const colors = ['#38bdf8', '#f97316', '#22c55e'];
    const accelCanvas = document.getElementById('accelChart');
    const gyroCanvas = document.getElementById('gyroChart');
    const batteryEl = document.getElementById('battery');
    const imuStatusEl = document.getElementById('imuStatus');
    const vibrationLed = document.getElementById('vibrationLed');
    const vibrationText = document.getElementById('vibrationText');
    const vibrationButton = document.getElementById('vibrationButton');

    function fitCanvas(canvas) {
      const dpr = window.devicePixelRatio || 1;
      const rect = canvas.getBoundingClientRect();
      const width = Math.max(1, Math.floor(rect.width * dpr));
      const height = Math.max(1, Math.floor(rect.height * dpr));
      if (canvas.width !== width || canvas.height !== height) {
        canvas.width = width;
        canvas.height = height;
      }
    }

    function pushPoint(history, point) {
      history.push(point);
      while (history.length > maxPoints) history.shift();
    }

    function drawChart(canvas, history, minValue, maxValue) {
      fitCanvas(canvas);
      const ctx = canvas.getContext('2d');
      const w = canvas.width;
      const h = canvas.height;
      ctx.clearRect(0, 0, w, h);

      ctx.strokeStyle = 'rgba(255,255,255,.08)';
      ctx.lineWidth = 1;
      for (let i = 1; i < 5; i++) {
        const y = (h * i) / 5;
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(w, y);
        ctx.stroke();
      }

      const zeroY = h - ((0 - minValue) / (maxValue - minValue)) * h;
      ctx.strokeStyle = 'rgba(255,255,255,.18)';
      ctx.beginPath();
      ctx.moveTo(0, zeroY);
      ctx.lineTo(w, zeroY);
      ctx.stroke();

      ['x', 'y', 'z'].forEach((key, seriesIndex) => {
        ctx.strokeStyle = colors[seriesIndex];
        ctx.lineWidth = 2;
        ctx.beginPath();
        history.forEach((point, index) => {
          const x = history.length <= 1 ? w : (index / (maxPoints - 1)) * w;
          const clamped = Math.max(minValue, Math.min(maxValue, point[key]));
          const y = h - ((clamped - minValue) / (maxValue - minValue)) * h;
          if (index === 0) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
        });
        ctx.stroke();
      });
    }

    function updateVibrationUi(enabled) {
      vibrationLed.classList.toggle('on', enabled);
      vibrationText.textContent = enabled ? 'On' : 'Off';
      vibrationButton.textContent = enabled ? 'Disable vibration' : 'Enable vibration';
      vibrationButton.classList.toggle('active', enabled);
    }

    async function poll() {
      try {
        const response = await fetch('/api/sample', { cache: 'no-store' });
        const data = await response.json();
        batteryEl.textContent = data.battery_v.toFixed(2) + ' V';
        imuStatusEl.textContent = data.imu_ready ? 'OK' : 'Fault';
        updateVibrationUi(data.vibration_enabled);
        pushPoint(accelHistory, { x: data.accel_x_g, y: data.accel_y_g, z: data.accel_z_g });
        pushPoint(gyroHistory, { x: data.gyro_x_dps, y: data.gyro_y_dps, z: data.gyro_z_dps });
        drawChart(accelCanvas, accelHistory, -2, 2);
        drawChart(gyroCanvas, gyroHistory, -500, 500);
      } catch (error) {
        imuStatusEl.textContent = 'Offline';
      }
    }

    vibrationButton.addEventListener('click', async () => {
      const nextState = !vibrationButton.classList.contains('active');
      await fetch('/api/vibration', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'enabled=' + (nextState ? '1' : '0')
      });
      updateVibrationUi(nextState);
    });

    window.addEventListener('resize', () => {
      drawChart(accelCanvas, accelHistory, -2, 2);
      drawChart(gyroCanvas, gyroHistory, -500, 500);
    });

    setInterval(poll, 100);
    poll();
  </script>
</body>
</html>
)HTML";

    bool beginBmi270()
    {
        const uint8_t addresses[] = {BMI2_I2C_PRIM_ADDR, BMI2_I2C_SEC_ADDR};

        for (uint8_t address : addresses)
        {
            if (imu.beginI2C(address, Wire) == BMI2_OK)
            {
                Serial.print("bmi270_address:0x");
                Serial.println(address, HEX);
                return true;
            }
        }

        Serial.println("bmi270_status:not_found");
        return false;
    }

    float readBatteryVoltage()
    {
        const uint32_t measuredMillivolts = analogReadMilliVolts(Pin::BATTERY_ADC);
        return (static_cast<float>(measuredMillivolts) * BATTERY_DIVIDER_RATIO) / 1000.0F;
    }

    void setVibrationEnabled(bool enabled)
    {
        vibrationEnabled = enabled;
        digitalWrite(Pin::VIBRATION_MOTOR, vibrationEnabled ? HIGH : LOW);
    }

    void updateMotionSample()
    {
        const uint32_t now = millis();
        if (now - lastSampleMs < SAMPLE_PERIOD_MS)
        {
            return;
        }

        lastSampleMs = now;
        latestSample.batteryVoltage = readBatteryVoltage();
        latestSample.updatedAtMs = now;

        if (!latestSample.imuReady)
        {
            latestSample.imuReady = beginBmi270();
            if (!latestSample.imuReady)
            {
                latestSample.imuStatus = BMI2_E_DEV_NOT_FOUND;
                return;
            }
        }

        latestSample.imuStatus = imu.getSensorData();
        if (latestSample.imuStatus != BMI2_OK)
        {
            latestSample.imuReady = false;
            return;
        }

        latestSample.accelX = imu.data.accelX;
        latestSample.accelY = imu.data.accelY;
        latestSample.accelZ = imu.data.accelZ;
        latestSample.gyroX = imu.data.gyroX;
        latestSample.gyroY = imu.data.gyroY;
        latestSample.gyroZ = imu.data.gyroZ;
    }

    void reportBatteryVoltage()
    {
        const uint32_t now = millis();
        if (now - lastBatteryReportMs < BATTERY_REPORT_PERIOD_MS)
        {
            return;
        }

        lastBatteryReportMs = now;
        Serial.print(">battery_v:");
        Serial.println(latestSample.batteryVoltage, 3);
    }

    void handleIndex()
    {
        server.send_P(200, "text/html", INDEX_HTML);
    }

    void handleSampleApi()
    {
        char json[384];
        snprintf(json, sizeof(json),
                 "{\"accel_x_g\":%.4f,\"accel_y_g\":%.4f,\"accel_z_g\":%.4f,"
                 "\"gyro_x_dps\":%.3f,\"gyro_y_dps\":%.3f,\"gyro_z_dps\":%.3f,"
                 "\"battery_v\":%.3f,\"vibration_enabled\":%s,\"imu_ready\":%s,"
                 "\"imu_status\":%d,\"updated_at_ms\":%lu}",
                 latestSample.accelX,
                 latestSample.accelY,
                 latestSample.accelZ,
                 latestSample.gyroX,
                 latestSample.gyroY,
                 latestSample.gyroZ,
                 latestSample.batteryVoltage,
                 vibrationEnabled ? "true" : "false",
                 latestSample.imuReady ? "true" : "false",
                 latestSample.imuStatus,
                 static_cast<unsigned long>(latestSample.updatedAtMs));

        server.send(200, "application/json", json);
    }

    void handleVibrationApi()
    {
        const bool enabled = server.hasArg("enabled") && server.arg("enabled") == "1";
        setVibrationEnabled(enabled);
        server.send(200, "application/json", enabled ? "{\"vibration_enabled\":true}" : "{\"vibration_enabled\":false}");
    }

    void handleNotFound()
    {
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "");
    }

    void beginDebugWifi()
    {
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASSWORD);
        const IPAddress apIp = WiFi.softAPIP();

        dnsServer.start(DNS_PORT, "*", apIp);
        server.on("/", HTTP_GET, handleIndex);
        server.on("/api/sample", HTTP_GET, handleSampleApi);
        server.on("/api/vibration", HTTP_POST, handleVibrationApi);
        server.onNotFound(handleNotFound);
        server.begin();

        Serial.print("wifi_ap_ssid:");
        Serial.println(AP_SSID);
        Serial.print("wifi_ap_password:");
        Serial.println(AP_PASSWORD);
        Serial.print("wifi_ap_ip:");
        Serial.println(apIp);
    }
}

void setup()
{
    Serial.begin(SERIAL_BAUD);
    delay(500);

    pinMode(Pin::VIBRATION_MOTOR, OUTPUT);
    setVibrationEnabled(false);
    analogReadResolution(12);
    analogSetPinAttenuation(Pin::BATTERY_ADC, ADC_11db);

    Wire.begin(Pin::IMU_SDA, Pin::IMU_SCL);
    Wire.setClock(400000);
    latestSample.imuReady = beginBmi270();

    beginDebugWifi();
}

void loop()
{
    dnsServer.processNextRequest();
    server.handleClient();
    updateMotionSample();
    reportBatteryVoltage();
}
