/*
  This code is part of FreeWeb - an FCP-based client for Freenet

  Designed and implemented by David McNab, david@rebirthing.co.nz
  CopyLeft (c) 2001 by David McNab

  The FreeWeb website is at http://freeweb.sourceforge.net
  The website for Freenet is at http://freenet.sourceforge.net

  This code is distributed under the GNU Public Licence (GPL) version 2.
  See http://www.gnu.org/ for further details of the GPL.
*/

#include <stdio.h>
#include <stdarg.h>

#include "ezFCPlib.h"

/* extern int vsnprintf(char *str, size_t size, const char *format, va_list ap); */

/*
  Function:    _fcpLog()
	
  Arguments:   as for printf
	
  Description:
*/
void _fcpLog(int level, char *format, ...)
{
	char buf[FCP_LOG_MESSAGE_SIZE + 1];
	
	/* thanks mjr for the idea */
	va_list ap;

	va_start(ap, format);
	vsnprintf(buf, FCP_LOG_MESSAGE_SIZE, format, ap);
	va_end(ap);

	switch (level) {
	case FCP_LOG_CRITICAL:
		printf("Error: %s\n", buf);
		break;

	case FCP_LOG_NORMAL:
		printf("%s\n", buf);
		break;

	case FCP_LOG_VERBOSE:
		printf("%s\n", buf);
		break;

	case FCP_LOG_DEBUG:
		printf("DEBUG: %s\n", buf);
		break;
	}
}

