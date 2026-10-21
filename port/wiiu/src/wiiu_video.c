/*
WIIU_VIDEO.C

The TV's picture. Not yet: the renderer over GX2 is the next milestone
(port/wiiu/STATUS.md); until then the game runs without a picture, as the
dedicated server does.
*/

#include "wiiu_port.h"

int wiiu_video_initialize(void)
{
	wiiu_log("video: no renderer yet: running without a picture");
	return 0;
}

void wiiu_video_swap(void)
{
}
