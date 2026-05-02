# 🏠 Smart Home Automation System (ESP8266)

A lightweight, modular IoT-based home automation system built on ESP8266 with web UI control, time scheduling, and weather-based automation.

---

## 🚀 Features

### ❄ Cooling System

* Auto & Manual modes
* 14 configurable time slots
* Temperature-based control (OpenWeather API)
* Adjustable threshold (15°C–25°C)

### 🐔 Chicken House Light

* Seasonal automation:

  * Summer: temp-based night lighting
  * Winter: fixed schedule
* Hourly temperature re-evaluation
* Manual override

### ⚙ Motor Control (Water Pump)

* 433 MHz RF trigger
* Stateless control (no feedback)
* UI-based trigger button

---

## 🌐 Web Interface

* Lightweight HTML/CSS (no frameworks)
* Mobile-friendly design
* Card-based layout
* No page reload (AJAX-based control)

### Pages

* `/` → Dashboard
* `/slots` → Time Slot Management
* `/pump` → Motor Control Page

---

## 🧠 System Logic

### Cooling Logic

```
IF (time slot active) AND (temperature >= threshold)
→ Fan ON
ELSE → OFF
```

### Chicken Light (Summer)

```
Every hour:
IF temp < threshold → ON
ELSE → OFF
```

---

## 🧰 Tech Stack

* ESP8266 (NodeMCU / Wemos)
* Arduino (C++)
* ESP8266WebServer
* EEPROM
* NTPClient
* OpenWeatherMap API
* RCSwitch (433 MHz)

---

## ⚡ Hardware

| Component | Details    |
| --------- | ---------- |
| MCU       | ESP8266    |
| Relays    | 2x 5V      |
| RF Module | 433 MHz TX |
| Power     | 5V 1A      |

---

## 🔌 Pin Configuration

| Device         | Pin |
| -------------- | --- |
| Cooling Relay  | D1  |
| RF Transmitter | D2  |
| Chicken Light  | D6  |

---

## 💾 Persistence (EEPROM)

Stored data:

* Cooling mode & threshold
* Time slots
* Chicken light mode & threshold
* Last states

---

## 📡 Network

* WiFi STA mode
* Auto reconnect
* Custom port: `6843`

---

## ⚠ Limitations

* No RF feedback
* No authentication
* UI not auto-syncing external changes
* millis() overflow (~49 days)

---

## 🔮 Future Improvements

* Authentication system
* Real-time UI sync
* Sensor integration
* Modular device plugins
* REST API layer

---

## 🧱 Design Philosophy

* Lightweight > complex
* Stable > feature-heavy
* Modular & expandable
* Hardware-safe

---

## 📌 Summary

A modular ESP8266-based smart automation system combining time-based scheduling, weather intelligence, and manual control through a lightweight web interface.
