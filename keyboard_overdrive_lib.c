#include "common.h"
#include "stdbool.h"
#include "system.h"
#include "host_command.h" // prune
#include "keyboard_overdrive.h"

#define CPUTS(outstr) cputs(CC_KEYBOARD, outstr)
#define CPRINTS(format, args...) cprints(CC_KEYBOARD, format, ## args)
#define CPRINTF(format, args...) cprintf(CC_KEYBOARD, format, ## args)

// much easier to have a lookup table (256 bytes) for all keycodes to scancodes.
// Instead of storing the entire raw set2 scancode, which is somewhat expensive,
// we're going to compress it. All of the scancodes we care about range from 0x00
// to 0x8f and 0xe000 to 0xe07f. We have slightly less than one magic bit to work
// with. If bit 0x80 is set, it might be 0x83. Otherwise, we can reserve it as the
// prefix that means |0xE000. There is no 0xE003, so there's no chance of collision.
// We can also reserve 0xFF and 0xFE for the two complicated keys, PAUSE and BREAK,
// which require 6 and 4 byte scancodes respectively. That way lies madness.
//
#define _E0(x) (0x80|(x))
static const uint8_t s_keyCodeToCompressedScanCodeMapping[] = {
	[KC_0] = 0x45,         [KC_1] = 0x16,         [KC_2] = 0x1E,         [KC_3] = 0x26,         [KC_4] = 0x25,         [KC_5] = 0x2E,         [KC_6] = 0x36,
	[KC_7] = 0x3D,         [KC_8] = 0x3E,         [KC_9] = 0x46,         [KC_A] = 0x1C,         [KC_B] = 0x32,         [KC_C] = 0x21,         [KC_D] = 0x23,
	[KC_E] = 0x24,         [KC_F] = 0x2B,         [KC_G] = 0x34,         [KC_H] = 0x33,         [KC_I] = 0x43,         [KC_J] = 0x3B,         [KC_K] = 0x42,
	[KC_L] = 0x4B,         [KC_M] = 0x3A,         [KC_N] = 0x31,         [KC_O] = 0x44,         [KC_P] = 0x4D,         [KC_Q] = 0x15,         [KC_R] = 0x2D,
	[KC_S] = 0x1B,         [KC_T] = 0x2C,         [KC_U] = 0x3C,         [KC_V] = 0x2A,         [KC_W] = 0x1D,         [KC_X] = 0x22,         [KC_Y] = 0x35,
	[KC_Z] = 0x1A,         [KC_BS] = 0x66,        [KC_BSLS] = 0x5D,      [KC_CAPS] = 0x58,      [KC_COMM] = 0x41,      [KC_INS] = _E0(0x70),  [KC_DEL] = _E0(0x71),
	[KC_DOT] = 0x49,       [KC_DOWN] = _E0(0x72), [KC_END] = _E0(0x69),  [KC_ENT] = 0x5A,       [KC_EQL] = 0x55,       [KC_ESC] = 0x76,       [KC_GRV] = 0x0E,
	[KC_HOME] = _E0(0x6C), [KC_LALT] = 0x11,      [KC_LBRC] = 0x54,      [KC_LCTL] = 0x14,      [KC_LEFT] = _E0(0x6B), [KC_LSFT] = 0x12,      [KC_LWIN] = _E0(0x1F),
	[KC_MENU] = _E0(0x2F), [KC_MINS] = 0x4E,      [KC_NLCK] = 0x77,      [KC_NUBS] = 0x61,      [KC_QUOT] = 0x52,      [KC_RALT] = _E0(0x11), [KC_RBRC] = 0x5B,
	[KC_RCTL] = _E0(0x14), [KC_RGHT] = _E0(0x74), [KC_RSFT] = 0x59,      [KC_SCLN] = 0x4C,      [KC_SLSH] = 0x4A,      [KC_SPC] = 0x29,       [KC_TAB] = 0x0D,
	[KC_UP] = _E0(0x75),   [KC_F1] = 0x05,        [KC_F2] = 0x06,        [KC_F3] = 0x04,        [KC_F4] = 0x0C,        [KC_F5] = 0x03,        [KC_F6] = 0x0B,
	[KC_F7] = 0x83,        [KC_F8] = 0x0A,        [KC_F9] = 0x01,        [KC_F10] = 0x09,       [KC_F11] = 0x78,       [KC_F12] = 0x07,       [KC_KP_0] = 0x70,
	[KC_KP_1] = 0x69,      [KC_KP_2] = 0x72,      [KC_KP_3] = 0x7A,      [KC_KP_4] = 0x6B,      [KC_KP_5] = 0x73,      [KC_KP_6] = 0x74,      [KC_KP_7] = 0x6C,
	[KC_KP_8] = 0x75,      [KC_KP_9] = 0x7D,      [KC_PAST] = 0x7C,      [KC_PDOT] = 0x71,      [KC_PENT] = _E0(0x5A), [KC_PGDN] = _E0(0x7A), [KC_PGUP] = _E0(0x7D),
	[KC_PINS] = _E0(0x70), [KC_PMNS] = 0x7B,      [KC_PPLS] = 0x79,      [KC_PSLS] = _E0(0x4A), [KC_MSEL] = _E0(0x50), [KC_MNXT] = _E0(0x4D), [KC_MPRV] = _E0(0x15),
	[KC_MPLY] = _E0(0x34), [KC_VOLD] = _E0(0x21), [KC_VOLU] = _E0(0x32), [KC_MUTE] = _E0(0x23), [KC_SLCK] = 0x7E,
	[KC_PSCR] = 0xFC, // E0-7C PrintScr - Might need another special key (E0 12)
	[KC_CTBR] = 0xFE, // E0-7E Ctrl+Break
	[KC_PAUS] = 0xFF, // E1-14-77-E1-F0-14-F0-77 Pause
};
#undef _E0

