/*
WIIU_PAD.C

The Wii U's controllers as the Xbox's gamepads (wiiu_port.h's
wiiu_pad_state, which platform/wiiu_input.c hands the game as XInput's):
the GamePad (VPAD) in the first port, then the Wii Remotes' channels (KPAD)
with a Pro Controller or a Classic Controller, in the ports after it, as
they connect.

The layout follows the Xbox controller's: A, B, X and Y where the Wii U's
are (Nintendo's A is the Xbox's B's place: the buttons go by place, not by
letter, so the jump is the bottom button as on the Xbox), ZL and ZR the
triggers, L and R the black and white buttons (L white, R black, the
Xbox's left and right of the face buttons), + Start, - Back, the sticks'
clicks the thumb buttons. The triggers and the face buttons are digital on
these controllers: fully pressed or not.
*/

#include <string.h>

#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <vpad/input.h>

#include "wiiu_port.h"

/* XINPUT_GAMEPAD's digital bits and analog indices (port/include/xdk) */
#define DPAD_UP 0x0001
#define DPAD_DOWN 0x0002
#define DPAD_LEFT 0x0004
#define DPAD_RIGHT 0x0008
#define START 0x0010
#define BACK 0x0020
#define LEFT_THUMB 0x0040
#define RIGHT_THUMB 0x0080
enum { ANALOG_A, ANALOG_B, ANALOG_X, ANALOG_Y, ANALOG_BLACK, ANALOG_WHITE, ANALOG_LEFT_TRIGGER, ANALOG_RIGHT_TRIGGER };

/* the buttons of each kind of controller, in the order: up, down, left,
right, +, -, left stick, right stick, then A, B, X, Y (by their labels, as
the Wii U's own menus use them: A accepts, B backs out), black (R), white
(L), left trigger (ZL), right (ZR) */
struct layout
{
	unsigned int dpad[4], start, back, thumbs[2];
	unsigned int analog[8];
};

static const struct layout gamepad_layout =
{
	{ VPAD_BUTTON_UP, VPAD_BUTTON_DOWN, VPAD_BUTTON_LEFT, VPAD_BUTTON_RIGHT },
	VPAD_BUTTON_PLUS, VPAD_BUTTON_MINUS, { VPAD_BUTTON_STICK_L, VPAD_BUTTON_STICK_R },
	{ VPAD_BUTTON_A, VPAD_BUTTON_B, VPAD_BUTTON_X, VPAD_BUTTON_Y, VPAD_BUTTON_R, VPAD_BUTTON_L,
	  VPAD_BUTTON_ZL, VPAD_BUTTON_ZR },
};

static const struct layout pro_layout =
{
	{ WPAD_PRO_BUTTON_UP, WPAD_PRO_BUTTON_DOWN, WPAD_PRO_BUTTON_LEFT, WPAD_PRO_BUTTON_RIGHT },
	WPAD_PRO_BUTTON_PLUS, WPAD_PRO_BUTTON_MINUS, { WPAD_PRO_BUTTON_STICK_L, WPAD_PRO_BUTTON_STICK_R },
	{ WPAD_PRO_BUTTON_A, WPAD_PRO_BUTTON_B, WPAD_PRO_BUTTON_X, WPAD_PRO_BUTTON_Y, WPAD_PRO_TRIGGER_R,
	  WPAD_PRO_TRIGGER_L, WPAD_PRO_TRIGGER_ZL, WPAD_PRO_TRIGGER_ZR },
};

static const struct layout classic_layout =
{
	{ WPAD_CLASSIC_BUTTON_UP, WPAD_CLASSIC_BUTTON_DOWN, WPAD_CLASSIC_BUTTON_LEFT, WPAD_CLASSIC_BUTTON_RIGHT },
	WPAD_CLASSIC_BUTTON_PLUS, WPAD_CLASSIC_BUTTON_MINUS, { 0, 0 },
	{ WPAD_CLASSIC_BUTTON_A, WPAD_CLASSIC_BUTTON_B, WPAD_CLASSIC_BUTTON_X, WPAD_CLASSIC_BUTTON_Y,
	  WPAD_CLASSIC_BUTTON_R, WPAD_CLASSIC_BUTTON_L, WPAD_CLASSIC_BUTTON_ZL, WPAD_CLASSIC_BUTTON_ZR },
};

static int pads_ready;
static struct wiiu_pad_state states[WIIU_PAD_PORTS];
/* the port each of the four Wii Remote channels has, or -1 */
static int channel_ports[4] = { -1, -1, -1, -1 };

static void pads_initialize(void)
{
	if (pads_ready)
		return;
	VPADInit();
	KPADInit();
	/* the Pro Controller (the Wii U's own, URCC) */
	WPADEnableURCC(TRUE);
	pads_ready = 1;
	wiiu_log("input: the GamePad and the Wii Remote channels (Pro and Classic Controllers)");
}

