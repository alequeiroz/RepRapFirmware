/*
 * ScaraKinematics.cpp
 *
 *  Created on: 24 Apr 2017
 *      Author: David
 */

#include "ScaraKinematics.h"

ScaraKinematics::ScaraKinematics()
	: Kinematics(KinematicsType::scara, DefaultSegmentsPerSecond, DefaultMinSegmentSize, true),
	  proximalArmLength(DefaultProximalArmLength), distalArmLength(DefaultDistalArmLength)
{
	crosstalk[0] = crosstalk[1] = crosstalk[2] = 0.0;
	thetaLimits[0] = DefaultMinTheta;
	thetaLimits[1] = DefaultMaxTheta;
	phiMinusThetaLimits[0] = DefaultMinPhiMinusTheta;
	phiMinusThetaLimits[1] = DefaultMaxPhiMinusTheta;
	Recalc();
}

// Return the name of the current kinematics
const char *ScaraKinematics::GetName(bool forStatusReport) const
{
	return "Scara";
}

// Convert Cartesian coordinates to motor coordinates
// In the following, theta is the proximal arm angle relative to the X axis, psi is the distal arm angle relative to the X axis
bool ScaraKinematics::CartesianToMotorSteps(const float machinePos[], const float stepsPerMm[], size_t numAxes, int32_t motorPos[]) const
{
	// No need to limit x,y to reachable positions here, we already did that in class GCodes
	const float x = machinePos[X_AXIS];
	const float y = machinePos[Y_AXIS];
	const float cosPsiMinusTheta = (fsquare(x) + fsquare(y) - proximalArmLengthSquared - distalArmLengthSquared) / (2.0f * proximalArmLength * distalArmLength);

	// SCARA position is undefined if abs(SCARA_C2) >= 1. In reality abs(SCARA_C2) >0.95 can be problematic.
	const float square = 1.0f - fsquare(cosPsiMinusTheta);
	if (square < 0.01f)
	{
		return false;		// not reachable
	}

	const float sinPsiMinusTheta = sqrtf(square);
	float psiMinusTheta = atan2f(sinPsiMinusTheta, cosPsiMinusTheta);
	const float SCARA_K1 = proximalArmLength + distalArmLength * cosPsiMinusTheta;
	const float SCARA_K2 = distalArmLength * sinPsiMinusTheta;
	float theta;

	// Try the current arm mode, then the other one
	bool switchedMode = false;
	for (;;)
	{
		if (isDefaultArmMode)
		{
			// The following equations choose arm mode 0 i.e. distal arm rotated anticlockwise relative to proximal arm
			theta = atan2f(SCARA_K2 * x - SCARA_K1 * y, SCARA_K1 * x + SCARA_K2 * y);
			if (theta >= thetaLimits[0])
			{
				break;
			}
		}
		else
		{
			// The following equations choose arm mode 1 i.e. distal arm rotated clockwise relative to proximal arm
			theta = atan2f(SCARA_K2 * x + SCARA_K1 * y, SCARA_K1 * x - SCARA_K2 * y);
			if (theta <= thetaLimits[1])
			{
				psiMinusTheta = -psiMinusTheta;
				break;
			}
		}

		if (switchedMode)
		{
			return false;		// not reachable
		}
		isDefaultArmMode = !isDefaultArmMode;
		switchedMode = true;
	}

	const float psi   = theta + psiMinusTheta;
	motorPos[X_AXIS] = theta * RadiansToDegrees * stepsPerMm[X_AXIS];
	motorPos[Y_AXIS] = (psi * RadiansToDegrees * stepsPerMm[Y_AXIS]) - (crosstalk[0] * motorPos[X_AXIS]);
	motorPos[Z_AXIS] = (int32_t)((machinePos[Z_AXIS] * stepsPerMm[Z_AXIS]) - (motorPos[X_AXIS] * crosstalk[1]) - (motorPos[Y_AXIS] * crosstalk[2]));
	return true;
}

// Convert motor coordinates to machine coordinates. Used after homing and after individual motor moves.
// For Scara, the X and Y components of stepsPerMm are actually steps per degree angle.
void ScaraKinematics::MotorStepsToCartesian(const int32_t motorPos[], const float stepsPerMm[], size_t numDrives, float machinePos[]) const
{
	const float arm1Angle = ((float)motorPos[X_AXIS]/stepsPerMm[X_AXIS]) * DegreesToRadians;
    const float arm2Angle = (((float)motorPos[Y_AXIS] + ((float)motorPos[X_AXIS] * crosstalk[0]))/stepsPerMm[Y_AXIS]) * DegreesToRadians;

    machinePos[X_AXIS] = cosf(arm1Angle) * proximalArmLength + cosf(arm2Angle) * distalArmLength;
    machinePos[Y_AXIS] = sinf(arm1Angle) * proximalArmLength + sinf(arm2Angle) * distalArmLength;

    // On some machines (e.g. Helios), the X and/or Y arm motors also affect the Z height
    machinePos[Z_AXIS] = ((float)motorPos[Z_AXIS] + ((float)motorPos[X_AXIS] * crosstalk[1]) + ((float)motorPos[Y_AXIS] * crosstalk[2]))/stepsPerMm[Z_AXIS];
}

