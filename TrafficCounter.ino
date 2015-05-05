#include <SPI\SPI.h>
#include <SD.h>
#include <ProgmemString.h>
#include <StraightBuffer.h>
#include <PirMotion.h>
#include <Logging.h>
#include <JsonParser.h>
#include <JsonGenerator.h>
#include <Maxbotix.h>
#include <SimpleTimer.h>
#include <CommandHandler.h>
#include "config.h"

using namespace ArduinoJson::Generator;

const float TRAFFIC_COUNTER_VERSION = 1;

//////////////////////////////////////////////////////////////////////////
// Traffic Counter
// 
// Uses non-invasive sensors to count vehicular and pedestrian traffic 
// from a distance. 
//
//////////////////////////////////////////////////////////////////////////
// Config
//////////////////////////////////////////////////////////////////////////

Maxbotix rangeSensor(RANGEFINDER_AN_PIN, Maxbotix::AN, Maxbotix::XL);
float rangeBaseline;
float rangeVariance;

bool motionDetected;
long timeOfLastMotion;

SimpleTimer timer;
int rangeTimerID;
int motionTimerID;
int logTimerID;

JsonObject<6> trafficEntry;

char _commandCache[COMMAND_CACHE_SIZE];
CommandHandler commandHandler(_commandCache, COMMAND_CACHE_SIZE);

File trafficLog;
long entryNumber;

//////////////////////////////////////////////////////////////////////////
// Arduino Functions
//////////////////////////////////////////////////////////////////////////

/**
* Startup code - Run once
*/
void setup()
{
	// Start up comms
	Log.Init(LOGGER_LEVEL, SERIAL_BAUD);
	Log.Info(P("Traffic Counter - ver %d"), TRAFFIC_COUNTER_VERSION);
  
	// Start up sensors
	startSensors();
	addSerialCommands();

	startStorage();
}

/**
* Main loop - run forever
*/
void loop()
{
	timer.run();
	checkSerial();
}


//////////////////////////////////////////////////////////////////////////
// User Functions
//////////////////////////////////////////////////////////////////////////

/**
* startSensors
*
* Enable and configure the sensor suite for reading
*/
void startSensors(){
	trafficEntry["count_uvd"] = 0;
	trafficEntry["count_pir"] = 0;
	trafficEntry["range"] = 0;
	trafficEntry["pir_status"] = false;
	trafficEntry["time"] = 0;
	trafficEntry["entry_num"] = 0;

	startRangefinder();
	startMotionDetector();

	rangeTimerID = timer.setInterval(CHECK_RANGE_INTERVAL, checkRange);
}

/**
* CheckSerial
* 
* Check serial comms for incoming commands
*/
void checkSerial(){
	while (Serial.available()){
		char inChar = Serial.read();
		commandHandler.readIn(inChar);
	}
}

/**
* addSerialCommands
*
* Add commands to the response list
*/
void addSerialCommands(){
	commandHandler.setTerminator(COMMAND_TERMINATOR);
	
	commandHandler.addCommand(RESET_UVD_COUNT, resetRangeCount);
	commandHandler.addCommand(DISABLE_UVD, startRangefinder);
	commandHandler.addCommand(ENABLE_UVD, stopRangefinder);

	commandHandler.setDefaultHandler(defaultHandler);
}

/**
* defaultHandler
*
* Bad command received. Make fun of the sender
*/
void defaultHandler(char c){
	Log.Error(P("Command not recognised: %s"), c);
}


//////////////////////////////////////////////////////////////////////////
// SD Card

/**
* startStorage
*
* Start and configure the onboard SD card for logging.
*/
void startStorage(){
	Log.Debug(P("Initialising SD card..."));

	pinMode(SS, OUTPUT);
	if (!SD.begin(SD_CHIP_SELECT)) {
		Log.Error(P("Error initialising SD card"));
		return;
	}

	trafficLog = SD.open(LOG_FILENAME, FILE_WRITE);
	trafficLog.println(P("Traffic counter - output in JSON"));
	trafficLog.close();

	startLogging();
}

/**
* writeLogEntry
*
* Write a traffic entry to the SD log file
*/
void writeLogEntry(){
	trafficLog = SD.open(LOG_FILENAME, FILE_WRITE);

	if (trafficLog){
		trafficEntry["time"] = long(millis());
		long entryNumber = trafficEntry["entry_num"];
		entryNumber++;
		trafficEntry["entry_num"] = entryNumber;

		trafficEntry.printTo(trafficLog);
		trafficLog.println();

		trafficLog.close();

		Log.Debug(P("Log entry written"));
	}
	else{
		Log.Error(P("Error writing to log file"));
	}
	
}

/**
* startLogging
*
* Allow timed log entries
*/
void startLogging(){
	logTimerID = timer.setInterval(LOG_TIMER_INTERVAL, writeLogEntry);
}

/**
* stopLogging
*
* Disabled timed log entries.
*/
void stopLogging(){
	timer.deleteTimer(logTimerID);
}


//////////////////////////////////////////////////////////////////////////
// Rangefinder