static layer_state_t base_layers   = 0b00000001;
static layer_state_t active_layers = 0b00000000;

// This is used to cache which layer a pressed key came from
uint8_t act_pressed_layers[(KEYBOARD_COLS_MAX * KEYBOARD_ROWS + 7)/8][LAYER_BITS] = {{0}};

static void set_pressed_layer(uint8_t row, uint8_t col, uint8_t layer) {
	uint8_t keyNum = row * KEYBOARD_COLS_MAX + col;
	int cidx = keyNum / 8;
	int rbit = keyNum % 8;
	for(int i = 0; i < LAYER_BITS; ++i) {
		act_pressed_layers[cidx][i] ^= (-((layer & (1U << i)) != 0) ^ act_pressed_layers[cidx][i]) & (1U << rbit);
	}
}

static uint8_t get_pressed_layer(uint8_t row, uint8_t col) {
	uint8_t keyNum = row * KEYBOARD_COLS_MAX + col;
	int cidx = keyNum / 8;
	int rbit = keyNum % 8;
	uint8_t layer = 0;
	for(int i = 0; i < LAYER_BITS; ++i) {
		layer |= ((act_pressed_layers[cidx][i] & (1U << rbit)) != 0) << i;
	}
	return layer;
}

struct action {
	uint16_t value;
	uint8_t layer;
};

static struct action get_key_action(uint8_t row, uint8_t col) {
	struct action act = {0};
	uint8_t layers = base_layers | active_layers;
	for(int i = NUM_LAYERS_MAX - 1; i >= 0; --i) {
		if (layers & (1<<i)) {
			act.value = keymaps[i][col][row];
			act.layer = i;
			if (act.value == KC_TRANSPARENT) {
				continue; // peer through this layer
			}
			goto done;
		}
	}
	// We didn't hit it somehow?
	act.value = KC_NO;
	act.layer = 0;
done:
	return act;
}
// 8< LIB >8

#if 0
#define _RING_BUF_RD(name) name##_buf_rd
#define _RING_BUF_WR(name) name##_buf_wr
#define RING_BUFFER(type, name, size) \
	static type name##_buf[size]; \
	static uint8_t name##_buf_wr, name##buf_rd; \
	static bool name##_push(const type* v) { \
		if (((_RING_BUF_WR(name) + 1) % size) == _RING_BUF_RD(name)) { \
			return false; \
		} \
		memcpy(&name##_buf[_RING_BUF_WR(name)], v, sizeof(*v)); \
		_RING_BUF_WR(name) = (_RING_BUF_WR(name) + 1) % size; \
		return true; \
	} \
	static bool name##_pop(type* v) { \
		if (_RING_BUF_RD(name) == _RING_BUF_WR(name)) { \
			return false; \
		} \
		memcpy(v, &name##_buf[_RING_BUF_RD(name)], sizeof(*v)); \
		_RING_BUF_RD(name) = (_RING_BUF_RD(name) + 1) % size; \
		return true; \
	}