// Set the parameters from a M665, M666 or M669 command
// Return true if we changed any parameters. Set 'error' true if there was an error, otherwise leave it alone.
bool ScaraKinematics::SetOrReportParameters(unsigned int mCode, GCodeBuffer& gb, StringRef& reply, bool& error) /*override*/
{
	if (mCode == 669)
	{
		bool seen = false;
		gb.TryGetFValue('P', proximalArmLength, seen);
		gb.TryGetFValue('D', distalArmLength, seen);
		gb.TryGetFValue('S', segmentsPerSecond, seen);
		gb.TryGetFValue('T', minSegmentLength, seen);
		if (gb.TryGetFloatArray('A', 2, thetaLimits, reply, seen))
		{
			return true;
		}
		if (gb.TryGetFloatArray('B', 2, phiMinusThetaLimits, reply, seen))
		{
			return true;
		}
		if (gb.TryGetFloatArray('C', 3, crosstalk, reply, seen))
		{
			return true;
		}

		if (seen)
		{
			Recalc();
		}
		else
		{
			reply.printf("Printer mode is Scara with proximal arm %.2fmm range %.1f to %.1f" DEGREE_SYMBOL
							", distal arm %.2fmm range %.1f to %.1f" DEGREE_SYMBOL ", crosstalk %.1f:%.1f:%.1f, segments/sec %d, min. segment length %.2f",
							proximalArmLength, thetaLimits[0], thetaLimits[1],
							distalArmLength, phiMinusThetaLimits[0], phiMinusThetaLimits[1],
							crosstalk[0], crosstalk[1], crosstalk[2],
							(int)segmentsPerSecond, minSegmentLength);
		}
		return seen;
	}
	else
	{
		return Kinematics::SetOrReportParameters(mCode, gb, reply, error);
	}
}

// Return true if the specified XY position is reachable by the print head reference point.
// TODO add an arm mode parameter?
bool ScaraKinematics::IsReachable(float x, float y) const
{
	// TODO The following isn't quite right, in particular it doesn't take account of the maximum arm travel
    const float r = sqrtf(fsquare(x) + fsquare(y));
    return r >= minRadius && r <= maxRadius && x > 0.0;
}

// Limit the Cartesian position that the user wants to move to
// TODO take account of arm angle limits
void ScaraKinematics::LimitPosition(float coords[], size_t numAxes, uint16_t axesHomed) const
{
	const float halfPi = PI/2.0;

	float& x = coords[X_AXIS];
    float& y = coords[Y_AXIS];
    const float r = sqrtf(fsquare(x) + fsquare(y));
    const float arcLength = halfPi * minRadius;
    if (r < minRadius && y >= 0.0)
    {
		const float xmax = sqrtf(fsquare(minRadius) - fsquare(y));
		const float arc = (float)(PI/2.0) - atan2f(y, xmax);
		const float p = x/xmax;
		const float pArcLength = arc * p;
		x = minRadius * cosf(halfPi - pArcLength);
		y = minRadius * sinf(halfPi - pArcLength);
	}
	else if ((r < minRadius || fabs(x) < minRadius) && y < 0.0)
	{
		const float length = -y + arcLength;
		const float p = x/minRadius;
		const float subLength = p * length;
		if (fabs(subLength) > arcLength)
		{
			x = copysignf(minRadius, x);
			y = -fabs(subLength) + arcLength;
		}
		else
		{
			const float angle = halfPi * (1.0f - subLength/arcLength);
			x = minRadius * cosf(angle);
			y = minRadius * sinf(angle);
		}
	}
	else if (r > maxRadius)
	{
		x *= maxRadius/r;
		y *= maxRadius/r;
	}
}

// Recalculate the derived parameters
void ScaraKinematics::Recalc()
{
	minRadius = (proximalArmLength + distalArmLength * max<float>(cosf(phiMinusThetaLimits[0]), cosf(phiMinusThetaLimits[1]))) * 1.01;
	maxRadius = (proximalArmLength + distalArmLength) * 0.99;
	proximalArmLengthSquared = fsquare(proximalArmLength);
	distalArmLengthSquared = fsquare(distalArmLength);
}

// End
