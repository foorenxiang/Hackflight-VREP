/*
   V-REP simulator plugin code for Hackflight

   Copyright (C) Simon D. Levy, Matt Lubas, and Julio Hidalgo Lopez 2016

   This file is part of Hackflight-VREP.

   Hackflight-VREP is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Hackflight-VREP is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License
   along with Hackflight-VREP.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _USE_MATH_DEFINES
#include <cmath>

// Physics simulation parameters

static const int PARTICLE_COUNT_PER_SECOND = 750;
static const int PARTICLE_DENSITY          = 20000;
static const float PARTICLE_SIZE           = .005f;

static const int BARO_NOISE_PASCALS        = 3;

#include "v_repExt.h"
#include "scriptFunctionData.h"
#include "v_repLib.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <iostream>
using namespace std;

#include "sim_extras.hpp"

// Header-only Hackflight firmware
#include <hackflight.hpp>

#define CONCAT(x,y,z) x y z
#define strConCat(x,y,z)	CONCAT(x,y,z)

#define PLUGIN_NAME  "Hackflight"

LIBRARY vrepLib;

// OS-dependent stuff
#ifdef _WIN32
#define SPRINTF sprintf_s
#else
#define SPRINTF sprintf
#endif

// Hackflight interface
extern void setup(void);
extern void loop(void);
static uint64_t micros;

// Launch support
static bool ready;

// IMU support
static float accel[3];
static float gyro[3];
static float eulerAngles[3];

// Barometer support
static int baroPressure;

// Motor support
static float thrusts[4];

// 100 Hz timestep, used for simulating microsend timer
static float timestep;

static int particleCount;

// Handles from scene
static int motorList[4];
static int motorJointList[4];
static int quadcopterHandle;
static int accelHandle;
static int greenLedHandle;
static int redLedHandle;

// LED support

class LED {

    private:

        int handle;
        float color[3];
        bool on;

    public:

        LED(void) { }

        void init(int _handle, float r, float g, float b)
        {
            this->handle = _handle;
            this->color[0] = r;
            this->color[1] = g;
            this->color[2] = b;
            this->on = false;
        }

        void set(bool status)
        {
            this->on = status;
            float black[3] = {0,0,0};
            simSetShapeColor(this->handle, NULL, 0, this->on ? this->color : black);
        }
};

static LED leds[2];

// forward declaration
//static void startToast(const char * message, int colorR, int colorG, int colorB);

// Board implementation ======================================================

#include "vrepsimboard.hpp"

namespace hf {

void VrepSimBoard::init(Config& config)
{

    // Loop timing overrides
    config.loop.imuLoopMicro       = 10000;    // VREP's shortest simulation period

    for (int k=0; k<3; ++k) {
        EstG[k] = 0;
    }

    EstN[0] = 1.0f;
    EstN[1] = 1.0f;
    EstN[2] = 0.0f;

    // Convert gyro scale from degrees to radians
    // Config is available because VrepSimBoard is a subclass of Board
    leds[0].init(greenLedHandle, 0, 1, 0);
    leds[1].init(redLedHandle, 1, 0, 0);

}


bool VrepSimBoard::skipArming(void)
{
    return true;
}

void VrepSimBoard::getImu(float eulerAnglesRadians[3], int16_t gyroRaw[3])
{
    eulerAnglesRadians[0] = -eulerAngles[1];
    eulerAnglesRadians[1] = -eulerAngles[0];
    eulerAnglesRadians[2] =  eulerAngles[2];

    gyroRaw[1] = -(int16_t)(250 * gyro[0]);
    gyroRaw[0] = -(int16_t)(250 * gyro[1]);
    gyroRaw[2] = -(int16_t)(250 * gyro[2]);
}

void VrepSimBoard::ledSet(uint8_t id, bool is_on) 
{ 
    leds[id].set(is_on);
}

uint64_t VrepSimBoard::getMicros()
{
    return micros; 
}

void VrepSimBoard::writeMotor(uint8_t index, float value)
{
    thrusts[index] = value;
}


void VrepSimBoard::delayMilliseconds(uint32_t msec)
{
}


} // namespace hf

// Model implementation ======================================================

#include "vrepsimmodel.hpp"

// Receiver implementation ======================================================

#include <receivers/sim.hpp>
hf::Controller controller;

// --------------------------------------------------------------------------------------------------

// Hackflight object
static hf::Hackflight h;

// Dialog support
static int displayDialog(const char * title, char * message, float r, float g, float b, int style)
{
   float colors[6] = {0,0,0, 0,0,0};
   colors[0] = r;
   colors[1] = g;
   colors[2] = b;

   return simDisplayDialog(title, message, style, NULL, colors, colors, NULL);
}

// "Toast" dialog support

static int      toastDialogHandle;
static uint32_t toastDialogStartMicros; 
static float    TOAST_DIALOG_DURATION_SEC = 0.5;

static void hideToastDialog(void)
{
    if (toastDialogHandle > -1) 
        simEndDialog(toastDialogHandle);
    toastDialogHandle = -1;
}

/*
static void startToast(const char * message, int colorR, int colorG, int colorB)
{
    hideToastDialog();
    toastDialogHandle = displayDialog("", (char *)message, (float)colorR, (float)colorG, (float)colorB, sim_dlgstyle_message);
    toastDialogStartMicros = micros; 
}
*/

