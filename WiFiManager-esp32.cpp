/**************************************************************
   WiFiManager is a library for the ESP8266/Arduino platform
   (https://github.com/esp8266/Arduino) to enable easy
   configuration and reconfiguration of WiFi credentials using a Captive Portal
   inspired by:
   http://www.esp8266.com/viewtopic.php?f=29&t=2520
   https://github.com/chriscook8/esp-arduino-apboot
   https://github.com/esp8266/Arduino/tree/master/libraries/DNSServer/examples/CaptivePortalAdvanced
   Built by AlexT https://github.com/tzapu
   Licensed under MIT license
 **************************************************************/

#include <Preferences.h>

#include "WiFiManager-esp32.h"

#define DEFAULT_TIMEOUT 300

int n_wifi_networks = 0;
Preferences preferences;

WiFiManagerParameter::WiFiManagerParameter(const char *custom) {
  _id = NULL;
  _placeholder = NULL;
  _length = 0;
  _value = NULL;

  _customHTML = custom;
}

WiFiManagerParameter::WiFiManagerParameter(const char *id,
                                           const char *placeholder,
                                           const char *defaultValue,
                                           int length) {
  init(id, placeholder, defaultValue, length, "");
}

WiFiManagerParameter::WiFiManagerParameter(const char *id,
                                           const char *placeholder,
                                           const char *defaultValue, int length,
                                           const char *custom) {
  init(id, placeholder, defaultValue, length, custom);
}

void WiFiManagerParameter::init(const char *id, const char *placeholder,
                                const char *defaultValue, int length,
                                const char *custom) {
  _id = id;
  _placeholder = placeholder;
  _length = length;
  _value = new char[length + 1];

  for (int i = 0; i < length; i++) {
    _value[i] = 0;
  }
  if (defaultValue != NULL) {
    strncpy(_value, defaultValue, length);
  }

  _customHTML = custom;
}

const char *WiFiManagerParameter::getValue() { return _value; }
const char *WiFiManagerParameter::getID() { return _id; }
const char *WiFiManagerParameter::getPlaceholder() { return _placeholder; }
int WiFiManagerParameter::getValueLength() { return _length; }
const char *WiFiManagerParameter::getCustomHTML() { return _customHTML; }

void WiFiManager::configure(String hostname, void (*statusCb)(Status status)) {
  // Open Preferences with my-app namespace. Each application module, library,
  // etc has to use a namespace name to prevent key name collisions. We will
  // open storage in RW-mode (second parameter has to be false). Note: Namespace
  // name is limited to 15 chars.
  preferences.begin("WiFiManager", false);
  _preferences_opened = true;

  _statusCb = statusCb;

  status.mode = CONNECTING;
  if (_statusCb) {
    _statusCb(status);
  }

  appendMacToHostname(true);
  setDefaultHostname(hostname);
  readHostname();
  readNetworkCredentials();
}

WiFiManager::WiFiManager() {}

void WiFiManager::addParameter(WiFiManagerParameter *p) {
  if (_paramsCount + 1 > WIFI_MANAGER_MAX_PARAMS) {
    // Max parameters exceeded!
    DEBUG_WM(
        F("WIFI_MANAGER_MAX_PARAMS exceeded, increase number (in "
          "WiFiManager.h) before adding more parameters!"));
    DEBUG_WM(F("Skipping parameter with ID: "));
    DEBUG_WM(p->getID());
    return;
  }
  _params[_paramsCount] = p;
  _paramsCount++;
  DEBUG_WM(F("Adding parameter: "));
  DEBUG_WM(p->getID());
}

