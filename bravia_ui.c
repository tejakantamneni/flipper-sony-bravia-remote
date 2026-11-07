
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/canvas.h>
#include <input/input.h>
#include <infrared.h>
#include <furi_hal_infrared.h>

// =====================================
// Application state
// =====================================

typedef enum {
    BraviaScreenNavigation,
    BraviaScreenPlayback,
    BraviaScreenTV,
    BraviaScreenCount
} BraviaScreen;

typedef enum {
    BraviaNavUp,
    BraviaNavLeft,
    BraviaNavOk,
    BraviaNavRight,
    BraviaNavDown,
    BraviaNavBack,
    BraviaNavNone
} BraviaNavButton;

enum {
    BraviaPlaybackButtonCount = 5,
    BraviaTVButtonCount = 5,
};

typedef struct {
    BraviaScreen screen;
    uint8_t selected;
    bool ir_sent;
    FuriMutex* mutex;
} BraviaState;

typedef struct {
    InfraredProtocol protocol;
    uint32_t address;
    uint32_t command;
} BraviaIrCommand;

// =====================================
// Sony infrared command mapping
// =====================================

static bool bravia_get_command(
    BraviaScreen screen,
    uint8_t selected,
    BraviaIrCommand* command) {

    switch(screen) {

    case BraviaScreenNavigation:
        switch(selected) {
        case BraviaNavUp:
            *command = (BraviaIrCommand){
                InfraredProtocolSIRC, 0x01, 0x74};
            return true;

        case BraviaNavLeft:
            *command = (BraviaIrCommand){
                InfraredProtocolSIRC, 0x01, 0x34};
            return true;

        case BraviaNavOk:
            *command = (BraviaIrCommand){
                InfraredProtocolSIRC, 0x01, 0x65};
            return true;

        case BraviaNavRight:
            *command = (BraviaIrCommand){
                InfraredProtocolSIRC, 0x01, 0x33};
            return true;

        case BraviaNavDown:
            *command = (BraviaIrCommand){
                InfraredProtocolSIRC, 0x01, 0x75};
            return true;
        case BraviaNavBack:
            *command = (BraviaIrCommand){
                InfraredProtocolSIRC15, 0x97, 0x23};
            return true;
        }
        break;

    case BraviaScreenPlayback:
        switch(selected) {
        case 0: // REWIND
            *command = (BraviaIrCommand){
                InfraredProtocolSIRC15, 0x97, 0x1B};
            return true;

        case 1: // PLAY
        case 3: // PAUSE (same toggle command)
            *command = (BraviaIrCommand){
                InfraredProtocolSIRC15, 0x97, 0x1A};
            return true;

        case 2: // FAST FORWARD
            *command = (BraviaIrCommand){
                InfraredProtocolSIRC15, 0x97, 0x1C};
            return true;

        case 4: // STOP
            *command = (BraviaIrCommand){
                InfraredProtocolSIRC15, 0x97, 0x18};
            return true;
        }
        break;

    case BraviaScreenTV:
        switch(selected) {
        case 0: // POWER
            *command = (BraviaIrCommand){
                InfraredProtocolSIRC, 0x01, 0x15};
            return true;

        case 1: // HOME
            *command = (BraviaIrCommand){
                InfraredProtocolSIRC, 0x01, 0x60};
            return true;

        case 2: // INPUT
            *command = (BraviaIrCommand){
                InfraredProtocolSIRC, 0x01, 0x25};
            return true;

        // Standard TV volume keys from Sony_SFRTV5.ir.
        case 3: // VOL+
            *command = (BraviaIrCommand){
                InfraredProtocolSIRC, 0x01, 0x12};
            return true;

        case 4: // VOL-
            *command = (BraviaIrCommand){
                InfraredProtocolSIRC, 0x01, 0x13};
            return true;
        }
        break;

    default:
        break;
    }

    return false;
}

// =====================================
// Infrared HAL callback
// =====================================

