/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_temp_layer

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>
#include <zmk/keymap.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* 構造体定義：名前を ZMK 標準のお作法に合わせます */
struct input_processor_temp_layer_config {
    int16_t require_prior_idle_ms;
    const uint16_t *excluded_positions;
    size_t excluded_positions_len;
};

struct input_processor_temp_layer_data {
    uint8_t toggle_layer;
    bool is_active;
    int64_t last_tapped_timestamp;
    struct k_mutex lock;
};

/* --- ユーティリティ関数 --- */

static bool position_is_excluded(const struct input_processor_temp_layer_config *config,
                                 uint32_t position) {
    for (size_t i = 0; i < config->excluded_positions_len; i++) {
        if (config->excluded_positions[i] == position) {
            return true;
        }
    }
    return false;
}

static void update_layer_state(const struct device *dev, bool activate) {
    struct input_processor_temp_layer_data *data = dev->data;
    if (data->is_active == activate)
        return;

    data->is_active = activate;
    if (activate) {
        zmk_keymap_layer_activate(data->toggle_layer, false);
    } else {
        zmk_keymap_layer_deactivate(data->toggle_layer, false);
    }
}

/* --- コンパイルエラー対策済みの判定関数 --- */

static int
input_processor_temp_layer_handle_position(struct input_processor_temp_layer_data *data,
                                           const struct input_processor_temp_layer_config *config,
                                           uint32_t position) {
    if (config->excluded_positions_len == 0) {
        return 0;
    }
    if (position_is_excluded(config, position)) {
        return 1;
    }
    return 0;
}

/* --- イベントハンドラ --- */

static int handle_layer_state_changed(const struct device *dev, const zmk_event_t *eh) {
    struct input_processor_temp_layer_data *data = dev->data;
    k_mutex_lock(&data->lock, K_FOREVER);
    if (!zmk_keymap_layer_active(data->toggle_layer)) {
        data->is_active = false;
    }
    k_mutex_unlock(&data->lock);
    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_position_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    struct input_processor_temp_layer_data *data = dev->data;
    const struct input_processor_temp_layer_config *config = dev->config;

    if (!ev->state)
        return ZMK_EV_EVENT_BUBBLE;

    k_mutex_lock(&data->lock, K_FOREVER);
    if (data->is_active && !position_is_excluded(config, ev->position)) {
        update_layer_state(dev, false);
    }
    k_mutex_unlock(&data->lock);
    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_keycode_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    struct input_processor_temp_layer_data *data = dev->data;

    if (!ev->state)
        return ZMK_EV_EVENT_BUBBLE;

    k_mutex_lock(&data->lock, K_FOREVER);
    data->last_tapped_timestamp = ev->timestamp;
    k_mutex_unlock(&data->lock);
    return ZMK_EV_EVENT_BUBBLE;
}

/* --- ディスパッチャ（複数のインスタンスに対応） --- */

static int handle_event_dispatcher(const zmk_event_t *eh) {
#define DISPATCH(n)                                                                                \
    if (as_zmk_position_state_changed(eh))                                                         \
        handle_position_state_changed(DEVICE_DT_INST_GET(n), eh);                                  \
    if (as_zmk_layer_state_changed(eh))                                                            \
        handle_layer_state_changed(DEVICE_DT_INST_GET(n), eh);                                     \
    if (as_zmk_keycode_state_changed(eh))                                                          \
        handle_keycode_state_changed(DEVICE_DT_INST_GET(n), eh);

    DT_INST_FOREACH_STATUS_OKAY(DISPATCH)
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(input_processor_temp_layer, handle_event_dispatcher);
ZMK_SUBSCRIPTION(input_processor_temp_layer, zmk_position_state_changed);
ZMK_SUBSCRIPTION(input_processor_temp_layer, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(input_processor_temp_layer, zmk_keycode_state_changed);

/* --- ドライバー実装 --- */

static int temp_layer_handle_event(const struct device *dev, struct input_event *event,
                                   uint32_t param1, uint32_t param2,
                                   struct zmk_input_processor_state *state) {
    struct input_processor_temp_layer_data *data = dev->data;
    const struct input_processor_temp_layer_config *config = dev->config;

    k_mutex_lock(&data->lock, K_FOREVER);
    data->toggle_layer = param1;

    int64_t idle_time = k_uptime_get() - data->last_tapped_timestamp;
    if (idle_time > config->require_prior_idle_ms) {
        update_layer_state(dev, true);
    }
    k_mutex_unlock(&data->lock);

    return ZMK_INPUT_PROC_CONTINUE;
}

static int temp_layer_init(const struct device *dev) {
    struct input_processor_temp_layer_data *data = dev->data;
    k_mutex_init(&data->lock);
    return 0;
}

static const struct zmk_input_processor_driver_api temp_layer_driver_api = {
    .handle_event = temp_layer_handle_event,
};

/* --- デバイスインスタンス作成マクロ --- */

#define TEMP_LAYER_INST(n)                                                                         \
    static struct input_processor_temp_layer_data data_##n = {                                     \
        .is_active = false,                                                                        \
        .last_tapped_timestamp = 0,                                                                \
    };                                                                                             \
    static const uint16_t excluded_positions_##n[] = DT_INST_PROP_OR(n, excluded_positions, {});   \
    static const struct input_processor_temp_layer_config config_##n = {                           \
        .require_prior_idle_ms = DT_INST_PROP_OR(n, require_prior_idle_ms, 0),                     \
        .excluded_positions = excluded_positions_##n,                                              \
        .excluded_positions_len = DT_INST_PROP_LEN_OR(n, excluded_positions, 0),                   \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, temp_layer_init, NULL, &data_##n, &config_##n, POST_KERNEL,           \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &temp_layer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TEMP_LAYER_INST)