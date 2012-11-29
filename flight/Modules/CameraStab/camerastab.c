/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup CameraStab Camera Stabilization Module
 * @brief Camera stabilization module
 * Updates accessory outputs with values appropriate for camera stabilization
 * @{
 *
 * @file       camerastab.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Stabilize camera against the roll pitch and yaw of aircraft
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Output object: Accessory
 *
 * This module will periodically calculate the output values for stabilizing the camera
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include "openpilot.h"

#include "accessorydesired.h"
#include "attitudeactual.h"
#include "camerastabsettings.h"
#include "cameradesired.h"
#include "hwsettings.h"

//
// Configuration
//
#define SAMPLE_PERIOD_MS		10

// Private types

// Private variables
static struct CameraStab_data {
	portTickType lastSysTime;
	float inputs[CAMERASTABSETTINGS_INPUT_NUMELEM];

#ifdef USE_GIMBAL_LPF
	float attitudeFiltered[CAMERASTABSETTINGS_INPUT_NUMELEM];
#endif

#ifdef USE_GIMBAL_FF
	float ffLastAttitude[CAMERASTABSETTINGS_INPUT_NUMELEM];
	float ffLastAttitudeFiltered[CAMERASTABSETTINGS_INPUT_NUMELEM];
	float ffFilterAccumulator[CAMERASTABSETTINGS_INPUT_NUMELEM];
#endif

} *csd;

// Private functions
static void attitudeUpdated(UAVObjEvent* ev);
static float bound(float val, float limit);

#ifdef USE_GIMBAL_FF
static void applyFeedForward(uint8_t index, float dT, float *attitude, CameraStabSettingsData *cameraStab);
#endif


/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t CameraStabInitialize(void)
{
	bool cameraStabEnabled;

#ifdef MODULE_CameraStab_BUILTIN
	cameraStabEnabled = true;
#else
	uint8_t optionalModules[HWSETTINGS_OPTIONALMODULES_NUMELEM];

	HwSettingsInitialize();
	HwSettingsOptionalModulesGet(optionalModules);

	if (optionalModules[HWSETTINGS_OPTIONALMODULES_CAMERASTAB] == HWSETTINGS_OPTIONALMODULES_ENABLED)
		cameraStabEnabled = true;
	else
		cameraStabEnabled = false;
#endif

	if (cameraStabEnabled) {

		// allocate and initialize the static data storage only if module is enabled
		csd = (struct CameraStab_data *) pvPortMalloc(sizeof(struct CameraStab_data));
		if (!csd)
			return -1;

		// initialize camera state variables
		memset(csd, 0, sizeof(struct CameraStab_data));
		csd->lastSysTime = xTaskGetTickCount();

		AttitudeActualInitialize();
		CameraStabSettingsInitialize();
		CameraDesiredInitialize();

		UAVObjEvent ev = {
			.obj = AttitudeActualHandle(),
			.instId = 0,
			.event = 0,
		};
		EventPeriodicCallbackCreate(&ev, attitudeUpdated, SAMPLE_PERIOD_MS / portTICK_RATE_MS);

		return 0;
	}

	return -1;
}

/* stub: module has no module thread */
int32_t CameraStabStart(void)
{
	return 0;
}

MODULE_INITCALL(CameraStabInitialize, CameraStabStart)

static void attitudeUpdated(UAVObjEvent* ev)
{
	if (ev->obj != AttitudeActualHandle())
		return;

	AccessoryDesiredData accessory;

	CameraStabSettingsData cameraStab;
	CameraStabSettingsGet(&cameraStab);

	// check how long since last update, time delta between calls in ms
	portTickType thisSysTime = xTaskGetTickCount();
	float dT = (thisSysTime > csd->lastSysTime) ?
			(float)((thisSysTime - csd->lastSysTime) * portTICK_RATE_MS) :
			(float)SAMPLE_PERIOD_MS;
	csd->lastSysTime = thisSysTime;

	// process axes
	for (uint8_t i = 0; i < CAMERASTABSETTINGS_INPUT_NUMELEM; i++) {

		// read and process control input
		if (cameraStab.Input[i] != CAMERASTABSETTINGS_INPUT_NONE) {
			if (AccessoryDesiredInstGet(cameraStab.Input[i] - CAMERASTABSETTINGS_INPUT_ACCESSORY0, &accessory) == 0) {
				float input_rate;
				switch (cameraStab.StabilizationMode[i]) {
				case CAMERASTABSETTINGS_STABILIZATIONMODE_ATTITUDE:
					csd->inputs[i] = accessory.AccessoryVal * cameraStab.InputRange[i];
					break;
				case CAMERASTABSETTINGS_STABILIZATIONMODE_AXISLOCK:
					input_rate = accessory.AccessoryVal * cameraStab.InputRate[i];
					if (fabs(input_rate) > cameraStab.MaxAxisLockRate)
						csd->inputs[i] = bound(csd->inputs[i] + input_rate * 0.001f * dT, cameraStab.InputRange[i]);
					break;
				default:
					PIOS_Assert(0);
				}
			}
		}

		// calculate servo output
		float attitude;

		switch (i) {
		case CAMERASTABSETTINGS_INPUT_ROLL:
			AttitudeActualRollGet(&attitude);
			break;
		case CAMERASTABSETTINGS_INPUT_PITCH:
			AttitudeActualPitchGet(&attitude);
			break;
		case CAMERASTABSETTINGS_INPUT_YAW:
			AttitudeActualYawGet(&attitude);
			break;
		default:
			PIOS_Assert(0);
		}

#ifdef USE_GIMBAL_LPF
		if (cameraStab.ResponseTime) {
			float rt = (float)cameraStab.ResponseTime[i];
			attitude = csd->attitudeFiltered[i] = ((rt * csd->attitudeFiltered[i]) + (dT * attitude)) / (rt + dT);
		}
#endif

#ifdef USE_GIMBAL_FF
		if (cameraStab.FeedForward[i])
			applyFeedForward(i, dT, &attitude, &cameraStab);
#endif

		// set output channels
		float output = bound((attitude + csd->inputs[i]) / cameraStab.OutputRange[i], 1.0f);
		switch (i) {
		case CAMERASTABSETTINGS_INPUT_ROLL:
			CameraDesiredRollSet(&output);
			break;
		case CAMERASTABSETTINGS_INPUT_PITCH:
			CameraDesiredPitchSet(&output);
			break;
		case CAMERASTABSETTINGS_INPUT_YAW:
			CameraDesiredYawSet(&output);
			break;
		default:
			PIOS_Assert(0);
		}
	}
}

float bound(float val, float limit)
{
	return (val > limit) ? limit :
		(val < -limit) ? -limit :
		val;
}

#ifdef USE_GIMBAL_FF
void applyFeedForward(uint8_t index, float dT, float *attitude, CameraStabSettingsData *cameraStab)
{
	// compensate high feed forward values depending on gimbal type
	float gimbalTypeCorrection = 1.0f;

	switch (cameraStab->GimbalType) {
	case CAMERASTABSETTINGS_GIMBALTYPE_GENERIC:
		// no correction
		break;
	case CAMERASTABSETTINGS_GIMBALTYPE_YAWROLLPITCH:
		if (index == CAMERASTABSETTINGS_INPUT_ROLL) {
			float pitch;
			AttitudeActualPitchGet(&pitch);
			gimbalTypeCorrection = (cameraStab->OutputRange[CAMERASTABSETTINGS_OUTPUTRANGE_PITCH] - fabs(pitch))
							/ cameraStab->OutputRange[CAMERASTABSETTINGS_OUTPUTRANGE_PITCH];
		}
		break;
	case CAMERASTABSETTINGS_GIMBALTYPE_YAWPITCHROLL:
		if (index == CAMERASTABSETTINGS_INPUT_PITCH) {
			float roll;
			AttitudeActualRollGet(&roll);
			gimbalTypeCorrection = (cameraStab->OutputRange[CAMERASTABSETTINGS_OUTPUTRANGE_ROLL] - fabs(roll))
							/ cameraStab->OutputRange[CAMERASTABSETTINGS_OUTPUTRANGE_ROLL];
		}
		break;
	default:
		PIOS_Assert(0);
	}

	// apply feed forward
	float accumulator = csd->ffFilterAccumulator[index];
	accumulator += (*attitude - csd->ffLastAttitude[index]) * (float)cameraStab->FeedForward[index] * gimbalTypeCorrection;
	csd->ffLastAttitude[index] = *attitude;
	*attitude += accumulator;

	float filter = (float)((accumulator > 0.0f) ? cameraStab->AccelTime[index] : cameraStab->DecelTime[index]) / dT;
	if (filter < 1.0f)
		filter = 1.0f;
	accumulator -= accumulator / filter;
	csd->ffFilterAccumulator[index] = accumulator;
	*attitude += accumulator;

	// apply acceleration limit
	float delta = *attitude - csd->ffLastAttitudeFiltered[index];
	float maxDelta = (float)cameraStab->MaxAccel * 0.001f * dT;

	if (fabs(delta) > maxDelta) {
		// we are accelerating too hard
		*attitude = csd->ffLastAttitudeFiltered[index] + ((delta > 0.0f) ? maxDelta : -maxDelta);
	}
	csd->ffLastAttitudeFiltered[index] = *attitude;
}
#endif // USE_GIMBAL_FF

/**
  * @}
  */

/**
 * @}
 */
