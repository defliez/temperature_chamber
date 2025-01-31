/*
   Temperature Chamber
   Ver. 0003

    This project builds upon the original work by Adam, who designed the hardware and implemented
    the first iteration of the software. The chamber consists of a ceramic heating element, an 
    air-blower for cooling and two 1-wire thermocouple sensors. It can reach and maintain temperatures
    up to 100 deg C, providing a standardized environment for stress-testing boards. 
    Note: The chamber does not control humidity.

    Key Use Case:
    The goal of this chamber is to create a standardised mechanism and benchmark for measuring board 
    performance. For example, it helps answer questions like "How long can a board
    operate at 60C before failure?" It prioritizes accessibility and replicability by using cost-effective,
    off-the-shelf components.

    Original Design (by Adam):
    - Utilized an Arduino Uno R3 with a custom shield including solid-state relays for heater and blower.
    - Blower sourced from an inflatable mattress, featuring mechanical airflow shutoff when inactive.
    - Designed a basic state machine allowing manual control via physical buttons and relays.

    Updates by Valentino:
    - Integrated remote serial communication, enabling control via a Python-based application.
    - Modernized and enhanced the state machine to suport queueing and running complex test sequences.
    - Introduced JSON-based communication protocol for:
        - Receiving and sending handshake and ping to external application.
        - Receiving and parsing test parameters (e.g., target temperature, duration).
        - Receiving and parsing commands from external application (e.g., RESET, SET_TEMP, RUN_QUEUE etc.).
    - Added RTC support for precise timestamping and tracking of ping consistency.
    - Implemented robust error handling, including emergency stop for serial disconnections.
    - Improved feedback via real-time serial updates on temperature, test status, and queued tests.

    Software Structure:
    - A state machine manages states like HEATING, COOLING, EVALUATE, and EMERGENCY STOP, supporting both 
      manual and remote operation.
    - The chamber can queue and execute multiple test scenarios, each defined by a temperature and
      duration.
    - Provides serial feedback for real-time monitoring, including test progress and machine state.

    Acknowledgments:
    - Adam for hardware design, the custom shield, and the original state machine implementation.
    - Valentino for modernizing functionality, integrating remote control, and enhancing workflows.

    For inquiries or further development, please contact via the provided channels.

Authors:
 * Adam Harb, <adam.harb@hotmail.com>
 * Valentino Glave, <valentinoglave@protonmail.com>
 * David Cuartielles, <d.cuartielles@arduino.cc>

 (cc-sa-by-nc) 2024 Arduino, Sweden

 */

// Include Libraries
#include <DallasTemperature.h>
#include <OneWire.h>
#include <LiquidCrystal_I2C.h>
#include <EduIntro.h>
#include <ArduinoJson.h>
#include <RTC.h>

// Defines
#define ONE_WIRE_BUS_1      A0  // temp sensor 1
#define ONE_WIRE_BUS_2      A1  // temp sensor 2
#define RELAY_COOLER        2
#define RELAY_HEATER        3
#define BUTTON_INCREMENT    5
#define BUTTON_DECREMENT    4
#define SWITCH_START        6
#define SWITCH_SYSTEM_STATE 7

// States for the state machine
#define IDLE                0
#define HEATING             1
#define COOLING             2
#define EVALUATE            3
#define EMERGENCY_STOP      4

// Define default temperature limits
#define TEMPERATURE_MAX     100
#define TEMPERATURE_MIN     0

// Default room temperature to set in case of emergency stop
#define ROOM_TEMP           22  

// Overriding default temperature limit
bool tempOverride = false;
float tempLimit = TEMPERATURE_MAX;

// Setup OneWire instances to communicate with any OneWire devices
OneWire oneWire1(ONE_WIRE_BUS_1);
OneWire oneWire2(ONE_WIRE_BUS_2);

// Pass the oneWire references to DallasTemperature
DallasTemperature sensors1(&oneWire1);
DallasTemperature sensors2(&oneWire2);