static FuriHalInfraredTxGetDataState bravia_ir_callback(
    void* context,
    uint32_t* duration,
    bool* level) {

    InfraredEncoderHandler* encoder = context;

    InfraredStatus status =
        infrared_encode(encoder, duration, level);

    if(status == InfraredStatusDone) {
        return FuriHalInfraredTxGetDataStateLastDone;
    }

    return FuriHalInfraredTxGetDataStateOk;
}

// =====================================
// Infrared transmitter
// =====================================

static bool bravia_send_command(const BraviaIrCommand* command) {
    InfraredMessage message = {
        .protocol = command->protocol,
        .address = command->address,
        .command = command->command,
        .repeat = false,
    };

    InfraredEncoderHandler* encoder =
        infrared_alloc_encoder();

    if(!encoder) {
        return false;
    }

    uint32_t frequency =
        infrared_get_protocol_frequency(
            message.protocol);

    float duty_cycle =
        infrared_get_protocol_duty_cycle(
            message.protocol);

    // Three Sony SIRC frames.
    // This approach was confirmed working
    // with HOME on the user's television.
    for(uint8_t i = 0; i < 3; i++) {

        infrared_reset_encoder(
            encoder, &message);

        furi_hal_infrared_async_tx_set_data_isr_callback(
            bravia_ir_callback,
            encoder);

        furi_hal_infrared_async_tx_start(
            frequency,
            duty_cycle);

        furi_hal_infrared_async_tx_wait_termination();

        if(i < 2) {
            furi_delay_ms(45);
        }
    }

    infrared_free_encoder(encoder);

    return true;
}

// =====================================
// Input handling
// =====================================

static bool bravia_handle_input(
    BraviaState* state,
    const InputEvent* event,
    BraviaIrCommand* command) {

    // The firmware emits either Short or Long for a physical press.
    if(event->type == InputTypeLong) {
        if(event->key == InputKeyLeft || event->key == InputKeyRight) {
            const int step = event->key == InputKeyLeft ? -1 : 1;
            state->screen = (state->screen + BraviaScreenCount + step) % BraviaScreenCount;
            state->selected = state->screen == BraviaScreenNavigation ? BraviaNavNone : 0;
            state->ir_sent = false;
        }
        return false;
    }

    if(event->type != InputTypeShort) {
        return false;
    }

    state->ir_sent = false;
    if(event->key == InputKeyBack) {
        if(state->screen == BraviaScreenNavigation) {
            state->selected = BraviaNavBack;
        }
        return bravia_get_command(BraviaScreenNavigation, BraviaNavBack, command);
    }

    if(state->screen == BraviaScreenNavigation) {
        switch(event->key) {
        case InputKeyUp:
            state->selected = BraviaNavUp;
            break;
        case InputKeyDown:
            state->selected = BraviaNavDown;
            break;
        case InputKeyLeft:
            state->selected = BraviaNavLeft;
            break;
        case InputKeyRight:
            state->selected = BraviaNavRight;
            break;
        case InputKeyOk:
            state->selected = BraviaNavOk;
            break;
        default:
            return false;
        }
        return bravia_get_command(state->screen, state->selected, command);
    }

    const int count = state->screen == BraviaScreenPlayback ?
                          BraviaPlaybackButtonCount : BraviaTVButtonCount;
    switch(event->key) {
    case InputKeyUp:
    case InputKeyLeft:
        state->selected = (state->selected + count - 1) % count;
        break;
    case InputKeyDown:
    case InputKeyRight:
        state->selected = (state->selected + 1) % count;
        break;
    case InputKeyOk:
        return bravia_get_command(state->screen, state->selected, command);
    default:
        break;
    }
    return false;
}

// =====================================
// Drawing helpers
// =====================================

static void draw_button_surface(
    Canvas* canvas, int x, int y, int w, int h, bool selected) {
    canvas_set_color(canvas, ColorBlack);
    if(selected) {
        canvas_draw_rbox(canvas, x, y, w, h, 3);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_draw_rframe(canvas, x, y, w, h, 3);
    }
}