static short stick(float value)
{
	if (value > 1.0f)
		value = 1.0f;
	if (value < -1.0f)
		value = -1.0f;
	return (short)(value * 32767.0f);
}

static void fill(struct wiiu_pad_state *state, const struct layout *layout, unsigned int held,
	float lx, float ly, float rx, float ry)
{
	int index;

	state->connected = 1;
	state->digital = 0;
	for (index = 0; index < 4; index++)
	{
		if (held & layout->dpad[index])
			state->digital |= (unsigned short)(DPAD_UP << index);
	}
	if (held & layout->start)
		state->digital |= START;
	if (held & layout->back)
		state->digital |= BACK;
	if (layout->thumbs[0] && (held & layout->thumbs[0]))
		state->digital |= LEFT_THUMB;
	if (layout->thumbs[1] && (held & layout->thumbs[1]))
		state->digital |= RIGHT_THUMB;
	for (index = 0; index < 8; index++)
		state->analog[index] = (held & layout->analog[index]) ? 255 : 0;
	state->thumb_lx = stick(lx);
	state->thumb_ly = stick(ly);
	state->thumb_rx = stick(rx);
	state->thumb_ry = stick(ry);
}

/* the next free port for a Wii Remote channel, or -1 */
static int free_port(void)
{
	int port, channel, taken;

	for (port = 1; port < WIIU_PAD_PORTS; port++)
	{
		taken = 0;
		for (channel = 0; channel < 4; channel++)
			taken |= channel_ports[channel] == port;
		if (!taken)
			return port;
	}
	return -1;
}

void wiiu_pad_update(void)
{
	VPADStatus gamepad;
	VPADReadError gamepad_error = VPAD_READ_SUCCESS;
	int channel;

	pads_initialize();
	memset(&gamepad, 0, sizeof(gamepad));
	if (VPADRead(VPAD_CHAN_0, &gamepad, 1, &gamepad_error) > 0 && gamepad_error == VPAD_READ_SUCCESS)
	{
		fill(&states[0], &gamepad_layout, gamepad.hold, gamepad.leftStick.x, gamepad.leftStick.y,
			gamepad.rightStick.x, gamepad.rightStick.y);
	}
	else if (gamepad_error != VPAD_READ_NO_SAMPLES)
	{
		/* (no samples: the last state stands) */
		states[0].connected = gamepad_error != VPAD_READ_INVALID_CONTROLLER;
	}
	for (channel = 0; channel < 4; channel++)
	{
		KPADStatus status;
		KPADError error = KPAD_ERROR_OK;
		int port = channel_ports[channel];
		int count;

		memset(&status, 0, sizeof(status));
		count = KPADReadEx((KPADChan)channel, &status, 1, &error);
		if (error == KPAD_ERROR_INVALID_CONTROLLER)
		{
			if (port >= 0)
			{
				wiiu_log("input: Wii Remote channel %d left port %d", channel + 1, port + 1);
				memset(&states[port], 0, sizeof(states[port]));
				channel_ports[channel] = -1;
			}
			continue;
		}
		if (count <= 0)
			continue;
		if (status.extensionType != WPAD_EXT_PRO_CONTROLLER && status.extensionType != WPAD_EXT_CLASSIC &&
			status.extensionType != WPAD_EXT_MPLUS_CLASSIC)
		{
			continue;
		}
		if (port < 0)
		{
			port = free_port();
			if (port < 0)
				continue;
			channel_ports[channel] = port;
			wiiu_log("input: a %s on Wii Remote channel %d is port %d",
				status.extensionType == WPAD_EXT_PRO_CONTROLLER ? "Pro Controller" : "Classic Controller", channel + 1,
				port + 1);
		}
		if (status.extensionType == WPAD_EXT_PRO_CONTROLLER)
		{
			fill(&states[port], &pro_layout, status.pro.hold, status.pro.leftStick.x, status.pro.leftStick.y,
				status.pro.rightStick.x, status.pro.rightStick.y);
		}
		else
		{
			fill(&states[port], &classic_layout, status.classic.hold, status.classic.leftStick.x,
				status.classic.leftStick.y, status.classic.rightStick.x, status.classic.rightStick.y);
		}
	}
}

int wiiu_pad_read(int port, struct wiiu_pad_state *state)
{
	if (port < 0 || port >= WIIU_PAD_PORTS)
		return 0;
	*state = states[port];
	return state->connected;
}

/* (the GamePad's motor alone: a pattern of on and off, here a steady one
while the game asks for any rumble) */
void wiiu_pad_rumble(int port, int on)
{
	static uint8_t pattern[15] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff };
	static int rumbling;

	if (port != 0 || on == rumbling)
		return;
	rumbling = on;
	if (on)
		VPADControlMotor(VPAD_CHAN_0, pattern, 120);
	else
		VPADStopMotor(VPAD_CHAN_0);
}