void process_deferred_key_events(void) {
}
#endif

__attribute__((weak)) uint8_t layer_state_set_kb(uint8_t state) { return state; }
__attribute__((weak)) uint8_t layer_state_set_user(uint8_t state) { return state; }

void layer_state_set(layer_state_t state) {
	state = layer_state_set_kb(state);
	state = layer_state_set_user(state);
	active_layers = state;
}

void layer_on(uint8_t layer) {
	layer_state_set(active_layers | (1<<layer));
}

void layer_off(uint8_t layer) {
	layer_state_set(active_layers & ~(1<<layer));
}

void layer_invert(uint8_t layer) {
	layer_state_set(active_layers ^ (1<<layer));
}

bool layer_state_cmp(layer_state_t state, uint8_t layer) {
	return (state & (1<<layer)) != 0;
}

bool layer_state_is(uint8_t layer) {
	return layer_state_cmp(active_layers, layer);
}

__attribute__((weak)) bool process_record_kb(uint16_t keycode, uint8_t pressed) { return true; }
__attribute__((weak)) bool process_record_user(uint16_t keycode, uint8_t pressed) { return true; }

static int8_t global_enable_keyboard_overload = 0;
// return 0 = NOT INSTALLED 1 = make_code normal SC 2 = STOP EVENT
int8_t process_record_overload(int8_t row, int8_t col, int8_t pressed, uint16_t* make_code) {
	struct action act;
	if (!global_enable_keyboard_overload) {
		return 0;
	}

	CPRINTF("--- KEYSCAN ROW %d COL %d %s LMASK %x ---\n", row, col, pressed?"DOWN":"UP", active_layers|base_layers);
	if (pressed) {
		act = get_key_action(row, col);
		CPRINTF("ACT=%4.04x LAY=%d\n", act.value, act.layer);
		set_pressed_layer(row, col, act.layer);
	} else {
		uint8_t l = get_pressed_layer(row, col);
		CPRINTF("ACT=%4.04x LAY=%d [FORCED]\n", keymaps[l][col][row], l);
		act.value = keymaps[l][col][row];
		act.layer = l;
		set_pressed_layer(row, col, 0);
	}

	if (!process_record_kb(act.value, pressed)) {
		return 2;
	}

	if (!process_record_user(act.value, pressed)) {
		return 2;
	}

	switch (KEY_GET_OP(act.value)) {
		case OP_NONE: {
			uint16_t scancode = s_keyCodeToCompressedScanCodeMapping[KEY_GET_KC(act.value)];
			scancode ^= (-((scancode & 0x80) != 0) & 0xE080); // simultaneously sets high 0xE000 and clears 0x80 iff scancode contains 0x80
			if (scancode == 0xe003) scancode = 0x83; // special-case the only value that breaks our optimization
			*make_code = scancode;
			return 1; // SCAN IT OUT LIKE NORMAL
			break;
		}
		case OP_MOD_TAP:
			if (pressed)
				CPUTS("mod tap\n");
			break;
		case OP_LAYER_TAP: {
			uint8_t layer = KEY_GET_LAYER(act.value);
			layer_state_set(active_layers ^ ((-(pressed != 0) ^ active_layers) & (1<<layer)));
			if (pressed) {
				CPRINTF("+LAY %d\n", layer);
				//active_layers |= 1<<layer;
			} else {
				CPRINTF("-LAY %d\n", layer);
				//active_layers &= ~(1<<layer);
			}
			break;
		}
		case OP_LAYER_TOGGLE: {
			uint8_t layer = KEY_GET_LAYER(act.value);
			if (pressed) {
				CPRINTF("~LAY %d\n", layer);
				layer_invert(layer);
				//active_layers ^= 1<<layer;
			} // toggle actions take effect on press, not release
			break;
		}
	}
	return 2;
}

//////////////////////////////////
#define EC_CMD_SET_KEYBOARD_OVERDRIVE 0x3E7F

struct ec_params_set_keyboard_overdrive {
	uint8_t on;
} __ec_align1;
static enum ec_status keyboard_overdrive(struct host_cmd_handler_args *args)
{
	const struct ec_params_set_keyboard_overdrive *p = args->params;
	global_enable_keyboard_overload = p->on != 0;
	args->response_size = 0;
	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_SET_KEYBOARD_OVERDRIVE, keyboard_overdrive, EC_VER_MASK(0));