static void draw_button(
    Canvas* canvas, int x, int y, int w, int h, const char* label, bool selected) {
    draw_button_surface(canvas, x, y, w, h, selected);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, x + w / 2, y + h / 2, AlignCenter, AlignCenter, label);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_chrome(Canvas* canvas, const char* title, BraviaScreen screen, bool ir_sent) {
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 4, 11, title);
    draw_button(canvas, 78, 1, 47, 12, ir_sent ? "IR SENT" : "IR READY", ir_sent);
    canvas_draw_line(canvas, 0, 14, 127, 14);

    canvas_draw_line(canvas, 0, 55, 127, 55);
    canvas_draw_str(canvas, 3, 63, "Hold L/R:page Back:exit");
    for(BraviaScreen page = BraviaScreenNavigation; page < BraviaScreenCount; page++) {
        const int x = 110 + 6 * page;
        if(page == screen) {
            canvas_draw_disc(canvas, x, 60, 2);
        } else {
            canvas_draw_circle(canvas, x, 60, 2);
        }
    }
}

enum {
    BraviaPadX = 63,
    BraviaPadY = 35,
    BraviaPadRadius = 18,
    BraviaPadOkRadius = 7,
};

static void draw_pad_direction(Canvas* canvas, int dx, int dy, bool selected) {
    canvas_set_color(canvas, ColorBlack);
    if(selected) {
        for(int forward = 1; forward < BraviaPadRadius; forward++) {
            for(int across = 1 - forward; across < forward; across++) {
                const int radius_squared = forward * forward + across * across;
                if(radius_squared > BraviaPadOkRadius * BraviaPadOkRadius &&
                   radius_squared < BraviaPadRadius * BraviaPadRadius) {
                    canvas_draw_dot(
                        canvas,
                        BraviaPadX + forward * dx - across * dy,
                        BraviaPadY + forward * dy + across * dx);
                }
            }
        }
        canvas_set_color(canvas, ColorWhite);
    }

    const int x = BraviaPadX + 12 * dx;
    const int y = BraviaPadY + 12 * dy;
    canvas_draw_line(canvas, x - dx - 3 * dy, y - dy + 3 * dx, x + 2 * dx, y + 2 * dy);
    canvas_draw_line(canvas, x - dx + 3 * dy, y - dy - 3 * dx, x + 2 * dx, y + 2 * dy);
}

static void draw_navigation(Canvas* canvas, uint8_t selected) {
    draw_pad_direction(canvas, 0, -1, selected == BraviaNavUp);
    draw_pad_direction(canvas, -1, 0, selected == BraviaNavLeft);
    draw_pad_direction(canvas, 1, 0, selected == BraviaNavRight);
    draw_pad_direction(canvas, 0, 1, selected == BraviaNavDown);

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_circle(canvas, BraviaPadX, BraviaPadY, BraviaPadRadius);
    for(int dx = -1; dx <= 1; dx += 2) {
        for(int dy = -1; dy <= 1; dy += 2) {
            canvas_draw_line(
                canvas,
                BraviaPadX + 5 * dx,
                BraviaPadY + 5 * dy,
                BraviaPadX + 12 * dx,
                BraviaPadY + 12 * dy);
        }
    }

    if(selected == BraviaNavOk) {
        canvas_draw_disc(canvas, BraviaPadX, BraviaPadY, BraviaPadOkRadius);
        canvas_set_color(canvas, ColorWhite);
    } else {
        canvas_draw_circle(canvas, BraviaPadX, BraviaPadY, BraviaPadOkRadius);
    }
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, BraviaPadX, BraviaPadY, AlignCenter, AlignCenter, "OK");
    draw_button(canvas, 88, 29, 35, 13, "BACK", selected == BraviaNavBack);
}

