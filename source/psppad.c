/*
# based on orbisPad from ORBISDEV Open Source Project.
# Copyright 2010-2020, orbisdev - http://orbisdev.github.io
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/time.h>
#include <dbglogger.h>
#include "psppad.h"

#define LOG dbglogger_log

static PspPadConfig pspPadConf;
static int orbispad_initialized = 0;
static uint64_t g_time;


static uint64_t timeInMilliseconds(void)
{
    struct timeval tv;

    gettimeofday(&tv,NULL);
    return (((uint64_t)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

void pspPadFinish()
{
	int ret;

	if(orbispad_initialized)
	{
		LOG("scePadClose");
	}
	orbispad_initialized=0;

	LOG("ORBISPAD finished");
}

PspPadConfig *pspPadGetConf()
{
	if(orbispad_initialized)
	{
		return (&pspPadConf);
	}
	
	return NULL; 
}

static int pspPadInitConf()
{	
	if(orbispad_initialized)
	{
		return orbispad_initialized;
	}

	memset(&pspPadConf, 0, sizeof(PspPadConfig));
	
	return 0;
}

unsigned int pspPadGetCurrentButtonsPressed()
{
	return pspPadConf.buttonsPressed;
}

void pspPadSetCurrentButtonsPressed(unsigned int buttons)
{
	pspPadConf.buttonsPressed=buttons;
}

unsigned int pspPadGetCurrentButtonsReleased()
{
	return pspPadConf.buttonsReleased;
}

void pspPadSetCurrentButtonsReleased(unsigned int buttons)
{
	pspPadConf.buttonsReleased=buttons;
}

bool pspPadGetButtonHold(unsigned int filter)
{
	uint64_t time = timeInMilliseconds();
	uint64_t delta = time - g_time;

	if((pspPadConf.buttonsHold&filter)==filter && delta > 150)
	{
		g_time = time;
		return 1;
	}

	return 0;
}

bool pspPadGetButtonPressed(unsigned int filter)
{
	if((pspPadConf.buttonsPressed&filter)==filter)
	{
		pspPadConf.buttonsPressed ^= filter;
		return 1;
	}

	return 0;
}

bool pspPadGetButtonReleased(unsigned int filter)
{
 	if((pspPadConf.buttonsReleased&filter)==filter)
	{
		if(~(pspPadConf.padDataLast.Buttons)&filter)
		{
			return 0;
		}
		return 1;
	}

	return 0;
}

int pspPadUpdate()
{
	int ret;
	unsigned int actualButtons=0;
	unsigned int lastButtons=0;

	memcpy(&pspPadConf.padDataLast, &pspPadConf.padDataCurrent, sizeof(SceCtrlData));	
	ret=sceCtrlPeekBufferPositive(&pspPadConf.padDataCurrent, 1);

	if(ret > 0)
	{
		if (pspPadConf.padDataCurrent.Ly < ANALOG_MIN)
			pspPadConf.padDataCurrent.Buttons |= PSP_CTRL_UP;

		if (pspPadConf.padDataCurrent.Ly > ANALOG_MAX)
			pspPadConf.padDataCurrent.Buttons |= PSP_CTRL_DOWN;

		if (pspPadConf.padDataCurrent.Lx < ANALOG_MIN)
			pspPadConf.padDataCurrent.Buttons |= PSP_CTRL_LEFT;

		if (pspPadConf.padDataCurrent.Lx > ANALOG_MAX)
			pspPadConf.padDataCurrent.Buttons |= PSP_CTRL_RIGHT;

		actualButtons=pspPadConf.padDataCurrent.Buttons;
		lastButtons=pspPadConf.padDataLast.Buttons;
		pspPadConf.buttonsPressed=(actualButtons)&(~lastButtons);
		if(actualButtons!=lastButtons)
		{
			pspPadConf.buttonsReleased=(~actualButtons)&(lastButtons);
			pspPadConf.idle=0;
		}
		else
		{
			pspPadConf.buttonsReleased=0;
			if (actualButtons == 0)
			{
				pspPadConf.idle++;
			}
		}
		pspPadConf.buttonsHold=actualButtons&lastButtons;

		return 0;
	}

	return -1;
}

int pspPadInit()
{
	int ret;

	if(pspPadInitConf()==1)
	{
		LOG("ORBISPAD already initialized!");
		return orbispad_initialized;
	}

	sceCtrlSetSamplingCycle(0);
	ret=sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
	if (ret < 0)
	{
		LOG("sceCtrlSetSamplingMode Error 0x%8X", ret);
		return -1;
	}

	orbispad_initialized=1;
	g_time = timeInMilliseconds();
	LOG("ORBISPAD initialized: sceCtrlSetSamplingMode return 0x%X", ret);

	return orbispad_initialized;
}