// --------------------------------------------------------------------------------------
// simExtHackflight_start
// --------------------------------------------------------------------------------------
#define LUA_START_COMMAND  "simExtHackflight_start"

static int get_indexed_object_handle(const char * name, int index)
{
    char tmp[100];
    SPRINTF(tmp, "%s%d", name, index+1);
    return simGetObjectHandle(tmp);
}

static int get_indexed_suffixed_object_handle(const char * name, int index, const char * suffix)
{
    char tmp[100];
    SPRINTF(tmp, "%s%d_%s", name, index+1, suffix);
    return simGetObjectHandle(tmp);
}

void LUA_START_CALLBACK(SScriptCallBack* cb)
{
    // Get the object handles for the motors, joints, respondables
    for (int i=0; i<4; ++i) {
        motorList[i]         = get_indexed_object_handle("Motor", i);
		motorJointList[i]    = get_indexed_suffixed_object_handle("Motor", i, "joint");
    }

    // Get handle for objects we'll access
    quadcopterHandle   = simGetObjectHandle("Quadcopter");
    accelHandle        = simGetObjectHandle("Accelerometer_forceSensor");
    greenLedHandle     = simGetObjectHandle("Green_LED_visible");
    redLedHandle       = simGetObjectHandle("Red_LED_visible");

    // Timestep is used in various places
    timestep = simGetSimulationTimeStep();

    particleCount = (int)(PARTICLE_COUNT_PER_SECOND * timestep);

    CScriptFunctionData D;

    // Init Hackflight object
    h.init(new hf::VrepSimBoard(), &controller, new hf::VrepSimModel());

    // Do any extra initialization needed
    //simExtrasStart();

    // Now we're ready
    ready = true;

    // No toast dialog yet
    toastDialogHandle = -1;

    // Return success to V-REP
    D.pushOutData(CScriptFunctionDataItem(true));
    D.writeDataToStack(cb->stackID);
}

// --------------------------------------------------------------------------------------
// simExtHackflight_update
// --------------------------------------------------------------------------------------

#define LUA_UPDATE_COMMAND "simExtHackflight_update"

static void set_indexed_float_signal(const char * name, int i, int k, float value)
{
    char tmp[100];
    SPRINTF(tmp, "%s%d%d", name, i+1, k+1);
    simSetFloatSignal(tmp, value);
}

static void scalarTo3D(float s, float a[12], float out[3])
{
    out[0] = s*a[2];
    out[1] = s*a[6];
    out[2] = s*a[10];
}