static void draw_playback(Canvas* canvas, uint8_t selected) {
    draw_button(canvas, 4, 17, 38, 16, "REW", selected == 0);
    draw_button(canvas, 45, 17, 38, 16, "PLAY", selected == 1);
    draw_button(canvas, 86, 17, 38, 16, "FWD", selected == 2);
    draw_button(canvas, 23, 36, 40, 16, "PAUSE", selected == 3);
    draw_button(canvas, 65, 36, 40, 16, "STOP", selected == 4);
}

static void draw_tv(Canvas* canvas, uint8_t selected) {
    draw_button(canvas, 4, 17, 38, 16, "POWER", selected == 0);
    draw_button(canvas, 45, 17, 38, 16, "HOME", selected == 1);
    draw_button(canvas, 86, 17, 38, 16, "INPUT", selected == 2);
    draw_button(canvas, 23, 36, 40, 16, "VOL+", selected == 3);
    draw_button(canvas, 65, 36, 40, 16, "VOL-", selected == 4);
}

// =====================================
// Drawing callback
// =====================================

static void bravia_draw(
    Canvas* canvas,
    void* context) {

    BraviaState* state = context;

    furi_mutex_acquire(
        state->mutex, FuriWaitForever);

    BraviaScreen screen = state->screen;
    uint8_t selected = state->selected;
    bool ir_sent = state->ir_sent;

    furi_mutex_release(state->mutex);

    canvas_clear(canvas);

    switch(screen) {

    case BraviaScreenNavigation:
        draw_chrome(canvas, "Navigation", screen, ir_sent);
        draw_navigation(canvas, selected);
        break;

    case BraviaScreenPlayback:
        draw_chrome(canvas, "Playback", screen, ir_sent);
        draw_playback(canvas, selected);
        break;

    case BraviaScreenTV:
        draw_chrome(canvas, "TV Control", screen, ir_sent);
        draw_tv(canvas, selected);
        break;

    default:
        break;
    }
}

// =====================================
// Input callback
// =====================================

static void bravia_input(
    InputEvent* event,
    void* context) {

    FuriMessageQueue* queue = context;

    if(event->type == InputTypeShort ||
       event->type == InputTypeLong) {

        furi_message_queue_put(
            queue, event, 0);
    }
}

// =====================================
// Main application
// =====================================

int32_t bravia_ui_app(void* p) {
    UNUSED(p);

    BraviaState state = {
        .screen = BraviaScreenNavigation,
        .selected = BraviaNavNone,
        .ir_sent = false,
        .mutex = furi_mutex_alloc(
            FuriMutexTypeNormal),
    };

    FuriMessageQueue* queue =
        furi_message_queue_alloc(
            8, sizeof(InputEvent));

    Gui* gui = furi_record_open(RECORD_GUI);
    ViewPort* view_port = view_port_alloc();

    view_port_draw_callback_set(
        view_port, bravia_draw, &state);

    view_port_input_callback_set(
        view_port, bravia_input, queue);

    gui_add_view_port(
        gui, view_port, GuiLayerFullscreen);

    InputEvent event;

    while(true) {

        if(furi_message_queue_get(
               queue,
               &event,
               FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        if(event.type == InputTypeLong && event.key == InputKeyBack) {
            break;
        }

        BraviaIrCommand command;
        furi_mutex_acquire(state.mutex, FuriWaitForever);
        bool send_command = bravia_handle_input(&state, &event, &command);
        furi_mutex_release(state.mutex);

        if(send_command) {
            bool sent = bravia_send_command(&command);
            furi_mutex_acquire(state.mutex, FuriWaitForever);
            state.ir_sent = sent;
            furi_mutex_release(state.mutex);
        }

        view_port_update(view_port);
    }

    // =================================
    // Cleanup
    // =================================

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);

    furi_record_close(RECORD_GUI);

    furi_message_queue_free(queue);
    furi_mutex_free(state.mutex);

    return 0;
}
