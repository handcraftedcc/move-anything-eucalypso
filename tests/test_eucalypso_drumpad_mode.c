#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

extern midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host);

static int test_get_clock_status(void) {
    return MOVE_CLOCK_STATUS_RUNNING;
}

static void fail(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static int count_note_ons(uint8_t out_msgs[][3], int out_lens[], int count) {
    int i;
    int n = 0;
    for (i = 0; i < count; i++) {
        if (out_lens[i] >= 3 && (out_msgs[i][0] & 0xF0) == 0x90 && out_msgs[i][2] > 0) n++;
    }
    return n;
}

static int has_note_on(uint8_t out_msgs[][3], int out_lens[], int count, int note) {
    int i;
    for (i = 0; i < count; i++) {
        if (out_lens[i] >= 3 &&
            (out_msgs[i][0] & 0xF0) == 0x90 &&
            out_msgs[i][2] > 0 &&
            out_msgs[i][1] == (uint8_t)note) {
            return 1;
        }
    }
    return 0;
}

static void send_midi(midi_fx_api_v1_t *api, void *inst, uint8_t s, uint8_t d1, uint8_t d2) {
    uint8_t in[3];
    uint8_t out_msgs[16][3];
    int out_lens[16];
    in[0] = s;
    in[1] = d1;
    in[2] = d2;
    (void)api->process_midi(inst, in, 3, out_msgs, out_lens, 16);
}

static int drain_step(midi_fx_api_v1_t *api, void *inst, uint8_t out_msgs[][3], int out_lens[], int max_out) {
    return api->tick(inst, 0, 44100, out_msgs, out_lens, max_out);
}

static void set_common_params(midi_fx_api_v1_t *api, void *inst) {
    api->set_param(inst, "sync", "clock");

    api->set_param(inst, "lane1_enabled", "on");
    api->set_param(inst, "lane2_enabled", "on");
    api->set_param(inst, "lane3_enabled", "off");
    api->set_param(inst, "lane4_enabled", "off");

    api->set_param(inst, "lane1_steps", "1");
    api->set_param(inst, "lane1_pulses", "1");
    api->set_param(inst, "lane2_steps", "1");
    api->set_param(inst, "lane2_pulses", "1");
}

static void test_drumpad_lane_gate_and_lane_note(midi_fx_api_v1_t *api) {
    void *inst = api->create_instance(".", NULL);
    uint8_t out_msgs[64][3];
    int out_lens[64];
    int out_count;

    if (!inst) fail("create_instance failed (drumpad gate)");

    set_common_params(api, inst);
    api->set_param(inst, "register_mode", "drumpad");
    api->set_param(inst, "scale_rng", "16");
    api->set_param(inst, "lane1_note", "1");
    api->set_param(inst, "lane2_note", "4"); /* drumpad index 4 -> MIDI 39 */

    /* Hold only drumpad #2 (MIDI 37): should gate lane2 only. */
    send_midi(api, inst, 0x90, 37, 100);
    send_midi(api, inst, 0xFA, 0, 0); /* Start: arm one pending step */
    out_count = drain_step(api, inst, out_msgs, out_lens, 64);

    if (count_note_ons(out_msgs, out_lens, out_count) != 1) {
        fail("drumpad mode should emit exactly one lane when only pad2 is held");
    }
    if (!has_note_on(out_msgs, out_lens, out_count, 39)) {
        fail("lane2 should emit lane2_note-selected drumpad note (MIDI 39)");
    }

    api->destroy_instance(inst);
}

static void test_drumpad_respects_register_range(midi_fx_api_v1_t *api) {
    void *inst = api->create_instance(".", NULL);
    uint8_t out_msgs[64][3];
    int out_lens[64];
    int out_count;

    if (!inst) fail("create_instance failed (drumpad range)");

    set_common_params(api, inst);
    api->set_param(inst, "register_mode", "drumpad");
    api->set_param(inst, "scale_rng", "1");  /* only first drumpad included */
    api->set_param(inst, "lane2_note", "2"); /* out-of-range -> skip (default policy) */

    send_midi(api, inst, 0x90, 37, 100); /* gate lane2 */
    send_midi(api, inst, 0xFA, 0, 0);
    out_count = drain_step(api, inst, out_msgs, out_lens, 64);

    if (count_note_ons(out_msgs, out_lens, out_count) != 0) {
        fail("drumpad mode should respect register range and skip out-of-range lane note");
    }

    api->destroy_instance(inst);
}

static void test_held_mode_unchanged(midi_fx_api_v1_t *api) {
    void *inst = api->create_instance(".", NULL);
    uint8_t out_msgs[64][3];
    int out_lens[64];
    int out_count;

    if (!inst) fail("create_instance failed (held unchanged)");

    set_common_params(api, inst);
    api->set_param(inst, "register_mode", "held");
    api->set_param(inst, "lane1_note", "1");
    api->set_param(inst, "lane2_note", "2");

    send_midi(api, inst, 0x90, 60, 100);
    send_midi(api, inst, 0x90, 62, 100);
    send_midi(api, inst, 0xFA, 0, 0);
    out_count = drain_step(api, inst, out_msgs, out_lens, 64);

    if (count_note_ons(out_msgs, out_lens, out_count) != 2) {
        fail("held mode should remain unaffected (both enabled lanes should emit)");
    }

    api->destroy_instance(inst);
}

static void test_scale_mode_unchanged(midi_fx_api_v1_t *api) {
    void *inst = api->create_instance(".", NULL);
    uint8_t out_msgs[64][3];
    int out_lens[64];
    int out_count;

    if (!inst) fail("create_instance failed (scale unchanged)");

    set_common_params(api, inst);
    api->set_param(inst, "register_mode", "scale");
    api->set_param(inst, "lane1_note", "1");
    api->set_param(inst, "lane2_note", "2");

    /* Any held note keeps existing hold/latch gating semantics in scale mode. */
    send_midi(api, inst, 0x90, 60, 100);
    send_midi(api, inst, 0xFA, 0, 0);
    out_count = drain_step(api, inst, out_msgs, out_lens, 64);

    if (count_note_ons(out_msgs, out_lens, out_count) != 2) {
        fail("scale mode should remain unaffected (both enabled lanes should emit)");
    }

    api->destroy_instance(inst);
}

int main(void) {
    host_api_v1_t host;
    midi_fx_api_v1_t *api;

    memset(&host, 0, sizeof(host));
    host.api_version = MOVE_PLUGIN_API_VERSION;
    host.get_clock_status = test_get_clock_status;

    api = move_midi_fx_init(&host);
    if (!api || !api->create_instance || !api->set_param || !api->process_midi || !api->tick || !api->destroy_instance) {
        fail("eucalypso API init/callbacks missing");
    }

    test_drumpad_lane_gate_and_lane_note(api);
    test_drumpad_respects_register_range(api);
    test_held_mode_unchanged(api);
    test_scale_mode_unchanged(api);

    printf("PASS: eucalypso drumpad register mode\n");
    return 0;
}
