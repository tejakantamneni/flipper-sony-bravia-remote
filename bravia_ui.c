#include <furi.h>
#include <furi_hal_infrared.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/canvas.h>
#include <input/input.h>
#include <infrared.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#include "bravia_glyphs.h"

// Sony Bravia remote: four pages (NAV, MEDIA, TV, MORE). Each page maps the five D-pad keys
// straight to five commands; hold OK runs the page's secondary action, short Back sends TV Back,
// hold Left/Right changes page and hold Back exits.

#define BRAVIA_PAGE_FILE     APP_DATA_PATH("page")
#define BRAVIA_FLASH_MS      1100
#define BRAVIA_TICK_MS       100
#define BRAVIA_SIRC_FRAMES   3 // the TV needs three frames per command (found testing HOME)
#define BRAVIA_FRAME_GAP_MS  45

// =====================================
// Infrared commands (codes from Sony_SFRTV5.ir)
// =====================================

typedef enum {
    CmdNone,
    CmdUp,
    CmdDown,
    CmdLeft,
    CmdRight,
    CmdOk,
    CmdBack,
    CmdHome,
    CmdInput,
    CmdPower,
    CmdVolUp,
    CmdVolDown,
    CmdMute,
    CmdRewind,
    CmdPlay,
    CmdPause,
    CmdForward,
    CmdStop,
    CmdOptions,
    CmdApps,
    CmdPrevious,
    CmdNext,
    CmdPictureOff,
    CmdCount,
    CmdPlayPause, // not a code: sends Pause or Play in turn, see bravia_resolve_cmd()
} BraviaCmd;

typedef struct {
    const char* name; // shown in the footer after sending
    InfraredProtocol protocol;
    uint8_t address;
    uint8_t command;
} BraviaIrCode;

static const BraviaIrCode bravia_codes[CmdCount] = {
    [CmdNone] = {"", InfraredProtocolSIRC, 0, 0},
    [CmdUp] = {"UP", InfraredProtocolSIRC, 0x01, 0x74},
    [CmdDown] = {"DOWN", InfraredProtocolSIRC, 0x01, 0x75},
    [CmdLeft] = {"LEFT", InfraredProtocolSIRC, 0x01, 0x34},
    [CmdRight] = {"RIGHT", InfraredProtocolSIRC, 0x01, 0x33},
    [CmdOk] = {"OK", InfraredProtocolSIRC, 0x01, 0x65},
    [CmdBack] = {"BACK", InfraredProtocolSIRC15, 0x97, 0x23},
    [CmdHome] = {"HOME", InfraredProtocolSIRC, 0x01, 0x60},
    [CmdInput] = {"INPUT", InfraredProtocolSIRC, 0x01, 0x25},
    [CmdPower] = {"POWER", InfraredProtocolSIRC, 0x01, 0x15},
    [CmdVolUp] = {"VOL+", InfraredProtocolSIRC, 0x01, 0x12},
    [CmdVolDown] = {"VOL-", InfraredProtocolSIRC, 0x01, 0x13},
    [CmdMute] = {"MUTE", InfraredProtocolSIRC, 0x01, 0x14},
    [CmdRewind] = {"REW", InfraredProtocolSIRC15, 0x97, 0x1B},
    [CmdPlay] = {"PLAY", InfraredProtocolSIRC15, 0x97, 0x1A},
    [CmdPause] = {"PAUSE", InfraredProtocolSIRC15, 0x97, 0x19},
    [CmdForward] = {"FWD", InfraredProtocolSIRC15, 0x97, 0x1C},
    [CmdStop] = {"STOP", InfraredProtocolSIRC15, 0x97, 0x18},
    [CmdOptions] = {"OPTIONS", InfraredProtocolSIRC15, 0x97, 0x36},
    [CmdApps] = {"APPS", InfraredProtocolSIRC15, 0xC4, 0x2A},
    [CmdPrevious] = {"PREV", InfraredProtocolSIRC15, 0x97, 0x3C},
    [CmdNext] = {"NEXT", InfraredProtocolSIRC15, 0x97, 0x3D},
    [CmdPictureOff] = {"PIC OFF", InfraredProtocolSIRC, 0x01, 0x3E},
};

