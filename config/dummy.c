#include <stdint.h>

__attribute__((weak)) uint8_t zmk_keymap_highest_layer_active(void) {
    return 0;
}
