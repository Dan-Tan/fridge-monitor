#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <math.h>

/*
 * Bushman Fridge Fan Controller - ESP32-C6
 * 
 * Hardware Config:
 * - Battery Divider: 20k Ohm (Top) / 4.7k Ohm (Bottom)
 * - Thermistors: 10k NTC 3950 (with 10k Pull-up to 3.3V)
 * - BLE Service: Nordic UART Service (NUS)
 */

// Pin Definitions
const int PIN_FRIDGE_VOLT = 5;
const int PIN_COMPRESSOR_STAT = 4;
const int PIN_FREEZER_TEMP = 6;
const int PIN_FRIDGE_TEMP = 7;
const int PIN_LDR = 16;
const int PIN_FAN_ON = 17;

// Calibration
const float V_REF = 3.3;
const float ADC_MAX = 4095.0; 
const float BATT_MULTIPLIER = (V_REF / ADC_MAX) * ((20000.0 + 4700.0) / 4700.0);
const float COMP_MULTIPLIER = (V_REF / ADC_MAX) * ((10000.0 + 10000.0) / 10000.0);

// Thermistor Constants (NTC 3950)
const float SERIES_RESISTOR = 10000.0;
const float NOMINAL_RESISTANCE = 10000.0;
const float NOMINAL_TEMPERATURE = 25.0;
const float B_COEFFICIENT = 3950.0;

// BLE Setup
BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println(">>> App Connected");
    };
    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println(">>> App Disconnected");
    }
};

float readCelsius(int pin) {
    int raw = analogRead(pin);
    if (raw == 0 || raw == 4095) return -99.9;
    float resistance = SERIES_RESISTOR / (ADC_MAX / (float)raw - 1);
    float steinhart = log(resistance / NOMINAL_RESISTANCE) / B_COEFFICIENT;
    steinhart += 1.0 / (NOMINAL_TEMPERATURE + 273.15);
    return (1.0 / steinhart) - 273.15;
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n--- BUSHMAN FRIDGE MONITOR ---");

    pinMode(PIN_FAN_ON, OUTPUT);
    digitalWrite(PIN_FAN_ON, LOW);
    analogReadResolution(12);

    // Initialize BLE
    BLEDevice::init("Fridge Monitor");
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);

    // TX Characteristic (ESP -> Phone)
    pTxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_TX,
        BLECharacteristic::PROPERTY_NOTIFY
    );
    pTxCharacteristic->addDescriptor(new BLE2902());

    // RX Characteristic (Phone -> ESP) - Required for Serial Profile
    BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID_RX,
        BLECharacteristic::PROPERTY_WRITE
    );

    pService->start();

    // Start Advertising
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pServer->getAdvertising()->start();
    
    Serial.println("BLE Active. Waiting for connection...");
}

// Thresholds
const int LDR_THRESHOLD = 1000; // Adjust based on ambient light in the fridge

void loop() {
    // 1. Connection Management
    if (!deviceConnected && oldDeviceConnected) {
        delay(500); 
        pServer->startAdvertising(); 
        Serial.println("Restarting Advertising...");
        oldDeviceConnected = deviceConnected;
    }
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
    }

    // 2. Sensory Inputs
    float batteryVoltage = analogRead(PIN_FRIDGE_VOLT) * BATT_MULTIPLIER;
    float compVoltage = analogRead(PIN_COMPRESSOR_STAT) * COMP_MULTIPLIER;
    float fridgeTemp = readCelsius(PIN_FRIDGE_TEMP);
    float freezerTemp = readCelsius(PIN_FREEZER_TEMP);
    bool isCompressorRunning = (compVoltage < 2.5);
    bool isDoorOpen = (analogRead(PIN_LDR) > LDR_THRESHOLD); // Light = Open, Dark = Closed

    // 3. Logic
    if (isCompressorRunning && !isDoorOpen) {
        digitalWrite(PIN_FAN_ON, HIGH);
    } else {
        digitalWrite(PIN_FAN_ON, LOW);
    }

    // 4. Construct Data String
    int ldrRaw = analogRead(PIN_LDR);
    String output = "V:" + String(batteryVoltage, 1) + 
                   " Frg:" + String(fridgeTemp, 0) + 
                   "C Frz:" + String(freezerTemp, 0) + 
                   "C Comp:" + (isCompressorRunning ? "ON" : "OFF") + 
                   " Door:" + (isDoorOpen ? "OPEN" : "CLOSED") +
                   " LDR:" + String(ldrRaw) + "\n";

    // 5. Output
    Serial.print(output);
    if (deviceConnected) {
        pTxCharacteristic->setValue(output.c_str());
        pTxCharacteristic->notify();
    }

    delay(2000);
}