// =====================================
// Page layout
// =====================================

typedef enum {
    PadUp,
    PadDown,
    PadLeft,
    PadRight,
    PadOk,
    PadCount,
} PadKey;

// Highlight targets besides the five pad keys.
enum {
    PressedNone = -1,
    PressedBack = PadCount,
    PressedHold,
};

typedef enum {
    ArrowNone,
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
} BraviaArrow;

typedef struct {
    BraviaArrow arrow;
    BraviaIconId icon;
    const char* label;
    BraviaCmd cmd;
} BraviaKeySpec;

typedef struct {
    const char* tab;
    BraviaKeySpec keys[PadCount];
    BraviaKeySpec hold; // hold OK; cmd == CmdNone opens the sleep timer
} BraviaPage;

#define BRAVIA_PAGE_COUNT 4

static const BraviaPage bravia_pages[BRAVIA_PAGE_COUNT] = {
    {
        .tab = "NAV",
        .keys =
            {
                [PadUp] = {ArrowUp, BraviaIconNone, NULL, CmdUp},
                [PadDown] = {ArrowDown, BraviaIconNone, NULL, CmdDown},
                [PadLeft] = {ArrowLeft, BraviaIconNone, NULL, CmdLeft},
                [PadRight] = {ArrowRight, BraviaIconNone, NULL, CmdRight},
                [PadOk] = {ArrowNone, BraviaIconNone, "OK", CmdOk},
            },
        .hold = {ArrowNone, BraviaIconHome, "HOME", CmdHome},
    },
    {
        .tab = "MEDIA",
        .keys =
            {
                [PadUp] = {ArrowNone, BraviaIconVolup, "VOL+", CmdVolUp},
                [PadDown] = {ArrowNone, BraviaIconVoldn, "VOL-", CmdVolDown},
                [PadLeft] = {ArrowNone, BraviaIconRew, "REW", CmdRewind},
                [PadRight] = {ArrowNone, BraviaIconFf, "FWD", CmdForward},
                [PadOk] = {ArrowNone, BraviaIconPlaypause, NULL, CmdPlayPause},
            },
        .hold = {ArrowNone, BraviaIconStop, "STOP", CmdStop},
    },
    {
        .tab = "TV",
        .keys =
            {
                [PadUp] = {ArrowNone, BraviaIconVolup, "VOL+", CmdVolUp},
                [PadDown] = {ArrowNone, BraviaIconVoldn, "VOL-", CmdVolDown},
                [PadLeft] = {ArrowNone, BraviaIconInput, "INPUT", CmdInput},
                [PadRight] = {ArrowNone, BraviaIconHome, "HOME", CmdHome},
                [PadOk] = {ArrowNone, BraviaIconMute, "MUTE", CmdMute},
            },
        .hold = {ArrowNone, BraviaIconPower, "POWER", CmdPower},
    },
    {
        .tab = "MORE",
        .keys =
            {
                [PadUp] = {ArrowNone, BraviaIconNone, "OPTIONS", CmdOptions},
                [PadDown] = {ArrowNone, BraviaIconApps, "APPS", CmdApps},
                [PadLeft] = {ArrowNone, BraviaIconPrev, "PREV", CmdPrevious},
                [PadRight] = {ArrowNone, BraviaIconNext, "NEXT", CmdNext},
                [PadOk] = {ArrowNone, BraviaIconPicoff, "OFF", CmdPictureOff},
            },
        .hold = {ArrowNone, BraviaIconMoon, "SLEEP", CmdNone},
    },
};

