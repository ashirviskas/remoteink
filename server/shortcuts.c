#include "../common/exceptions.h"
#include "../common/messages.h"
#include "control.h"
#include "screen.h"
#include "shortcuts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xutil.h>
#include <xcb/xcb_keysyms.h>


xcb_connection_t *display;
xcb_screen_t *screen;
xcb_window_t root;

xcb_key_symbols_t *key_symbols_table;

ExcCode shortcuts_init(xcb_connection_t *cur_display,
		xcb_screen_t *cur_screen, xcb_window_t cur_root) {
	display = cur_display;
	screen = cur_screen;
	root = cur_root;
			
	key_symbols_table = xcb_key_symbols_alloc(display);
	if (key_symbols_table == NULL)
		THROW(ERR_X_REQUEST);
	return 0;
}


struct ModifierRecord {
	const char *title;
	int value;
};

struct ModifierRecord modifier_records[] = {
	{"Shift", XCB_MOD_MASK_SHIFT},
	{"CapsLock", XCB_MOD_MASK_LOCK},
	{"Ctrl", XCB_MOD_MASK_CONTROL},
	{"Alt", XCB_MOD_MASK_1},
	{"NumLock", XCB_MOD_MASK_2},
	{"AltLang", XCB_MOD_MASK_3},
	{"Kana", XCB_MOD_MASK_4},
	{"ScrollLock", XCB_MOD_MASK_5},
	{NULL, 0},
};

#define ERR_SHORTCUT_UNKNOWN_MODIFIER "Unknown modifier in shortcut \"%s\""
#define ERR_SHORTCUT_UNKNOWN_KEY "Unknown key \"%s\" in shortcut \"%s\""

ExcCode shortcuts_parse(const char *str, struct Hotkey *res) {
	int word_begin = 0;
	res->modifiers = 0;
	int i;
	for (i = 0; str[i]; i++)
		if (str[i] == '+') {
			int modifier_found = 0;
			int j;
			for (j = 0; modifier_records[j].title != NULL; j++)
				if (!strncmp(str + word_begin, modifier_records[j].title,
						i - word_begin)) {
					modifier_found = 1;
					res->modifiers |= modifier_records[j].value;
					break;
				}
			if (!modifier_found)
				THROW(ERR_SHORTCUT_UNKNOWN_MODIFIER, str);
			word_begin = i + 1;
		}
		
	KeySym keysym = XStringToKeysym(str + word_begin);
	if (keysym == NoSymbol)
		THROW(ERR_SHORTCUT_UNKNOWN_KEY, str + word_begin, str);
	res->keycodes = xcb_key_symbols_get_keycode(key_symbols_table, keysym);
	if (res->keycodes == NULL)
		THROW(ERR_SHORTCUT_UNKNOWN_KEY, str + word_begin, str);
	return 0;
}


// Modifiers that can be set in addition to required
#define EXTRA_MODIFIERS (XCB_MOD_MASK_LOCK | \
		XCB_MOD_MASK_2 | XCB_MOD_MASK_3 | XCB_MOD_MASK_4 | XCB_MOD_MASK_5)

ExcCode grab_keycode(uint8_t owner_events,
		xcb_keycode_t keycode, unsigned modifiers,
		Window grab_window, uint8_t pointer_mode, uint8_t keyboard_mode) {
	unsigned rem_modifiers = EXTRA_MODIFIERS & ~modifiers;
	// Iterate through all submasks of rem_modifiers and grab key with
	// required modifiers and all variants of combinations of extra modifiers
	for (unsigned submask = rem_modifiers; ;
			submask = (submask - 1) & rem_modifiers) {
		xcb_void_cookie_t cookie = xcb_grab_key_checked(display,
				owner_events, grab_window,
				modifiers | submask, keycode, pointer_mode, keyboard_mode);
		if (xcb_request_check(display, cookie) != NULL)
			THROW(ERR_X_REQUEST);
		if (!submask)
			break;
	}
	return 0;
}

ExcCode grab_hotkey(const struct Hotkey *hotkey) {
	for (int i = 0; hotkey->keycodes[i] != XCB_NO_SYMBOL; i++)
		TRY(grab_keycode(1, hotkey->keycodes[i], hotkey->modifiers, root,
				 XCB_GRAB_MODE_ASYNC,  XCB_GRAB_MODE_ASYNC));
	return 0;
}


ExcCode shortcuts_handle_start(const struct Shortcut shortcuts[]) {
	for (int i = 0; shortcuts[i].handler != NULL; i++)
		TRY(grab_hotkey(&shortcuts[i].hotkey));
	
	xcb_generic_event_t *event;
	while ((event = xcb_wait_for_event(display)) != NULL) {
		pthread_mutex_lock(&control_lock);
		
		do {
			if ((event->response_type & ~0x80) == XCB_KEY_RELEASE &&
					control_context_get() != NULL) {
				xcb_key_release_event_t *key_release_event =
						(xcb_key_release_event_t *) event;
						
				for (int i = 0; shortcuts[i].handler != NULL; i++) {
					unsigned modifiers = shortcuts[i].hotkey.modifiers;
					if ((modifiers & key_release_event->state) != modifiers)
						continue;
					for (int j = 0;
							shortcuts[i].hotkey.keycodes[j] != XCB_NO_SYMBOL;
							j++)
						if (key_release_event->detail ==
								shortcuts[i].hotkey.keycodes[j]) {
							shortcuts[i].handler();
							break;
						}
				}
			}
			free(event);
		} while ((event = xcb_poll_for_event(display)) != NULL);
		
		pthread_mutex_unlock(&control_lock);
	}
	THROW(ERR_X_REQUEST);
}
