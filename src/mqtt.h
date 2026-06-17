#pragma once

#include <WiFi.h>
#include <PubSubClient.h>
#include "config.h"

// ─── Global PubSubClient ───────────────────────────────────────────────────
extern PubSubClient mqtt;
extern WiFiClient wifiClient;

// ─── Functions ─────────────────────────────────────────────────────────────
void mqttApplyServerConfig();
void connectMQTT();
void mqttFlushPostConnect();
void mqttCallback(char* topic, byte* payload, unsigned int len);
void pub(const String& subtopic, const String& value);

// ─── Status / Discovery ────────────────────────────────────────────────────
void publishStatus();
void publishDiscovery();
void mqttStatusLoop();     // periodic status publish (10s)
void setupMQTT();           // wire callbacks