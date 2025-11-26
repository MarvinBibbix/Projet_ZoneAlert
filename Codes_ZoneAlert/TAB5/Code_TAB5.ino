#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <algorithm>
#include <vector>
#include <time.h>

// -------------------------------------------------------------------
//  Structure utilitaire pour gérer les zones tactiles (UI)
// -------------------------------------------------------------------
struct Rect { int x, y, w, h; };
static inline bool inRect(const Rect& r, int tx, int ty) {
  return (tx >= r.x && tx <= r.x + r.w && ty >= r.y && ty <= r.y + r.h);
}

// -------------------------------------------------------------------
//  Configuration Wi-Fi
// -------------------------------------------------------------------
static const char* WIFI_SSID     = "OnePlus10T";
static const char* WIFI_PASSWORD = "coucoutoi78";

// IP statique TAB5 
IPAddress local_IP(10,83,234,138);
IPAddress gateway(10,83,234,1);
IPAddress subnet(255,255,255,0);
IPAddress dns(10,83,234,1);

// -------------------------------------------------------------------
//  Définition SDIO interne (liaison ESP32-P4 ↔ ESP32-C6 de la TAB5)
// -------------------------------------------------------------------
#ifndef BOARD_SDIO_ESP_HOSTED_CLK
  #define SDIO2_CLK GPIO_NUM_12
  #define SDIO2_CMD GPIO_NUM_13
  #define SDIO2_D0  GPIO_NUM_11
  #define SDIO2_D1  GPIO_NUM_10
  #define SDIO2_D2  GPIO_NUM_9
  #define SDIO2_D3  GPIO_NUM_8
  #define SDIO2_RST GPIO_NUM_15
#endif

// Serveur HTTP interne de la TAB5
WebServer server(80);

// -------------------------------------------------------------------
//  Variables runtime : dernières mesures reçues, angle, IP ESP32, etc.
// -------------------------------------------------------------------
static float lastMedian = NAN;
static float lastD[4]   = {NAN, NAN, NAN, NAN};
static int   lastAngle  = 0;
static IPAddress lastEsp32IP(0,0,0,0);  // Apprise automatiquement lors du POST /data

// -------------------------------------------------------------------
//  Stockage permanent (via Preferences)
// -------------------------------------------------------------------
Preferences prefs;
static const char* PREF_NS       = "ZoneAlert";
static const char* KEY_SYS_ON    = "sys_on";
static const char* KEY_ALERT_CM  = "alert_cm";
static const char* KEY_ALARM_POW = "alarm_pow";

static bool systemOn = true;     // état ON/OFF du système

// Seuils disponibles (boutons UI)
static const int THRESH_BTNS[4] = {10, 100, 200, 300};
static int alertThresholdCm = 110;

// Niveaux de puissance du buzzer
static const int POWER_BTNS[4] = {50, 150, 200, 250};
static int alarmPower = 150;

// -------------------------------------------------------------------
//  Buzzer + Pushover (alertes distantes)
// -------------------------------------------------------------------
static const uint16_t BEEP_FREQ_HZ = 1800;
static const uint16_t BEEP_DUR_MS  = 110;
static const uint32_t BEEP_COOLDOWN_MS = 800;
static uint32_t lastBeepMs = 0;

// Identifiants Pushover (notification HTTPS)
static const char* PUSHOVER_USER_KEY  = "ub2shp8g37wodo23g7oaz59hpevjio";
static const char* PUSHOVER_APP_TOKEN = "agp2bky66fbap5q46uqkvig69xhjw2";
static const uint32_t PUSH_COOLDOWN_MS = 60000;
static uint32_t lastPushMs = 0;

// Anti-rebonds UI (tactile + bouton système)
static const uint32_t UI_DEBOUNCE_MS = 2000;
static uint32_t lastThreshChangeMs = 0;
static uint32_t lastPowerChangeMs  = 0;

static const uint32_t SYS_TOGGLE_GUARD_MS = 5000;
static uint32_t lastSysToggleMs = 0;

// -------------------------------------------------------------------
//  Layout graphique (disposition des panneaux et boutons)
// -------------------------------------------------------------------
static Rect R_TOP, R_BTN, R_CLOCK, R_NET, R_STATUS, R_BTN_ALERTS, R_BTN_POWER;
static Rect R_ALERT_B[4], R_POWER_B[4];

