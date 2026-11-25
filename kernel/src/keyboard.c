#include "keyboard.h"
#include "arch/x86_64/io.h"
#include <stdbool.h>
#include <stdint.h>

#define PS2_DATA              0x60
#define PS2_STATUS            0x64
#define PS2_COMMAND           0x64
#define PS2_STATUS_OUTPUT     0x01U
#define PS2_STATUS_INPUT      0x02U
#define PS2_STATUS_AUX        0x20U

#define PS2_CMD_READ_CONFIG   0x20U
#define PS2_CMD_WRITE_CONFIG  0x60U
#define PS2_CMD_DISABLE_PORT1 0xadU
#define PS2_CMD_ENABLE_PORT1  0xaeU
#define PS2_CMD_DISABLE_PORT2 0xa7U

#define PS2_CFG_IRQ1          0x01U
#define PS2_CFG_PORT1_CLOCK   0x10U
#define PS2_CFG_TRANSLATION   0x40U

#define KBD_CMD_DISABLE_SCAN  0xf5U
#define KBD_CMD_SET_SCANCODE  0xf0U
#define KBD_CMD_ENABLE_SCAN   0xf4U
#define KBD_ACK               0xfaU
#define KBD_RESEND            0xfeU

#define BUFFER_SIZE           128U
#define IO_TIMEOUT            1000000U

static volatile struct keyboard_event input_buffer[BUFFER_SIZE];
static volatile uint32_t input_head;
static volatile uint32_t input_tail;
static volatile uint64_t irq_count;
static volatile uint8_t last_scancode;
static uint8_t controller_config;
static bool left_shift;
static bool right_shift;
static bool caps_lock;
static bool extended_code;
/* ITEM-392: evidence behind the status keyboard_init() reports. */
static struct keyboard_init_diagnostic diagnostic;
/* ITEM-411: whether keyboard_init() has run at all.  Before it has, the
   diagnostic holds the initialiser's zeroes, which would otherwise read as
   KEYBOARD_INIT_OK - a controller that was never probed must not be treated as
   one that answered. */
static bool init_observed;
static uint8_t last_status_port;