// Instantiate the lcd
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Instantiate the buttons
Button buttonIncrease(BUTTON_INCREMENT);
Button buttonDecrease(BUTTON_DECREMENT);

// Instantiate the switches
Button switchStart(SWITCH_START);
Button switchSystem(SWITCH_SYSTEM_STATE);

// Instantiate the relays
Led cooler(RELAY_COOLER);
Led heater(RELAY_HEATER);

// PWM for heater and cooler
unsigned long periodHeater = 0;
unsigned long periodCooler = 0;
int dutyCycleHeater = 0;
int dutyCycleCooler = 0;

// State machine
int status = EMERGENCY_STOP;

// global flags to store switch states
bool systemSwitchState = false;
bool startSwitchState = false;

// Global flags to track if certain logs have been printed
bool printedWaiting = false;
bool printedRunning = false;

struct Sequence {
    float targetTemp;
    unsigned long duration;
};

struct Test {
    std::vector<Sequence> sequences;
    String sketch;
    String expectedOutput;
};

// Test variables
bool isTestRunning = false;
Test currentTest;
int currentSequenceIndex = 0;
unsigned long sequenceStartTime = 0;
unsigned long currentDuration = 0;

// queue for tests
std::vector<Test> testQueue;
std::vector<String> testNames;
int currentTestIndex = 0;
String currentTestName = "";

// avoid printing ad infinitum in condition checks in loop
bool printedTestsCleared = false;
bool printedNoPing = false;

// JSON Buffer for parsing
char incomingString[1024];
StaticJsonDocument<2048> jsonBuffer;

float temperatureThreshold = 0;

// struct for state of the heating/cooling system
struct ChamberState {
    bool isHeating;
    bool isCooling;
    float temperatureRoom;
    int temperatureDesired;
    int longHeatingFlag;
    unsigned long lastHeaterOnTime;
    unsigned long lastCoolerOnTime;
};

ChamberState chamberState;

String lastShutdownCause = "Unknown";
String lastHeatingTime = "";

unsigned long lastPingTime = 0;
const unsigned long TIMEOUT_DURATION = 300000;

bool displayingEmergency = false;

void setup() {
    // Initialise Serial
    Serial.begin(9600);

    // Initialize RTC
    if (!RTC.begin()) {
        Serial.println("RTC initialization failed!");
    }

    if (!RTC.isRunning()) {
        RTCTime defaultTime(2024, Month::NOVEMBER, 1, 10, 26, 0, DayOfWeek::FRIDAY, SaveLight::SAVING_TIME_INACTIVE);
        RTC.setTimeIfNotRunning(defaultTime);
    }

    // Initialise thermocouples
    sensors1.begin();
    sensors2.begin();

    // Initialise LCD
    lcd.init();

    // Initiate chamber state variables
    chamberState.temperatureRoom = getTemperature();
    chamberState.temperatureDesired = 0;
    chamberState.longHeatingFlag = 0;
    chamberState.isHeating = false;
    chamberState.isCooling = false;
    chamberState.lastHeaterOnTime = millis();
    chamberState.lastCoolerOnTime = millis();

    readAndParseSerial();
}

String getCurrentTimestamp() {
    RTCTime now;
    if (RTC.getTime(now)) {
        return now.toString();
    } else {
        return "Error: Unable to get time";
    }
}

String getLastHeatingTime() {
    return lastHeatingTime.isEmpty() ? "N/A" : lastHeatingTime;
}

void setInitialTimestamp(JsonObject& commandParams) {
    if (commandParams.containsKey("timestamp")) {
        String timestamp = commandParams["timestamp"].as<String>();
        int year, month, day, hour, minute, second;
        sscanf(timestamp.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second);

        RTCTime initialTime(day, static_cast<Month>(month - 1), year, hour, minute, second, DayOfWeek::SUNDAY, SaveLight::SAVING_TIME_INACTIVE);
        if (RTC.setTime(initialTime)) {
            Serial.println("RTC updated with initial timestamp from python app.");
        } else {
            Serial.println("Error setting RTC time.");
        }
    } else {
        Serial.println("Timestamp key not found in commandParams.");
    }
}

