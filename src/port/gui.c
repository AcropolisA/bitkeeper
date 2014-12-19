#include "../sccs.h"

/*
 * Copyright (c) 2001 Andrew Chang       All rights reserved.
 */

int
gui_useDisplay(void)
{
	char	*p;

	if ((p = getenv("BK_NO_GUI_PROMPT")) && *p) return (0);
	if (win32() || macosx()) return ((p = getenv("BK_GUI")) && *p);
	return (getenv("DISPLAY") && (p = getenv("BK_GUI")) && *p);
}

char *
gui_displayName(void)
{
#ifdef WIN32
	return ("monitor");
#else
	if (gui_useAqua()) {return ("monitor");}
	return (getenv("DISPLAY"));
#endif

}

int
gui_useAqua(void)
{
#ifdef	__APPLE__
	char	*disp = getenv("DISPLAY");

	if ((disp == 0) || strneq(disp, "/tmp/launch", 11) ||
	    strstr(disp, "com.apple.launchd")) return (1);
#endif
	return 0;
}