static void computeLayout() {
  int W = M5.Display.width();
  int H = M5.Display.height();

  int margin = W * 0.025;
  int pad    = W * 0.018;
  int topH   = H * 0.14;
  int rowH   = (H - topH - margin*3) / 3;

  R_TOP = { margin, margin/2, W - 2*margin, topH };
  R_BTN   = { R_TOP.x + pad, R_TOP.y + pad,
              (R_TOP.w - 3*pad)/2, R_TOP.h - 2*pad };
  R_CLOCK = { R_BTN.x + R_BTN.w + pad, R_TOP.y + pad,
              (R_TOP.w - 3*pad)/2, R_TOP.h - 2*pad };

  R_NET       = { margin, R_TOP.y + R_TOP.h + margin, W - 2*margin, rowH };
  R_STATUS    = { margin, R_NET.y + R_NET.h + margin, W - 2*margin, rowH };
  R_BTN_ALERTS= { margin, R_STATUS.y + R_STATUS.h + margin,
                  W - 2*margin, rowH/2 - margin/2 };
  R_BTN_POWER = { margin, R_BTN_ALERTS.y + R_BTN_ALERTS.h + margin,
                  W - 2*margin, rowH/2 - margin/2 };

  int gap = 12;
  int bw = (R_BTN_ALERTS.w - 3*gap) / 4;

  for (int i=0;i<4;i++) {
    int x = R_BTN_ALERTS.x + i * (bw + gap);
    R_ALERT_B[i] = {x, R_BTN_ALERTS.y, bw, R_BTN_ALERTS.h};
    R_POWER_B[i] = {x, R_BTN_POWER.y, bw, R_BTN_POWER.h};
  }
}

// -------------------------------------------------------------------
//  Fonctions utilitaires : médiane, validation mesures, gestion prefs
// -------------------------------------------------------------------
static inline bool isValidCm(float v){
  return !isnan(v) && v>=2.0f && v<=400.0f;
}

static float medianOfValid(const float* arr,int n){
  float tmp[4]; int m=0;
  for(int i=0;i<n;i++) if(isValidCm(arr[i])) tmp[m++]=arr[i];
  if(m==0) return NAN;
  std::sort(tmp,tmp+m);
  return (m&1)? tmp[m/2] : 0.5f*(tmp[m/2]+tmp[m/2-1]);
}

static void prefsLoad(){
  prefs.begin(PREF_NS,true);
  systemOn        = prefs.getBool(KEY_SYS_ON,true);
  alertThresholdCm= prefs.getInt(KEY_ALERT_CM,alertThresholdCm);
  alarmPower      = prefs.getInt(KEY_ALARM_POW,alarmPower);
  prefs.end();
}

static void prefsSave(){
  prefs.begin(PREF_NS,false);
  prefs.putBool(KEY_SYS_ON,systemOn);
  prefs.putInt(KEY_ALERT_CM,alertThresholdCm);
  prefs.putInt(KEY_ALARM_POW,alarmPower);
  prefs.end();
}

// -------------------------------------------------------------------
//  Notification Pushover HTTPS (client.setInsecure())
// -------------------------------------------------------------------
static bool sendPushover(const String& title,const String& message){
  if(strlen(PUSHOVER_USER_KEY)==0 || strlen(PUSHOVER_APP_TOKEN)==0)
    return false;

  uint32_t now=millis();
  if(now-lastPushMs<PUSH_COOLDOWN_MS)
    return false;

  WiFiClientSecure client;
  client.setInsecure();   // On ignore la validation du certificat (nécessaire ici)

  HTTPClient http;
  if(!http.begin(client,"https://api.pushover.net/1/messages.json"))
    return false;

  http.addHeader("Content-Type","application/x-www-form-urlencoded");

  String body = "token="+String(PUSHOVER_APP_TOKEN)
              +"&user="+String(PUSHOVER_USER_KEY)
              +"&title="+title
              +"&message="+message;

  int code=http.POST(body);
  http.end();

  if(code==200){
    lastPushMs=now;
    return true;
  }

  return false;
}