void sendHandshake() {
    StaticJsonDocument<512> handshakeDoc;
    handshakeDoc["handshake"]["timestamp"] = getCurrentTimestamp();
    handshakeDoc["handshake"]["machine_state"] = getMachineState();
    handshakeDoc["handshake"]["last_shutdown_cause"] = lastShutdownCause;
    handshakeDoc["handshake"]["last_heat_time"] = getLastHeatingTime();

    serializeJson(handshakeDoc, Serial);
    Serial.println();
}

float getTemperature() {
    float roomTemperature = 0;
    sensors1.setWaitForConversion(false);
    sensors2.setWaitForConversion(false);
    sensors1.requestTemperatures();
    sensors2.requestTemperatures();
    // get average room_temperature
    roomTemperature = (sensors1.getTempCByIndex(0) + sensors2.getTempCByIndex(0)) / 2;
    return roomTemperature;
}

void displayLCD(float tempRoom, int tempDesired) {
    lcd.backlight();  // turn off backlight
    lcd.display();
    lcd.setCursor(0, 0);
    lcd.print("Room: ");
    lcd.print(tempRoom);
    lcd.print(" C");
    lcd.setCursor(0, 1);
    lcd.print("Goal: ");
    if (tempDesired == 0) {
        lcd.print("-");
    } else {
        lcd.print(tempDesired);
        lcd.print(" C");
    }
}

void displayLCDEmergency() {
    lcd.backlight();  // turn off backlight
    lcd.display();
    lcd.setCursor(0, 0);
    lcd.print("CONNECTION LOST");
    lcd.setCursor(0, 1);
    lcd.print("EMERGENCY STOP");
}

void displaySerial() {
    Serial.print(F("Room_temp: "));
    Serial.print(chamberState.temperatureRoom);
    Serial.print(F(" | Desired_temp: "));
    if (chamberState.temperatureDesired == 0) {
        Serial.print("-");
    } else {
        Serial.print(chamberState.temperatureDesired);
    }
    Serial.print(F(" | Heater: "));
    Serial.print(chamberState.isHeating ? 1 : 0);
    Serial.print(F(" | Cooler: "));
    Serial.print(chamberState.isCooling ? 1 : 0);
    Serial.print(F(" | LH Indicator: "));
    Serial.println(chamberState.longHeatingFlag);
}

void displayLCDOff() {
    lcd.noBacklight();
    lcd.noDisplay();
}

bool isTemperatureReached(float targetTemp, float currentTemp) {
    return currentTemp >= targetTemp - 0.5 && currentTemp <= targetTemp + 5;
}

bool holdForPeriod(unsigned long duration) {
   return millis() - sequenceStartTime >= duration;
}

int getTimeLeft(unsigned long duration, Sequence currentSequence) {
    if (isTestRunning && isTemperatureReached(currentSequence.targetTemp, chamberState.temperatureRoom)) {
        int timeLeft = (duration - (millis() - sequenceStartTime)) / 1000;
        return timeLeft;
    } else {
        return 0;
    }
}

// dutyCycle has to be 0..100
void controlRelay(Led& relay, int dutyCycle, unsigned long period, unsigned long& lastOnTimer) {
    unsigned long currentTime = millis();
    unsigned long elapsedTime = currentTime - lastOnTimer;
    if (elapsedTime > (dutyCycle * period) / 100) {
        relay.off();
    } else {
        relay.on();
    }
    if (elapsedTime > period) {
        lastOnTimer = currentTime;
    }
}

void queueTest(const Test& test, const String& testName) {
    testQueue.push_back(test);
    testNames.push_back(testName);
}

