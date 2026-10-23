/*
WIIU_INPUT.C

The Xbox's controllers on the Wii U (XInput, in place of xinput_sdl.c): the
four ports are the GamePad and the Pro or Classic Controllers on the Wii
Remotes' channels, as port/wiiu/src/wiiu_pad.c reads them. No keyboard or
mouse: the desktop's keyboard and mouse functions answer that there is none.
Reading the first port, every frame, is also when the platform layer pumps
the system's events (wiiu_platform.c), as xinput_sdl.c does.
*/

#include "platform.h"
#include "sdl_platform.h"

#include <stdio.h>
#include <string.h>

#include "wiiu_port.h"

#define PORT_COUNT WIIU_PAD_PORTS

XPP_DEVICE_TYPE XDEVICE_TYPE_GAMEPAD_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_MEMORY_UNIT_TABLE;
XPP_DEVICE_TYPE XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE;

struct controller
{
	BOOL open;
	DWORD packet;
};

static struct controller controllers[PORT_COUNT];
static struct controller keyboard_device;
/* the ports reported as connected (XGetDeviceChanges) */
static DWORD reported_gamepads;
static BOOL reported_keyboard;

/* ---------- the keyboard and mouse's (none) */

int halo_linux_mouse_look(short gamepad_index, float *yaw, float *pitch)
{
	(void)gamepad_index;
	*yaw = 0.0f;
	*pitch = 0.0f;
	return FALSE;
}

int halo_linux_mouse_aiming(short gamepad_index)
{
	(void)gamepad_index;
	return FALSE;
}

int platform_input_scheme(void)
{
	return 0;
}

int platform_prompt_scheme(void)
{
	return 0;
}

void platform_text_typing(int typing)
{
	(void)typing;
}

void platform_chat_typing(int typing)
{
	(void)typing;
}

int platform_chat_escape(void)
{
	return 0;
}

void platform_text_field(int typing)
{
	(void)typing;
}

int platform_gamepad_count(void)
{
	struct wiiu_pad_state state;
	int port, count = 0;

	for (port = 0; port < PORT_COUNT; port++)
		count += wiiu_pad_read(port, &state) != 0;
	return count;
}

/* (controller_layout.h's kinds: 0, the Xbox's layout, for every one) */
int platform_gamepad_describe(int index, char *name, size_t size)
{
	if (size)
		snprintf(name, size, "%s", index <= 0 ? "Wii U GamePad" : "Controller");
	return 0;
}

int halo_input_from_name(const char *name)
{
	(void)name;
	return -1;
}

void halo_input_name(int input, char *name, size_t size)
{
	(void)input;
	if (size)
		name[0] = 0;
}

void halo_input_shown_name(const char *binding, char *shown, size_t size)
{
	if (size)
		snprintf(shown, size, "%s", binding);
}

unsigned long halo_keyboard_actions(short controller_index)
{
	(void)controller_index;
	return 0;
}

int halo_keyboard_prompts(void)
{
	return 0;
}

int halo_keyboard_action_shown(int action, char *shown, unsigned long size)
{
	(void)action;
	if (size)
		shown[0] = 0;
	return 0;
}

void test_input_hold_action(int hold)
{
	(void)hold;
}

/* ---------- XAPI */

static DWORD connected_ports(void)
{
	struct wiiu_pad_state state;
	DWORD mask = 0;
	int port;

	for (port = 0; port < PORT_COUNT; port++)
	{
		if (wiiu_pad_read(port, &state))
			mask |= 1u << port;
	}
	return mask;
}

VOID WINAPI XInitDevices(DWORD preallocation_type_count, PXDEVICE_PREALLOC_TYPE preallocation_types)
{
	(void)preallocation_type_count;
	(void)preallocation_types;
	platform_sdl_initialize();
	wiiu_pad_update();
}

BOOL WINAPI XGetDeviceChanges(PXPP_DEVICE_TYPE device_type, PDWORD insertions, PDWORD removals)
{
	*insertions = 0;
	*removals = 0;
	if (device_type == XDEVICE_TYPE_GAMEPAD)
	{
		DWORD connected = connected_ports();

		*insertions = connected & ~reported_gamepads;
		*removals = reported_gamepads & ~connected;
		reported_gamepads = connected;
	}
	else if (device_type == XDEVICE_TYPE_DEBUG_KEYBOARD && !reported_keyboard)
	{
		*insertions = 1;
		reported_keyboard = TRUE;
	}
	return *insertions || *removals;
}

HANDLE WINAPI XInputOpen(PXPP_DEVICE_TYPE device_type, DWORD port, DWORD slot,
	PXINPUT_POLLING_PARAMETERS polling_parameters)
{
	(void)slot;
	(void)polling_parameters;
	if (device_type == XDEVICE_TYPE_GAMEPAD && port < PORT_COUNT)
	{
		controllers[port].open = TRUE;
		return (HANDLE)&controllers[port];
	}
	if (device_type == XDEVICE_TYPE_DEBUG_KEYBOARD && port == 0)
	{
		keyboard_device.open = TRUE;
		return (HANDLE)&keyboard_device;
	}
	SetLastError(ERROR_DEVICE_NOT_CONNECTED);
	return NULL;
}

VOID WINAPI XInputClose(HANDLE device)
{
	struct controller *controller = (struct controller *)device;

	if (controller)
		controller->open = FALSE;
}

DWORD WINAPI XInputGetState(HANDLE device, PXINPUT_STATE state)
{
	struct wiiu_pad_state pad;
	int port;

	memset(state, 0, sizeof(*state));
	for (port = 0; port < PORT_COUNT; port++)
	{
		if (device == (HANDLE)&controllers[port])
			break;
	}
	if (port == PORT_COUNT || !controllers[port].open)
		return ERROR_DEVICE_NOT_CONNECTED;
	/* (the first port's read is the frame's: the system's events and the
	controllers' new states) */
	if (port == 0)
	{
		platform_pump_events();
		wiiu_pad_update();
	}
	if (!wiiu_pad_read(port, &pad))
		return ERROR_DEVICE_NOT_CONNECTED;
	state->dwPacketNumber = ++controllers[port].packet;
	state->Gamepad.wButtons = pad.digital;
	memcpy(state->Gamepad.bAnalogButtons, pad.analog, sizeof(state->Gamepad.bAnalogButtons));
	state->Gamepad.sThumbLX = pad.thumb_lx;
	state->Gamepad.sThumbLY = pad.thumb_ly;
	state->Gamepad.sThumbRX = pad.thumb_rx;
	state->Gamepad.sThumbRY = pad.thumb_ry;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputSetState(HANDLE device, PXINPUT_FEEDBACK feedback)
{
	if (!feedback)
		return ERROR_INVALID_PARAMETER;
	if (device == (HANDLE)&controllers[0])
		wiiu_pad_rumble(0, feedback->Rumble.wLeftMotorSpeed || feedback->Rumble.wRightMotorSpeed);
	feedback->Header.dwStatus = ERROR_SUCCESS;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugInitKeyboardQueue(PXINPUT_DEBUG_KEYQUEUE_PARAMETERS parameters)
{
	(void)parameters;
	return ERROR_SUCCESS;
}

DWORD WINAPI XInputDebugGetKeystroke(PXINPUT_DEBUG_KEYSTROKE keystroke)
{
	memset(keystroke, 0, sizeof(*keystroke));
	return ERROR_HANDLE_EOF;
}