void WiFiManager::setupConfigPortal() {
  DEBUG_WM(F("Configuring access point... "));

  dnsServer.reset(new DNSServer());
#ifdef ESP8266
  server.reset(new ESP8266WebServer(80));
#else
  server.reset(new WebServer(80));
#endif

  _configPortalStart = millis();

  DEBUG_WM(F("Access point name: "));
  DEBUG_WM(_apName);
  if (_apPassword != NULL) {
    if (strlen(_apPassword) < 8 || strlen(_apPassword) > 63) {
      // fail passphrase to short or long!
      DEBUG_WM(F("Invalid AccessPoint password. Ignoring"));
      _apPassword = NULL;
    }
    DEBUG_WM(F("Password: "));
    DEBUG_WM(_apPassword);
  }

  // optional soft ip config
  if (_ap_static_ip) {
    DEBUG_WM(F("Custom AP IP/GW/Subnet"));
    WiFi.softAPConfig(_ap_static_ip, _ap_static_gw, _ap_static_sn);
  }

  if (_apPassword != NULL) {
    WiFi.softAP(_apName, _apPassword);  // password option
  } else {
    WiFi.softAP(_apName);
  }

  delay(500);  // Without delay I've seen the IP address blank
  DEBUG_WM(F("AP IP address: "));
  DEBUG_WM(WiFi.softAPIP());

  /* Setup the DNS server redirecting all the domains to the apIP */
  dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer->start(DNS_PORT, "*", WiFi.softAPIP());

  /* Setup web pages: root, wifi config pages, SO captive portal detectors and
   * not found. */
  // server->on("/", std::bind(&WiFiManager::handleRoot, this));
  server->on("/", std::bind(&WiFiManager::handleWifi, this, false));
  server->on("/wifi", std::bind(&WiFiManager::handleWifi, this, true));
  server->on("/0wifi", std::bind(&WiFiManager::handleWifi, this, false));
  server->on("/wifisave", std::bind(&WiFiManager::handleWifiSave, this));
  server->on("/i", std::bind(&WiFiManager::handleInfo, this));
  server->on("/r", std::bind(&WiFiManager::handleReset, this));
  server->on("/changename",
             std::bind(&WiFiManager::handleChangeName, this, false));
  server->on("/savename", std::bind(&WiFiManager::handleSaveName, this));
  // server->on("/generate_204", std::bind(&WiFiManager::handle204, this));
  // //Android/Chrome OS captive portal check. server->on("/fwlink",
  // std::bind(&WiFiManager::handleRoot, this));  //Microsoft captive portal.
  // Maybe not needed. Might be handled by notFound handler.
  server->on("/fwlink",
             std::bind(&WiFiManager::handleWifi, this,
                       false));  // Microsoft captive portal. Maybe not needed.
                                 // Might be handled by notFound handler.
  server->onNotFound(std::bind(&WiFiManager::handleNotFound, this));
  server->begin();  // Web server start
  DEBUG_WM(F("HTTP server started"));
}

boolean WiFiManager::autoConnect() {
  String ssid = getHostname();

  return autoConnect(ssid.c_str(), NULL);
}

boolean WiFiManager::autoConnect(char const *apName, char const *apPassword) {
  bool connected = false;

  setTimeout(DEFAULT_TIMEOUT);

  DEBUG_WM(F(""));
  DEBUG_WM(F("AutoConnect"));

  String macStr = getMacAsString(true);
  DEBUG_WM("MAC: " + macStr);

  // attempt to connect; should it fail, fall back to AP
  WiFi.mode(WIFI_STA);

  // Do not use connectWifi("", ""). While this method should use the last used
  // SSID and password, these settings are not always properly stored. Instead,
  // it is better to rely on the ssid and password stored in user preferences
  // (already loaded in _ssid and _pass)
  if (_ssid != "") {
    DEBUG_WM(F("Connecting to network: "));
    DEBUG_WM(_ssid);
    if (connectWifi(_ssid, _pass) == WL_CONNECTED) {
      DEBUG_WM(F("IP Address:"));
      DEBUG_WM(WiFi.localIP());
      connected = true;
    } else {
      connected = startConfigPortal(apName, apPassword);
    }
  } else {
    DEBUG_WM("Starting portal");
    connected = startConfigPortal(apName, apPassword);
  }

  preferences.end();
  _preferences_opened = false;

  return connected;
}

