#include <WiFiManager.h>          //https://github.com/admarschoonen/WiFiManager
#include <ESPmDNS.h>

String hostname;
WiFiServer server(80);

void setup() {
    // put your setup code here, to run once:
    Serial.begin(115200);

    //WiFiManager
    //Local intialization. Once its business is done, there is no need to keep it around
    WiFiManager wifiManager;
    
    wifiManager.configure("my-esp");

    //fetches ssid and pass and tries to connect
    //if it does not connect it starts an access point
    //and goes into a blocking loop awaiting configuration
    if (!wifiManager.autoConnect()) {
      Serial.println("failed to connect and hit timeout");
      //reset and try again, or maybe put it to deep sleep
      ESP.restart();
      delay(1000);
    }

    //if you get here you have connected to the WiFi
    Serial.print("connected with address: ");
    Serial.println(WiFi.localIP());

    hostname = wifiManager.getHostname();
    if (!MDNS.begin(hostname.c_str())) {
      Serial.println("Error setting up MDNS responder!");
      while(1) {
        delay(1000);
      }
    }
    Serial.println("mDNS responder started");

    //keep LED on
    digitalWrite(LED_BUILTIN, HIGH);

    server.begin();
    MDNS.addService("http", "tcp", 80);
}

void loop() {
    // put your main code here, to run repeatedly:
    
}