// -------------------------------------------------------------------
//  Dessins UI (panneaux, boutons, header…)
// -------------------------------------------------------------------
static void drawBox(const Rect& r,uint16_t border,uint16_t fill){
  M5.Display.fillRect(r.x,r.y,r.w,r.h,fill);
  M5.Display.drawRect(r.x,r.y,r.w,r.h,border);
}

static void drawWifiIcon(int cx,int cy,int radius,uint16_t color){
  M5.Display.drawArc(cx,cy,radius,radius-2,200,340,color);
  M5.Display.drawArc(cx,cy,radius-5,radius-7,200,340,color);
  M5.Display.drawArc(cx,cy,radius-10,radius-12,200,340,color);
  M5.Display.fillCircle(cx,cy,3,color);
}

static String nowHM(){
  struct tm ti;
  if(!getLocalTime(&ti)) return "--:--:--";
  char b[12];
  strftime(b,sizeof(b),"%H:%M:%S",&ti);
  return b;
}

static String nowDate(){
  struct tm ti;
  if(!getLocalTime(&ti)) return "--/--/----";
  char b[16];
  strftime(b,sizeof(b),"%d/%m/%Y",&ti);
  return b;
}

// UI : en-tête (ON/OFF + horloge)
static void drawHeader(bool full){
  if(full) drawBox(R_TOP,DARKGREY,BLACK);

  uint16_t col = systemOn ? GREEN : RED;
  drawBox(R_BTN,col,BLACK);

  M5.Display.setTextColor(WHITE,BLACK);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(R_BTN.x+10,R_BTN.y+10);
  M5.Display.print("SYSTEME");

  M5.Display.setTextSize(4);
  const char* stateStr = systemOn ? "ON" : "OFF";
  M5.Display.setCursor(R_BTN.x+30,R_BTN.y+70);
  M5.Display.print(stateStr);

  drawBox(R_CLOCK,DARKGREY,BLACK);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(R_CLOCK.x+10,R_CLOCK.y+20);
  M5.Display.print(nowHM());
  M5.Display.setCursor(R_CLOCK.x+10,R_CLOCK.y+60);
  M5.Display.print(nowDate());
}

// UI : panneau réseau
static void drawNetPanel(){
  drawBox(R_NET,DARKGREY,BLACK);
  drawWifiIcon(R_NET.x+36,R_NET.y+28,18,WHITE);

  M5.Display.setTextColor(WHITE,BLACK);
  M5.Display.setTextSize(2);

  int x=R_NET.x+16,y=R_NET.y+60,lh=28;
  M5.Display.setCursor(x,y); M5.Display.printf("SSID : %s",WIFI_SSID); y+=lh;
  M5.Display.setCursor(x,y); M5.Display.printf("TAB5 : %s",WiFi.localIP().toString().c_str()); y+=lh;

  M5.Display.setCursor(x,y);
  if(lastEsp32IP[0]==0) M5.Display.print("ESP32 : en attente...");
  else M5.Display.printf("ESP32 : %s",lastEsp32IP.toString().c_str());
}

// UI : panneau d'état
static void drawStatusPanel(){
  drawBox(R_STATUS,DARKGREY,BLACK);

  M5.Display.setTextColor(WHITE,BLACK);
  M5.Display.setTextSize(3);

  int x=R_STATUS.x+16, y=R_STATUS.y+12, lh=30;
  M5.Display.setCursor(x,y); M5.Display.printf("Angle : %3d deg",lastAngle); y+=lh;

  if(isValidCm(lastMedian)){
    bool alert = systemOn && lastMedian<=alertThresholdCm;
    M5.Display.setTextColor(alert?RED:GREEN,BLACK);
    M5.Display.setCursor(x,y);
    M5.Display.printf("Mediane : %.2f cm",lastMedian);
    M5.Display.setTextColor(WHITE,BLACK);
  } else {
    M5.Display.setCursor(x,y); M5.Display.print("Mediane : --");
  }
  y+=lh;

  M5.Display.setCursor(x,y); M5.Display.printf("Seuil : %d cm",alertThresholdCm); y+=lh;
  M5.Display.setCursor(x,y); M5.Display.printf("Buzzer : %d",alarmPower);
}

// UI : boutons seuils et puissance
static void drawButton(const Rect& r,const String& label,bool sel){
  drawBox(r, sel?CYAN:DARKGREY, sel?TFT_DARKCYAN:BLACK);
  M5.Display.setTextColor(WHITE,BLACK);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(r.x+10,r.y+r.h/2-10);
  M5.Display.print(label);
}