boolean WiFiManager::configPortalHasTimeout() {
#if defined(ESP8266)
  if (_configPortalTimeout == 0 || wifi_softap_get_station_num() > 0) {
#else
  if (_configPortalTimeout == 0) {  // TODO
#endif
    _configPortalStart =
        millis();  // kludge, bump configportal start time to skew timeouts
    return false;
  }
  return (millis() > _configPortalStart + _configPortalTimeout);
}

boolean WiFiManager::startConfigPortal() {
  String ssid = getHostname();

  return startConfigPortal(ssid.c_str(), NULL);
}

boolean WiFiManager::startConfigPortal(char const *apName,
                                       char const *apPassword) {
  // First scan networks (so user doesn't have to wait)
  // Scan does not seem to work without disconnecting first
  WiFi.disconnect(true);

  status.mode = SCANNING;
  if (_statusCb) {
    _statusCb(status);
  }

  n_wifi_networks = WiFi.scanNetworks();
  DEBUG_WM(F("Scan done"));

  // setup AP
  WiFi.mode(WIFI_AP_STA);
  DEBUG_WM("SET AP STA");

  status.mode = PORTAL;
  if (_statusCb) {
    _statusCb(status);
  }

  _apName = apName;
  _apPassword = apPassword;

  // notify we entered AP mode
  if (_apcallback != NULL) {
    _apcallback(this);
  }

  connect = false;
  setupConfigPortal();

  while (1) {
    // check if timeout
    if (configPortalHasTimeout()) break;

    // DNS
    dnsServer->processNextRequest();
    // HTTP
    server->handleClient();

    if (connect) {
      connect = false;
      delay(2000);

      status.mode = CONNECTING;
      if (_statusCb) {
        _statusCb(status);
      }

      DEBUG_WM(F("Connecting to new AP"));

      // using user-provided  _ssid, _pass in place of system-stored ssid and
      // pass
      if (connectWifi(_ssid, _pass) != WL_CONNECTED) {
        DEBUG_WM(F("Failed to connect."));

        status.mode = DISCONNECTED;
        if (_statusCb) {
          _statusCb(status);
        }
      } else {
        // connected
        WiFi.mode(WIFI_STA);

        status.mode = CONNECTED;
        if (_statusCb) {
          _statusCb(status);
        }

        // notify that configuration has changed and any optional parameters
        // should be saved
        if (_savecallback != NULL) {
          // todo: check if any custom parameters actually exist, and check if
          // they really changed maybe
          _savecallback();
        }
        break;
      }

      if (_shouldBreakAfterConfig) {
        // flag set to exit after config after trying to connect
        // notify that configuration has changed and any optional parameters
        // should be saved
        if (_savecallback != NULL) {
          // todo: check if any custom parameters actually exist, and check if
          // they really changed maybe
          _savecallback();
        }
        break;
      }
    }
    yield();
  }

  server.reset();
  dnsServer.reset();

  return WiFi.status() == WL_CONNECTED;
}

int WiFiManager::connectWifi(String ssid, String pass) {
  DEBUG_WM(F("Connecting as wifi client..."));

  int connRes = doConnectWifi(ssid, pass, 0);
  if (connRes != WL_CONNECTED) {
    // Connection failed; could be due to this issue where every 2nd connect
    // attempt fails: https://github.com/espressif/arduino-esp32/issues/234
    // As temporary workaround: try to connect for second time
    WiFi.disconnect(true);
    connRes = doConnectWifi(ssid, pass, 1);
  }
  DEBUG_WM(F("Connection result: "));
  DEBUG_WM(connRes);

  if (WiFi.status() == WL_CONNECTED) {
    status.mode = CONNECTED;
  } else if (WiFi.status() == WL_DISCONNECTED) {
    status.mode = DISCONNECTED;
  }

  if (_statusCb) {
    _statusCb(status);
  }

  // not connected, WPS enabled, no pass - first attempt
  if (_tryWPS && connRes != WL_CONNECTED && pass == "") {
    startWPS();
    // should be connected at the end of WPS
    connRes = waitForConnectResult();
  }
  return connRes;
}

int WiFiManager::doConnectWifi(String ssid, String pass, int count) {
  // check if we've got static_ip settings, if we do, use those.
  if (_sta_static_ip) {
    if (count == 0) {
      DEBUG_WM(F("Custom STA IP/GW/Subnet"));
    }
    WiFi.config(_sta_static_ip, _sta_static_gw, _sta_static_sn);
    DEBUG_WM(F("Local IP:"));
    DEBUG_WM(WiFi.localIP());
  }
  // fix for auto connect racing issue
  if (WiFi.status() == WL_CONNECTED) {
    status.mode = CONNECTED;
    if (_statusCb) {
      _statusCb(status);
    }
    DEBUG_WM("Already connected. Bailing out.");
    return WL_CONNECTED;
  }
  // check if we have ssid and pass and force those, if not, try with last saved
  // values
  String hostname = getHostname();
  DEBUG_WM(F("Setting hostname to: "));
  DEBUG_WM(hostname.c_str());

  // Workaround for issue where ESP32 forgets its hostname when DHCP lease
  // renewal is required; see
  // https://github.com/espressif/arduino-esp32/issues/2537
  //
  // Apparently, with arduino-esp32 2.0.2, this workaround kills the wifi.
  // Only known solution is to remove the workaround and accept that device
  // is difficult to find on the network. See also
  // https://github.com/espressif/arduino-esp32/issues/4732#issuecomment-763982089
  // and https://github.com/dJPoida/garage_bot/issues/1
  //
  // WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);

  WiFi.setHostname(hostname.c_str());
  if (ssid != "") {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    if (_ssid) {
      if (count == 0) {
        DEBUG_WM(F("Connecting to network "));
        DEBUG_WM(_ssid);
      }
#if defined(ESP8266)
      // trying to fix connection in progress hanging
      ETS_UART_INTR_DISABLE();
      wifi_station_disconnect();
      ETS_UART_INTR_ENABLE();
#else
      esp_wifi_disconnect();
#endif

      WiFi.begin();
      WiFi.mode(WIFI_STA);
      connectWifi(_ssid, _pass);
    } else {
      if (count == 0) {
        DEBUG_WM("No saved credentials");
      }
    }
  }

  int connRes = waitForConnectResult();

  return connRes;
}

uint8_t WiFiManager::waitForConnectResult() {
  if (_connectTimeout == 0) {
    return WiFi.waitForConnectResult();
  } else {
    DEBUG_WM(F("Waiting for connection result with time out"));
    unsigned long start = millis();
    boolean keepConnecting = true;
    uint8_t wifiStatus;
    while (keepConnecting) {
      wifiStatus = WiFi.status();
      if (millis() > start + _connectTimeout) {
        keepConnecting = false;
        DEBUG_WM(F("Connection timed out"));
      }
      if (wifiStatus == WL_CONNECTED || wifiStatus == WL_CONNECT_FAILED) {
        keepConnecting = false;
      }
      delay(100);
    }

    if (wifiStatus == WL_CONNECTED) {
      status.mode = CONNECTED;
    } else if (wifiStatus == WL_CONNECT_FAILED) {
      status.mode = CONNECTED;
    }

    if (_statusCb) {
      _statusCb(status);
    }
    return wifiStatus;
  }
}

void WiFiManager::startWPS() {
#if defined(ESP8266)
  DEBUG_WM("START WPS");
  WiFi.beginWPSConfig();
  DEBUG_WM("END WPS");
#else
  // TODO
  DEBUG_WM("ESP32 WPS TODO");
#endif
}

String WiFiManager::getSSID() { return _ssid; }

String WiFiManager::getPassword() { return _pass; }

String WiFiManager::getConfigPortalSSID() { return _apName; }

void WiFiManager::resetSettings() {
  Mode mode_prev = status.mode;
  status.mode = ERASING;
  if (_statusCb) {
    _statusCb(status);
  }
  DEBUG_WM(F("settings invalidated"));
  WiFi.disconnect(true);

  bool preferences_was_already_opened = _preferences_opened;

  if (not _preferences_opened) {
    preferences.begin("WiFiManager", false);
    _preferences_opened = true;
  }

  // Ugly workaround for a bug that prevents proper erasing SSID and password.
  // See
  // https://github.com/espressif/arduino-esp32/issues/400#issuecomment-411076993
  WiFi.begin("0", "0");
  preferences.remove("useHostname");
  preferences.remove("hostname");
  preferences.remove("ssid");
  preferences.remove("pass");
  readHostname();

  if (not preferences_was_already_opened) {
    preferences.end();
    _preferences_opened = false;
  }

  status.mode = mode_prev;
  if (_statusCb) {
    _statusCb(status);
  }
}

void WiFiManager::setTimeout(unsigned long seconds) {
  setConfigPortalTimeout(seconds);
}

void WiFiManager::setConfigPortalTimeout(unsigned long seconds) {
  _configPortalTimeout = seconds * 1000;
}

void WiFiManager::setConnectTimeout(unsigned long seconds) {
  _connectTimeout = seconds * 1000;
}

void WiFiManager::setDebugOutput(boolean debug) { _debug = debug; }

void WiFiManager::setAPStaticIPConfig(IPAddress ip, IPAddress gw,
                                      IPAddress sn) {
  _ap_static_ip = ip;
  _ap_static_gw = gw;
  _ap_static_sn = sn;
}

void WiFiManager::setSTAStaticIPConfig(IPAddress ip, IPAddress gw,
                                       IPAddress sn) {
  _sta_static_ip = ip;
  _sta_static_gw = gw;
  _sta_static_sn = sn;
}

void WiFiManager::setMinimumSignalQuality(int quality) {
  _minimumQuality = quality;
}

void WiFiManager::setBreakAfterConfig(boolean shouldBreak) {
  _shouldBreakAfterConfig = shouldBreak;
}

/** Handle root or redirect to captive portal */
void WiFiManager::handleRoot() {
  DEBUG_WM(F("Handle root"));
  if (captivePortal()) {  // If caprive portal redirect instead of displaying
                          // the page.
    return;
  }

  String page = FPSTR(WM_HTTP_HEAD);
  page.replace("{v}", "Options");
  page += FPSTR(WM_HTTP_SCRIPT);
  page += FPSTR(WM_HTTP_STYLE);
  page += _customHeadElement;
  page += FPSTR(WM_HTTP_HEAD_END);
  page += "<h1>";
  page += getHostname().c_str();
  page += "</h1>";
  page += F("<h3>WiFiManager</h3>");
  page += FPSTR(WM_HTTP_PORTAL_OPTIONS);
  page += FPSTR(WM_HTTP_END);

  server->sendHeader("Content-Length", String(page.length()));
  server->send(200, "text/html", page);
}

void WiFiManager::handleChangeName(bool showError) {
  String page = FPSTR(WM_HTTP_HEAD);
  page.replace("{v}", "Config ESP");
  page += FPSTR(WM_HTTP_SCRIPT);
  page += FPSTR(WM_HTTP_STYLE);
  page += _customHeadElement;
  page += FPSTR(WM_HTTP_HEAD_END);

  page += F("<h3>WiFiManager</h3>");

  if (showError) {
    page += FPSTR(WM_HTTP_CHANGE_NAME_ERROR_MSG);
  }

  page += FPSTR(WM_HTTP_CHANGE_NAME_FORM_START);
  page.replace("{p}", getHostname().c_str());

  page += FPSTR(WM_HTTP_CHANGE_NAME_FORM_END);

  page += FPSTR(WM_HTTP_END);

  server->sendHeader("Content-Length", String(page.length()));
  server->send(200, "text/html", page);

  DEBUG_WM(F("Sent config page"));
}

bool WiFiManager::checkName(String name) {
  bool valid = true;
  char c;

  if ((name.length() == 0) || (name.length() >= 64)) {
    valid = false;
  } else {
    for (uint32_t n = 0; n < name.length(); n++) {
      c = name.charAt(n);

      if (!(((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) ||
            ((c >= '0') && (c <= '9')) || (c == '-'))) {
        valid = false;
        break;
      }
    }
  }

  return valid;
}

void WiFiManager::handleSaveName(void) {
  bool validName = false;
  String tmp = server->arg("n");

  validName = checkName(tmp);

  if (validName) {
    preferences.putString("hostname", tmp);
    preferences.putBool("useHostname", true);
    _hostname = tmp;
    handleWifi(false);
  } else {
    handleChangeName(true);
  }
}

/** Wifi config page handler */
void WiFiManager::handleWifi(boolean scan) {
  int n;
  bool scanBusy = false;

  if (captivePortal()) {  // If caprive portal redirect instead of displaying
                          // the page.
    return;
  }

  n = WiFi.scanComplete();
  if (n < 0) {
    scanBusy = true;
  } else {
    n_wifi_networks = n;
  }

  if (scan) {
    if (scanBusy == false) {
      /* Scan does not seem to work without disconnecting first */
      WiFi.disconnect(true);
      n_wifi_networks = 0;
      WiFi.scanNetworks(true);
      scanBusy = true;
    } else {
      DEBUG_WM(F("Scan busy; not starting another one"));
    }
  }

  String page = FPSTR(WM_HTTP_HEAD);
  page.replace("{v}", "Config ESP");
  page += FPSTR(WM_HTTP_SCRIPT);
  page += FPSTR(WM_HTTP_STYLE);
  page += _customHeadElement;
  if (scanBusy) {
    // Scan busy; enable auto refresh
    page += FPSTR(WM_HTTP_HEAD_REFRESH);
  }
  page += FPSTR(WM_HTTP_HEAD_END);

  page += "<h1>";
  page += getHostname().c_str();
  page += "</h1>";
  page += F("<center>(<a href=\"/changename\">change name</a>)</center>");
  page += F("<h3>WiFiManager</h3>");

  if (scanBusy) {
    page += F("Scan busy. Please wait.");
    page += FPSTR(WM_HTTP_BODY_REFRESH);
  } else {
    DEBUG_WM(F("Scan done"));

    if (n_wifi_networks == 0) {
      DEBUG_WM(F("No networks found"));
      page += F("No networks found. Refresh to scan again.");
    } else {
      // sort networks
      int indices[n_wifi_networks];
      for (int i = 0; i < n_wifi_networks; i++) {
        indices[i] = i;
      }

      // RSSI SORT

      // old sort
      for (int i = 0; i < n_wifi_networks; i++) {
        for (int j = i + 1; j < n_wifi_networks; j++) {
          if (WiFi.RSSI(indices[j]) > WiFi.RSSI(indices[i])) {
            std::swap(indices[i], indices[j]);
          }
        }
      }

      /*std::sort(indices, indices + n_wifi_networks , [](const int & a, const
        int & b) -> bool
        {
        return WiFi.RSSI(a) > WiFi.RSSI(b);
        });*/

      // remove duplicates ( must be RSSI sorted )
      if (_removeDuplicateAPs) {
        String cssid;
        for (int i = 0; i < n_wifi_networks; i++) {
          if (indices[i] == -1) continue;
          cssid = WiFi.SSID(indices[i]);
          for (int j = i + 1; j < n_wifi_networks; j++) {
            if (cssid == WiFi.SSID(indices[j])) {
              DEBUG_WM("DUP AP: " + WiFi.SSID(indices[j]));
              indices[j] = -1;  // set dup aps to index -1
            }
          }
        }
      }

      // display networks in page
      if (n_wifi_networks > 0) {
        page += F("Found the following networks:");
      } else {
        page += F("No networks found. Refresh to scan again.");
      }
      for (int i = 0; i < n_wifi_networks; i++) {
        if (indices[i] == -1) continue;  // skip dups
        DEBUG_WM(WiFi.SSID(indices[i]));
        DEBUG_WM(WiFi.RSSI(indices[i]));
        int quality = getRSSIasQuality(WiFi.RSSI(indices[i]));

        if (_minimumQuality == -1 || _minimumQuality < quality) {
          String item = FPSTR(WM_HTTP_ITEM);
          String rssiQ;
          rssiQ += quality;
          item.replace("{v}", WiFi.SSID(indices[i]));
          item.replace("{r}", rssiQ);
#if defined(ESP8266)
          if (WiFi.encryptionType(indices[i]) != ENC_TYPE_NONE)
#else
          if (WiFi.encryptionType(indices[i]) != WIFI_AUTH_OPEN)
#endif
          {
            item.replace("{i}", "l");
          } else {
            item.replace("{i}", "");
          }
          // DEBUG_WM(item);
          page += item;
          delay(0);
        } else {
          DEBUG_WM(F("Skipping due to quality"));
        }
      }
      page += "<br/>";
    }

    page += FPSTR(WM_HTTP_FORM_START);
    char parLength[2];
    // add the extra parameters to the form
    for (int i = 0; i < _paramsCount; i++) {
      if (_params[i] == NULL) {
        break;
      }

      String pitem = FPSTR(WM_HTTP_FORM_PARAM);
      if (_params[i]->getID() != NULL) {
        pitem.replace("{i}", _params[i]->getID());
        pitem.replace("{n}", _params[i]->getID());
        pitem.replace("{p}", _params[i]->getPlaceholder());
        snprintf(parLength, 2, "%d", _params[i]->getValueLength());
        pitem.replace("{l}", parLength);
        pitem.replace("{v}", _params[i]->getValue());
        pitem.replace("{c}", _params[i]->getCustomHTML());
      } else {
        pitem = _params[i]->getCustomHTML();
      }

      page += pitem;
    }
    if (_params[0] != NULL) {
      page += "<br/>";
    }

    if (_sta_static_ip) {
      String item = FPSTR(WM_HTTP_FORM_PARAM);
      item.replace("{i}", "ip");
      item.replace("{n}", "ip");
      item.replace("{p}", "Static IP");
      item.replace("{l}", "15");
      item.replace("{v}", _sta_static_ip.toString());

      page += item;

      item = FPSTR(WM_HTTP_FORM_PARAM);
      item.replace("{i}", "gw");
      item.replace("{n}", "gw");
      item.replace("{p}", "Static Gateway");
      item.replace("{l}", "15");
      item.replace("{v}", _sta_static_gw.toString());

      page += item;

      item = FPSTR(WM_HTTP_FORM_PARAM);
      item.replace("{i}", "sn");
      item.replace("{n}", "sn");
      item.replace("{p}", "Subnet");
      item.replace("{l}", "15");
      item.replace("{v}", _sta_static_sn.toString());

      page += item;

      page += "<br/>";
    }

    page += FPSTR(WM_HTTP_FORM_END);
    page += FPSTR(WM_HTTP_SCAN_LINK);
  }

  page += FPSTR(WM_HTTP_END);

  server->sendHeader("Content-Length", String(page.length()));
  server->send(200, "text/html", page);

  DEBUG_WM(F("Sent config page"));
}

/** Handle the WLAN save form and redirect to WLAN config page again */
void WiFiManager::handleWifiSave() {
  DEBUG_WM(F("WiFi save"));

  // SAVE/connect here
  _ssid = server->arg("s").c_str();
  _pass = server->arg("p").c_str();

  DEBUG_WM("Network: " + _ssid);
  DEBUG_WM("Password: " + _pass);

  preferences.putString("ssid", _ssid);
  preferences.putString("pass", _pass);

  // parameters
  for (int i = 0; i < _paramsCount; i++) {
    if (_params[i] == NULL) {
      break;
    }
    // read parameter
    String value = server->arg(_params[i]->getID()).c_str();
    // store it in array
    value.toCharArray(_params[i]->_value, _params[i]->_length);
    DEBUG_WM(F("Parameter: "));
    DEBUG_WM(_params[i]->getID());
    DEBUG_WM(F("Value: "));
    DEBUG_WM(value);
  }

  if (server->arg("ip") != "") {
    DEBUG_WM(F("static ip: "));
    DEBUG_WM(server->arg("ip"));
    //_sta_static_ip.fromString(server->arg("ip"));
    String ip = server->arg("ip");
    optionalIPFromString(&_sta_static_ip, ip.c_str());
  }
  if (server->arg("gw") != "") {
    DEBUG_WM(F("static gateway: "));
    DEBUG_WM(server->arg("gw"));
    String gw = server->arg("gw");
    optionalIPFromString(&_sta_static_gw, gw.c_str());
  }
  if (server->arg("sn") != "") {
    DEBUG_WM(F("static netmask: "));
    DEBUG_WM(server->arg("sn"));
    String sn = server->arg("sn");
    optionalIPFromString(&_sta_static_sn, sn.c_str());
  }

  String page = FPSTR(WM_HTTP_HEAD);
  page.replace("{v}", "Credentials Saved");
  page += FPSTR(WM_HTTP_SCRIPT);
  page += FPSTR(WM_HTTP_STYLE);
  page += _customHeadElement;
  page += FPSTR(WM_HTTP_HEAD_END);
  page += FPSTR(WM_HTTP_SAVED);
  page.replace("{h}", getHostname());
  page.replace("{n}", _ssid);
  page += FPSTR(WM_HTTP_END);

  server->sendHeader("Content-Length", String(page.length()));
  server->send(200, "text/html", page);

  DEBUG_WM(F("Sent wifi save page"));

  connect = true;  // signal ready to connect/reset
}

/** Handle the info page */
void WiFiManager::handleInfo() {
  DEBUG_WM(F("Info"));

  String page = FPSTR(WM_HTTP_HEAD);
  page.replace("{v}", "Info");
  page += FPSTR(WM_HTTP_SCRIPT);
  page += FPSTR(WM_HTTP_STYLE);
  page += _customHeadElement;
  page += FPSTR(WM_HTTP_HEAD_END);
  page += F("<dl>");
  page += F("<dt>Chip ID</dt><dd>");
  page += ESP_getChipId();
  page += F("</dd>");
  page += F("<dt>Flash Chip ID</dt><dd>");
#if defined(ESP8266)
  page += ESP.getFlashChipId();
#else
  // TODO
  page += F("TODO");
#endif
  page += F("</dd>");
  page += F("<dt>IDE Flash Size</dt><dd>");
  page += ESP.getFlashChipSize();
  page += F(" bytes</dd>");
  page += F("<dt>Real Flash Size</dt><dd>");
#if defined(ESP8266)
  page += ESP.getFlashChipRealSize();
#else
  // TODO
  page += F("TODO");
#endif
  page += F(" bytes</dd>");
  page += F("<dt>Soft AP IP</dt><dd>");
  page += WiFi.softAPIP().toString();
  page += F("</dd>");
  page += F("<dt>Soft AP MAC</dt><dd>");
  page += WiFi.softAPmacAddress();
  page += F("</dd>");
  page += F("<dt>Station MAC</dt><dd>");
  page += WiFi.macAddress();
  page += F("</dd>");
  page += F("</dl>");
  page += FPSTR(WM_HTTP_END);

  server->sendHeader("Content-Length", String(page.length()));
  server->send(200, "text/html", page);

  DEBUG_WM(F("Sent info page"));
}

/** Handle the reset page */
void WiFiManager::handleReset() {
  DEBUG_WM(F("Reset"));

  String page = FPSTR(WM_HTTP_HEAD);
  page.replace("{v}", "Info");
  page += FPSTR(WM_HTTP_SCRIPT);
  page += FPSTR(WM_HTTP_STYLE);
  page += _customHeadElement;
  page += FPSTR(WM_HTTP_HEAD_END);
  page += F("Module will reset in a few seconds.");
  page += FPSTR(WM_HTTP_END);

  server->sendHeader("Content-Length", String(page.length()));
  server->send(200, "text/html", page);

  DEBUG_WM(F("Sent reset page"));
  delay(5000);
#if defined(ESP8266)
  ESP.reset();
#else
  ESP.restart();
#endif
  delay(2000);
}

void WiFiManager::handleNotFound() {
  if (captivePortal()) {  // If captive portal redirect instead of displaying
                          // the error page.
    return;
  }
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server->uri();
  message += "\nMethod: ";
  message += (server->method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server->args();
  message += "\n";

  for (uint8_t i = 0; i < server->args(); i++) {
    message += " " + server->argName(i) + ": " + server->arg(i) + "\n";
  }
  server->sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server->sendHeader("Pragma", "no-cache");
  server->sendHeader("Expires", "-1");
  server->sendHeader("Content-Length", String(message.length()));
  server->send(404, "text/plain", message);
}

/** Redirect to captive portal if we got a request for another domain. Return
 * true in that case so the page handler do not try to handle the request again.
 */
boolean WiFiManager::captivePortal() {
  if (!isIp(server->hostHeader())) {
    DEBUG_WM(F("Request redirected to captive portal"));
    server->sendHeader("Location",
                       String("http://") + toStringIp(WiFi.softAPIP()), true);
    server->send(302, "text/plain",
                 "");  // Empty content inhibits Content-length header so we
                       // have to close the socket ourselves.
    server->client()
        .stop();  // Stop is needed because we sent no content length
    return true;
  }
  return false;
}

// start up config portal callback
void WiFiManager::setAPCallback(void (*func)(WiFiManager *myWiFiManager)) {
  _apcallback = func;
}

// start up save config callback
void WiFiManager::setSaveConfigCallback(void (*func)(void)) {
  _savecallback = func;
}

// sets a custom element to add to head, like a new style tag
void WiFiManager::setCustomHeadElement(const char *element) {
  _customHeadElement = element;
}

// if this is true, remove duplicated Access Points - defaut true
void WiFiManager::setRemoveDuplicateAPs(boolean removeDuplicates) {
  _removeDuplicateAPs = removeDuplicates;
}

void WiFiManager::setDefaultHostname(String hostname) {
  _defaultHostname = hostname;
}

String WiFiManager::getHostname() { return _hostname; }

uint64_t WiFiManager::getMac() {
  uint64_t tmp;
  uint64_t tmp64;
  uint64_t mac_rev = 0;
  uint64_t n;
  uint64_t byte;
  tmp = ESP.getEfuseMac();

  tmp64 = (tmp & 0xFFFFFF);
  for (n = 0; n < 3; n++) {
    byte = ((tmp64 >> (n * 8)) & 0xFF);
    mac_rev |= (byte << ((5 - n) * 8));
  }
  tmp64 = ((tmp >> 24) & 0xFFFFFF);
  for (n = 0; n < 3; n++) {
    byte = ((tmp64 >> (n * 8)) & 0xFF);
    mac_rev |= (byte << ((2 - n) * 8));
  }
  return mac_rev;
}

String WiFiManager::getMacAsString(bool insertColons) {
  uint64_t mac64 = getMac();
  String macStr =
      String((uint16_t)(mac64 >> 32), HEX) + String((uint32_t)mac64, HEX);
  int length = macStr.length();

  macStr.toUpperCase();

  if (insertColons) {
    for (int i = length - 2; i > 0; i -= 2) {
      macStr = macStr.substring(0, i) + ":" + macStr.substring(i);
    }
  }

  return macStr;
}

void WiFiManager::readHostname() {
  String macStr;
  uint64_t mac64;
  if (preferences.getBool("useHostname", false)) {
    _hostname = preferences.getString("hostname", "ESP");
  } else {
    if (_appendMacToHostname) {
      macStr = getMacAsString(false);
      _hostname = (_defaultHostname + "-" + macStr);
    } else {
      _hostname = _defaultHostname;
    }
  }
}

void WiFiManager::readNetworkCredentials() {
  _ssid = preferences.getString("ssid", "ESP");
  _pass = preferences.getString("pass", "ESP");
}

void WiFiManager::appendMacToHostname(bool value) {
  _appendMacToHostname = value;
  readHostname();
}

template <typename Generic>
void WiFiManager::DEBUG_WM(Generic text) {
  if (_debug) {
    Serial.print("*WM: ");
    Serial.println(text);
  }
}

int WiFiManager::getRSSIasQuality(int RSSI) {
  int quality = 0;

  if (RSSI <= -100) {
    quality = 0;
  } else if (RSSI >= -50) {
    quality = 100;
  } else {
    quality = 2 * (RSSI + 100);
  }
  return quality;
}

/** Is this an IP? */
boolean WiFiManager::isIp(String str) {
  for (int i = 0; i < str.length(); i++) {
    int c = str.charAt(i);
    if (c != '.' && (c < '0' || c > '9')) {
      return false;
    }
  }
  return true;
}

/** IP to String? */
String WiFiManager::toStringIp(IPAddress ip) {
  String res = "";
  for (int i = 0; i < 3; i++) {
    res += String((ip >> (8 * i)) & 0xFF) + ".";
  }
  res += String(((ip >> 8 * 3)) & 0xFF);
  return res;
}
