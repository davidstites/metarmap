# Aviation Weather Map

An ESP32-based aviation weather display that shows real-time METAR data from airports using WS2812B addressable LEDs. Each LED represents an airport and displays its current flight category with additional visual effects for wind warnings and thunderstorms.

![Version](https://img.shields.io/badge/version-1.3-blue.svg)
![Platform](https://img.shields.io/badge/platform-ESP32-green.svg)
![License](https://img.shields.io/badge/license-MIT-orange.svg)

## Features

### Core Functionality
- **Real-time METAR data** from aviationweather.gov API
- **Flight category visualization** using color-coded LEDs
  - 🟢 Green = VFR (Visual Flight Rules)
  - 🔵 Blue = MVFR (Marginal VFR)
  - 🔴 Red = IFR (Instrument Flight Rules)
  - 🟣 Magenta = LIFR (Low IFR)
  - 🟠 Orange/Red = No Data
- **Wind warnings** with blinking yellow LEDs for high wind conditions
- **Thunderstorm alerts** with lightning-style white flashes
- **Adjustable LED brightness** (0-255)
- **Web interface** for configuration and monitoring
- **Persistent settings** stored in LittleFS filesystem
- **WiFi configuration portal** for easy setup
- **Automatic weather updates** every 15 minutes
- **mDNS support** for easy network access

### Visual Effects
- **Wind Blink**: LEDs blink yellow when wind speed exceeds threshold (default: 25kt, configurable 5-100kt)
- **Thunderstorm Flash**: Brief white flashes simulate lightning when thunderstorms are reported
- **Rainbow Sweep**: Startup animation when successfully connected to WiFi
- **Error Indication**: Pulsing red when connection or data fetch fails

## Hardware Requirements

### Essential Components
- **ESP32 development board** (any variant with WiFi)
- **WS2812B LED strip** (up to 50 LEDs supported)
- **5V power supply** (capacity depends on number of LEDs)
  - Calculate: ~60mA per LED at full brightness
  - Example: 50 LEDs = 3A minimum recommended
- **Push button** (optional, for WiFi reset)
- **Resistor** 330Ω-470Ω (recommended for LED data line protection)
- **Capacitor** 1000µF (recommended across power supply)

### Pin Connections
| Component | ESP32 Pin | Notes |
|-----------|-----------|-------|
| LED Data | GPIO 25 | Configurable via `DATA_PIN` |
| Status LED | GPIO 2 | Internal LED (optional) |
| Reset Button | GPIO 22 | Pull-up enabled, active LOW |
| Power | 5V + GND | Ensure adequate current supply |

## Software Requirements

### Arduino IDE Setup
1. Install [Arduino IDE](https://www.arduino.cc/en/software) 1.8.19 or newer
2. Add ESP32 board support:
   - File → Preferences → Additional Board Manager URLs
   - Add: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
3. Install ESP32 boards: Tools → Board → Boards Manager → Search "ESP32" → Install

### Required Libraries
Install via Library Manager (Sketch → Include Library → Manage Libraries):

| Library | Version | Purpose |
|---------|---------|---------|
| FastLED | Latest | LED control |
| ESPAsyncWebServer | Latest | Web interface |
| AsyncTCP | Latest | Async web server dependency |
| LittleFS | Built-in | File system for settings |

### File System Setup
1. Install [ESP32 LittleFS Uploader](https://github.com/lorol/arduino-esp32littlefs-plugin)
2. Create `data` folder in sketch directory
3. Add required web files to `data` folder:
   - `index.html` - Main dashboard
   - `detailed_weather.html` - Detailed METAR view
   - `api_status.html` - System status page
   - `settings.html` - Configuration page
   - `wifimanager.html` - WiFi setup page
   - `404.html` - Error page
4. Upload: Tools → ESP32 LittleFS Data Upload

## Installation

### 1. Clone Repository
```bash
git clone https://github.com/yourusername/aviation-weather-map.git
cd aviation-weather-map
```

### 2. Configure Airports
Edit the `airportLEDs[]` array in the code with your desired airports:

```cpp
AirportLED airportLEDs[] = {
    AirportLED("KSFO"),  // LED 0: San Francisco
    AirportLED("KLAX"),  // LED 1: Los Angeles
    AirportLED("KPDX"),  // LED 2: Portland
    // Add up to 50 airports
};
```

**Tips:**
- Use `NULL` entries to skip LED positions
- Order matches physical LED strip sequence
- Maximum 50 airports (adjustable via `MAX_AIRPORTS`)

### 3. Configure Settings (Optional)
Modify constants in the code if needed:

```cpp
#define DATA_PIN 25                    // LED data pin
#define BUTTON_PIN 22                  // Reset button pin
#define DEFAULT_BRIGHTNESS 30          // Initial brightness (0-255)
#define WIND_THRESHOLD 25              // Wind warning threshold (knots)
#define REQUEST_INTERVAL 900000        // Update interval (15 min)
```

### 4. Upload Code
1. Connect ESP32 via USB
2. Select: Tools → Board → ESP32 Dev Module
3. Select correct COM port
4. Click Upload
5. Upload LittleFS data (Tools → ESP32 LittleFS Data Upload)

### 5. WiFi Setup
**First Boot:**
1. ESP32 creates WiFi access point: `ESP-WIFI-MANAGER`
2. Connect to this network
3. Navigate to `192.168.4.1`
4. Enter your WiFi credentials
5. Device restarts and connects to your network

**Finding Device:**
- Check serial monitor for IP address
- Or use mDNS: `http://metarmap.local`

## Web Interface

### Main Dashboard (`/`)
- Visual airport status overview
- Current flight categories
- Quick weather summary
- System status indicators

### Detailed Weather (`/detailed`)
- Complete METAR breakdown per airport
- Temperature, dewpoint, winds
- Visibility, altimeter settings
- Cloud conditions
- Raw METAR text

### System Status (`/status`)
- API connection status
- WiFi signal strength
- System uptime
- Memory usage
- Next update countdown
- Airport data statistics

### Settings (`/settings`)
- LED brightness adjustment (0-255)
- Enable/disable wind warnings
- Wind speed threshold configuration (5-100 knots)
- Enable/disable thunderstorm effects
- Settings persist across reboots

## Configuration

### Adjustable Parameters

#### LED Settings
```cpp
#define DEFAULT_BRIGHTNESS 30     // 0-255, adjustable via web
#define MAX_AIRPORTS 50           // Maximum supported airports
```

#### Weather Thresholds
```cpp
#define WIND_THRESHOLD 25         // Wind warning speed (knots), configurable 5-100
```

#### Timing
```cpp
#define REQUEST_INTERVAL 900000   // Weather update (15 min)
#define LOOP_INTERVAL 5000        // Main loop interval (5 sec)
#define RETRY_TIMEOUT 15000       // Retry after failure (15 sec)
```

#### Flight Category Thresholds
```cpp
namespace FlightCategory {
    const int LIFR_CEILING = 500;       // feet
    const float LIFR_VISIBILITY = 1.0;  // statute miles
    const int IFR_CEILING = 1000;
    const float IFR_VISIBILITY = 3.0;
    const int MVFR_CEILING = 3000;
    const float MVFR_VISIBILITY = 5.0;
}
```

## Troubleshooting

### LED Issues
**Problem:** LEDs not lighting up
- Check power supply voltage (should be 5V)
- Verify `DATA_PIN` matches your wiring
- Ensure sufficient current capacity
- Check LED strip polarity (DI/DO direction)

**Problem:** Wrong colors or flickering
- Add 330Ω resistor on data line
- Add 1000µF capacitor across power supply
- Keep data wire short (<6 inches from ESP32)
- Use level shifter if needed (3.3V→5V)

### WiFi Issues
**Problem:** Can't connect to WiFi
- Press reset button to enter AP mode
- Check WiFi credentials (case-sensitive)
- Ensure 2.4GHz network (5GHz not supported)
- Check router compatibility with ESP32

**Problem:** Frequent disconnections
- Move closer to WiFi router
- Check for interference
- Verify power supply stability
- Review serial monitor for errors

### Weather Data Issues
**Problem:** No weather data (orange LEDs)
- Check internet connectivity
- Verify airport ICAO codes (4 letters)
- Check aviationweather.gov accessibility
- Review `/status` page for API errors

**Problem:** Intermittent data
- Check available memory in `/status`
- Reduce number of airports if low memory
- Verify stable WiFi connection

### Memory Issues
**Problem:** System crashes or restarts
- Reduce `MAX_AIRPORTS` value
- Check free heap in serial monitor
- Ensure LittleFS uploaded correctly
- Consider reducing number of active airports

## API Information

### Data Source
- **Provider:** Aviation Weather Center (NOAA)
- **Endpoint:** `https://aviationweather.gov/api/data/metar`
- **Format:** Raw METAR text
- **Rate Limit:** Respects reasonable use (15-min intervals)
- **Documentation:** [Aviation Weather Center API](https://aviationweather.gov/data/api/)

### METAR Parsing
The system parses raw METAR text to extract:
- Flight category (derived from ceiling/visibility)
- Wind speed and direction
- Temperature and dewpoint
- Visibility
- Altimeter setting
- Weather phenomena (thunderstorms)
- Cloud conditions

## Advanced Features

### Multiple Controllers
Support for multiple ESP32 controllers to exceed 50-airport limit:
```cpp
// Configure unique hostname for each controller
WiFiManager wifiManager("metarmap");    // Controller 1
WiFiManager wifiManager("metarmap2");   // Controller 2
```

### Custom Colors
Modify LED colors in the `LEDColors` namespace:
```cpp
namespace LEDColors {
    const CRGB VFR = CRGB::Green;
    const CRGB MVFR = CRGB::Blue;
    const CRGB IFR = CRGB::Red;
    const CRGB LIFR = CRGB::Magenta;
    const CRGB WIND_WARNING = 0xFFC003;  // Custom amber
    const CRGB THUNDERSTORM = CRGB::White;
}
```

### Persistent Storage
Settings stored in LittleFS `/settings.json`:
- LED brightness
- Wind warnings enabled/disabled
- Wind speed threshold
- Thunderstorm effects enabled/disabled

### Button Functions
- **Short press during operation:** Reserved for future features
- **Hold during boot:** Forces WiFi configuration mode
- **Reset WiFi:** Erases stored credentials and restarts

## Serial Monitor Commands

Monitor at **115200 baud** for diagnostic information:
- WiFi connection status
- Weather fetch attempts
- METAR parsing details
- Memory usage
- System errors

Example output:
```
📡 WiFi connected: 192.168.1.100
✅ LittleFS mounted
=== Fetching Weather ===
📋 Parsing: KSFO 081856Z 28015G25KT 10SM FEW015 BKN250 15/12 A3012
✈️ KSFO: VFR, vis=10.0 SM, temp=15.0°C, wind=280°@15kt
✅ Weather updated - next in 900s
```

## Contributing

Contributions welcome! Please:
1. Fork the repository
2. Create a feature branch
3. Commit your changes
4. Push to the branch
5. Open a Pull Request

### Development Guidelines
- Follow existing code style
- Test thoroughly before submitting
- Update documentation as needed
- Include serial debug output for new features

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Credits

- **Aviation Weather Data:** NOAA Aviation Weather Center
- **LED Library:** FastLED by Daniel Garcia
- **Async Web Server:** ESPAsyncWebServer by me-no-dev
- **Inspiration:** Aviation community and sectional chart enthusiasts

## Support

- **Issues:** [GitHub Issues](https://github.com/yourusername/aviation-weather-map/issues)
- **Discussions:** [GitHub Discussions](https://github.com/yourusername/aviation-weather-map/discussions)
- **Documentation:** [Wiki](https://github.com/yourusername/aviation-weather-map/wiki)

## Changelog

### Version 1.3
- Added configurable wind speed threshold (5-100 knots)
- Improved thunderstorm detection logic
- Enhanced METAR parsing accuracy
- Added detailed weather view
- Improved memory management
- Enhanced error handling

### Version 1.2
- Added thunderstorm lightning effects
- Improved wind warning system
- Added web-based settings page
- Enhanced WiFi reconnection logic

### Version 1.1
- Initial public release
- Basic METAR display functionality
- WiFi configuration portal
- Web dashboard

## Future Enhancements

- [ ] TAF (Terminal Aerodrome Forecast) support
- [ ] Customizable color schemes
- [ ] MQTT integration for home automation
- [ ] Mobile app companion
- [ ] Historical weather data logging
- [ ] Multiple airport groups/pages
- [ ] OTA (Over-The-Air) updates
- [ ] Battery backup support
- [ ] Weather alerts/notifications

---

**Made with ✈️ by aviation enthusiasts, for aviation enthusiasts**