void LUA_UPDATE_CALLBACK(SScriptCallBack* cb)
{
    CScriptFunctionData D;
    float eulerFromSim[3];

    // For simulating gyro
    static float anglesPrev[3];

    // Get Euler angles for gyroscope simulation
    simGetObjectOrientation(quadcopterHandle, -1, eulerFromSim);

    // Convert Euler angles to pitch and roll via rotation formula
    eulerAngles[0] =  sin(eulerFromSim[2])*eulerFromSim[0] - cos(eulerFromSim[2])*eulerFromSim[1];
    eulerAngles[1] = -cos(eulerFromSim[2])*eulerFromSim[0] - sin(eulerFromSim[2])*eulerFromSim[1]; 
    eulerAngles[2] = -eulerFromSim[2]; // yaw direct from Euler

    // Compute pitch, roll, yaw first derivative to simulate gyro
    for (int k=0; k<3; ++k) {
        gyro[k] = (eulerAngles[k] - anglesPrev[k]) / timestep;
        anglesPrev[k] = eulerAngles[k];
    }

    // Convert vehicle's Z coordinate in meters to barometric pressure in Pascals (millibars)
    // At low altitudes above the sea level, the pressure decreases by about 1200 Pa for every 100 meters
    // (See https://en.wikipedia.org/wiki/Atmospheric_pressure#Altitude_variation)
    float position[3];
    simGetObjectPosition(quadcopterHandle, -1, position);
    baroPressure = (int)(1000 * (101.325 - 1.2 * position[2] / 100));
    
    // Add some simulated measurement noise to the baro    
    baroPressure += rand() % (2*BARO_NOISE_PASCALS + 1) - BARO_NOISE_PASCALS;

    // Read accelerometer
    simReadForceSensor(accelHandle, accel, NULL);

    // Increment microsecond count
    micros += (uint64_t)(1e6 * timestep);

    // Do any extra update needed
    //simExtrasUpdate();

    const float tsigns[4] = {+1, -1, -1, +1};
	const int propDirections[4] = {-1,+1,+1,-1};

    // Loop over motors
    for (int i=0; i<4; ++i) {

        // Get motor thrust in interval [0,1] from plugin
        float thrust = thrusts[i];

        // Simulate prop spin as a function of thrust
        float jointAngleOld;
        simGetJointPosition(motorJointList[i], &jointAngleOld);
        float jointAngleNew = jointAngleOld + propDirections[i] * thrust * 1.25f;
        simSetJointPosition(motorJointList[i], jointAngleNew);

        // Convert thrust to force and torque
        float force = particleCount * PARTICLE_DENSITY * thrust * (float)M_PI * pow(PARTICLE_SIZE,3) / timestep;
        float torque = tsigns[i] * thrust;

        // Get motor matrix
        float motorMatrix[12];
        simGetObjectMatrix(motorList[i],-1, motorMatrix);

        // Convert force to 3D forces
        float forces[3];
        scalarTo3D(force, motorMatrix, forces);

        // Convert force to 3D torques
        float torques[3];
        scalarTo3D(torque, motorMatrix,torques);

        // Send forces and torques to props
        for (int k=0; k<3; ++k) {
            set_indexed_float_signal("force",  i, k, forces[k]);
            set_indexed_float_signal("torque", i, k, torques[k]);
        }

    } // loop over motors

    // Hide toast dialog if needed
    if (toastDialogHandle > -1 && (micros - toastDialogStartMicros) > TOAST_DIALOG_DURATION_SEC*1e6) {
        simEndDialog(toastDialogHandle);
        toastDialogHandle = -1;
    }

    // Return success to V-REP
    D.pushOutData(CScriptFunctionDataItem(true)); 
    D.writeDataToStack(cb->stackID);

} // LUA_UPDATE_COMMAND


// --------------------------------------------------------------------------------------
// simExtHackflight_stop
// --------------------------------------------------------------------------------------
#define LUA_STOP_COMMAND "simExtHackflight_stop"


void LUA_STOP_CALLBACK(SScriptCallBack* cb)
{
    // Disconnect from handheld controller
    controller.halt();

    // Turn off LEDs
    leds[0].set(false);
    leds[1].set(false);

    // Hide any toast dialogs that may still be visible
    hideToastDialog();

    // Do any extra shutdown needed
    //simExtrasStop();

    // Return success to V-REP
    CScriptFunctionData D;
    D.pushOutData(CScriptFunctionDataItem(true));
    D.writeDataToStack(cb->stackID);
}
// --------------------------------------------------------------------------------------