// parse tests and add to queue by name
void parseAndQueueTests(JsonObject& tests) {
    for (JsonPair testPair : tests) {
        JsonObject testJson = testPair.value().as<JsonObject>();
        // check for required fields
        if (!testJson.containsKey("chamber_sequences")) {
            Serial.println("Error: Missing 'chamber_sequences' in test data");
            continue;
        }
        Test newTest;
        String testName = testPair.key().c_str();
        JsonArray sequences = testJson["chamber_sequences"];

        newTest.sketch = testJson["sketch"].as<String>();
        newTest.expectedOutput = testJson["expected_output"].as<String>();

        for (JsonObject sequence : sequences) {
            if (!sequence.containsKey("temp") || !sequence.containsKey("duration")) {
                Serial.println("Error: Missing 'temp' or 'duration' in JSON sequence");
                break;  // Other sequences will not be queued if a sequence is missing key values
            }
            Sequence newSequence;

            newSequence.targetTemp = sequence["temp"].as<float>();
            newSequence.duration = sequence["duration"].as<unsigned long>();
            newTest.sequences.push_back(newSequence);
        }
        queueTest(newTest, testName);
    }
}

void runQueue() {
    if (!isTestRunning && testQueue.size() > 0) {
        runNextTest();
    }
}

void runNextTest() {
    if (currentTestIndex < testQueue.size()) {
        currentTest = testQueue[currentTestIndex];
        isTestRunning = true;
        currentSequenceIndex = 0;
        currentTestName = testNames[currentTestIndex];
        setTemperature(currentTest.sequences[currentSequenceIndex].targetTemp);
        status = EVALUATE;
    }
}

void clearTests() {
    isTestRunning = false;
    currentSequenceIndex = 0;
    currentTestIndex = 0;
    currentTestName = "";
    sequenceStartTime = 0;
    currentDuration = 0;
    chamberState.temperatureDesired = 0;
    chamberState.isHeating = false;
    chamberState.isCooling = false;
    chamberState.longHeatingFlag = 0;

    // clear queued tests
    testQueue.clear();
    testNames.clear();
    currentTest.sequences.clear();

    lcd.clear();
}

void parseAndRunManualSet(JsonObject& commandParams) {
        float temp = commandParams["temp"];
        unsigned long duration = commandParams["duration"];
        setTemperature(temp);
        Serial.print("Manual temp set to ");
        Serial.println(temp);
        Serial.print("Duration: ");
        Serial.println(duration);
}

void parseAndRunCommands(JsonObject& commands) {
    for (JsonPair commandPair : commands) {
        String command = commandPair.key().c_str();
        JsonObject commandParams = commandPair.value().as<JsonObject>();

        if (command == "PING") {
            sendPingResponse();
            lastPingTime = millis();
            printedNoPing = false;
            if (displayingEmergency) {
                displayingEmergency = false;
                lcd.clear();
            }
        } 
        if (!systemSwitchState) {
            return;
        }
        if (command == "GET_TEST_QUEUE") {
            sendQueue();
        } else if (command == "RUN_QUEUE") {
            runQueue();
        } else if (command == "SET_TEMP") {
            clearTests();
            parseAndRunManualSet(commandParams);

            // Handle temporary override
            if (commandParams.containsKey("override") && commandParams["override"].as<bool>()) {
                tempOverride = true;
                tempLimit = commandParams["temp"].as<float>();
            } else {
                tempOverride = false;
                tempLimit = TEMPERATURE_MAX;
            }
        } else if (command == "RESET") {
            clearTests();
            displayingEmergency = false;
            status = IDLE;
        } else if (command == "EMERGENCY_STOP") {
            runEmergencyStop();
            sendPingResponse();
            Serial.println("Emergency Stop initiated via command.");
        } 
    }
}

void runEmergencyStop() {
    clearTests();
    if (chamberState.temperatureRoom > ROOM_TEMP) {
        setTemperature(ROOM_TEMP);
        status = COOLING;
    }
}