// x, y, width, height of each pad key on screen
static const uint8_t bravia_key_boxes[PadCount][4] = {
    [PadUp] = {38, 13, 52, 11},
    [PadDown] = {38, 41, 52, 11},
    [PadLeft] = {1, 25, 41, 15},
    [PadRight] = {86, 25, 41, 15},
    [PadOk] = {45, 25, 38, 15},
};

static const uint8_t bravia_sleep_minutes[] = {0, 15, 30, 60, 90};
#define BRAVIA_SLEEP_OPTIONS  COUNT_OF(bravia_sleep_minutes)
#define BRAVIA_SLEEP_DEFAULT  2 // 30 minutes

// =====================================
// Application state
// =====================================

typedef struct {
    uint8_t page;
    int8_t pressed; // PadKey, PressedBack, PressedHold or PressedNone
    int8_t picker; // index into bravia_sleep_minutes while the picker is open, else -1
    char flash[24]; // footer message, empty when idle
    uint32_t flash_until;
    bool sleep_on;
    uint32_t sleep_end;
    // The TV has no Play/Pause toggle: 0x1A only plays and 0x19 only pauses. The app assumes
    // something is playing at launch, alternates, and shows the next command on the OK key, so
    // one extra press resyncs if the TV was paused another way.
    bool paused;
} BraviaState;

typedef struct {
    BraviaState state;
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;
    Gui* gui;
    NotificationApp* notifications;
    bool backlight_forced;
    volatile bool repeat_queued;
} BraviaApp;

// What the input handler asks the main loop to do.
typedef struct {
    BraviaCmd cmd;
    bool exit;
    bool sleep_changed;
} BraviaAction;

// =====================================
// Bitmap drawing helpers
// =====================================

