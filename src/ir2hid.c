#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <infrared_worker.h>
#include <infrared.h>
#include <storage/storage.h>
#include <lib/toolbox/hex.h>
#include <string.h>

// --- Data Structures ---

typedef enum {
    EventTypeTick,
    EventTypeKey,
    EventTypeIRSignal,
} EventType;

typedef struct {
    EventType type;
    union {
        InputEvent input;
        InfraredMessage ir_message;
    };
} AppEvent;

typedef struct {
    InfraredMessage ir;
    uint8_t hid_code;
} IR2HIDLutEntry;

typedef struct {
    FuriMessageQueue* event_queue;
    FuriMutex* mutex;
    Gui* gui;
    ViewPort* view_port;
    InfraredWorker* ir_worker;
    
    // VISUAL STATE: store text here so render_callback does ZERO logic
    char text_proto[32];
    char text_addr[32];
    char text_cmd[32];
    bool has_signal;
    
    // LUT
    IR2HIDLutEntry* lut;
    size_t lut_count;

    // USB HID
    FuriHalUsbInterface* usb_prev_if;
    bool usb_hid_active;

    // Simple IR debounce
    InfraredProtocol last_proto;
    uint32_t last_addr;
    uint32_t last_cmd;
    uint32_t last_tick;
} IR2HIDApp;

// --- Helpers ---

// Strip 0x/0X prefix from a hex string
static const char* ir2hid_strip_hex_prefix(const char* s) {
    if(s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        return s + 2;
    }
    return s;
}

// Parse variable-length hex string into uint32 using hex_char_to_hex_nibble function
static bool ir2hid_parse_hex_u32(const char* s, uint32_t* out) {
    uint32_t value = 0;
    uint8_t nibble = 0;
    bool any = false;

    while(*s) {
        if(!hex_char_to_hex_nibble(*s, &nibble)) {
            return false;
        }
        value = (value << 4) | nibble;
        any = true;
        s++;
    }

    if(!any) return false;
    *out = value;
    return true;
}

// --- LUT Loading ---

static bool ir2hid_parse_lut_line(const char* line, IR2HIDLutEntry* entry) {
    // Expected CSV:
    // ir_protocol,ir_address,ir_command,hid_command,ir_key_comment,hid_key_comment
    const size_t MaxColumns = 4; // ignore the rest as col 5-6 are comments
    const char* cols[4] = {0};
    size_t col_index = 0;

    const char* p = line;
    cols[col_index] = p;
    while(*p && col_index < MaxColumns) {
        if(*p == ',') {
            // terminate current column
            ((char*)p)[0] = '\0';
            col_index++;
            if(col_index < MaxColumns) {
                cols[col_index] = p + 1;
            }
        }
        p++;
    }

    if(col_index < MaxColumns - 1) return false;

    const char* proto_str = cols[0];
    const char* addr_str = cols[1];
    const char* cmd_str = cols[2];
    const char* hid_str = cols[3];

    // Protocol
    InfraredProtocol proto = infrared_get_protocol_by_name(proto_str);
    if(!infrared_is_protocol_valid(proto)) return false;

    // Strip optional 0x/0X prefixes
    addr_str = ir2hid_strip_hex_prefix(addr_str);
    cmd_str = ir2hid_strip_hex_prefix(cmd_str);
    hid_str = ir2hid_strip_hex_prefix(hid_str);

    uint32_t addr_val = 0;
    uint32_t cmd_val = 0;
    uint32_t hid_val = 0;

    if(!ir2hid_parse_hex_u32(addr_str, &addr_val)) return false;
    if(!ir2hid_parse_hex_u32(cmd_str, &cmd_val)) return false;
    if(!ir2hid_parse_hex_u32(hid_str, &hid_val)) return false;
    if(hid_val > 0xFF) return false;

    memset(&entry->ir, 0, sizeof(entry->ir));
    entry->ir.protocol = proto;
    entry->ir.address = addr_val;
    entry->ir.command = cmd_val;
    entry->ir.repeat = false;
    entry->hid_code = (uint8_t)hid_val;

    return true;
}