VREP_DLLEXPORT unsigned char v_repStart(void* reservedPointer,int reservedInt)
{ 
    char curDirAndFile[1024];

#ifdef _WIN32

    GetModuleFileName(NULL,curDirAndFile,1023);
    PathRemoveFileSpec(curDirAndFile);

#elif defined (__linux) || defined (__APPLE__)
    getcwd(curDirAndFile, sizeof(curDirAndFile));
#endif

    std::string currentDirAndPath(curDirAndFile);
    std::string temp(currentDirAndPath);

#ifdef _WIN32
    temp+="\\v_rep.dll";
#elif defined (__linux)
    temp+="/libv_rep.so";
#elif defined (__APPLE__)
    temp+="/libv_rep.dylib";
#endif

// Posix
    vrepLib=loadVrepLibrary(temp.c_str());
    if (vrepLib==NULL)
    {
        std::cout << "Error, could not find or correctly load v_rep.dll. Cannot start 'Hackflight' plugin.\n";
        return(0); // Means error, V-REP will unload this plugin
    }
    if (getVrepProcAddresses(vrepLib)==0)
    {
        std::cout << "Error, could not find all required functions in v_rep plugin. Cannot start 'Hackflight' plugin.\n";
        unloadVrepLibrary(vrepLib);
        return(0); // Means error, V-REP will unload this plugin
    }

    // Check the V-REP version:
    int vrepVer;
    simGetIntegerParameter(sim_intparam_program_version,&vrepVer);
    if (vrepVer<30200) // if V-REP version is smaller than 3.02.00
    {
        std::cout << "Sorry, your V-REP copy is somewhat old, V-REP 3.2.0 or higher is required. Cannot start 'Hackflight' plugin.\n";
        unloadVrepLibrary(vrepLib);
        return(0); // Means error, V-REP will unload this plugin
    }

    // Register new Lua commands:
    simRegisterScriptCallbackFunction(strConCat(LUA_START_COMMAND,"@",PLUGIN_NAME),
            strConCat("boolean result=",LUA_START_COMMAND,
                "(number HackflightHandle,number duration,boolean returnDirectly=false)"),LUA_START_CALLBACK);
    simRegisterScriptCallbackFunction(strConCat(LUA_UPDATE_COMMAND,"@",PLUGIN_NAME), NULL, LUA_UPDATE_CALLBACK);
    simRegisterScriptCallbackFunction(strConCat(LUA_STOP_COMMAND,"@",PLUGIN_NAME),
            strConCat("boolean result=",LUA_STOP_COMMAND,"(number HackflightHandle)"),LUA_STOP_CALLBACK);

    // Enable camera callbacks
    simEnableEventCallback(sim_message_eventcallback_openglcameraview, "Hackflight", -1);

    return 8; // initialization went fine, we return the version number of this plugin (can be queried with simGetModuleName)
}

VREP_DLLEXPORT void v_repEnd()
{ // This is called just once, at the end of V-REP
    unloadVrepLibrary(vrepLib); // release the library
}

VREP_DLLEXPORT void* v_repMessage(int message, int * auxiliaryData, void * customData, int * replyData)
{
    // Don't do anything till start() has been called
    if (!ready)
        return NULL;

    // Handle messages mission-specifically
    //simExtrasMessage(message, auxiliaryData, customData);

    int errorModeSaved;
    simGetIntegerParameter(sim_intparam_error_report_mode,&errorModeSaved);
    simSetIntegerParameter(sim_intparam_error_report_mode,sim_api_errormessage_ignore);
    simSetIntegerParameter(sim_intparam_error_report_mode,errorModeSaved); // restore previous settings

    // Call Hackflight::update() from here for most realistic simulation
    h.update();

    return NULL;
}

// Error handling
void errorDialog(char * message)
{
    // 1,0,0 = red
    displayDialog("ERROR", message, 1,0,0, sim_dlgstyle_ok);
}
