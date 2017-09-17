/*
   controller_Linux.cpp : Linux support for controllers in Hackflight-VREP simulator plugin

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

static const char * JOY_DEV = "/dev/input/js0";
static const char * DSM_DEV = "/dev/ttyACM0";

#include "controller.hpp"
#include "controller_Posix.hpp"
#include "MSPPG.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

#include <linux/joystick.h>

static int joyfd;

// Support for Spektrum DSM dongle
static int dsmfd;
static int dsmvals[5];
static bool dsmrunning;

// A class for handling RC messages from the Spektrum DSM dongle
class My_RC_Handler : public RC_Handler {

    public:

        void handle_RC(short c1, short c2, short c3, short c4, short c5, short c6, short c7, short c8)
        {
            dsmvals[0] = c1;
            dsmvals[1] = c2;
            dsmvals[2] = c3;
            dsmvals[3] = c4;
            dsmvals[4] = c5;
        }
};


// A thread for grabbing serial-port input from the Spektrum DSM dongle
static void * dsmthread(void * v)
{
    MSP_Parser parser;

    My_RC_Handler handler;

    parser.set_RC_Handler(&handler);

    while (dsmrunning) {
        int avail;
        ioctl(dsmfd, FIONREAD, &avail);
        if (avail > 0) {
            char c;
            read(dsmfd, &c, 1);
            parser.parse(c);
        }
    }

    pthread_exit(NULL);
}

controller_t controllerInit(void)
{ 
    // First try to open wired joystick device
    if ((joyfd=open(JOY_DEV, O_RDONLY)) > 0) {

        fcntl(joyfd, F_SETFL, O_NONBLOCK);

        char name[128];

        if (ioctl(joyfd, JSIOCGNAME(sizeof(name)), name) < 0)
            printf("Uknown controller\n");

        else 
            controller = posixControllerInit(name, "MY-POWER CO.");
    }

    // Next try to open wireless DSM dongle
    else if ((dsmfd=open(DSM_DEV, O_RDONLY)) > 0) {

        dsmrunning = true;

        pthread_t thread;
        pthread_create(&thread, NULL, dsmthread, NULL);
        controller = DSM;
    }

    return controller;
} 

void controllerRead(controller_t controller, float * demands)
{
    // Have a joystick; grab its axes
    if (joyfd > 0) {

        struct js_event js;

        read(joyfd, &js, sizeof(struct js_event));

        int jstype = js.type & ~JS_EVENT_INIT;

        // Grab demands from axes
        if (jstype == JS_EVENT_AXIS) 
            posixControllerGrabAxis(controller, demands, js.number, js.value);

        // Grab aux demand from buttons when detected
        if ((jstype == JS_EVENT_BUTTON) && (js.value==1)) 
            posixControllerGrabButton(demands, js.number);
    }

    // No joystick; try DSM dongle
    else if (dsmfd > 0) {
        for (int k=0; k<5; ++k)
            demands[k] = (dsmvals[k] - 1500) / 500.;
    }
}

void controllerClose(void)
{
    dsmrunning = false;

    if (joyfd > 0)
        close(joyfd);

    else if (dsmfd > 0)
        close(dsmfd);
}