static void ir2hid_load_lut(IR2HIDApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    // Path for `lut.csv` on the SD card: /ext/apps_data/ir2hid/lut.csv
    const char* lut_path = EXT_PATH("apps_data/ir2hid/lut.csv");

    if(!storage_file_open(file, lut_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        // Could not open/find LUT, display error
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        strlcpy(app->text_proto, "lut.csv not found", sizeof(app->text_proto));
        app->text_addr[0] = '\0';
        app->text_cmd[0] = '\0';
        app->has_signal = true;
        furi_mutex_release(app->mutex);
        return;
    }

    // Get file size and allocate buffer
    uint64_t file_size = storage_file_size(file);
    if(file_size == 0 || file_size > 8192) { // Sanity check: max 8KB
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return;
    }

    char* buf = malloc((size_t)file_size + 1);
    if(!buf) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return;
    }

    size_t read = storage_file_read(file, buf, (size_t)file_size);
    buf[read] = '\0';

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    // First pass: split into lines (in-place) and count data lines (skip header)
    size_t line_no = 0;
    size_t data_lines = 0;
    char* line_start = buf;

    for(size_t i = 0; i <= read; i++) {
        char c = buf[i];
        if(c == '\r' || c == '\n' || c == '\0') {
            buf[i] = '\0';
            if(line_start[0] != '\0') {
                if(line_no > 0) {
                    data_lines++;
                }
                line_no++;
            }
            line_start = &buf[i + 1];
        }
    }

    if(data_lines == 0) {
        free(buf);
        return;
    }

    IR2HIDLutEntry* lut = malloc(sizeof(IR2HIDLutEntry) * data_lines);
    if(!lut) {
        free(buf);
        return;
    }

    // Second pass: parse each data line into LUT
    memset(lut, 0, sizeof(IR2HIDLutEntry) * data_lines);
    size_t lut_index = 0;
    line_no = 0;
    line_start = buf;

    for(size_t i = 0; i <= read && lut_index < data_lines; i++) {
        if(buf[i] == '\0') {
            if(line_start[0] != '\0') {
                if(line_no > 0) {
                    if(ir2hid_parse_lut_line(line_start, &lut[lut_index])) {
                        lut_index++;
                    }
                }
                line_no++;
            }
            line_start = &buf[i + 1];
        }
    }

    // Store in app state
    app->lut = lut;
    app->lut_count = lut_index;

    // Free the file buffer (we've parsed everything we need)
    free(buf);
}

// Simple linear lookup when comparing incoming IR signal 
static bool ir2hid_lookup_hid_code(IR2HIDApp* app, const InfraredMessage* ir, uint8_t* hid_code) {
    if(!app->lut || app->lut_count == 0) return false;

    for(size_t i = 0; i < app->lut_count; i++) {
        const IR2HIDLutEntry* e = &app->lut[i];
        if(e->ir.protocol == ir->protocol && e->ir.address == ir->address &&
           e->ir.command == ir->command) {
            if(hid_code) *hid_code = e->hid_code;
            return true;
        }
    }
    return false;
}

// --- IR Worker Callback ---

// Runs in background thread
static void ir_worker_callback(void* context, InfraredWorkerSignal* signal) {
    IR2HIDApp* app = (IR2HIDApp*)context;

    // 1. Decodes signal
    const InfraredMessage* msg = infrared_worker_get_decoded_signal(signal);
    
    // 2. Copies signal to Queue
    // Don't make GUI changes to avoid race conditions
    if(msg) {
        AppEvent event;
        event.type = EventTypeIRSignal;
        event.ir_message = *msg; 
        furi_message_queue_put(app->event_queue, &event, 0);
    }
}

// --- GUI Rendering ---

static void render_callback(Canvas* canvas, void* ctx) {
    IR2HIDApp* app = (IR2HIDApp*)ctx;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    
    // Check USB HID connection status
    bool hid_connected = app->usb_hid_active && furi_hal_hid_is_connected();
    if(hid_connected) {
        canvas_draw_str(canvas, 2, 10, "IR > HID [Connected]");
    } else {
        canvas_draw_str(canvas, 2, 10, "IR > HID");
    }
    
    canvas_draw_line(canvas, 0, 12, 128, 12);
    canvas_set_font(canvas, FontSecondary);

    furi_mutex_acquire(app->mutex, FuriWaitForever);

    if(!app->has_signal) {
        canvas_draw_str(canvas, 10, 35, "Waiting for signal...");
    } else {
        canvas_draw_str(canvas, 2, 25, app->text_proto);
        canvas_draw_str(canvas, 2, 37, app->text_addr);
        canvas_draw_str(canvas, 2, 49, app->text_cmd);
    }

    furi_mutex_release(app->mutex);
}

// --- Input Handling ---

static void input_callback(InputEvent* input_event, void* ctx) {
    IR2HIDApp* app = (IR2HIDApp*)ctx;
    AppEvent event = {.type = EventTypeKey, .input = *input_event};
    furi_message_queue_put(app->event_queue, &event, 0);
}

// --- Main Entry Point ---

