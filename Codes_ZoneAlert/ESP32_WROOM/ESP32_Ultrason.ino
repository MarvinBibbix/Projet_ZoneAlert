#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>

// ---------- Capteurs / actionneurs ----------
#define TRIG_PIN   5
#define ECHO_PIN   18
#define SERVO_PIN  21   // ton choix, OK

// ---------- Réseau ----------
static const char* ssid     = "OnePlus10T";
static const char* password = "coucoutoi78";

// IP/tab5 cible (serveur de réception des mesures)
static const char* tab5IP   = "10.83.234.138";
static const int   tab5Port = 80;

// ---------- Mesures ----------
static const uint32_t PULSE_TIMEOUT_US   = 30000;
static const uint16_t SAMPLE_INTERVAL_MS = 70;
static const float    MIN_CM = 2.0f, MAX_CM = 400.0f;

// ---------- Balayage ----------
static const float START_ANGLE = 7.5f;
static const float END_ANGLE   = 172.5f;
static const float STEP_DEG    = 15.0f;

Servo servo;
float currentAngle = START_ANGLE;
int   dirSign      = +1; // +1 horaire, -1 anti-horaire

// ---------- Contrôle ON/OFF ----------
volatile bool systemOn = true;
static const uint32_t TOGGLE_GUARD_MS = 5000;
static uint32_t lastToggleMs = 0;

// ---------- Serveur de contrôle ----------
WebServer server(80);

// ---------- Debug / trace HTTP ----------
String g_lastBody;
int    g_lastHttpCode = 0;

// ---------- Utils ----------
float readDistanceOnce() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, PULSE_TIMEOUT_US);
  if (duration == 0) return NAN;
  float cm = duration * 0.034f / 2.0f;
  if (cm < MIN_CM || cm > MAX_CM) return NAN;
  return cm;
}

static inline int roundAngleToWrite(float a) {
  if (a < 0) a = 0; if (a > 180) a = 180;
  return (int)(a + 0.5f);
}

void gotoStartAngle() {
  currentAngle = START_ANGLE;
  dirSign = +1;
  servo.write(roundAngleToWrite(currentAngle));
  delay(300); // petite stabilisation
}

// ---------- HTTP handlers ----------
void handleControl() {
  String state = server.hasArg("state") ? server.arg("state") : "";
  uint32_t now = millis();
  if (now - lastToggleMs < TOGGLE_GUARD_MS) {
    server.send(429, "text/plain", "Too Many Requests");
    return;
  }

  if (state.equalsIgnoreCase("on")) {
    systemOn = true;
    gotoStartAngle();
    lastToggleMs = now;
    Serial.println("[CTRL] ON");
    server.send(200, "text/plain", "OK");
  } else if (state.equalsIgnoreCase("off")) {
    systemOn = false;  // on s'immobilise : plus de mesures ni de servo
    lastToggleMs = now;
    Serial.println("[CTRL] OFF");
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Bad state");
  }
}

void handleHealth() { server.send(200, "text/plain", "OK"); }

// Endpoint d'inspection: renvoie le dernier POST /data et l'état courant
void handleStatus() {
  String json = "{";
  json += "\"systemOn\":" + String(systemOn ? "true":"false");
  json += ",\"angle\":" + String((int)(currentAngle + 0.5f));
  json += ",\"direction\":\"" + String(dirSign>0? "forward":"back") + "\"";
  json += ",\"lastBody\":\"" + g_lastBody + "\"";
  json += ",\"lastCode\":" + String(g_lastHttpCode);
  json += "}";
  server.send(200, "application/json", json);
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // Servo
  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, 500, 2400);
  gotoStartAngle();

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.printf("\nESP32 IP: %s\n", WiFi.localIP().toString().c_str());

  // Serveur de contrôle
  server.on("/control", HTTP_GET, handleControl);
  server.on("/health",  HTTP_GET, handleHealth);
  server.on("/status",  HTTP_GET, handleStatus);
  server.begin();
}

void loop() {
  server.handleClient();

  if (!systemOn) {   // OFF : aucune acquisition ni rotation
    delay(50);
    return;
  }

  // 4 échantillons à l'angle courant (avec trace)
  float s[4];
  for (int i = 0; i < 4; ++i) {
    s[i] = readDistanceOnce();   // NaN si invalide
    delay(SAMPLE_INTERVAL_MS);
  }
  Serial.printf("[MEAS] angle=%3d  d=[%s,%s,%s,%s]\n",
                (int)(currentAngle + 0.5f),
                isnan(s[0])?"NaN":String(s[0],2).c_str(),
                isnan(s[1])?"NaN":String(s[1],2).c_str(),
                isnan(s[2])?"NaN":String(s[2],2).c_str(),
                isnan(s[3])?"NaN":String(s[3],2).c_str());

  // Envoi à la TAB5 (+ trace détaillée)
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.setConnectTimeout(4000);
    if (http.begin(tab5IP, tab5Port, "/data")) {
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      auto norm = [](float v)->String { return isnan(v) ? String("-1") : String(v, 2); };
      String body = "angle=" + String((int)(currentAngle + 0.5f)) +
                    "&d1=" + norm(s[0]) + "&d2=" + norm(s[1]) +
                    "&d3=" + norm(s[2]) + "&d4=" + norm(s[3]);

      Serial.printf("[TX] angle=%3d  body=\"%s\"\n",
                    (int)(currentAngle + 0.5f), body.c_str());

      int code = http.POST(body);
      g_lastBody = body;
      g_lastHttpCode = code;
      Serial.printf("[TX] /data -> %d\n", code);
      http.end();
    } else {
      Serial.println("[HTTP] begin(/data) a echoue");
    }
  }

  // Avance de 15° et rebond 7.5° ↔ 172.5°
  currentAngle += dirSign * STEP_DEG;
  if (currentAngle > END_ANGLE)  { currentAngle = END_ANGLE;  dirSign = -1; }
  if (currentAngle < START_ANGLE){ currentAngle = START_ANGLE; dirSign = +1; }
  servo.write(roundAngleToWrite(currentAngle));

  delay(120);
}