/**
* startRangefinder
*
* Start the rangefinder sensor
*
* A baseline range (to the ground or static surrounds) is established for
* comparing against new measuremets.
* 
*/
void startRangefinder(){

	// Configure control pint
	enableRangefinder();

	Log.Debug(P("Establishing baseline range..."));

	// Establish baseline (fixed height) of the range sensor
	int averageRange = getRange();
	int baselineReads = 1;
	int averageVariance = 500;

	// Keep reading in the baseline until it stablises
	while (baselineReads < MIN_BASELINE_READS || averageVariance > BASELINE_VARIANCE_THRESHOLD){
		int newRange = getRange();
		int newVariance = (averageRange - newRange);
		if (newVariance < 0){newVariance *= -1;}
		
		averageVariance = ((averageVariance + newVariance)/2);
		averageRange = ((averageRange + newRange)/2);

		Log.Debug(P("Calibration: Range - %d, Variance - %d"), int(averageRange), averageVariance);

		baselineReads++;
		delay(BASELINE_READ_INTERVAL);
	}
	
	// Baseline finished
	rangeBaseline = averageRange;
	rangeVariance = averageVariance;

	Log.Debug(P("Baseline established: %d cm, %d cm variance"), averageRange, averageVariance);
}

/**
* stopRangefinder
*
* Stop reading the ultrasonic range data and disable the sensor
*
*/
void stopRangefinder(){
	disableRangefinder();
	timer.deleteTimer(rangeTimerID);
}

/**
* enabledRangefinder
*
* Allow the ultrasonic rangefinder to send ping signals
*/
void enableRangefinder(){
	pinMode(RANGEFINDER_CONTROL_PIN, OUTPUT);
	digitalWrite(RANGEFINDER_CONTROL_PIN, HIGH);
}

/**
* disableRangefinder
*
* Stop sending range pings.
*/
void disableRangefinder(){
	digitalWrite(RANGEFINDER_CONTROL_PIN, LOW);
}

/**
* getRange
*
* Get the range in cm from the ultrasonic rangefinder
*
* @return Target distance from sensor in cm
*/
int getRange(){
	int targetDistance = rangeSensor.getRange();
	Log.Debug(P("Range: %d cm"), targetDistance);
	return targetDistance;
}

/**
* checkRange
* 
* Check the ultrasonic range sensor to see if a traffic event has occurred.
*
* Traffic events are counted as a break in the sensor's 'view' of the ground.
* Any object between the sensor and the ground baseline will cause the sensor
* to register a shorter range than usual.
*/
void checkRange(){
	int lastRange = trafficEntry["range"];
	int newRange = getRange();
	
	// Detection occurs when target breaks the LoS to the baseline
	if ((rangeBaseline - newRange) > RANGE_DETECT_THRESHOLD && (lastRange - newRange) > RANGE_DETECT_THRESHOLD){

		// Increase traffic count
		int trafficCount = trafficEntry["count_uvd"];
		trafficCount++;
		trafficEntry["count_uvd"] = trafficCount;

		Log.Info(P("Traffic count - UVD: %d counts"), trafficCount);
	}

	trafficEntry["range"] = newRange;
}

/**
* resetRangeCount
*
* Reset the traffic count for the ultrasonic rangefinder
*/
void resetRangeCount(){
	trafficEntry["count_uvd"] = 0;
	Log.Info(P("Traffic count reset - UVD"));
}


//////////////////////////////////////////////////////////////////////////
// Motion Detector

/**
* startMotionDetector
*
* Start the PIR motion sensor for motion detections
*/
void startMotionDetector(){
	enableMotion();

	pinMode(PIR_MOTION_PIN, INPUT);

	Log.Debug(P("Calibrating motion sensor - wait %d ms"), MOTION_INITIALISATION_TIME);
	delay(MOTION_INITIALISATION_TIME);
	motionDetected = false;

	motionTimerID = timer.setInterval(CHECK_MOTION_INTERVAL, getMotion);
	Log.Debug(P("Motion started"));
}

/**
* stopMotionDetector
*
* Stop checking the motion detector
*/
void stopMotionDetector(){
	timer.deleteTimer(motionTimerID);
	disableMotion();

	Log.Debug(P("Motion sensor stopped"));
}

/**
* Check the PIR sensor for detected movement
* The sensor will output HIGH when motion is detected.
* Detections will hold the detection status HIGH until the cool-down has lapsed (default: 60s)
*
* Returns:
*	True if motion has been detected recently
*/
void getMotion(){
	// Only check the motion alarm if the cool-off has been exceeded
	if ((millis() - timeOfLastMotion) > (MOTION_TIMEOUT)){

		// Check the sensor
		if (digitalRead(PIR_MOTION_PIN) == MOTION_DETECTED){
			motionDetected = true;
			timeOfLastMotion = millis();

			Log.Info(P("Motion detected"));
			long motionCount = trafficEntry["count_pir"];
			motionCount++;
			trafficEntry["count_pir"] = motionCount;

			Log.Info(P("Traffic count - PIR: %l counts"), motionCount);

		}
		else{
			// No alarm; is fine
			motionDetected = false;
		}
	}

	trafficEntry["pir_status"] = motionDetected;
}

/**
* resetMotionCount
*
* Reset the number of motion detections
*/
void resetMotionCount(){
	trafficEntry["count_pir"] = 0;
}

void enableMotion(){
	pinMode(PIR_CONTROL_PIN, OUTPUT);
	digitalWrite(PIR_CONTROL_PIN, HIGH);
}

void disableMotion(){
	digitalWrite(PIR_CONTROL_PIN, LOW);
}
