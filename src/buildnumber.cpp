#include <universal/q_shared.h>
#include "buildnumber.h"
#include <stdio.h>

char buildnumbuf[128];

/*
* Original Date: "Thu Oct 04 00:43:04 2007"
* Original Build: 13620
*/

// LWSS: shared between SP/MP for simplicity

char *__cdecl getBuildNumber()
{
	snprintf(buildnumbuf, sizeof(buildnumbuf), "%d %s %s", BUILD_NUMBER, __DATE__, __TIME__);
	return buildnumbuf;
}

int getBuildNumberAsInt()
{
	return BUILD_NUMBER;
}