void sendPingResponse() {
    StaticJsonDocument<512> responseDoc;
    Sequence currentSequence = currentTest.sequences[currentSequenceIndex];

    responseDoc["ping_response"]["alive"] = true;
    responseDoc["ping_response"]["timestamp"] = getCurrentTimestamp();
    responseDoc["ping_response"]["machine_state"] = getMachineState();
    responseDoc["ping_response"]["current_temp"] = chamberState.temperatureRoom;

    JsonObject testStatus = responseDoc["ping_response"].createNestedObject("test_status");
    testStatus["is_test_running"] = isTestRunning;
    testStatus["current_test"] = currentTestName;
    testStatus["current_sequence"] = currentSequenceIndex + 1;
    testStatus["desired_temp"] = chamberState.temperatureDesired;
    testStatus["current_duration"] = currentDuration;
    testStatus["time_left"] = getTimeLeft(currentDuration, currentSequence);
    testStatus["queued_tests"] = testQueue.size();

    serializeJson(responseDoc, Serial);
    Serial.println();
}

void sendQueue() {
    StaticJsonDocument<512> responseDoc;
    JsonObject queueObject = responseDoc.createNestedObject("queue");
    JsonObject testsObject = queueObject.createNestedObject("tests");

    for (int i = 0; i < testQueue.size(); i++) {
        JsonObject testObject = testsObject.createNestedObject(testNames[i]);
        JsonArray sequencesArray = testObject.createNestedArray("chamber_sequences");

        for (const Sequence& sequence : testQueue[i].sequences) {
            JsonObject sequenceObject = sequencesArray.createNestedObject();
            sequenceObject["temp"] = sequence.targetTemp;
            sequenceObject["duration"] = sequence.duration;
        }
        testObject["sketch"] = testQueue[i].sketch;
        testObject["expected_output"] = testQueue[i].expectedOutput;
    }

    serializeJson(responseDoc, Serial);
    Serial.println();
}

String getMachineState() {
    switch (status) {
        case IDLE:
            return "IDLE";
        case HEATING:
            return "HEATING";
        case COOLING:
            return "COOLING";
        case EVALUATE:
            return "EVALUATE";
        case EMERGENCY_STOP:
            return "EMERGENCY_STOP";
        default:
            return "UNKNOWN";
    }
}

void parseTextFromJson(JsonDocument& doc) {
    if (doc.containsKey("handshake")) {
        JsonObject handshake = doc["handshake"];
        setInitialTimestamp(handshake);
        sendHandshake();
        printedTestsCleared = false;
        lastShutdownCause = "";
    } else if (doc.containsKey("tests") && systemSwitchState) {     // if json consists of tests
        JsonObject test = doc["tests"];
        parseAndQueueTests(test);
    } else if (doc.containsKey("commands")) {   // if json consists of commands
        JsonObject commands = doc["commands"];
        parseAndRunCommands(commands);
    } else {
        Serial.println("Error: Invalid JSON format");
    }
}

// Manual control of temperature via physical buttons
void changeTemperature() {  
    if (buttonIncrease.read()== HIGH && buttonDecrease.read()== LOW) chamberState.temperatureDesired += 5;
    if (buttonDecrease.read()== HIGH && buttonIncrease.read()== LOW) chamberState.temperatureDesired -= 5;
    if (chamberState.temperatureDesired >= TEMPERATURE_MAX) {
        chamberState.temperatureDesired = TEMPERATURE_MAX;
    }
    else if (chamberState.temperatureDesired <= TEMPERATURE_MIN && chamberState.temperatureDesired != 0)  {
        chamberState.temperatureDesired = TEMPERATURE_MIN;
    }
}

void setTemperature(float temp) {
    float currentLimit = tempOverride ? tempLimit : TEMPERATURE_MAX;
    if (temp >= currentLimit) {
        chamberState.temperatureDesired = currentLimit;
    } else if (temp <= TEMPERATURE_MIN) {
        chamberState.temperatureDesired = TEMPERATURE_MIN;
        Serial.println("Specified temperature is lower than the minimum allowed temperature");
        Serial.println("Setting temperature to " + String(TEMPERATURE_MIN) + "°C");
    } else {
        chamberState.temperatureDesired = temp;
        Serial.print("Setting temperature to ");
        Serial.print(temp);
        Serial.println("°C");
    }
}