static void drawButtons(){
  String la[4]={"10 cm","1 m","2 m","3 m"};
  for(int i=0;i<4;i++)
    drawButton(R_ALERT_B[i],la[i],alertThresholdCm==THRESH_BTNS[i]);

  String lp[4]={"50","150","200","250"};
  for(int i=0;i<4;i++)
    drawButton(R_POWER_B[i],lp[i],alarmPower==POWER_BTNS[i]);
}

// Rafraîchissement global
static void drawUI(bool full=true){
  if(full) M5.Display.clear();
  drawHeader(true);
  drawNetPanel();
  drawStatusPanel();
  drawButtons();
}

// -------------------------------------------------------------------
//  Déclenchement d’une alerte locale + distante (si seuil dépassé)
// -------------------------------------------------------------------
static void maybeBeepAndNotify(){
  if(!systemOn) return;
  if(!isValidCm(lastMedian)) return;
  if(lastMedian>alertThresholdCm) return;

  uint32_t now=millis();
  if(now-lastBeepMs>=BEEP_COOLDOWN_MS){
    M5.Speaker.setVolume(alarmPower);
    M5.Speaker.tone(BEEP_FREQ_HZ,BEEP_DUR_MS);
    lastBeepMs=now;
  }

  sendPushover("ZoneAlert - Alerte",
               "Distance <= "+String(alertThresholdCm)+" cm ; angle "+String(lastAngle));
}

// -------------------------------------------------------------------
//  HTTP : /health, /data, /last
// -------------------------------------------------------------------

// Simple vérification réseau
void handleHealth(){ server.send(200,"text/plain","OK"); }

// Réception des mesures envoyées par l’ESP32
void handleData(){
  lastEsp32IP = server.client().remoteIP();

  bool updated=false;

  if(server.hasArg("d1") && server.hasArg("d2") &&
     server.hasArg("d3") && server.hasArg("d4"))
  {
    auto parse=[](const String& s)->float{ return (s=="-1")?NAN:s.toFloat(); };

    lastAngle = server.hasArg("angle") ? server.arg("angle").toInt() : 0;
    lastD[0]=parse(server.arg("d1"));
    lastD[1]=parse(server.arg("d2"));
    lastD[2]=parse(server.arg("d3"));
    lastD[3]=parse(server.arg("d4"));

    lastMedian = medianOfValid(lastD,4);
    updated=true;

    Serial.printf("[RX %s] angle=%d median=%.2f\n",
                   lastEsp32IP.toString().c_str(),
                   lastAngle,lastMedian);
  }

  if(!updated){
    server.send(400,"text/plain","bad");
    return;
  }

  drawUI(false);
  maybeBeepAndNotify();
  server.send(200,"text/plain","OK");
}

// Réponse JSON → inspection rapide de l’état
void handleLast(){
  String json="{";
  json+="\"systemOn\":"+(systemOn?"true":"false");
  json+=",\"angle\":"+String(lastAngle);
  json+=",\"median\":"+(isnan(lastMedian)?String("null"):String(lastMedian,2));
  json+=",\"threshold\":"+String(alertThresholdCm);
  json+=",\"alarmPower\":"+String(alarmPower);
  json+=",\"tab5\":\""+WiFi.localIP().toString()+"\"";
  json+=",\"esp32\":\""+(lastEsp32IP[0]==0?String(""):lastEsp32IP.toString())+"\"";
  json+=",\"d\":[";
  for(int i=0;i<4;i++){
    if(i) json+=",";
    json+=(isnan(lastD[i])?String("null"):String(lastD[i],2));
  }
  json+="]}";

  server.send(200,"application/json",json);
}

