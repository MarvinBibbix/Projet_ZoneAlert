# Project_ZoneAlert Wireless Space Surveillance & Alert System
ZoneAlert is a compact, low-cost, autonomous system designed to monitor an indoor zone (garage, workshop, technical room, etc.) and alert the user in case of intrusion or unexpected obstacle detection. The system performs periodic ultrasonic scanning, displays results in real time on a touchscreen, triggers a local alarm, and can notify the user remotely via push notification. The goal is to provide a simple, accessible, yet reliable surveillance solution that can be deployed in various domestic or technical settings

## Hardware Components
ESP32 WROOM – Wi-Fi, low power, easy firmware development
HC-SR04 ultrasonic sensor – common, documented, precise enough for indoor use
SG90 servo motor – 0–180° rotation, used for scanning
M5TAB5 Tablet – main UI, visualization, treatment, alert trigger

### Core features:
180° scanning with servo + ultrasonic sensor
Real-time measurement filtering (median)
On-screen interface (status, thresholds, ON/OFF, buzzer power)
Local alerts (buzzer) and remote alerts (push notifications)
Simple installation: USB power + Wi-Fi 2.4 GHz

### Repository Contents
Codes_ZoneAlert/ : Full source code for ESP32 (sensor + servo) and M5TAB5 (UI + processing)
Docs/ : Technical documents, drafts, diagrams, configuration files

## How to Build & Run
#### 1. Clone the repository 
    git clone https://github.com/MarvinBibbix/Projet_ZoneAlert.git
#### 2. Install required Arduino libraries
    Make sure the following libraries are installed:  
      M5Unified
      M5GFX
      ESP32Servo
      WebServer (built-in)
      Preferences / WiFi / HTTPClient / WiFiClientSecure (built-in with ESP32 core)
    Recommended ESP32 Core: 3.0.x (avoids compatibility issues with M5Unified/M5GFX)
#### 3. Open and flash the two firmwares
    ESP32 WROOM -> Sensor + servo scanning
    M5TAB5 -> UI + processing + alerts
#### 4. Power and connect
      Power both devices over USB 5V
      Connect both to the same Wi-Fi network
      TAB5 uses a static IP (set directly in the code)
#### 5.Run
    The ESP32 scans the zone -> sends data to the TAB5-> TAB5 displays values, triggers buzzer, and sends optional Pushover notifications


    