// centralized switch handling
void updateSwitchStates() {
    systemSwitchState = switchSystem.read() == LOW;
    startSwitchState = switchStart.read() == LOW;
}

void handleIdleState() {
    displayLCD(chamberState.temperatureRoom, chamberState.temperatureDesired);

    if (!systemSwitchState) {
        status = EMERGENCY_STOP;
        return;
    }
    if (startSwitchState) {
        status = EVALUATE;
    }
    // allow manual control of temperature from buttons
    changeTemperature();

    // turn off all outputs
    heater.off();
    cooler.off();
    chamberState.isHeating = false;
    chamberState.isCooling = false;
    chamberState.longHeatingFlag = 0;
}

void handleHeatingState() {
    if (!systemSwitchState || !startSwitchState) {
        status = IDLE;
        return;
    }
    cooler.off();

    if(temperatureThreshold > -0.1) {
        chamberState.longHeatingFlag = 0;
        chamberState.isHeating = false;
        lastHeatingTime = getCurrentTimestamp();    // capture current timestamp for handshake
        status = EVALUATE;
    } else {
        adjustDutyCycleAndPeriod(temperatureThreshold, dutyCycleHeater, periodHeater, chamberState.longHeatingFlag, true);
        controlRelay(heater, dutyCycleHeater, periodHeater, chamberState.lastHeaterOnTime);
        chamberState.isHeating = true;
    }
}

void handleCoolingState() {
    if (!systemSwitchState || !startSwitchState) {
        status = IDLE;
        return;
    }
    heater.off();

    if(temperatureThreshold < 0.1) {
        chamberState.isCooling = false;
        status = EVALUATE;
    } else {
        adjustDutyCycleAndPeriod(temperatureThreshold, dutyCycleCooler, periodCooler, chamberState.longHeatingFlag, false);
        controlRelay(cooler, dutyCycleCooler, periodCooler, chamberState.lastCoolerOnTime);
        chamberState.isCooling = true;
    }
}

void adjustDutyCycleAndPeriod(float threshold, int& dutyCycle, unsigned long& period, int& longHeatingFlag, bool isHeating) {
    if (isHeating) {
        if (threshold < -4) {
            dutyCycle = 100;
            period = (threshold < -8) ? 120000 : 60000;
            longHeatingFlag = 1;
        } else {
            dutyCycle = (longHeatingFlag) ? 0 : 80;
            period = 25000; // on for 20 seconds and off for 5
        }
    } else {
        if (threshold > 0.1) {
            dutyCycle = 100;
            period = 2000;
        }
    }
}

void handleEvaluateState() {
    if (!systemSwitchState) {
        status = EMERGENCY_STOP;
        return;
    }
    if (!startSwitchState) {
        status = IDLE;
        return;
    }
    if (chamberState.temperatureDesired == 0) {
        return;
    }
    if(temperatureThreshold > 0.4) {
        status = COOLING;
    } else if(temperatureThreshold < -0.1) {
        status = HEATING;
    }
    readAndParseSerial();
}

void handleEmergencyStopState() {
    heater.off();
    cooler.off();
    chamberState.isHeating = false;
    chamberState.isCooling = false;
    chamberState.longHeatingFlag = 0;

    if (switchSystem.held()) {
        status = IDLE;
    }
}

void runTestSequence() {
    runCurrentSequence();
    if (currentSequenceIndex >= currentTest.sequences.size()) {
        tempOverride = false;   // Reset temperature override
        tempLimit = TEMPERATURE_MAX;    // Reset temperature limit
        Serial.print("Test completed: ");
        Serial.println(currentTestName);
        isTestRunning = false;

        // Proceed to next test if available
        if (currentTestIndex >= testQueue.size()) {
            Serial.print("All tests completed!");
            clearTests();
            return;
        }
        currentTestIndex++;
        runNextTest();
    }
}