static void draw_text5(Canvas* canvas, int x, int y, const char* text, bool bold) {
    for(; *text; text++) {
        char ch = *text;
        if(ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
        if(ch < 32 || ch > 90) ch = ' ';
        const uint8_t* glyph = bravia_font5[ch - 32];
        for(int col = 0; col < 5; col++) {
            for(int row = 0; row < 7; row++) {
                if(glyph[col] & (1 << row)) {
                    canvas_draw_dot(canvas, x + col, y + row);
                    if(bold) canvas_draw_dot(canvas, x + col + 1, y + row);
                }
            }
        }
        x += bold ? 7 : 6;
    }
}

static int text5_width(const char* text, bool bold) {
    const int advance = bold ? 7 : 6;
    const int len = strlen(text);
    return len ? len * advance - (advance - 5) : 0;
}

static void draw_text3(Canvas* canvas, int x, int y, const char* text) {
    for(; *text; text++) {
        char ch = *text;
        if(ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
        if(ch < 32 || ch > 90) ch = ' ';
        const uint16_t glyph = bravia_font3[ch - 32];
        for(int i = 0; i < 15; i++) {
            if(glyph & (1 << (14 - i))) canvas_draw_dot(canvas, x + i % 3, y + i / 3);
        }
        x += 4;
    }
}

static int text3_width(const char* text) {
    const int len = strlen(text);
    return len ? len * 4 - 1 : 0;
}

static void draw_icon(Canvas* canvas, int x, int y, BraviaIconId id) {
    const BraviaIcon* icon = &bravia_icons[id];
    for(int row = 0; row < icon->height; row++) {
        for(int col = 0; col < icon->width; col++) {
            if(icon->rows[row] & (1 << col)) canvas_draw_dot(canvas, x + col, y + row);
        }
    }
}

// Same corner test as the mockup, so the dotted chip matches canvas_draw_rframe closely.
static bool in_round_rect(int i, int j, int w, int h, int r) {
    if(i < 0 || j < 0 || i >= w || j >= h) return false;
    const int cx = i < r ? r : (i > w - 1 - r ? w - 1 - r : i);
    const int cy = j < r ? r : (j > h - 1 - r ? h - 1 - r : j);
    return (i - cx) * (i - cx) + (j - cy) * (j - cy) <= r * r + 1;
}

static void draw_dotted_rframe(Canvas* canvas, int x, int y, int w, int h, int r) {
    for(int j = 0; j < h; j++) {
        for(int i = 0; i < w; i++) {
            if(!in_round_rect(i, j, w, h, r) || ((i + j) & 1)) continue;
            if(!in_round_rect(i - 1, j, w, h, r) || !in_round_rect(i + 1, j, w, h, r) ||
               !in_round_rect(i, j - 1, w, h, r) || !in_round_rect(i, j + 1, w, h, r)) {
                canvas_draw_dot(canvas, x + i, y + j);
            }
        }
    }
}

static void draw_arrow(Canvas* canvas, int cx, int cy, BraviaArrow arrow) {
    // filled triangle, 9 wide and 5 deep; k = 0 is the tip
    for(int k = 0; k < 5; k++) {
        switch(arrow) {
        case ArrowUp:
            canvas_draw_line(canvas, cx - k, cy - 2 + k, cx + k, cy - 2 + k);
            break;
        case ArrowDown:
            canvas_draw_line(canvas, cx - k, cy + 2 - k, cx + k, cy + 2 - k);
            break;
        case ArrowLeft:
            canvas_draw_line(canvas, cx - 2 + k, cy - k, cx - 2 + k, cy + k);
            break;
        case ArrowRight:
            canvas_draw_line(canvas, cx + 2 - k, cy - k, cx + 2 - k, cy + k);
            break;
        default:
            break;
        }
    }
}

// =====================================
// Screen drawing
// =====================================

static const BraviaKeySpec bravia_key_play = {ArrowNone, BraviaIconPlay, "PLAY", CmdPlay};
static const BraviaKeySpec bravia_key_pause = {ArrowNone, BraviaIconPause, "PAUSE", CmdPause};

static void draw_key(Canvas* canvas, const uint8_t box[4], const BraviaKeySpec* spec, bool pressed) {
    const int x = box[0], y = box[1], w = box[2], h = box[3];
    canvas_set_color(canvas, ColorBlack);
    if(pressed) {
        canvas_draw_rbox(canvas, x, y, w, h, 3);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_draw_rframe(canvas, x, y, w, h, 3);
    }

    const int cx = x + w / 2, cy = y + h / 2;
    if(spec->arrow != ArrowNone) {
        draw_arrow(canvas, cx, cy, spec->arrow);
    } else {
        const int icon_w = bravia_icons[spec->icon].width;
        const int label_w = spec->label ? text5_width(spec->label, false) : 0;
        const int total = icon_w + (icon_w && label_w ? 3 : 0) + label_w;
        int sx = cx - total / 2;
        if(icon_w) {
            draw_icon(canvas, sx, cy - bravia_icons[spec->icon].height / 2, spec->icon);
            sx += icon_w + (label_w ? 3 : 0);
        }
        if(spec->label) draw_text5(canvas, sx, cy - 3, spec->label, false);
    }
    canvas_set_color(canvas, ColorBlack);
}

static void draw_tabs(Canvas* canvas, uint8_t page) {
    int x = 5;
    for(uint8_t i = 0; i < BRAVIA_PAGE_COUNT; i++) {
        const char* tab = bravia_pages[i].tab;
        const int w = text5_width(tab, false) + 8;
        if(i == page) {
            canvas_draw_rbox(canvas, x, 0, w, 10, 2);
            canvas_set_color(canvas, ColorWhite);
            draw_text5(canvas, x + 4, 1, tab, false);
            canvas_set_color(canvas, ColorBlack);
        } else {
            draw_text5(canvas, x + 4, 1, tab, false);
        }
        x += w + 1;
    }
    draw_text3(canvas, 1, 3, "<");
    draw_text3(canvas, 124, 3, ">");
    canvas_draw_line(canvas, 0, 11, 127, 11);
}

static void draw_picker(Canvas* canvas, int8_t selected) {
    const char* title = "SLEEP TIMER";
    draw_text5(canvas, 64 - text5_width(title, true) / 2, 14, title, true);

    for(uint8_t i = 0; i < BRAVIA_SLEEP_OPTIONS; i++) {
        const int x = 3 + i * 25;
        char label[4];
        if(bravia_sleep_minutes[i]) {
            snprintf(label, sizeof(label), "%u", bravia_sleep_minutes[i]);
        } else {
            strlcpy(label, "OFF", sizeof(label));
        }
        if(i == selected) {
            canvas_draw_rbox(canvas, x, 26, 22, 13, 3);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_draw_rframe(canvas, x, 26, 22, 13, 3);
        }
        draw_text5(canvas, x + 11 - text5_width(label, false) / 2, 29, label, false);
        canvas_set_color(canvas, ColorBlack);
    }

    const char* note = "MINUTES. SENDS POWER AT 00:00";
    draw_text3(canvas, 64 - text3_width(note) / 2, 44, note);
    canvas_draw_line(canvas, 0, 53, 127, 53);
    draw_text3(canvas, 2, 57, "< > PICK   OK SET   BACK CANCEL");
}

static void draw_hold_chip(Canvas* canvas, const BraviaKeySpec* hold, bool pressed) {
    // dotted outline = hold, ring glyph = the OK key
    const int x = 1, y = 13, w = 36, h = 11;
    if(pressed) {
        canvas_draw_rbox(canvas, x, y, w, h, 3);
        canvas_set_color(canvas, ColorWhite);
    } else {
        draw_dotted_rframe(canvas, x, y, w, h, 3);
    }

    const int label_w = text3_width(hold->label);
    int icon_w = bravia_icons[hold->icon].width;
    int total = 5 + 2 + icon_w + 2 + label_w;
    if(total > 33) { // no room for the icon (POWER)
        icon_w = 0;
        total = 5 + 2 + label_w;
    }
    int hx = x + w / 2 - total / 2;
    draw_icon(canvas, hx, 16, BraviaIconOkring);
    hx += 7;
    if(icon_w) {
        draw_icon(canvas, hx, 18 - bravia_icons[hold->icon].height / 2, hold->icon);
        hx += icon_w + 2;
    }
    draw_text3(canvas, hx, 16, hold->label);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_footer(Canvas* canvas, const BraviaState* state) {
    canvas_draw_line(canvas, 0, 53, 127, 53);
    if(state->flash[0]) {
        canvas_draw_box(canvas, 0, 54, 128, 10);
        canvas_set_color(canvas, ColorWhite);
        draw_text3(canvas, 64 - text3_width(state->flash) / 2, 57, state->flash);
        canvas_set_color(canvas, ColorBlack);
        return;
    }

    draw_text3(canvas, 2, 57, "HOLD < > PAGE");
    if(state->sleep_on) {
        const int32_t left_ms = (int32_t)(state->sleep_end - furi_get_tick());
        const uint32_t left_s = left_ms > 0 ? ((uint32_t)left_ms + 999) / 1000 : 0;
        char time[12];
        snprintf(time, sizeof(time), "%02lu:%02lu", left_s / 60, left_s % 60);
        const int w = text3_width(time);
        draw_icon(canvas, 126 - w - 8, 55, BraviaIconMoon);
        draw_text3(canvas, 126 - w, 57, time);
    } else {
        const char* exit_hint = "HOLD BACK EXIT";
        draw_text3(canvas, 126 - text3_width(exit_hint), 57, exit_hint);
    }
}

static void bravia_draw(Canvas* canvas, void* context) {
    BraviaApp* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    const BraviaState state = app->state;
    furi_mutex_release(app->mutex);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    draw_tabs(canvas, state.page);

    if(state.picker >= 0) {
        draw_picker(canvas, state.picker);
        return;
    }

    const BraviaPage* page = &bravia_pages[state.page];
    for(int key = 0; key < PadCount; key++) {
        const BraviaKeySpec* spec = &page->keys[key];
        if(spec->cmd == CmdPlayPause) spec = state.paused ? &bravia_key_play : &bravia_key_pause;
        draw_key(canvas, bravia_key_boxes[key], spec, state.pressed == key);
    }
    draw_hold_chip(canvas, &page->hold, state.pressed == PressedHold);

    // Back chip sits bottom-right, where the Back key is on the device.
    if(state.pressed == PressedBack) {
        canvas_draw_rbox(canvas, 92, 41, 35, 11, 3);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_draw_rframe(canvas, 92, 41, 35, 11, 3);
    }
    draw_icon(canvas, 96, 43, BraviaIconBack);
    draw_text3(canvas, 106, 44, "BACK");
    canvas_set_color(canvas, ColorBlack);

    draw_footer(canvas, &state);
}

// =====================================
// Infrared transmitter
// =====================================

static FuriHalInfraredTxGetDataState bravia_ir_callback(void* context, uint32_t* duration, bool* level) {
    InfraredEncoderHandler* encoder = context;
    InfraredStatus status = infrared_encode(encoder, duration, level);
    return status == InfraredStatusDone ? FuriHalInfraredTxGetDataStateLastDone :
                                          FuriHalInfraredTxGetDataStateOk;
}

static bool bravia_send(BraviaCmd cmd) {
    const BraviaIrCode* code = &bravia_codes[cmd];
    InfraredMessage message = {
        .protocol = code->protocol,
        .address = code->address,
        .command = code->command,
        .repeat = false,
    };

    InfraredEncoderHandler* encoder = infrared_alloc_encoder();
    if(!encoder) return false;

    const uint32_t frequency = infrared_get_protocol_frequency(message.protocol);
    const float duty_cycle = infrared_get_protocol_duty_cycle(message.protocol);

    for(uint8_t i = 0; i < BRAVIA_SIRC_FRAMES; i++) {
        infrared_reset_encoder(encoder, &message);
        furi_hal_infrared_async_tx_set_data_isr_callback(bravia_ir_callback, encoder);
        furi_hal_infrared_async_tx_start(frequency, duty_cycle);
        furi_hal_infrared_async_tx_wait_termination();
        if(i + 1 < BRAVIA_SIRC_FRAMES) furi_delay_ms(BRAVIA_FRAME_GAP_MS);
    }

    infrared_free_encoder(encoder);
    return true;
}

// =====================================
// Input handling (called with the mutex held)
// =====================================

static PadKey bravia_pad_key(InputKey key) {
    switch(key) {
    case InputKeyUp:
        return PadUp;
    case InputKeyDown:
        return PadDown;
    case InputKeyLeft:
        return PadLeft;
    case InputKeyRight:
        return PadRight;
    default:
        return PadOk;
    }
}

// Turns the Play/Pause key into the next real command and tracks the assumed TV state.
static BraviaCmd bravia_resolve_cmd(BraviaState* state, BraviaCmd cmd) {
    if(cmd == CmdPlayPause) {
        cmd = state->paused ? CmdPlay : CmdPause;
    }
    if(cmd == CmdPlay || cmd == CmdRewind || cmd == CmdForward) state->paused = false;
    if(cmd == CmdPause || cmd == CmdStop) state->paused = true;
    return cmd;
}

static BraviaAction bravia_handle_input(BraviaState* state, const InputEvent* event) {
    BraviaAction action = {CmdNone, false, false};

    // Hold Back always exits, even from the sleep timer picker.
    if(event->key == InputKeyBack && event->type == InputTypeLong) {
        action.exit = true;
        return action;
    }

    if(event->type == InputTypePress) {
        state->pressed = event->key == InputKeyBack ? PressedBack : (int8_t)bravia_pad_key(event->key);
        return action;
    }
    if(event->type == InputTypeRelease) {
        state->pressed = PressedNone;
        return action;
    }

    if(state->picker >= 0) {
        if(event->type != InputTypeShort) return action;
        switch(event->key) {
        case InputKeyLeft:
            state->picker = (state->picker + BRAVIA_SLEEP_OPTIONS - 1) % BRAVIA_SLEEP_OPTIONS;
            break;
        case InputKeyRight:
            state->picker = (state->picker + 1) % BRAVIA_SLEEP_OPTIONS;
            break;
        case InputKeyBack:
            state->picker = -1;
            break;
        case InputKeyOk: {
            const uint8_t minutes = bravia_sleep_minutes[state->picker];
            state->picker = -1;
            state->sleep_on = minutes > 0;
            state->sleep_end = furi_get_tick() + furi_ms_to_ticks(minutes * 60 * 1000UL);
            if(minutes) {
                snprintf(state->flash, sizeof(state->flash), "SLEEP %u MIN", minutes);
            } else {
                strlcpy(state->flash, "SLEEP TIMER OFF", sizeof(state->flash));
            }
            state->flash_until = furi_get_tick() + furi_ms_to_ticks(BRAVIA_FLASH_MS);
            action.sleep_changed = true;
            break;
        }
        default:
            break;
        }
        return action;
    }

    const BraviaPage* page = &bravia_pages[state->page];

    switch(event->key) {
    case InputKeyBack:
        if(event->type == InputTypeShort) action.cmd = CmdBack;
        break;

    case InputKeyLeft:
    case InputKeyRight:
        if(event->type == InputTypeShort) {
            action.cmd = page->keys[bravia_pad_key(event->key)].cmd;
        } else if(event->type == InputTypeLong) {
            const int step = event->key == InputKeyLeft ? -1 : 1;
            state->page = (state->page + BRAVIA_PAGE_COUNT + step) % BRAVIA_PAGE_COUNT;
            state->pressed = PressedNone;
        }
        break;

    case InputKeyUp:
    case InputKeyDown:
        // Short, Long and every Repeat send, so holding keeps stepping volume or scrolling.
        if(event->type == InputTypeShort || event->type == InputTypeLong ||
           event->type == InputTypeRepeat) {
            action.cmd = page->keys[bravia_pad_key(event->key)].cmd;
        }
        break;

    case InputKeyOk:
        if(event->type == InputTypeShort) {
            action.cmd = page->keys[PadOk].cmd;
        } else if(event->type == InputTypeLong) {
            if(page->hold.cmd == CmdNone) {
                state->picker = state->sleep_on ? 0 : BRAVIA_SLEEP_DEFAULT;
                state->pressed = PressedNone;
            } else {
                state->pressed = PressedHold;
                action.cmd = page->hold.cmd;
            }
        }
        break;

    default:
        break;
    }
    action.cmd = bravia_resolve_cmd(state, action.cmd);
    return action;
}

static void bravia_input_callback(InputEvent* event, void* context) {
    BraviaApp* app = context;
    // Sending blocks for ~170 ms, longer than the 150 ms repeat period. Keep at most one Repeat
    // queued so releasing a held key doesn't leave a backlog of sends.
    if(event->type == InputTypeRepeat) {
        if(app->repeat_queued) return;
        app->repeat_queued = true;
    }
    if(furi_message_queue_put(app->queue, event, 0) != FuriStatusOk &&
       event->type == InputTypeRepeat) {
        app->repeat_queued = false;
    }
}

// =====================================
// Backlight, persistence
// =====================================

// Keep the screen lit while the app is open, except while a sleep timer runs: then the normal
// backlight timeout applies so the screen goes dark on the nightstand.
static void bravia_update_backlight(BraviaApp* app, bool sleep_on) {
    const bool want_forced = !sleep_on;
    if(want_forced == app->backlight_forced) return;
    notification_message(
        app->notifications,
        want_forced ? &sequence_display_backlight_enforce_on :
                      &sequence_display_backlight_enforce_auto);
    app->backlight_forced = want_forced;
}

static uint8_t bravia_load_page(void) {
    uint8_t page = 0;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, BRAVIA_PAGE_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(storage_file_read(file, &page, 1) != 1 || page >= BRAVIA_PAGE_COUNT) page = 0;
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return page;
}

static void bravia_save_page(uint8_t page) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, BRAVIA_PAGE_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, &page, 1);
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

// =====================================
// Main application
// =====================================

int32_t bravia_ui_app(void* p) {
    UNUSED(p);

    BraviaApp* app = malloc(sizeof(BraviaApp));
    app->state = (BraviaState){
        .page = bravia_load_page(),
        .pressed = PressedNone,
        .picker = -1,
        .flash = "",
        .sleep_on = false,
    };
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->repeat_queued = false;
    app->backlight_forced = false;

    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    bravia_update_backlight(app, false);

    app->gui = furi_record_open(RECORD_GUI);
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, bravia_draw, app);
    view_port_input_callback_set(app->view_port, bravia_input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    uint32_t last_second_shown = UINT32_MAX;
    bool running = true;

    while(running) {
        InputEvent event;
        bool redraw = false;

        if(furi_message_queue_get(app->queue, &event, furi_ms_to_ticks(BRAVIA_TICK_MS)) ==
           FuriStatusOk) {
            if(event.type == InputTypeRepeat) app->repeat_queued = false;

            furi_mutex_acquire(app->mutex, FuriWaitForever);
            BraviaAction action = bravia_handle_input(&app->state, &event);
            const bool sleep_on = app->state.sleep_on;
            furi_mutex_release(app->mutex);

            if(action.exit) break;
            if(action.sleep_changed) bravia_update_backlight(app, sleep_on);

            if(action.cmd != CmdNone) {
                // Show the pressed key before the ~170 ms send.
                view_port_update(app->view_port);
                if(bravia_send(action.cmd)) {
                    notification_message(app->notifications, &sequence_blink_magenta_10);
                    furi_mutex_acquire(app->mutex, FuriWaitForever);
                    snprintf(
                        app->state.flash,
                        sizeof(app->state.flash),
                        "SENT  %s",
                        bravia_codes[action.cmd].name);
                    app->state.flash_until = furi_get_tick() + furi_ms_to_ticks(BRAVIA_FLASH_MS);
                    furi_mutex_release(app->mutex);
                }
            }
            redraw = true;
        }

        // Timers: footer message expiry, sleep countdown.
        BraviaCmd timer_cmd = CmdNone;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        const uint32_t now = furi_get_tick();
        if(app->state.flash[0] && (int32_t)(now - app->state.flash_until) >= 0) {
            app->state.flash[0] = '\0';
            redraw = true;
        }
        if(app->state.sleep_on) {
            const int32_t left = (int32_t)(app->state.sleep_end - now);
            if(left <= 0) {
                app->state.sleep_on = false;
                timer_cmd = CmdPower;
            } else if((uint32_t)left / 1000 != last_second_shown) {
                last_second_shown = (uint32_t)left / 1000;
                redraw = true;
            }
        }
        furi_mutex_release(app->mutex);

        if(timer_cmd != CmdNone) {
            bravia_update_backlight(app, false);
            notification_message(app->notifications, &sequence_display_backlight_on);
            if(bravia_send(timer_cmd)) {
                notification_message(app->notifications, &sequence_blink_magenta_10);
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                strlcpy(app->state.flash, "SLEEP TIMER  SENT POWER", sizeof(app->state.flash));
                app->state.flash_until = furi_get_tick() + furi_ms_to_ticks(3000);
                furi_mutex_release(app->mutex);
            }
            redraw = true;
        }

        if(redraw) view_port_update(app->view_port);
    }

    // =================================
    // Cleanup
    // =================================

    bravia_save_page(app->state.page);
    if(app->backlight_forced) {
        notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
    }

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);

    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