static const char normal_map[128] = {
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',[0x08]='7',[0x09]='8',[0x0a]='9',[0x0b]='0',
    [0x0c]='-',[0x0d]='=',[0x0e]='\b',[0x0f]='\t',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',
    [0x1a]='[',[0x1b]=']',[0x1c]='\n',
    [0x1e]='a',[0x1f]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',
    [0x2b]='\\',[0x2c]='z',[0x2d]='x',[0x2e]='c',[0x2f]='v',[0x30]='b',[0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',
    [0x39]=' '
};

static const char shifted_map[128] = {
    [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',[0x08]='&',[0x09]='*',[0x0a]='(',[0x0b]=')',
    [0x0c]='_',[0x0d]='+',[0x0e]='\b',[0x0f]='\t',
    [0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',[0x15]='Y',[0x16]='U',[0x17]='I',[0x18]='O',[0x19]='P',
    [0x1a]='{',[0x1b]='}',[0x1c]='\n',
    [0x1e]='A',[0x1f]='S',[0x20]='D',[0x21]='F',[0x22]='G',[0x23]='H',[0x24]='J',[0x25]='K',[0x26]='L',[0x27]=':',[0x28]='"',[0x29]='~',
    [0x2b]='|',[0x2c]='Z',[0x2d]='X',[0x2e]='C',[0x2f]='V',[0x30]='B',[0x31]='N',[0x32]='M',[0x33]='<',[0x34]='>',[0x35]='?',
    [0x39]=' '
};

static bool wait_input_empty(void) {
    for (uint32_t count = 0; count < IO_TIMEOUT; ++count) {
        last_status_port = inb(PS2_STATUS);
        if ((last_status_port & PS2_STATUS_INPUT) == 0U) {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static bool wait_output_full(void) {
    for (uint32_t count = 0; count < IO_TIMEOUT; ++count) {
        last_status_port = inb(PS2_STATUS);
        if ((last_status_port & PS2_STATUS_OUTPUT) != 0U) {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static bool write_controller(uint8_t command) {
    if (!wait_input_empty()) {
        return false;
    }
    outb(PS2_COMMAND, command);
    return true;
}

static bool write_data(uint8_t value) {
    if (!wait_input_empty()) {
        return false;
    }
    outb(PS2_DATA, value);
    return true;
}

static bool read_data(uint8_t *value) {
    if (value == 0 || !wait_output_full()) {
        return false;
    }
    *value = inb(PS2_DATA);
    return true;
}

static void flush_output(void) {
    for (uint32_t count = 0; count < 32U; ++count) {
        if ((inb(PS2_STATUS) & PS2_STATUS_OUTPUT) == 0U) {
            break;
        }
        (void)inb(PS2_DATA);
    }
}

static bool keyboard_command(uint8_t command) {
    for (unsigned int attempt = 0; attempt < 2U; ++attempt) {
        uint8_t response;
        if (!write_data(command) || !read_data(&response)) {
            return false;
        }
        if (response == KBD_ACK) {
            return true;
        }
        if (response != KBD_RESEND) {
            return false;
        }
    }
    return false;
}

static void enqueue(enum keyboard_event_type type, char character) {
    const uint32_t next = (input_head + 1U) % BUFFER_SIZE;
    if (next == input_tail) {
        return;
    }
    input_buffer[input_head].type = type;
    input_buffer[input_head].character = character;
    /* ITEM-482: the PS/2 IRQ is the report itself, so there is no separate
       completion clock to measure the echo from. */
    input_buffer[input_head].reported_cycles = 0U;
    input_head = next;
}

static enum keyboard_init_status keyboard_init_failed(
    enum keyboard_init_status status) {
    init_observed = true;
    diagnostic.status = status;
    diagnostic.status_port = last_status_port;
    diagnostic.controller_config = controller_config;
    return status;
}

enum keyboard_init_status keyboard_init(void) {
    input_head = 0;
    input_tail = 0;
    irq_count = 0;
    last_scancode = 0;
    controller_config = 0;
    left_shift = false;
    right_shift = false;
    caps_lock = false;
    extended_code = false;
    last_status_port = 0;
    diagnostic.status = KEYBOARD_INIT_STATUS_UNKNOWN;
    diagnostic.status_port = 0;
    diagnostic.controller_config = 0;

    if (!write_controller(PS2_CMD_DISABLE_PORT1) ||
        !write_controller(PS2_CMD_DISABLE_PORT2)) {
        /*
         * ITEM-392: a status port that reads all ones never drains the input
         * buffer because nothing is behind it; any other value means a
         * controller that is there but not answering.
         */
        return keyboard_init_failed(
            last_status_port == 0xFFU ? KEYBOARD_INIT_CONTROLLER_ABSENT
                                      : KEYBOARD_INIT_CONTROLLER_STUCK);
    }
    flush_output();

    if (!write_controller(PS2_CMD_READ_CONFIG) ||
        !read_data(&controller_config)) {
        return keyboard_init_failed(KEYBOARD_INIT_CONTROLLER_STUCK);
    }

    controller_config |= (uint8_t)(PS2_CFG_IRQ1 | PS2_CFG_TRANSLATION);
    controller_config &= (uint8_t)~PS2_CFG_PORT1_CLOCK;

    if (!write_controller(PS2_CMD_WRITE_CONFIG) ||
        !write_data(controller_config) ||
        !write_controller(PS2_CMD_ENABLE_PORT1)) {
        return keyboard_init_failed(KEYBOARD_INIT_CONTROLLER_STUCK);
    }

    /*
     * ITEM-392: read the configuration back instead of assuming the write
     * landed. A controller that silently drops the translation bit would
     * decode set-2 scan codes with the set-1 tables, so it must not be
     * reported as a usable keyboard.
     */
    {
        uint8_t read_back = 0;

        if (!write_controller(PS2_CMD_READ_CONFIG) ||
            !read_data(&read_back)) {
            return keyboard_init_failed(KEYBOARD_INIT_CONTROLLER_STUCK);
        }
        controller_config = read_back;
        if ((read_back &
             (uint8_t)(PS2_CFG_IRQ1 | PS2_CFG_TRANSLATION |
                       PS2_CFG_PORT1_CLOCK)) !=
            (uint8_t)(PS2_CFG_IRQ1 | PS2_CFG_TRANSLATION)) {
            return keyboard_init_failed(KEYBOARD_INIT_CONFIG_REJECTED);
        }
    }

    /* Force the keyboard into scan-code set 2. The controller translates it
       to set 1, which is what normal_map and shifted_map decode. */
    if (!keyboard_command(KBD_CMD_DISABLE_SCAN) ||
        !keyboard_command(KBD_CMD_SET_SCANCODE) ||
        !keyboard_command(0x02U) ||
        !keyboard_command(KBD_CMD_ENABLE_SCAN)) {
        return keyboard_init_failed(KEYBOARD_INIT_KEYBOARD_ABSENT);
    }

    flush_output();
    init_observed = true;
    diagnostic.status = KEYBOARD_INIT_OK;
    diagnostic.status_port = last_status_port;
    diagnostic.controller_config = controller_config;
    return KEYBOARD_INIT_OK;
}

const struct keyboard_init_diagnostic *keyboard_init_diagnostic(void) {
    return &diagnostic;
}

bool keyboard_controller_present(void) {
    if (!init_observed) {
        return false;
    }
    switch (diagnostic.status) {
    case KEYBOARD_INIT_OK:
    case KEYBOARD_INIT_CONFIG_REJECTED:
    case KEYBOARD_INIT_KEYBOARD_ABSENT:
        return true;
    default:
        return false;
    }
}

const char *keyboard_init_status_string(enum keyboard_init_status status) {
    switch (status) {
    case KEYBOARD_INIT_OK:
        return "OK";
    case KEYBOARD_INIT_CONTROLLER_ABSENT:
        return "CONTROLLER_ABSENT";
    case KEYBOARD_INIT_CONTROLLER_STUCK:
        return "CONTROLLER_STUCK";
    case KEYBOARD_INIT_CONFIG_REJECTED:
        return "CONFIG_REJECTED";
    case KEYBOARD_INIT_KEYBOARD_ABSENT:
        return "KEYBOARD_ABSENT";
    case KEYBOARD_INIT_STATUS_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

bool keyboard_init_status_boots(enum keyboard_init_status status) {
    switch (status) {
    case KEYBOARD_INIT_OK:
    case KEYBOARD_INIT_CONTROLLER_ABSENT:
    case KEYBOARD_INIT_CONTROLLER_STUCK:
    case KEYBOARD_INIT_CONFIG_REJECTED:
    case KEYBOARD_INIT_KEYBOARD_ABSENT:
        return true;
    case KEYBOARD_INIT_STATUS_UNKNOWN:
    default:
        return false;
    }
}

static void process_scancode(uint8_t scancode) {
    last_scancode = scancode;

    if (scancode == 0xe0U) {
        extended_code = true;
        return;
    }
    if (extended_code) {
        extended_code = false;
        const bool released = (scancode & 0x80U) != 0U;
        if (!released) {
            switch (scancode) {
            case 0x4bU:
                enqueue(KEYBOARD_EVENT_LEFT, '\0');
                return;
            case 0x4dU:
                enqueue(KEYBOARD_EVENT_RIGHT, '\0');
                return;
            case 0x47U:
                enqueue(KEYBOARD_EVENT_HOME, '\0');
                return;
            case 0x4fU:
                enqueue(KEYBOARD_EVENT_END, '\0');
                return;
            case 0x53U:
                enqueue(KEYBOARD_EVENT_DELETE, '\0');
                return;
            case 0x48U:
                enqueue(KEYBOARD_EVENT_UP, '\0');
                return;
            case 0x50U:
                enqueue(KEYBOARD_EVENT_DOWN, '\0');
                return;
            default:
                return;
            }
        }
        return;
    }

    const bool released = (scancode & 0x80U) != 0U;
    const uint8_t code = (uint8_t)(scancode & 0x7fU);
    if (code == 0x2aU) {
        left_shift = !released;
        return;
    }
    if (code == 0x36U) {
        right_shift = !released;
        return;
    }
    if (released) {
        return;
    }
    if (code == 0x3aU) {
        caps_lock = !caps_lock;
        return;
    }
    if (code == 0x1cU) {
        enqueue(KEYBOARD_EVENT_ENTER, '\0');
        return;
    }
    if (code == 0x0eU) {
        enqueue(KEYBOARD_EVENT_BACKSPACE, '\0');
        return;
    }

    const bool shifted = left_shift || right_shift;
    char value = shifted ? shifted_map[code] : normal_map[code];
    if (value >= 'a' && value <= 'z' && caps_lock) {
        value = (char)(value - 'a' + 'A');
    } else if (value >= 'A' && value <= 'Z' && caps_lock) {
        value = (char)(value - 'A' + 'a');
    }
    if (value != '\0') {
        enqueue(KEYBOARD_EVENT_CHARACTER, value);
    }
}

void keyboard_handle_irq(void) {
    const uint8_t status = inb(PS2_STATUS);
    if ((status & PS2_STATUS_OUTPUT) == 0U) {
        return;
    }

    const uint8_t scancode = inb(PS2_DATA);
    if ((status & PS2_STATUS_AUX) != 0U) {
        return;
    }

    ++irq_count;
    process_scancode(scancode);
}

bool keyboard_pop_event(struct keyboard_event *event) {
    if (event == 0 || input_tail == input_head) {
        return false;
    }
    *event = input_buffer[input_tail];
    input_tail = (input_tail + 1U) % BUFFER_SIZE;
    return true;
}

uint64_t keyboard_irq_count(void) {
    return irq_count;
}

uint8_t keyboard_last_scancode(void) {
    return last_scancode;
}

uint8_t keyboard_controller_config(void) {
    return controller_config;
}

#ifdef HFOC_KEYBOARD_HOST_TEST
void keyboard_test_reset(void) {
    input_head = 0U;
    input_tail = 0U;
    irq_count = 0U;
    last_scancode = 0U;
    controller_config = 0U;
    left_shift = false;
    right_shift = false;
    caps_lock = false;
    extended_code = false;
    last_status_port = 0U;
    diagnostic.status = KEYBOARD_INIT_STATUS_UNKNOWN;
    diagnostic.status_port = 0U;
    diagnostic.controller_config = 0U;
    for (unsigned int i = 0U; i < BUFFER_SIZE; ++i) {
        input_buffer[i].type = KEYBOARD_EVENT_CHARACTER;
        input_buffer[i].character = '\0';
    }
}
#endif