// -------------------------------------------------------------------
//  Commande ON/OFF envoyée à l’ESP32
// -------------------------------------------------------------------
bool sendSystemStateToESP32(bool turnOn){
  if(lastEsp32IP[0]==0){
    Serial.println("[CTRL] ESP32 inconnu");
    return false;
  }

  if(millis()-lastSysToggleMs<SYS_TOGGLE_GUARD_MS){
    Serial.println("[CTRL] garde 5s");
    return false;
  }

  HTTPClient http;
  http.setConnectTimeout(5000);

  String url = String("http://")
              + lastEsp32IP.toString()
              + "/control?state=" + (turnOn ? "on" : "off");

  Serial.printf("[CTRL] request: %s\n",url.c_str());

  if(!http.begin(url)){
    Serial.println("[CTRL] http.begin failed");
    return false;
  }

  int code=http.GET();
  String body=http.getString();
  http.end();

  Serial.printf("[CTRL] response: %d %s\n",code,body.c_str());

  if(code==200){
    lastSysToggleMs=millis();
    return true;
  }

  return false;
}

// -------------------------------------------------------------------
//  Gestion du tactile (ON/OFF, seuils, puissance)
// -------------------------------------------------------------------
static bool handleTouch(){
  auto t=M5.Touch.getDetail();
  if(!t.isPressed() && !t.wasClicked()) return false;

  int tx=t.x, ty=t.y;
  uint32_t now=millis();

  // ON/OFF
  if(inRect(R_BTN,tx,ty)){
    bool wantOn = !systemOn;
    if(sendSystemStateToESP32(wantOn)){
      systemOn=wantOn;
      prefsSave();
      drawUI(false);
    } else {
      M5.Speaker.tone(1200,80);
    }
    return true;
  }

  // Seuils
  for(int i=0;i<4;i++){
    if(inRect(R_ALERT_B[i],tx,ty)){
      if(now-lastThreshChangeMs>=UI_DEBOUNCE_MS){
        alertThresholdCm=THRESH_BTNS[i];
        lastThreshChangeMs=now;
        prefsSave();
        drawUI(false);
      }
      return true;
    }
  }

  // Puissance buzzer
  for(int i=0;i<4;i++){
    if(inRect(R_POWER_B[i],tx,ty)){
      if(now-lastPowerChangeMs>=UI_DEBOUNCE_MS){
        alarmPower=POWER_BTNS[i];
        M5.Speaker.setVolume(alarmPower);
        M5.Speaker.tone(BEEP_FREQ_HZ,80);
        lastPowerChangeMs=now;
        prefsSave();
        drawUI(false);
      }
      return true;
    }
  }

  return false;
}

// -------------------------------------------------------------------
//  Setup TAB5 : Wi-Fi, serveur, UI
// -------------------------------------------------------------------
void setup(){
  auto cfg=M5.config();
  cfg.output_power=true;
  M5.begin(cfg);

  Serial.begin(115200);
  prefsLoad();

#ifdef BOARD_SDIO_ESP_HOSTED_CLK
  WiFi.setPins(BOARD_SDIO_ESP_HOSTED_CLK,BOARD_SDIO_ESP_HOSTED_CMD,
               BOARD_SDIO_ESP_HOSTED_D0,BOARD_SDIO_ESP_HOSTED_D1,
               BOARD_SDIO_ESP_HOSTED_D2,BOARD_SDIO_ESP_HOSTED_D3,
               BOARD_SDIO_ESP_HOSTED_RESET);
#else
  WiFi.setPins(SDIO2_CLK,SDIO2_CMD,SDIO2_D0,
               SDIO2_D1,SDIO2_D2,SDIO2_D3,SDIO2_RST);
#endif

  WiFi.mode(WIFI_STA);

  // APPLICATION DE TON IP STATIQUE
  WiFi.config(local_IP, gateway, subnet, dns);

  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while(WiFi.status()!=WL_CONNECTED) delay(250);

  Serial.printf("[NET] TAB5 IP: %s\n",
                WiFi.localIP().toString().c_str());

  configTime(0,0,"pool.ntp.org");

  // Routes HTTP
  server.on("/health",HTTP_GET,handleHealth);
  server.on("/data",  HTTP_POST,handleData);
  server.on("/last",  HTTP_GET, handleLast);
  server.begin();

  computeLayout();
  M5.Speaker.setVolume(alarmPower);
  drawUI(true);
}

// -------------------------------------------------------------------
//  Loop principale
// -------------------------------------------------------------------
void loop(){
  M5.update();
  server.handleClient();

  static uint32_t lastClock=0;
  if(millis()-lastClock>1000){
    lastClock=millis();
    drawHeader(false);
  }

  handleTouch();
  delay(5);
}