int32_t ir2hid_app(void* p) {
    UNUSED(p);
    
    // 1. Initialization
    IR2HIDApp* app = malloc(sizeof(IR2HIDApp));
    app->event_queue = furi_message_queue_alloc(8, sizeof(AppEvent));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->has_signal = false;
    app->lut = NULL;
    app->lut_count = 0;
    app->usb_prev_if = NULL;
    app->usb_hid_active = false;
    app->last_proto = InfraredProtocolUnknown;
    app->last_addr = 0;
    app->last_cmd = 0;
    app->last_tick = 0;

    // 2. ViewPort Setup
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, render_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);
    
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    // 3. Configure USB as HID (remember previous mode)
    app->usb_prev_if = furi_hal_usb_get_config();
    furi_hal_usb_unlock();
    if(furi_hal_usb_set_config(&usb_hid, NULL)) {
        app->usb_hid_active = true;
    }

    // 4. Load LUT from CSV
    ir2hid_load_lut(app);

    // 5. IR Worker Setup
    app->ir_worker = infrared_worker_alloc();
    infrared_worker_rx_set_received_signal_callback(app->ir_worker, ir_worker_callback, app);
    infrared_worker_rx_start(app->ir_worker);
    infrared_worker_rx_enable_blink_on_receiving(app->ir_worker, true);

    // 6. Main Loop
    AppEvent event;
    bool running = true;
    while(running) {
        FuriStatus status = furi_message_queue_get(app->event_queue, &event, FuriWaitForever);
        
        if(status == FuriStatusOk) {
            if(event.type == EventTypeKey) {
                if(event.input.key == InputKeyBack && event.input.type == InputTypeShort) {
                    running = false;
                }
            } 
            else if (event.type == EventTypeIRSignal) {
                // --- HEAVY LIFTING DONE HERE (SAFE) ---

                // Ignore protocol-level repeat frames entirely, only first message
                if(event.ir_message.repeat) {
                    continue;
                }

                // Debounce: ignore immediate repeats of same code
                const uint32_t now = furi_get_tick();
                const uint32_t debounce_ticks = furi_ms_to_ticks(5); // cooldown
                if(event.ir_message.protocol == app->last_proto &&
                   event.ir_message.address == app->last_addr &&
                   event.ir_message.command == app->last_cmd &&
                   (now - app->last_tick) < debounce_ticks) {
                    continue;
                }
                app->last_proto = event.ir_message.protocol;
                app->last_addr = event.ir_message.address;
                app->last_cmd = event.ir_message.command;
                app->last_tick = now;

                // 1. Validate Protocol
                const char* name = NULL;
                if(infrared_is_protocol_valid(event.ir_message.protocol)) {
                    name = infrared_get_protocol_name(event.ir_message.protocol);
                }
                if(!name) name = "Unknown";

                // 2. Format Strings into temporary buffers
                char temp_proto[32];
                char temp_addr[32];
                char temp_cmd[32];

                snprintf(temp_proto, sizeof(temp_proto), "Proto: %s", name);
                snprintf(temp_addr, sizeof(temp_addr),  "Addr: 0x%04lX", event.ir_message.address);
                uint8_t hid_code = 0;
                if(ir2hid_lookup_hid_code(app, &event.ir_message, &hid_code)) {
                    // Send HID consumer key (media control)
                    if(app->usb_hid_active && furi_hal_hid_is_connected()) {
                        furi_hal_hid_kb_press(hid_code);
                        furi_hal_hid_kb_release(hid_code);
                    }

                    snprintf(
                        temp_cmd,
                        sizeof(temp_cmd),
                        "Cmd:0x%04lX HID:0x%02X",
                        event.ir_message.command,
                        hid_code);
                } else {
                    snprintf(
                        temp_cmd,
                        sizeof(temp_cmd),
                        "Cmd:0x%04lX (no map)",
                        event.ir_message.command);
                }

                // 3. Update Display State Safely
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                
                // Copy strings to app state
                strlcpy(app->text_proto, temp_proto, sizeof(app->text_proto));
                strlcpy(app->text_addr, temp_addr, sizeof(app->text_addr));
                strlcpy(app->text_cmd, temp_cmd, sizeof(app->text_cmd));
                app->has_signal = true;
                
                furi_mutex_release(app->mutex);

                // 4. Trigger Redraw
                view_port_update(app->view_port);
            }
        }
    }

    // 7. Cleanup
    infrared_worker_rx_stop(app->ir_worker);
    infrared_worker_free(app->ir_worker);

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    if(app->lut) {
        free(app->lut);
    }

    if(app->usb_hid_active) {
        furi_hal_usb_set_config(app->usb_prev_if, NULL);
    }

    furi_mutex_free(app->mutex);
    furi_message_queue_free(app->event_queue);
    free(app);

    return 0;
}