/*
# based on orbisPad from ORBISDEV Open Source Project.
# Copyright 2010-2020, orbisdev - http://orbisdev.github.io
*/

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <pspctrl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANALOG_CENTER       0x78
#define ANALOG_THRESHOLD    0x68
#define ANALOG_MIN          (ANALOG_CENTER - ANALOG_THRESHOLD)
#define ANALOG_MAX          (ANALOG_CENTER + ANALOG_THRESHOLD)


typedef struct PspPadConfig
{
	SceCtrlData padDataCurrent;
	SceCtrlData padDataLast;
	unsigned int buttonsPressed;
	unsigned int buttonsReleased;
	unsigned int buttonsHold;
	unsigned int idle;
} PspPadConfig;

int pspPadInit();
void pspPadFinish();
PspPadConfig *pspPadGetConf();
bool pspPadGetButtonHold(unsigned int filter);
bool pspPadGetButtonPressed(unsigned int filter);
bool pspPadGetButtonReleased(unsigned int filter);
unsigned int pspPadGetCurrentButtonsPressed();
unsigned int pspPadGetCurrentButtonsReleased();
void pspPadSetCurrentButtonsPressed(unsigned int buttons);
void pspPadSetCurrentButtonsReleased(unsigned int buttons);
int pspPadUpdate();

#ifdef __cplusplus
}
#endif