void runCurrentSequence() {
    if (currentSequenceIndex >= currentTest.sequences.size()) {
        status = EVALUATE;
        return;
    }

    Sequence currentSequence = currentTest.sequences[currentSequenceIndex];
    float targetTemp = currentSequence.targetTemp;
    unsigned long duration = currentSequence.duration;

    // store current duration for the reporting (PING)
    currentDuration = duration;

    if (!printedRunning) {
        Serial.print("Running sequence: Target temp = ");
        Serial.print(targetTemp);
        Serial.print("°C Duration = ");
        Serial.println(String(duration / 60000) + " minutes");
        printedRunning = true;
    }

    // check if target temperature is reached
    if (!isTemperatureReached(targetTemp, chamberState.temperatureRoom)) {
        if (!printedWaiting) {
            Serial.println("Waiting for target temperature to be reached...");
            printedWaiting = true;
        }
        return;
    }

    // Start the timer when the target temperature is reached
    if (sequenceStartTime == 0) {
        sequenceStartTime = millis();
        Serial.println("Target temperature reached! Starting timer.");
    }

    if (holdForPeriod(duration)) {
        Serial.println("Sequence completed.");
        currentSequenceIndex++;
        sequenceStartTime = 0;
        printedWaiting = false;
        printedRunning = false;

        // Set the temperature for the next sequence, if available
        if (currentSequenceIndex < currentTest.sequences.size()) {
            setTemperature(currentTest.sequences[currentSequenceIndex].targetTemp);
        }
    }
}


void readAndParseSerial() {
    if (Serial.available() > 0) {
        // Read the incoming data in chunks instead of one character at a time
        int len = Serial.readBytesUntil('\n', incomingString, sizeof(incomingString) - 1);
        incomingString[len] = '\0'; // null-terminate the string

        DeserializationError error = deserializeJson(jsonBuffer, incomingString);
        if (error) {
            Serial.print(F("deserializeJson() failed: "));
            Serial.println(error.f_str());
            Serial.println("Resetting buffer.");
            incomingString[0] = '\0';
            return;
        }
        parseTextFromJson(jsonBuffer);
        incomingString[0] = '\0';
    }
    jsonBuffer.clear();
}

unsigned long currentMillis = 0;
unsigned long lastUpdate = 0;
unsigned long updateInterval = 500;

void loop() {  
    currentMillis = millis();

    // Check for timeout (5 minutes without ping)
    if (currentMillis - lastPingTime > TIMEOUT_DURATION) {
        if (!printedNoPing) {
            Serial.println("No ping received for 5 minutes. Resetting and shutting down.");
            printedNoPing = true;
        }
        status = EMERGENCY_STOP;
        lastShutdownCause = "Lost connection";
        if (!printedTestsCleared) {
            printedTestsCleared = true;
        }
        displayingEmergency = true;
        runEmergencyStop();
    }

    // Update switch states and temperature readings
    updateSwitchStates();
    chamberState.temperatureRoom = getTemperature();
    if (chamberState.temperatureDesired != 0) {
        temperatureThreshold = chamberState.temperatureRoom - chamberState.temperatureDesired;
    }


    if (systemSwitchState) {
        if (displayingEmergency) {
            displayLCDEmergency();
        } else {
            displayLCD(chamberState.temperatureRoom, chamberState.temperatureDesired);
        }
        if (currentMillis - lastUpdate >= updateInterval) {
            lastUpdate = currentMillis;
        }
    } else {
        displayLCDOff();
    }

    readAndParseSerial();   // check serial input for new tests or commands
    if (isTestRunning) {
        runTestSequence();
    }

    switch (status) {
        case IDLE:
            handleIdleState();
            break;
        case HEATING:
            handleHeatingState();
            break;
        case COOLING:
            handleCoolingState();
            break;
        case EVALUATE:
            handleEvaluateState();
            break;
        case EMERGENCY_STOP:
            handleEmergencyStopState();
            break;
        default:
            Serial.println("Invalid state detected!");
    }
}

