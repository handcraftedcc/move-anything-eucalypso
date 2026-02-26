/*
 * Eucalypso MIDI FX
 *
 * First migration pass from Super Arp:
 * - keeps transport/clock/sync/swing/latch/voice handling
 * - replaces arp progression/rhythm engines with lane-based Euclidean triggering
 * - keeps deterministic seeded modifiers with per-lane controls
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

#define LANE_COUNT 4
#define MAX_POOL_NOTES 24
#define MAX_HELD_NOTES 16
#define MAX_VOICES 64

#define DEFAULT_BPM 120
#define DEFAULT_SCALE_BASE_NOTE 48

#define CLOCK_OUTPUT_DELAY_TICKS 1
#define MAX_PENDING_STEP_TRIGGERS 64

typedef enum {
    RATE_1_32 = 0,
    RATE_1_16T,
    RATE_1_16,
    RATE_1_8T,
    RATE_1_8,
    RATE_1_4T,
    RATE_1_4,
    RATE_1_2,
    RATE_1_1
} rate_mode_t;

typedef enum { SYNC_INTERNAL = 0, SYNC_CLOCK } sync_mode_t;
typedef enum { REG_HELD = 0, REG_SCALE } register_mode_t;
typedef enum { ORDER_UP = 0, ORDER_DOWN, ORDER_PLAYED, ORDER_RAND } held_order_mode_t;
typedef enum { RETRIG_RESTART = 0, RETRIG_CONT } retrigger_mode_t;
typedef enum {
    SCALE_MAJOR = 0,
    SCALE_NATURAL_MINOR,
    SCALE_HARMONIC_MINOR,
    SCALE_MELODIC_MINOR,
    SCALE_DORIAN,
    SCALE_PHRYGIAN,
    SCALE_LYDIAN,
    SCALE_MIXOLYDIAN,
    SCALE_LOCRIAN,
    SCALE_PENT_MAJOR,
    SCALE_PENT_MINOR,
    SCALE_BLUES,
    SCALE_WHOLE_TONE,
    SCALE_CHROMATIC
} scale_mode_t;
typedef enum {
    OCT_RAND_P1 = 0,
    OCT_RAND_M1,
    OCT_RAND_PM1,
    OCT_RAND_P2,
    OCT_RAND_M2,
    OCT_RAND_PM2
} octave_random_range_t;

typedef struct {
    int enabled;
    int steps;
    int pulses;
    int rotation;
    int note_step;
    int octave;
    int note_rnd;
    int seed;
    int velocity;
    int gate;

    int mod_len;
    int drop;
    int drop_seed;
    int swap;
    int swap_seed;
    int oct;
    int oct_seed;
    octave_random_range_t oct_rng;
    int vel_rnd;
    int vel_seed;
    int gate_rnd;
    int gate_seed;
    int time_rnd;
    int time_seed;

    int step_cursor;
} lane_state_t;

typedef struct {
    rate_mode_t rate;
    sync_mode_t sync_mode;
    register_mode_t register_mode;
    held_order_mode_t held_order;
    scale_mode_t scale_mode;
    retrigger_mode_t retrigger_mode;
    int root_note;
    int octave;
    int scale_range;
    int held_order_seed;
    int rand_cycle;
    int global_velocity;
    int global_gate;
    int global_v_rnd;
    int global_g_rnd;
    int global_rnd_seed;
    int bpm;
    int swing;
    int latch;
    int max_voices;

    lane_state_t lanes[LANE_COUNT];

    uint8_t physical_notes[MAX_HELD_NOTES];
    int physical_count;
    uint8_t physical_as_played[MAX_HELD_NOTES];
    int physical_as_played_count;
    int note_set_dirty;
    uint8_t active_notes[MAX_HELD_NOTES];
    int active_count;
    uint8_t as_played[MAX_HELD_NOTES];
    int as_played_count;
    int latch_ready_replace;
    int phrase_running;

    int sample_rate;
    int timing_dirty;
    int step_interval_base;
    int samples_until_step;
    int swing_phase;
    double step_interval_base_f;
    double samples_until_step_f;
    uint64_t internal_sample_total;

    int clock_counter;
    int clocks_per_step;
    int clock_running;
    uint64_t clock_tick_total;
    int pending_step_triggers;
    int delayed_step_triggers;
    int midi_transport_active;
    uint64_t global_step_index;

    uint8_t voice_notes[MAX_VOICES];
    int voice_clock_left[MAX_VOICES];
    int voice_sample_left[MAX_VOICES];
    int voice_count;

    char chain_params_json[65536];
    int chain_params_len;
} eucalypso_instance_t;

static const host_api_v1_t *g_host = NULL;

static const int k_scale_major[] = {0, 2, 4, 5, 7, 9, 11};
static const int k_scale_natural_minor[] = {0, 2, 3, 5, 7, 8, 10};
static const int k_scale_harmonic_minor[] = {0, 2, 3, 5, 7, 8, 11};
static const int k_scale_melodic_minor[] = {0, 2, 3, 5, 7, 9, 11};
static const int k_scale_dorian[] = {0, 2, 3, 5, 7, 9, 10};
static const int k_scale_phrygian[] = {0, 1, 3, 5, 7, 8, 10};
static const int k_scale_lydian[] = {0, 2, 4, 6, 7, 9, 11};
static const int k_scale_mixolydian[] = {0, 2, 4, 5, 7, 9, 10};
static const int k_scale_locrian[] = {0, 1, 3, 5, 6, 8, 10};
static const int k_scale_pent_major[] = {0, 2, 4, 7, 9};
static const int k_scale_pent_minor[] = {0, 3, 5, 7, 10};
static const int k_scale_blues[] = {0, 3, 5, 6, 7, 10};
static const int k_scale_whole_tone[] = {0, 2, 4, 6, 8, 10};
static const int k_scale_chromatic[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint32_t mix_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static uint32_t step_rand_u32(uint32_t seed, uint64_t step, uint32_t salt) {
    uint32_t lo = (uint32_t)(step & 0xFFFFFFFFu);
    uint32_t hi = (uint32_t)((step >> 32) & 0xFFFFFFFFu);
    return mix_u32(seed ^ lo ^ mix_u32(hi ^ salt) ^ salt);
}

static int rand_offset_signed(uint32_t r, int amount) {
    int span;
    if (amount <= 0) return 0;
    span = amount * 2 + 1;
    return (int)(r % (uint32_t)span) - amount;
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[64];
    const char *pos;
    const char *colon;
    const char *end;
    int len;
    if (!json || !key || !out || out_len < 1) return 0;
    snprintf(search, sizeof(search), "\"%s\"", key);
    pos = strstr(json, search);
    if (!pos) return 0;
    colon = strchr(pos, ':');
    if (!colon) return 0;
    while (*colon && (*colon == ':' || *colon == ' ' || *colon == '\t')) colon++;
    if (*colon != '"') return 0;
    colon++;
    end = strchr(colon, '"');
    if (!end) return 0;
    len = (int)(end - colon);
    if (len >= out_len) len = out_len - 1;
    strncpy(out, colon, (size_t)len);
    out[len] = '\0';
    return len;
}

static int json_get_int(const char *json, const char *key, int *out) {
    char search[64];
    const char *pos;
    const char *colon;
    if (!json || !key || !out) return 0;
    snprintf(search, sizeof(search), "\"%s\"", key);
    pos = strstr(json, search);
    if (!pos) return 0;
    colon = strchr(pos, ':');
    if (!colon) return 0;
    colon++;
    while (*colon && (*colon == ' ' || *colon == '\t')) colon++;
    *out = atoi(colon);
    return 1;
}

static void cache_chain_params_from_module_json(eucalypso_instance_t *inst, const char *module_dir) {
    char path[512];
    FILE *f;
    char *json = NULL;
    long size;
    const char *chain_params;
    const char *arr_start;
    const char *arr_end;
    int depth = 1;
    if (!inst || !module_dir || !module_dir[0]) return;
    snprintf(path, sizeof(path), "%s/module.json", module_dir);
    f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    size = ftell(f);
    if (size <= 0 || size > 300000) {
        fclose(f);
        return;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return;
    }
    json = (char *)malloc((size_t)size + 1);
    if (!json) {
        fclose(f);
        return;
    }
    if (fread(json, 1, (size_t)size, f) != (size_t)size) {
        free(json);
        fclose(f);
        return;
    }
    json[size] = '\0';
    fclose(f);

    chain_params = strstr(json, "\"chain_params\"");
    if (!chain_params) {
        free(json);
        return;
    }
    arr_start = strchr(chain_params, '[');
    if (!arr_start) {
        free(json);
        return;
    }
    arr_end = arr_start + 1;
    while (*arr_end && depth > 0) {
        if (*arr_end == '[') depth++;
        else if (*arr_end == ']') depth--;
        arr_end++;
    }
    if (depth == 0) {
        int len = (int)(arr_end - arr_start);
        if (len > 0 && len < (int)sizeof(inst->chain_params_json)) {
            memcpy(inst->chain_params_json, arr_start, (size_t)len);
            inst->chain_params_json[len] = '\0';
            inst->chain_params_len = len;
        }
    }
    free(json);
}

static int emit3(uint8_t out_msgs[][3], int out_lens[], int max_out, int *count, uint8_t s, uint8_t d1, uint8_t d2) {
    if (!out_msgs || !out_lens || !count || *count >= max_out) return 0;
    out_msgs[*count][0] = s;
    out_msgs[*count][1] = d1;
    out_msgs[*count][2] = d2;
    out_lens[*count] = 3;
    (*count)++;
    return 1;
}

static int arr_contains(const uint8_t *arr, int count, uint8_t n) {
    int i;
    for (i = 0; i < count; i++) {
        if (arr[i] == n) return 1;
    }
    return 0;
}

static void arr_add_sorted(uint8_t *arr, int *count, uint8_t n, int max_count) {
    int i;
    int j;
    if (!arr || !count || *count >= max_count) return;
    for (i = 0; i < *count; i++) {
        if (arr[i] == n) return;
        if (arr[i] > n) break;
    }
    for (j = *count; j > i; j--) arr[j] = arr[j - 1];
    arr[i] = n;
    (*count)++;
}

static void arr_add_tail_unique(uint8_t *arr, int *count, uint8_t n, int max_count) {
    if (!arr || !count || *count >= max_count) return;
    if (arr_contains(arr, *count, n)) return;
    arr[*count] = n;
    (*count)++;
}

static void arr_remove(uint8_t *arr, int *count, uint8_t n) {
    int i;
    int found = -1;
    if (!arr || !count) return;
    for (i = 0; i < *count; i++) {
        if (arr[i] == n) {
            found = i;
            break;
        }
    }
    if (found < 0) return;
    for (i = found; i < *count - 1; i++) arr[i] = arr[i + 1];
    (*count)--;
}

static void as_played_add(eucalypso_instance_t *inst, uint8_t n) {
    if (!inst || inst->as_played_count >= MAX_HELD_NOTES) return;
    if (arr_contains(inst->as_played, inst->as_played_count, n)) return;
    inst->as_played[inst->as_played_count++] = n;
}

static void clear_active(eucalypso_instance_t *inst) {
    if (!inst) return;
    inst->active_count = 0;
    inst->as_played_count = 0;
}

static void sync_active_to_physical(eucalypso_instance_t *inst) {
    int i;
    if (!inst) return;
    clear_active(inst);
    for (i = 0; i < inst->physical_count; i++) {
        arr_add_sorted(inst->active_notes, &inst->active_count, inst->physical_notes[i], MAX_HELD_NOTES);
    }
    for (i = 0; i < inst->physical_as_played_count; i++) {
        uint8_t n = inst->physical_as_played[i];
        if (arr_contains(inst->active_notes, inst->active_count, n)) {
            as_played_add(inst, n);
        }
    }
}

static int any_enabled_lane(const eucalypso_instance_t *inst) {
    int i;
    if (!inst) return 0;
    for (i = 0; i < LANE_COUNT; i++) {
        if (inst->lanes[i].enabled) return 1;
    }
    return 0;
}

static int live_note_count(const eucalypso_instance_t *inst) {
    if (!inst) return 0;
    return inst->latch ? inst->active_count : inst->physical_count;
}

static void reset_lane_runtime(eucalypso_instance_t *inst) {
    int i;
    if (!inst) return;
    for (i = 0; i < LANE_COUNT; i++) {
        inst->lanes[i].step_cursor = 0;
    }
}

static void reset_phrase(eucalypso_instance_t *inst) {
    if (!inst) return;
    reset_lane_runtime(inst);
    inst->swing_phase = 0;
}

static void update_phrase_running(eucalypso_instance_t *inst) {
    int should_run;
    if (!inst) return;
    if (inst->register_mode == REG_SCALE) should_run = any_enabled_lane(inst) && live_note_count(inst) > 0;
    else should_run = live_note_count(inst) > 0;

    if (should_run) {
        if (!inst->phrase_running) {
            inst->phrase_running = 1;
            if (inst->retrigger_mode == RETRIG_RESTART) reset_phrase(inst);
        }
    } else {
        if (inst->phrase_running) {
            inst->phrase_running = 0;
            if (inst->retrigger_mode == RETRIG_RESTART) reset_phrase(inst);
        }
    }
}

static void set_latch(eucalypso_instance_t *inst, int en) {
    if (!inst) return;
    en = en ? 1 : 0;
    if (inst->latch == en) return;
    inst->latch = en;
    if (en) {
        if (inst->physical_count > 0) {
            sync_active_to_physical(inst);
            inst->latch_ready_replace = 0;
        } else {
            inst->latch_ready_replace = 1;
        }
    } else {
        inst->latch_ready_replace = 0;
        sync_active_to_physical(inst);
    }
    inst->note_set_dirty = 0;
    update_phrase_running(inst);
}

static void apply_pending_note_set(eucalypso_instance_t *inst) {
    if (!inst || inst->latch || !inst->note_set_dirty) return;
    sync_active_to_physical(inst);
    inst->note_set_dirty = 0;
}

static void note_on(eucalypso_instance_t *inst, uint8_t note, uint8_t vel) {
    if (!inst) return;
    arr_add_sorted(inst->physical_notes, &inst->physical_count, note, MAX_HELD_NOTES);
    arr_add_tail_unique(inst->physical_as_played, &inst->physical_as_played_count, note, MAX_HELD_NOTES);
    if (inst->latch) {
        if (inst->latch_ready_replace) {
            clear_active(inst);
            inst->latch_ready_replace = 0;
            if (inst->retrigger_mode == RETRIG_RESTART) reset_phrase(inst);
        }
        arr_add_sorted(inst->active_notes, &inst->active_count, note, MAX_HELD_NOTES);
        as_played_add(inst, note);
    } else {
        inst->note_set_dirty = 1;
    }
    (void)vel;
    update_phrase_running(inst);
}

static void note_off(eucalypso_instance_t *inst, uint8_t note) {
    if (!inst) return;
    arr_remove(inst->physical_notes, &inst->physical_count, note);
    arr_remove(inst->physical_as_played, &inst->physical_as_played_count, note);
    if (inst->latch) {
        if (inst->physical_count == 0) inst->latch_ready_replace = 1;
    } else {
        if (inst->physical_count == 0) {
            clear_active(inst);
            inst->note_set_dirty = 0;
        } else {
            inst->note_set_dirty = 1;
        }
    }
    update_phrase_running(inst);
}

static double rate_beats_per_step(rate_mode_t r) {
    switch (r) {
        case RATE_1_32: return 0.125;
        case RATE_1_16T: return 1.0 / 6.0;
        case RATE_1_16: return 0.25;
        case RATE_1_8T: return 1.0 / 3.0;
        case RATE_1_8: return 0.5;
        case RATE_1_4T: return 2.0 / 3.0;
        case RATE_1_4: return 1.0;
        case RATE_1_2: return 2.0;
        case RATE_1_1:
        default: return 4.0;
    }
}

static int rate_is_triplet(rate_mode_t r) {
    return (r == RATE_1_16T || r == RATE_1_8T || r == RATE_1_4T) ? 1 : 0;
}

static void recalc_clock_timing(eucalypso_instance_t *inst) {
    int clocks;
    double beats;
    if (!inst) return;
    beats = rate_beats_per_step(inst->rate);
    clocks = (int)(24.0 * beats + 0.5);
    if (clocks < 1) clocks = 1;
    inst->clocks_per_step = clocks;
}

static void realign_clock_phase(eucalypso_instance_t *inst) {
    if (!inst) return;
    if (inst->clocks_per_step < 1) inst->clocks_per_step = 1;
    inst->clock_counter = (int)(inst->clock_tick_total % (uint64_t)inst->clocks_per_step);
    inst->pending_step_triggers = 0;
    inst->delayed_step_triggers = 0;
}

static void recalc_timing(eucalypso_instance_t *inst, int sample_rate) {
    double step_samples;
    double beats;
    if (!inst || sample_rate <= 0) return;
    inst->bpm = clamp_int(inst->bpm, 40, 240);
    beats = rate_beats_per_step(inst->rate);
    step_samples = ((double)sample_rate * 60.0 * beats) / (double)inst->bpm;
    if (step_samples < 1.0) step_samples = 1.0;
    inst->sample_rate = sample_rate;
    inst->step_interval_base_f = step_samples;
    inst->step_interval_base = (int)(step_samples + 0.5);
    if (inst->step_interval_base < 1) inst->step_interval_base = 1;
    if (inst->samples_until_step_f > inst->step_interval_base_f || inst->samples_until_step_f <= 0.0) {
        inst->samples_until_step_f = inst->step_interval_base_f;
    }
    inst->samples_until_step = (int)(inst->samples_until_step_f + 0.5);
    if (inst->samples_until_step < 1) inst->samples_until_step = 1;
    inst->timing_dirty = 0;
}

static double next_step_interval(eucalypso_instance_t *inst) {
    double base;
    double sw;
    double d;
    double out;
    if (!inst) return 1.0;
    base = inst->step_interval_base_f > 0.0 ? inst->step_interval_base_f : 1.0;
    if (rate_is_triplet(inst->rate)) return base;
    sw = (double)clamp_int(inst->swing, 0, 100);
    if (sw <= 0.0) return base;
    d = (base * sw) / 200.0;
    if (inst->swing_phase == 0) {
        out = base + d;
        inst->swing_phase = 1;
    } else {
        out = base - d;
        inst->swing_phase = 0;
    }
    if (out < 1.0) out = 1.0;
    return out;
}

static void realign_internal_phase(eucalypso_instance_t *inst) {
    double interval;
    double rem;
    double until_next;
    if (!inst) return;
    interval = inst->step_interval_base_f > 0.0 ? inst->step_interval_base_f : 1.0;
    rem = fmod((double)inst->internal_sample_total, interval);
    if (rem < 0.0) rem += interval;
    if (rem < 1e-9) until_next = interval;
    else until_next = interval - rem;
    if (until_next < 1.0) until_next = 1.0;
    inst->samples_until_step_f = until_next;
    inst->samples_until_step = (int)(until_next + 0.5);
    if (inst->samples_until_step < 1) inst->samples_until_step = 1;
    inst->swing_phase = 0;
}

static uint32_t held_set_hash(const eucalypso_instance_t *inst) {
    uint32_t h = 2166136261u;
    int i;
    if (!inst) return h;
    for (i = 0; i < inst->active_count; i++) {
        h ^= (uint32_t)inst->active_notes[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t lane_modifier_hash(const eucalypso_instance_t *inst, int lane_index) {
    uint32_t held_hash = held_set_hash(inst);
    return mix_u32(held_hash ^ (uint32_t)((lane_index + 1) * 0x9E37u));
}

static int lane_should_drop(const lane_state_t *lane, uint32_t mod_hash, uint64_t mod_step) {
    uint32_t r;
    int amt;
    if (!lane) return 0;
    amt = clamp_int(lane->drop, 0, 100);
    if (amt <= 0) return 0;
    if (amt >= 100) return 1;
    r = step_rand_u32((uint32_t)lane->drop_seed, mod_step, mod_hash ^ 0xD0A4u);
    return (int)(r % 100u) < amt;
}

static int lane_step_velocity(const eucalypso_instance_t *inst, const lane_state_t *lane, int lane_index, uint64_t mod_step) {
    int base;
    int amt;
    int delta;
    uint32_t seed;
    uint32_t r;
    if (!inst || !lane) return 100;
    base = lane->velocity > 0 ? lane->velocity : inst->global_velocity;
    base = clamp_int(base, 1, 127);
    amt = clamp_int(inst->global_v_rnd, 0, 127);
    if (amt <= 0) return base;
    seed = (uint32_t)clamp_int(inst->global_rnd_seed, 0, 65535) + (uint32_t)(10000 * (lane_index + 1));
    r = step_rand_u32(seed, mod_step, 0xA11CEu);
    delta = rand_offset_signed(r, amt);
    return clamp_int(base + delta, 1, 127);
}

static int lane_step_gate(const eucalypso_instance_t *inst, const lane_state_t *lane, int lane_index, uint64_t mod_step) {
    int base;
    int amt;
    int delta;
    uint32_t seed;
    uint32_t r;
    if (!inst || !lane) return 80;
    base = lane->gate > 0 ? lane->gate : inst->global_gate;
    base = clamp_int(base, 0, 1600);
    amt = clamp_int(inst->global_g_rnd, 0, 1600);
    if (amt <= 0) return base;
    seed = (uint32_t)clamp_int(inst->global_rnd_seed, 0, 65535) + (uint32_t)(10000 * (lane_index + 1));
    r = step_rand_u32(seed, mod_step, 0x6A73u);
    delta = rand_offset_signed(r, amt);
    return clamp_int(base + delta, 0, 1600);
}

static int lane_random_octave_offset(const lane_state_t *lane, uint32_t mod_hash, uint64_t mod_step) {
    uint32_t r;
    int amt;
    if (!lane) return 0;
    amt = clamp_int(lane->oct, 0, 100);
    if (amt <= 0) return 0;
    r = step_rand_u32((uint32_t)lane->oct_seed ^ 0x0C7A9Eu, mod_step, mod_hash ^ 0x7F1Du);
    if ((int)(r % 100u) >= amt) return 0;
    switch (lane->oct_rng) {
        case OCT_RAND_P1: return 12;
        case OCT_RAND_M1: return -12;
        case OCT_RAND_PM1: return ((r >> 8) & 1u) ? 12 : -12;
        case OCT_RAND_P2: return ((r >> 10) & 1u) ? 12 : 24;
        case OCT_RAND_M2: return ((r >> 10) & 1u) ? -12 : -24;
        case OCT_RAND_PM2: {
            int pick = (int)((r >> 10) % 4u);
            if (pick == 0) return -24;
            if (pick == 1) return -12;
            if (pick == 2) return 12;
            return 24;
        }
        default: return 0;
    }
}

static int add_unique_note(int *notes, int count, int max_count, int note) {
    int i;
    for (i = 0; i < count; i++) {
        if (notes[i] == note) return count;
    }
    if (count >= max_count) return count;
    notes[count] = note;
    return count + 1;
}

static void seeded_shuffle_notes(int *arr, int count, uint32_t seed) {
    int i;
    if (!arr || count <= 1) return;
    for (i = count - 1; i > 0; i--) {
        uint32_t r = step_rand_u32(seed, (uint64_t)i, 0x41C6u);
        int j = (int)(r % (uint32_t)(i + 1));
        int t = arr[i];
        arr[i] = arr[j];
        arr[j] = t;
    }
}

static void scale_intervals_for_mode(scale_mode_t mode, const int **intervals, int *count) {
    if (!intervals || !count) return;
    switch (mode) {
        case SCALE_MAJOR: *intervals = k_scale_major; *count = 7; break;
        case SCALE_NATURAL_MINOR: *intervals = k_scale_natural_minor; *count = 7; break;
        case SCALE_HARMONIC_MINOR: *intervals = k_scale_harmonic_minor; *count = 7; break;
        case SCALE_MELODIC_MINOR: *intervals = k_scale_melodic_minor; *count = 7; break;
        case SCALE_DORIAN: *intervals = k_scale_dorian; *count = 7; break;
        case SCALE_PHRYGIAN: *intervals = k_scale_phrygian; *count = 7; break;
        case SCALE_LYDIAN: *intervals = k_scale_lydian; *count = 7; break;
        case SCALE_MIXOLYDIAN: *intervals = k_scale_mixolydian; *count = 7; break;
        case SCALE_LOCRIAN: *intervals = k_scale_locrian; *count = 7; break;
        case SCALE_PENT_MAJOR: *intervals = k_scale_pent_major; *count = 5; break;
        case SCALE_PENT_MINOR: *intervals = k_scale_pent_minor; *count = 5; break;
        case SCALE_BLUES: *intervals = k_scale_blues; *count = 6; break;
        case SCALE_WHOLE_TONE: *intervals = k_scale_whole_tone; *count = 6; break;
        case SCALE_CHROMATIC:
        default: *intervals = k_scale_chromatic; *count = 12; break;
    }
}

static int build_scale_pool(const eucalypso_instance_t *inst, int *out, int max_notes) {
    const int *intervals = NULL;
    int interval_count = 0;
    int octave;
    int i;
    int count = 0;
    int base;
    int note_limit;
    if (!inst || !out || max_notes <= 0) return 0;
    if (live_note_count(inst) <= 0) return 0;
    base = DEFAULT_SCALE_BASE_NOTE + clamp_int(inst->root_note, 0, 11);
    note_limit = clamp_int(inst->scale_range, 1, max_notes);
    scale_intervals_for_mode(inst->scale_mode, &intervals, &interval_count);
    if (inst->held_order == ORDER_DOWN) {
        if (base >= 0 && base <= 127) {
            count = add_unique_note(out, count, note_limit, base);
        }
        for (octave = 1; octave < 4 && count < note_limit; octave++) {
            for (i = interval_count - 1; i >= 0 && count < note_limit; i--) {
                int note = base - octave * 12 + intervals[i];
                if (note < 0 || note > 127) continue;
                count = add_unique_note(out, count, note_limit, note);
            }
        }
    } else {
        for (octave = 0; octave < 3 && count < note_limit; octave++) {
            for (i = 0; i < interval_count && count < note_limit; i++) {
                int note = base + octave * 12 + intervals[i];
                if (note < 0 || note > 127) continue;
                count = add_unique_note(out, count, note_limit, note);
            }
        }
    }
    if (count == 0) {
        out[0] = clamp_int(base, 0, 127);
        count = 1;
    }
    if (inst->held_order == ORDER_RAND) {
        seeded_shuffle_notes(out, count, (uint32_t)inst->held_order_seed);
    }
    return count;
}

static int build_held_pool(const eucalypso_instance_t *inst, int *out, int max_notes) {
    int i;
    int count = 0;
    if (!inst || !out || max_notes <= 0) return 0;
    if (inst->held_order == ORDER_PLAYED && inst->as_played_count > 0) {
        for (i = 0; i < inst->as_played_count && count < max_notes; i++) {
            count = add_unique_note(out, count, max_notes, (int)inst->as_played[i]);
        }
        return count;
    }
    if (inst->held_order == ORDER_DOWN) {
        for (i = inst->active_count - 1; i >= 0 && count < max_notes; i--) {
            count = add_unique_note(out, count, max_notes, (int)inst->active_notes[i]);
        }
        return count;
    }
    if (inst->held_order == ORDER_RAND) {
        for (i = 0; i < inst->active_count && count < max_notes; i++) {
            count = add_unique_note(out, count, max_notes, (int)inst->active_notes[i]);
        }
        seeded_shuffle_notes(out, count, (uint32_t)inst->held_order_seed);
        return count;
    }
    for (i = 0; i < inst->active_count && count < max_notes; i++) {
        count = add_unique_note(out, count, max_notes, (int)inst->active_notes[i]);
    }
    return count;
}

static int build_register_pool(const eucalypso_instance_t *inst, int *out, int max_notes) {
    if (!inst || !out || max_notes <= 0) return 0;
    if (inst->register_mode == REG_SCALE) return build_scale_pool(inst, out, max_notes);
    return build_held_pool(inst, out, max_notes);
}

static int select_lane_note(const lane_state_t *lane, const int *pool, int pool_count) {
    int idx;
    int step;
    if (!lane || !pool || pool_count <= 0) return -1;
    step = clamp_int(lane->note_step, 1, MAX_POOL_NOTES);
    idx = step - 1;
    if (idx < 0) idx = 0;
    if (idx >= pool_count) idx = pool_count - 1;
    return pool[idx];
}

static int apply_lane_note_random(const lane_state_t *lane, int note,
                                  const int *pool, int pool_count, uint32_t mod_hash, uint64_t mod_step) {
    uint32_t r;
    int amt;
    int i;
    int start;
    if (!lane || !pool || pool_count < 2) return note;
    amt = clamp_int(lane->note_rnd, 0, 100);
    if (amt <= 0) return note;
    r = step_rand_u32((uint32_t)lane->seed, mod_step, mod_hash ^ 0x91E3u);
    if ((int)(r % 100u) >= amt) return note;
    start = (int)((r >> 8) % (uint32_t)pool_count);
    for (i = 0; i < pool_count; i++) {
        int pick = (start + i) % pool_count;
        if (pool[pick] != note) return pool[pick];
    }
    return note;
}

static int euclid_hit(int step_index, int steps, int pulses, int rotation) {
    int pos;
    steps = clamp_int(steps, 1, 128);
    pulses = clamp_int(pulses, 0, steps);
    rotation = clamp_int(rotation, 0, steps - 1);
    if (pulses <= 0) return 0;
    if (pulses >= steps) return 1;
    pos = (step_index + rotation) % steps;
    return ((pos * pulses) % steps) < pulses;
}

static int euclid_pulse_index(int step_index, int steps, int pulses, int rotation) {
    int i;
    int idx = 0;
    steps = clamp_int(steps, 1, 128);
    pulses = clamp_int(pulses, 0, steps);
    rotation = clamp_int(rotation, 0, steps - 1);
    if (!euclid_hit(step_index, steps, pulses, rotation)) return -1;
    for (i = 0; i < steps; i++) {
        if (!euclid_hit(i, steps, pulses, rotation)) continue;
        if (i == step_index) return idx;
        idx++;
    }
    return -1;
}

static void voice_remove_at(eucalypso_instance_t *inst, int idx) {
    int i;
    if (!inst || idx < 0 || idx >= inst->voice_count) return;
    for (i = idx; i < inst->voice_count - 1; i++) {
        inst->voice_notes[i] = inst->voice_notes[i + 1];
        inst->voice_clock_left[i] = inst->voice_clock_left[i + 1];
        inst->voice_sample_left[i] = inst->voice_sample_left[i + 1];
    }
    inst->voice_count--;
}

static int voice_note_off(eucalypso_instance_t *inst, int idx,
                          uint8_t out_msgs[][3], int out_lens[], int max_out, int *count) {
    uint8_t note;
    if (!inst || !count || idx < 0 || idx >= inst->voice_count) return 0;
    note = inst->voice_notes[idx];
    if (!emit3(out_msgs, out_lens, max_out, count, 0x80, note, 0)) return 0;
    voice_remove_at(inst, idx);
    return 1;
}

static int flush_all_voices(eucalypso_instance_t *inst,
                            uint8_t out_msgs[][3], int out_lens[], int max_out, int *count) {
    int emitted = 0;
    if (!inst || !count) return 0;
    while (inst->voice_count > 0) {
        if (!voice_note_off(inst, 0, out_msgs, out_lens, max_out, count)) break;
        emitted++;
    }
    return emitted;
}

static int kill_voice_notes(eucalypso_instance_t *inst, uint8_t note,
                            uint8_t out_msgs[][3], int out_lens[], int max_out, int *count) {
    int i = 0;
    int killed = 0;
    if (!inst || !count) return 0;
    while (i < inst->voice_count) {
        if (inst->voice_notes[i] == note) {
            if (!voice_note_off(inst, i, out_msgs, out_lens, max_out, count)) break;
            killed++;
        } else {
            i++;
        }
    }
    return killed;
}

static void voice_add(eucalypso_instance_t *inst, uint8_t note, int gate_pct) {
    int idx;
    int clocks;
    int samples;
    if (!inst || inst->voice_count >= MAX_VOICES) return;
    idx = inst->voice_count++;
    inst->voice_notes[idx] = note;
    inst->voice_clock_left[idx] = 0;
    inst->voice_sample_left[idx] = 0;
    if (inst->sync_mode == SYNC_CLOCK) {
        clocks = (inst->clocks_per_step * gate_pct) / 100;
        if (clocks < 1) clocks = 1;
        inst->voice_clock_left[idx] = clocks;
    } else {
        samples = (inst->step_interval_base * gate_pct) / 100;
        if (samples < 1) samples = 1;
        inst->voice_sample_left[idx] = samples;
    }
}

static int advance_voice_timers_clock(eucalypso_instance_t *inst,
                                      uint8_t out_msgs[][3], int out_lens[], int max_out, int *count) {
    int i = 0;
    int emitted = 0;
    if (!inst || !count) return 0;
    while (i < inst->voice_count) {
        if (inst->voice_clock_left[i] > 0) inst->voice_clock_left[i]--;
        if (inst->voice_clock_left[i] <= 0) {
            if (!voice_note_off(inst, i, out_msgs, out_lens, max_out, count)) break;
            emitted++;
        } else {
            i++;
        }
    }
    return emitted;
}

static int advance_voice_timers_samples(eucalypso_instance_t *inst, int frames,
                                        uint8_t out_msgs[][3], int out_lens[], int max_out, int *count) {
    int i = 0;
    int emitted = 0;
    if (!inst || !count) return 0;
    while (i < inst->voice_count) {
        if (inst->voice_sample_left[i] > 0) inst->voice_sample_left[i] -= frames;
        if (inst->voice_sample_left[i] <= 0) {
            if (!voice_note_off(inst, i, out_msgs, out_lens, max_out, count)) break;
            emitted++;
        } else {
            i++;
        }
    }
    return emitted;
}

static int schedule_note(eucalypso_instance_t *inst, int note, int velocity, int gate_pct,
                         uint8_t out_msgs[][3], int out_lens[], int max_out, int *count) {
    uint8_t n;
    uint8_t vel;
    int voice_limit;
    if (!inst || !count || note < 0 || note > 127) return 0;
    n = (uint8_t)note;
    vel = (uint8_t)clamp_int(velocity, 1, 127);
    gate_pct = clamp_int(gate_pct, 0, 1600);
    voice_limit = clamp_int(inst->max_voices, 1, MAX_VOICES);

    (void)kill_voice_notes(inst, n, out_msgs, out_lens, max_out, count);
    while (inst->voice_count >= voice_limit) {
        if (!voice_note_off(inst, 0, out_msgs, out_lens, max_out, count)) return 0;
    }
    if (!emit3(out_msgs, out_lens, max_out, count, 0x90, n, vel)) return 0;
    if (gate_pct <= 0) {
        if (!emit3(out_msgs, out_lens, max_out, count, 0x80, n, 0)) return 0;
    } else {
        voice_add(inst, n, gate_pct);
    }
    return 1;
}

static int handle_transport_stop(eucalypso_instance_t *inst,
                                 uint8_t out_msgs[][3], int out_lens[], int max_out) {
    int count = 0;
    if (!inst) return 0;
    if (max_out > 0) {
        (void)emit3(out_msgs, out_lens, max_out, &count, 0xB0, 123, 0);
    }
    if (inst->voice_count > 0 && count < max_out) {
        (void)flush_all_voices(inst, out_msgs, out_lens, max_out, &count);
    }
    inst->physical_count = 0;
    inst->physical_as_played_count = 0;
    clear_active(inst);
    inst->note_set_dirty = 0;
    inst->latch_ready_replace = inst->latch ? 1 : 0;
    inst->phrase_running = 0;
    reset_phrase(inst);
    return count;
}

static int run_step(eucalypso_instance_t *inst, uint8_t out_msgs[][3], int out_lens[], int max_out) {
    int count = 0;
    int lane_i;
    int should_advance;
    int global_octave_semitones;
    if (!inst || max_out < 1) return 0;

    apply_pending_note_set(inst);
    update_phrase_running(inst);

    should_advance = inst->phrase_running || inst->retrigger_mode == RETRIG_CONT;
    if (!should_advance) return 0;
    global_octave_semitones = clamp_int(inst->octave, -3, 3) * 12;

    for (lane_i = 0; lane_i < LANE_COUNT; lane_i++) {
        lane_state_t *lane = &inst->lanes[lane_i];
        int steps;
        int pulses;
        int rotation;
        uint64_t mod_step;
        uint32_t mod_hash;
        int pulse_idx;
        int pool[MAX_POOL_NOTES];
        int pool_count;
        int note;
        int vel;
        int gate_pct;
        int lane_octave_semitones;

        if (!lane->enabled) continue;

        steps = clamp_int(lane->steps, 1, 128);
        lane->steps = steps;
        pulses = clamp_int(lane->pulses, 0, steps);
        lane->pulses = pulses;
        rotation = clamp_int(lane->rotation, 0, steps - 1);
        lane->rotation = rotation;
        if (lane->step_cursor < 0 || lane->step_cursor >= steps) lane->step_cursor = 0;
        lane_octave_semitones = clamp_int(lane->octave, -3, 3) * 12;

        mod_step = (uint64_t)lane->step_cursor;
        mod_hash = lane_modifier_hash(inst, lane_i);

        if (euclid_hit(lane->step_cursor, steps, pulses, rotation)) {
            pulse_idx = euclid_pulse_index(lane->step_cursor, steps, pulses, rotation);
            mod_step = (uint64_t)(pulse_idx >= 0 ? pulse_idx : lane->step_cursor);
            if (!lane_should_drop(lane, mod_hash, mod_step)) {
                pool_count = build_register_pool(inst, pool, MAX_POOL_NOTES);
                if (pool_count > 0) {
                    note = select_lane_note(lane, pool, pool_count);
                    note = apply_lane_note_random(lane, note, pool, pool_count, mod_hash, mod_step);
                    note += lane_octave_semitones;
                    note += lane_random_octave_offset(lane, mod_hash, mod_step);
                    note += global_octave_semitones;
                    note = clamp_int(note, 0, 127);
                    vel = lane_step_velocity(inst, lane, lane_i, mod_step);
                    gate_pct = lane_step_gate(inst, lane, lane_i, mod_step);
                    if (lane->time_rnd > 0) {
                        (void)step_rand_u32((uint32_t)lane->seed, mod_step, mod_hash ^ 0x41D9u);
                    }
                    if (!schedule_note(inst, note, vel, gate_pct, out_msgs, out_lens, max_out, &count)) {
                        break;
                    }
                }
            }
        }

        lane->step_cursor = (lane->step_cursor + 1) % steps;
        if (count >= max_out) break;
    }

    inst->global_step_index++;
    return count;
}

static int process_clock_tick(eucalypso_instance_t *inst, uint8_t out_msgs[][3], int out_lens[], int max_out) {
    int count = 0;
    if (!inst || max_out < 1) return 0;
    if (CLOCK_OUTPUT_DELAY_TICKS == 1 && inst->delayed_step_triggers > 0) {
        inst->pending_step_triggers += inst->delayed_step_triggers;
        inst->delayed_step_triggers = 0;
        if (inst->pending_step_triggers > MAX_PENDING_STEP_TRIGGERS) {
            inst->pending_step_triggers = MAX_PENDING_STEP_TRIGGERS;
        }
    }
    (void)advance_voice_timers_clock(inst, out_msgs, out_lens, max_out, &count);
    inst->clock_tick_total++;
    inst->clock_counter = (int)(inst->clock_tick_total % (uint64_t)inst->clocks_per_step);
    if (inst->clock_counter == 0) {
        if (CLOCK_OUTPUT_DELAY_TICKS > 0) inst->delayed_step_triggers++;
        else inst->pending_step_triggers++;
        if (inst->delayed_step_triggers > MAX_PENDING_STEP_TRIGGERS) {
            inst->delayed_step_triggers = MAX_PENDING_STEP_TRIGGERS;
        }
        if (inst->pending_step_triggers > MAX_PENDING_STEP_TRIGGERS) {
            inst->pending_step_triggers = MAX_PENDING_STEP_TRIGGERS;
        }
    }
    return count;
}

static int enforce_voice_limit(eucalypso_instance_t *inst,
                               uint8_t out_msgs[][3], int out_lens[], int max_out, int *count) {
    int limit;
    int emitted = 0;
    if (!inst || !count) return 0;
    limit = clamp_int(inst->max_voices, 1, MAX_VOICES);
    while (inst->voice_count > limit) {
        if (!voice_note_off(inst, 0, out_msgs, out_lens, max_out, count)) break;
        emitted++;
    }
    return emitted;
}

static const char *rate_to_string(rate_mode_t r) {
    switch (r) {
        case RATE_1_32: return "1/32";
        case RATE_1_16T: return "1/16T";
        case RATE_1_16: return "1/16";
        case RATE_1_8T: return "1/8T";
        case RATE_1_8: return "1/8";
        case RATE_1_4T: return "1/4T";
        case RATE_1_4: return "1/4";
        case RATE_1_2: return "1/2";
        case RATE_1_1:
        default: return "1";
    }
}

static void set_rate_from_string(eucalypso_instance_t *inst, const char *v) {
    if (!inst || !v) return;
    if (strcmp(v, "1/32") == 0) inst->rate = RATE_1_32;
    else if (strcmp(v, "1/16T") == 0) inst->rate = RATE_1_16T;
    else if (strcmp(v, "1/16") == 0) inst->rate = RATE_1_16;
    else if (strcmp(v, "1/8T") == 0) inst->rate = RATE_1_8T;
    else if (strcmp(v, "1/8") == 0) inst->rate = RATE_1_8;
    else if (strcmp(v, "1/4T") == 0) inst->rate = RATE_1_4T;
    else if (strcmp(v, "1/4") == 0) inst->rate = RATE_1_4;
    else if (strcmp(v, "1/2") == 0) inst->rate = RATE_1_2;
    else if (strcmp(v, "1") == 0) inst->rate = RATE_1_1;
}

static const char *register_mode_to_string(register_mode_t v) {
    return v == REG_SCALE ? "scale" : "held";
}

static void set_register_mode_from_string(eucalypso_instance_t *inst, const char *v) {
    if (!inst || !v) return;
    if (strcmp(v, "scale") == 0) inst->register_mode = REG_SCALE;
    else inst->register_mode = REG_HELD;
}

static const char *scale_mode_to_string(scale_mode_t v) {
    switch (v) {
        case SCALE_MAJOR: return "major";
        case SCALE_NATURAL_MINOR: return "natural_minor";
        case SCALE_HARMONIC_MINOR: return "harmonic_minor";
        case SCALE_MELODIC_MINOR: return "melodic_minor";
        case SCALE_DORIAN: return "dorian";
        case SCALE_PHRYGIAN: return "phrygian";
        case SCALE_LYDIAN: return "lydian";
        case SCALE_MIXOLYDIAN: return "mixolydian";
        case SCALE_LOCRIAN: return "locrian";
        case SCALE_PENT_MAJOR: return "pentatonic_major";
        case SCALE_PENT_MINOR: return "pentatonic_minor";
        case SCALE_BLUES: return "blues";
        case SCALE_WHOLE_TONE: return "whole_tone";
        case SCALE_CHROMATIC:
        default: return "chromatic";
    }
}

static void set_scale_mode_from_string(eucalypso_instance_t *inst, const char *v) {
    if (!inst || !v) return;
    if (strcmp(v, "major") == 0) inst->scale_mode = SCALE_MAJOR;
    else if (strcmp(v, "natural_minor") == 0) inst->scale_mode = SCALE_NATURAL_MINOR;
    else if (strcmp(v, "harmonic_minor") == 0) inst->scale_mode = SCALE_HARMONIC_MINOR;
    else if (strcmp(v, "melodic_minor") == 0) inst->scale_mode = SCALE_MELODIC_MINOR;
    else if (strcmp(v, "dorian") == 0) inst->scale_mode = SCALE_DORIAN;
    else if (strcmp(v, "phrygian") == 0) inst->scale_mode = SCALE_PHRYGIAN;
    else if (strcmp(v, "lydian") == 0) inst->scale_mode = SCALE_LYDIAN;
    else if (strcmp(v, "mixolydian") == 0) inst->scale_mode = SCALE_MIXOLYDIAN;
    else if (strcmp(v, "locrian") == 0) inst->scale_mode = SCALE_LOCRIAN;
    else if (strcmp(v, "pentatonic_major") == 0) inst->scale_mode = SCALE_PENT_MAJOR;
    else if (strcmp(v, "pentatonic_minor") == 0) inst->scale_mode = SCALE_PENT_MINOR;
    else if (strcmp(v, "blues") == 0) inst->scale_mode = SCALE_BLUES;
    else if (strcmp(v, "whole_tone") == 0) inst->scale_mode = SCALE_WHOLE_TONE;
    else if (strcmp(v, "chromatic") == 0) inst->scale_mode = SCALE_CHROMATIC;
}

static const char *held_order_to_string(held_order_mode_t v) {
    switch (v) {
        case ORDER_DOWN: return "down";
        case ORDER_PLAYED: return "played";
        case ORDER_RAND: return "rand";
        case ORDER_UP:
        default: return "up";
    }
}

static void set_held_order_from_string(eucalypso_instance_t *inst, const char *v) {
    if (!inst || !v) return;
    if (strcmp(v, "down") == 0) inst->held_order = ORDER_DOWN;
    else if (strcmp(v, "played") == 0) inst->held_order = ORDER_PLAYED;
    else if (strcmp(v, "rand") == 0) inst->held_order = ORDER_RAND;
    else inst->held_order = ORDER_UP;
}

static const char *sync_to_string(sync_mode_t v) {
    return v == SYNC_CLOCK ? "clock" : "internal";
}

static const char *oct_rng_to_string(octave_random_range_t v) {
    switch (v) {
        case OCT_RAND_P1: return "+1";
        case OCT_RAND_M1: return "-1";
        case OCT_RAND_PM1: return "+-1";
        case OCT_RAND_P2: return "+2";
        case OCT_RAND_M2: return "-2";
        case OCT_RAND_PM2:
        default: return "+-2";
    }
}

static void set_oct_rng_from_string(lane_state_t *lane, const char *v) {
    if (!lane || !v) return;
    if (strcmp(v, "+1") == 0) lane->oct_rng = OCT_RAND_P1;
    else if (strcmp(v, "-1") == 0) lane->oct_rng = OCT_RAND_M1;
    else if (strcmp(v, "+-1") == 0) lane->oct_rng = OCT_RAND_PM1;
    else if (strcmp(v, "+2") == 0) lane->oct_rng = OCT_RAND_P2;
    else if (strcmp(v, "-2") == 0) lane->oct_rng = OCT_RAND_M2;
    else if (strcmp(v, "+-2") == 0) lane->oct_rng = OCT_RAND_PM2;
}

static int parse_lane_key(const char *key, int *lane_idx, const char **suffix) {
    const char *p;
    int n = 0;
    if (!key || strncmp(key, "lane", 4) != 0) return 0;
    p = key + 4;
    if (!isdigit((unsigned char)*p)) return 0;
    while (isdigit((unsigned char)*p)) {
        n = n * 10 + (*p - '0');
        p++;
    }
    if (n < 1 || n > LANE_COUNT) return 0;
    if (*p != '_') return 0;
    if (lane_idx) *lane_idx = n - 1;
    if (suffix) *suffix = p + 1;
    return 1;
}

static void set_lane_param(eucalypso_instance_t *inst, int lane_idx, const char *suffix, const char *val) {
    lane_state_t *lane;
    if (!inst || lane_idx < 0 || lane_idx >= LANE_COUNT || !suffix || !val) return;
    lane = &inst->lanes[lane_idx];
    if (strcmp(suffix, "enabled") == 0) lane->enabled = strcmp(val, "on") == 0 ? 1 : 0;
    else if (strcmp(suffix, "steps") == 0) lane->steps = clamp_int(atoi(val), 1, 128);
    else if (strcmp(suffix, "pulses") == 0) lane->pulses = clamp_int(atoi(val), 0, 128);
    else if (strcmp(suffix, "rotation") == 0) lane->rotation = clamp_int(atoi(val), 0, 127);
    else if (strcmp(suffix, "note") == 0) lane->note_step = clamp_int(atoi(val), 1, MAX_POOL_NOTES);
    else if (strcmp(suffix, "octave") == 0) lane->octave = clamp_int(atoi(val), -3, 3);
    else if (strcmp(suffix, "n_rnd") == 0) lane->note_rnd = clamp_int(atoi(val), 0, 100);
    else if (strcmp(suffix, "n_seed") == 0 || strcmp(suffix, "seed") == 0) lane->seed = clamp_int(atoi(val), 0, 65535);
    else if (strcmp(suffix, "velocity") == 0) lane->velocity = clamp_int(atoi(val), 0, 127);
    else if (strcmp(suffix, "gate") == 0) lane->gate = clamp_int(atoi(val), 0, 1600);
    else if (strcmp(suffix, "mod_len") == 0) lane->mod_len = clamp_int(atoi(val), 0, 64);
    else if (strcmp(suffix, "drop") == 0) lane->drop = clamp_int(atoi(val), 0, 100);
    else if (strcmp(suffix, "drop_seed") == 0) lane->drop_seed = clamp_int(atoi(val), 0, 65535);
    else if (strcmp(suffix, "swap") == 0) lane->swap = clamp_int(atoi(val), 0, 100);
    else if (strcmp(suffix, "swap_seed") == 0) lane->swap_seed = clamp_int(atoi(val), 0, 65535);
    else if (strcmp(suffix, "oct") == 0 || strcmp(suffix, "oct_rnd") == 0) lane->oct = clamp_int(atoi(val), 0, 100);
    else if (strcmp(suffix, "oct_seed") == 0) lane->oct_seed = clamp_int(atoi(val), 0, 65535);
    else if (strcmp(suffix, "oct_rng") == 0) set_oct_rng_from_string(lane, val);
    else if (strcmp(suffix, "vel_rnd") == 0) lane->vel_rnd = clamp_int(atoi(val), 0, 127);
    else if (strcmp(suffix, "vel_seed") == 0) lane->vel_seed = clamp_int(atoi(val), 0, 65535);
    else if (strcmp(suffix, "gate_rnd") == 0) lane->gate_rnd = clamp_int(atoi(val), 0, 1600);
    else if (strcmp(suffix, "gate_seed") == 0) lane->gate_seed = clamp_int(atoi(val), 0, 65535);
    else if (strcmp(suffix, "time_rnd") == 0) lane->time_rnd = clamp_int(atoi(val), 0, 100);
    else if (strcmp(suffix, "time_seed") == 0) lane->time_seed = clamp_int(atoi(val), 0, 65535);
    lane->pulses = clamp_int(lane->pulses, 0, lane->steps);
}

static int get_lane_param(const eucalypso_instance_t *inst, int lane_idx, const char *suffix, char *buf, int buf_len) {
    const lane_state_t *lane;
    if (!inst || lane_idx < 0 || lane_idx >= LANE_COUNT || !suffix || !buf || buf_len < 1) return -1;
    lane = &inst->lanes[lane_idx];
    if (strcmp(suffix, "enabled") == 0) return snprintf(buf, buf_len, "%s", lane->enabled ? "on" : "off");
    if (strcmp(suffix, "steps") == 0) return snprintf(buf, buf_len, "%d", lane->steps);
    if (strcmp(suffix, "pulses") == 0) return snprintf(buf, buf_len, "%d", lane->pulses);
    if (strcmp(suffix, "rotation") == 0) return snprintf(buf, buf_len, "%d", lane->rotation);
    if (strcmp(suffix, "note") == 0) return snprintf(buf, buf_len, "%d", lane->note_step);
    if (strcmp(suffix, "octave") == 0) return snprintf(buf, buf_len, "%d", lane->octave);
    if (strcmp(suffix, "n_rnd") == 0) return snprintf(buf, buf_len, "%d", lane->note_rnd);
    if (strcmp(suffix, "n_seed") == 0 || strcmp(suffix, "seed") == 0) return snprintf(buf, buf_len, "%d", lane->seed);
    if (strcmp(suffix, "velocity") == 0) return snprintf(buf, buf_len, "%d", lane->velocity);
    if (strcmp(suffix, "gate") == 0) return snprintf(buf, buf_len, "%d", lane->gate);
    if (strcmp(suffix, "mod_len") == 0) return snprintf(buf, buf_len, "%d", lane->mod_len);
    if (strcmp(suffix, "drop") == 0) return snprintf(buf, buf_len, "%d", lane->drop);
    if (strcmp(suffix, "drop_seed") == 0) return snprintf(buf, buf_len, "%d", lane->drop_seed);
    if (strcmp(suffix, "swap") == 0) return snprintf(buf, buf_len, "%d", lane->swap);
    if (strcmp(suffix, "swap_seed") == 0) return snprintf(buf, buf_len, "%d", lane->swap_seed);
    if (strcmp(suffix, "oct") == 0 || strcmp(suffix, "oct_rnd") == 0) return snprintf(buf, buf_len, "%d", lane->oct);
    if (strcmp(suffix, "oct_seed") == 0) return snprintf(buf, buf_len, "%d", lane->oct_seed);
    if (strcmp(suffix, "oct_rng") == 0) return snprintf(buf, buf_len, "%s", oct_rng_to_string(lane->oct_rng));
    if (strcmp(suffix, "vel_rnd") == 0) return snprintf(buf, buf_len, "%d", lane->vel_rnd);
    if (strcmp(suffix, "vel_seed") == 0) return snprintf(buf, buf_len, "%d", lane->vel_seed);
    if (strcmp(suffix, "gate_rnd") == 0) return snprintf(buf, buf_len, "%d", lane->gate_rnd);
    if (strcmp(suffix, "gate_seed") == 0) return snprintf(buf, buf_len, "%d", lane->gate_seed);
    if (strcmp(suffix, "time_rnd") == 0) return snprintf(buf, buf_len, "%d", lane->time_rnd);
    if (strcmp(suffix, "time_seed") == 0) return snprintf(buf, buf_len, "%d", lane->time_seed);
    return -1;
}

static void eucalypso_set_param(void *instance, const char *key, const char *val);

static void apply_state_json(eucalypso_instance_t *inst, const char *json) {
    static const char *global_str_keys[] = {
        "register_mode", "scale_mode", "held_order", "play_mode",
        "retrigger_mode", "rate", "sync"
    };
    static const char *global_int_keys[] = {
        "root_note", "octave", "scale_rng", "held_order_seed", "rand_cycle",
        "global_velocity", "global_gate", "global_v_rnd", "global_g_rnd", "global_rnd_seed",
        "bpm", "swing", "max_voices"
    };
    static const char *lane_str_suffixes[] = {
        "enabled", "oct_rng"
    };
    static const char *lane_int_suffixes[] = {
        "steps", "pulses", "rotation", "note", "octave",
        "oct_rnd", "oct_seed", "velocity", "gate", "drop", "drop_seed", "n_rnd", "n_seed"
    };
    char s[64];
    char b[16];
    char full[64];
    int i;
    int lane;
    int v;
    if (!inst || !json) return;
    for (i = 0; i < (int)(sizeof(global_str_keys) / sizeof(global_str_keys[0])); i++) {
        if (json_get_string(json, global_str_keys[i], s, sizeof(s))) {
            eucalypso_set_param(inst, global_str_keys[i], s);
        }
    }
    for (i = 0; i < (int)(sizeof(global_int_keys) / sizeof(global_int_keys[0])); i++) {
        if (json_get_int(json, global_int_keys[i], &v)) {
            snprintf(b, sizeof(b), "%d", v);
            eucalypso_set_param(inst, global_int_keys[i], b);
        }
    }
    for (lane = 1; lane <= LANE_COUNT; lane++) {
        for (i = 0; i < (int)(sizeof(lane_str_suffixes) / sizeof(lane_str_suffixes[0])); i++) {
            snprintf(full, sizeof(full), "lane%d_%s", lane, lane_str_suffixes[i]);
            if (json_get_string(json, full, s, sizeof(s))) {
                eucalypso_set_param(inst, full, s);
            }
        }
        for (i = 0; i < (int)(sizeof(lane_int_suffixes) / sizeof(lane_int_suffixes[0])); i++) {
            snprintf(full, sizeof(full), "lane%d_%s", lane, lane_int_suffixes[i]);
            if (json_get_int(json, full, &v)) {
                snprintf(b, sizeof(b), "%d", v);
                eucalypso_set_param(inst, full, b);
            }
        }
    }
}

static void eucalypso_set_param(void *instance, const char *key, const char *val) {
    eucalypso_instance_t *inst = (eucalypso_instance_t *)instance;
    int lane_idx;
    const char *suffix;
    if (!inst || !key || !val) return;

    if (strcmp(key, "state") == 0) {
        apply_state_json(inst, val);
        return;
    }

    if (parse_lane_key(key, &lane_idx, &suffix)) {
        set_lane_param(inst, lane_idx, suffix, val);
        update_phrase_running(inst);
        return;
    }

    if (strcmp(key, "register_mode") == 0) set_register_mode_from_string(inst, val);
    else if (strcmp(key, "root_note") == 0) inst->root_note = clamp_int(atoi(val), 0, 11);
    else if (strcmp(key, "octave") == 0) inst->octave = clamp_int(atoi(val), -3, 3);
    else if (strcmp(key, "scale_rng") == 0) inst->scale_range = clamp_int(atoi(val), 1, MAX_POOL_NOTES);
    else if (strcmp(key, "scale_mode") == 0) set_scale_mode_from_string(inst, val);
    else if (strcmp(key, "held_order") == 0) set_held_order_from_string(inst, val);
    else if (strcmp(key, "held_order_seed") == 0) inst->held_order_seed = clamp_int(atoi(val), 0, 65535);
    else if (strcmp(key, "play_mode") == 0) set_latch(inst, strcmp(val, "latch") == 0);
    else if (strcmp(key, "retrigger_mode") == 0) {
        retrigger_mode_t old = inst->retrigger_mode;
        inst->retrigger_mode = strcmp(val, "cont") == 0 ? RETRIG_CONT : RETRIG_RESTART;
        if (inst->retrigger_mode != old && inst->retrigger_mode == RETRIG_RESTART) reset_phrase(inst);
    }
    else if (strcmp(key, "rate") == 0) {
        set_rate_from_string(inst, val);
        inst->timing_dirty = 1;
        recalc_clock_timing(inst);
    }
    else if (strcmp(key, "rand_cycle") == 0) inst->rand_cycle = clamp_int(atoi(val), 1, 128);
    else if (strcmp(key, "global_velocity") == 0) inst->global_velocity = clamp_int(atoi(val), 1, 127);
    else if (strcmp(key, "global_gate") == 0) inst->global_gate = clamp_int(atoi(val), 1, 1600);
    else if (strcmp(key, "global_v_rnd") == 0) inst->global_v_rnd = clamp_int(atoi(val), 0, 127);
    else if (strcmp(key, "global_g_rnd") == 0) inst->global_g_rnd = clamp_int(atoi(val), 0, 1600);
    else if (strcmp(key, "global_rnd_seed") == 0) inst->global_rnd_seed = clamp_int(atoi(val), 0, 65535);
    else if (strcmp(key, "sync") == 0) {
        if (strcmp(val, "clock") == 0) {
            if (inst->sync_mode != SYNC_CLOCK) {
                inst->sync_mode = SYNC_CLOCK;
                inst->clock_running = 1;
                inst->clock_counter = 0;
                inst->clock_tick_total = 0;
                inst->pending_step_triggers = 0;
                inst->delayed_step_triggers = 0;
            }
            recalc_clock_timing(inst);
        } else {
            if (inst->sync_mode != SYNC_INTERNAL) {
                inst->sync_mode = SYNC_INTERNAL;
                inst->clock_counter = 0;
                inst->clock_tick_total = 0;
                inst->pending_step_triggers = 0;
                inst->delayed_step_triggers = 0;
                inst->internal_sample_total = 0;
                if (inst->sample_rate > 0) recalc_timing(inst, inst->sample_rate);
                inst->samples_until_step_f = inst->step_interval_base_f > 0.0 ? inst->step_interval_base_f : 1.0;
                inst->samples_until_step = (int)(inst->samples_until_step_f + 0.5);
                if (inst->samples_until_step < 1) inst->samples_until_step = 1;
                inst->swing_phase = 0;
            }
        }
    }
    else if (strcmp(key, "bpm") == 0) {
        inst->bpm = clamp_int(atoi(val), 40, 240);
        inst->timing_dirty = 1;
    }
    else if (strcmp(key, "swing") == 0) inst->swing = clamp_int(atoi(val), 0, 100);
    else if (strcmp(key, "max_voices") == 0) inst->max_voices = clamp_int(atoi(val), 1, MAX_VOICES);

    if (strcmp(key, "rate") == 0 &&
        inst->sync_mode == SYNC_CLOCK && inst->clock_running) {
        realign_clock_phase(inst);
    }

    if ((strcmp(key, "rate") == 0 || strcmp(key, "bpm") == 0) &&
        inst->sync_mode == SYNC_INTERNAL && inst->sample_rate > 0) {
        recalc_timing(inst, inst->sample_rate);
        realign_internal_phase(inst);
    }

    update_phrase_running(inst);
}

static int appendf(char *buf, int buf_len, int *used, const char *fmt, ...) {
    va_list ap;
    int n;
    if (!buf || !used || !fmt || *used < 0 || *used >= buf_len) return 0;
    va_start(ap, fmt);
    n = vsnprintf(buf + *used, (size_t)(buf_len - *used), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= buf_len - *used) return 0;
    *used += n;
    return 1;
}

static int append_state_sep(char *buf, int buf_len, int *used, int *first) {
    if (*first) {
        *first = 0;
        return 1;
    }
    return appendf(buf, buf_len, used, ",");
}

static int append_state_string(char *buf, int buf_len, int *used, int *first, const char *key, const char *val) {
    if (!append_state_sep(buf, buf_len, used, first)) return 0;
    return appendf(buf, buf_len, used, "\"%s\":\"%s\"", key, val);
}

static int append_state_int(char *buf, int buf_len, int *used, int *first, const char *key, int val) {
    if (!append_state_sep(buf, buf_len, used, first)) return 0;
    return appendf(buf, buf_len, used, "\"%s\":%d", key, val);
}

static int build_state_json(const eucalypso_instance_t *inst, char *buf, int buf_len) {
    int used = 0;
    int first = 1;
    int lane;
    int i;
    char key[64];
    static const char *lane_int_suffixes[] = {
        "steps", "pulses", "rotation", "note", "octave",
        "oct_rnd", "oct_seed", "velocity", "gate", "drop", "drop_seed", "n_rnd", "n_seed"
    };
    if (!inst || !buf || buf_len < 2) return -1;
    if (!appendf(buf, buf_len, &used, "{")) return -1;

    if (!append_state_string(buf, buf_len, &used, &first, "register_mode", register_mode_to_string(inst->register_mode))) return -1;
    if (!append_state_int(buf, buf_len, &used, &first, "root_note", inst->root_note)) return -1;
    if (!append_state_int(buf, buf_len, &used, &first, "octave", inst->octave)) return -1;
    if (!append_state_int(buf, buf_len, &used, &first, "scale_rng", inst->scale_range)) return -1;
    if (!append_state_string(buf, buf_len, &used, &first, "scale_mode", scale_mode_to_string(inst->scale_mode))) return -1;
    if (!append_state_string(buf, buf_len, &used, &first, "held_order", held_order_to_string(inst->held_order))) return -1;
    if (!append_state_int(buf, buf_len, &used, &first, "held_order_seed", inst->held_order_seed)) return -1;
    if (!append_state_string(buf, buf_len, &used, &first, "play_mode", inst->latch ? "latch" : "hold")) return -1;
    if (!append_state_string(buf, buf_len, &used, &first, "retrigger_mode", inst->retrigger_mode == RETRIG_CONT ? "cont" : "restart")) return -1;
    if (!append_state_string(buf, buf_len, &used, &first, "rate", rate_to_string(inst->rate))) return -1;
    if (!append_state_int(buf, buf_len, &used, &first, "rand_cycle", inst->rand_cycle)) return -1;
    if (!append_state_int(buf, buf_len, &used, &first, "global_velocity", inst->global_velocity)) return -1;
    if (!append_state_int(buf, buf_len, &used, &first, "global_gate", inst->global_gate)) return -1;
    if (!append_state_int(buf, buf_len, &used, &first, "global_v_rnd", inst->global_v_rnd)) return -1;
    if (!append_state_int(buf, buf_len, &used, &first, "global_g_rnd", inst->global_g_rnd)) return -1;
    if (!append_state_int(buf, buf_len, &used, &first, "global_rnd_seed", inst->global_rnd_seed)) return -1;
    if (!append_state_string(buf, buf_len, &used, &first, "sync", sync_to_string(inst->sync_mode))) return -1;
    if (!append_state_int(buf, buf_len, &used, &first, "bpm", inst->bpm)) return -1;
    if (!append_state_int(buf, buf_len, &used, &first, "swing", inst->swing)) return -1;
    if (!append_state_int(buf, buf_len, &used, &first, "max_voices", inst->max_voices)) return -1;

    for (lane = 0; lane < LANE_COUNT; lane++) {
        const lane_state_t *ln = &inst->lanes[lane];
        snprintf(key, sizeof(key), "lane%d_enabled", lane + 1);
        if (!append_state_string(buf, buf_len, &used, &first, key, ln->enabled ? "on" : "off")) return -1;
        snprintf(key, sizeof(key), "lane%d_oct_rng", lane + 1);
        if (!append_state_string(buf, buf_len, &used, &first, key, oct_rng_to_string(ln->oct_rng))) return -1;

        for (i = 0; i < (int)(sizeof(lane_int_suffixes) / sizeof(lane_int_suffixes[0])); i++) {
            char vbuf[16];
            snprintf(key, sizeof(key), "lane%d_%s", lane + 1, lane_int_suffixes[i]);
            if (get_lane_param(inst, lane, lane_int_suffixes[i], vbuf, sizeof(vbuf)) < 0) return -1;
            if (!append_state_int(buf, buf_len, &used, &first, key, atoi(vbuf))) return -1;
        }
    }

    if (!appendf(buf, buf_len, &used, "}")) return -1;
    return used;
}

static int eucalypso_get_param(void *instance, const char *key, char *buf, int buf_len) {
    eucalypso_instance_t *inst = (eucalypso_instance_t *)instance;
    int lane_idx;
    const char *suffix;
    if (!inst || !key || !buf || buf_len < 1) return -1;

    if (parse_lane_key(key, &lane_idx, &suffix)) {
        return get_lane_param(inst, lane_idx, suffix, buf, buf_len);
    }

    if (strcmp(key, "register_mode") == 0) return snprintf(buf, buf_len, "%s", register_mode_to_string(inst->register_mode));
    if (strcmp(key, "root_note") == 0) return snprintf(buf, buf_len, "%d", inst->root_note);
    if (strcmp(key, "octave") == 0) return snprintf(buf, buf_len, "%d", inst->octave);
    if (strcmp(key, "scale_rng") == 0) return snprintf(buf, buf_len, "%d", inst->scale_range);
    if (strcmp(key, "scale_mode") == 0) return snprintf(buf, buf_len, "%s", scale_mode_to_string(inst->scale_mode));
    if (strcmp(key, "held_order") == 0) return snprintf(buf, buf_len, "%s", held_order_to_string(inst->held_order));
    if (strcmp(key, "held_order_seed") == 0) return snprintf(buf, buf_len, "%d", inst->held_order_seed);
    if (strcmp(key, "play_mode") == 0) return snprintf(buf, buf_len, "%s", inst->latch ? "latch" : "hold");
    if (strcmp(key, "retrigger_mode") == 0) return snprintf(buf, buf_len, "%s", inst->retrigger_mode == RETRIG_CONT ? "cont" : "restart");
    if (strcmp(key, "rate") == 0) return snprintf(buf, buf_len, "%s", rate_to_string(inst->rate));
    if (strcmp(key, "rand_cycle") == 0) return snprintf(buf, buf_len, "%d", inst->rand_cycle);
    if (strcmp(key, "global_velocity") == 0) return snprintf(buf, buf_len, "%d", inst->global_velocity);
    if (strcmp(key, "global_gate") == 0) return snprintf(buf, buf_len, "%d", inst->global_gate);
    if (strcmp(key, "global_v_rnd") == 0) return snprintf(buf, buf_len, "%d", inst->global_v_rnd);
    if (strcmp(key, "global_g_rnd") == 0) return snprintf(buf, buf_len, "%d", inst->global_g_rnd);
    if (strcmp(key, "global_rnd_seed") == 0) return snprintf(buf, buf_len, "%d", inst->global_rnd_seed);
    if (strcmp(key, "sync") == 0) return snprintf(buf, buf_len, "%s", sync_to_string(inst->sync_mode));
    if (strcmp(key, "bpm") == 0) return snprintf(buf, buf_len, "%d", inst->bpm);
    if (strcmp(key, "swing") == 0) return snprintf(buf, buf_len, "%d", inst->swing);
    if (strcmp(key, "max_voices") == 0) return snprintf(buf, buf_len, "%d", inst->max_voices);

    if (strcmp(key, "name") == 0) return snprintf(buf, buf_len, "Eucalypso");
    if (strcmp(key, "bank_name") == 0) return snprintf(buf, buf_len, "Factory");
    if (strcmp(key, "chain_params") == 0) {
        if (inst->chain_params_len > 0) return snprintf(buf, buf_len, "%s", inst->chain_params_json);
        return -1;
    }
    if (strcmp(key, "state") == 0) {
        return build_state_json(inst, buf, buf_len);
    }

    return -1;
}

static void *eucalypso_create_instance(const char *module_dir, const char *config_json) {
    eucalypso_instance_t *inst;
    int i;
    (void)config_json;
    inst = (eucalypso_instance_t *)calloc(1, sizeof(eucalypso_instance_t));
    if (!inst) return NULL;

    inst->rate = RATE_1_16;
    inst->sync_mode = SYNC_INTERNAL;
    inst->register_mode = REG_HELD;
    inst->held_order = ORDER_UP;
    inst->scale_mode = SCALE_MAJOR;
    inst->retrigger_mode = RETRIG_RESTART;
    inst->root_note = 0;
    inst->octave = 0;
    inst->scale_range = MAX_POOL_NOTES;
    inst->held_order_seed = 1;
    inst->rand_cycle = 16;
    inst->global_velocity = 100;
    inst->global_gate = 80;
    inst->global_v_rnd = 0;
    inst->global_g_rnd = 0;
    inst->global_rnd_seed = 1;
    inst->bpm = DEFAULT_BPM;
    inst->swing = 0;
    inst->latch = 0;
    inst->max_voices = 8;

    for (i = 0; i < LANE_COUNT; i++) {
        lane_state_t *lane = &inst->lanes[i];
        lane->enabled = (i == 0) ? 1 : 0;
        lane->steps = 16;
        lane->pulses = 4;
        lane->rotation = 0;
        lane->note_step = i + 1;
        lane->octave = 0;
        lane->note_rnd = 0;
        lane->seed = 1;
        lane->velocity = 0;
        lane->gate = 0;
        lane->mod_len = 0;
        lane->drop = 0;
        lane->drop_seed = 1;
        lane->swap = 0;
        lane->swap_seed = 1;
        lane->oct = 0;
        lane->oct_seed = 1;
        lane->oct_rng = OCT_RAND_PM1;
        lane->vel_rnd = 0;
        lane->vel_seed = 1;
        lane->gate_rnd = 0;
        lane->gate_seed = 1;
        lane->time_rnd = 0;
        lane->time_seed = 1;
        lane->step_cursor = 0;
    }

    inst->sample_rate = 0;
    inst->timing_dirty = 1;
    inst->step_interval_base = 1;
    inst->samples_until_step = 0;
    inst->step_interval_base_f = 1.0;
    inst->samples_until_step_f = 0.0;
    inst->internal_sample_total = 0;
    inst->swing_phase = 0;
    inst->clock_counter = 0;
    inst->clocks_per_step = 6;
    inst->clock_running = 1;
    inst->clock_tick_total = 0;
    inst->pending_step_triggers = 0;
    inst->delayed_step_triggers = 0;
    inst->midi_transport_active = 0;
    inst->global_step_index = 0;
    inst->voice_count = 0;

    inst->chain_params_json[0] = '\0';
    inst->chain_params_len = 0;
    cache_chain_params_from_module_json(inst, module_dir);
    recalc_clock_timing(inst);
    update_phrase_running(inst);
    return inst;
}

static void eucalypso_destroy_instance(void *instance) {
    eucalypso_instance_t *inst = (eucalypso_instance_t *)instance;
    if (!inst) return;
    free(inst);
}

static int eucalypso_process_midi(void *instance, const uint8_t *in_msg, int in_len,
                                  uint8_t out_msgs[][3], int out_lens[], int max_out) {
    eucalypso_instance_t *inst = (eucalypso_instance_t *)instance;
    int count = 0;
    int live_before = 0;
    uint8_t status;
    uint8_t type;
    if (!inst || !in_msg || in_len < 1) return 0;
    status = in_msg[0];
    type = status & 0xF0;

    if (inst->sync_mode == SYNC_CLOCK) {
        if (status == 0xFA) {
            inst->midi_transport_active = 1;
            inst->clock_running = 1;
            inst->clock_counter = 0;
            inst->clock_tick_total = 0;
            inst->pending_step_triggers = 0;
            inst->delayed_step_triggers = 0;
            reset_phrase(inst);
            return 0;
        }
        if (status == 0xFB) {
            inst->midi_transport_active = 1;
            inst->clock_running = 1;
            inst->pending_step_triggers = 0;
            inst->delayed_step_triggers = 0;
            return 0;
        }
        if (status == 0xFC) {
            inst->midi_transport_active = 0;
            inst->clock_running = 0;
            inst->clock_counter = 0;
            inst->pending_step_triggers = 0;
            inst->delayed_step_triggers = 0;
            return handle_transport_stop(inst, out_msgs, out_lens, max_out);
        }
        if (status == 0xF8) {
            if (!inst->clock_running) return 0;
            return process_clock_tick(inst, out_msgs, out_lens, max_out);
        }
    } else {
        if (status == 0xFA || status == 0xFB) {
            inst->midi_transport_active = 1;
            if (inst->timing_dirty && inst->sample_rate > 0) recalc_timing(inst, inst->sample_rate);
            inst->swing_phase = 0;
            inst->internal_sample_total = 0;
            inst->samples_until_step_f = inst->step_interval_base_f > 0.0 ? inst->step_interval_base_f : 1.0;
            inst->samples_until_step = (int)(inst->samples_until_step_f + 0.5);
            if (inst->samples_until_step < 1) inst->samples_until_step = 1;
            reset_phrase(inst);
            return 0;
        }
        if (status == 0xFC) {
            inst->midi_transport_active = 0;
            return handle_transport_stop(inst, out_msgs, out_lens, max_out);
        }
    }

    if ((type == 0x90 || type == 0x80) && in_len >= 3) {
        uint8_t note = in_msg[1];
        uint8_t vel = in_msg[2];
        live_before = live_note_count(inst);
        if (type == 0x90 && vel > 0) {
            note_on(inst, note, vel);
            /* Note-on can anchor only before MIDI transport has become active. */
            if (inst->sync_mode == SYNC_CLOCK &&
                inst->clock_running &&
                !inst->midi_transport_active &&
                inst->register_mode == REG_HELD &&
                live_before == 0 &&
                live_note_count(inst) > 0 &&
                max_out > 0) {
                return run_step(inst, out_msgs, out_lens, max_out);
            }
            if (inst->sync_mode == SYNC_INTERNAL &&
                inst->register_mode == REG_HELD &&
                !inst->midi_transport_active &&
                live_before == 0 &&
                live_note_count(inst) > 0 &&
                max_out > 0) {
                count = run_step(inst, out_msgs, out_lens, max_out);
                inst->samples_until_step_f = next_step_interval(inst);
                if (inst->samples_until_step_f < 1.0) inst->samples_until_step_f = 1.0;
                inst->samples_until_step = (int)(inst->samples_until_step_f + 0.5);
                if (inst->samples_until_step < 1) inst->samples_until_step = 1;
                return count;
            }
        } else {
            note_off(inst, note);
        }
        return 0;
    }

    if (max_out < 1) return 0;
    out_msgs[0][0] = in_msg[0];
    out_msgs[0][1] = in_len > 1 ? in_msg[1] : 0;
    out_msgs[0][2] = in_len > 2 ? in_msg[2] : 0;
    out_lens[0] = in_len > 3 ? 3 : in_len;
    return 1;
}

static int eucalypso_tick(void *instance, int frames, int sample_rate,
                          uint8_t out_msgs[][3], int out_lens[], int max_out) {
    eucalypso_instance_t *inst = (eucalypso_instance_t *)instance;
    int count = 0;
    int live_count;
    int keep_progressing;
    if (!inst || frames < 0 || max_out < 1) return 0;
    if (inst->timing_dirty || inst->sample_rate != sample_rate) recalc_timing(inst, sample_rate);
    (void)enforce_voice_limit(inst, out_msgs, out_lens, max_out, &count);
    if (count >= max_out) return count;

    if (inst->sync_mode == SYNC_INTERNAL) {
        (void)advance_voice_timers_samples(inst, frames, out_msgs, out_lens, max_out, &count);
        if (count >= max_out) return count;
    }

    live_count = live_note_count(inst);
    if (inst->register_mode == REG_SCALE) live_count = (any_enabled_lane(inst) && live_note_count(inst) > 0) ? 1 : 0;
    keep_progressing = (inst->retrigger_mode == RETRIG_CONT);

    if (live_count == 0) {
        if (inst->voice_count > 0) {
            (void)flush_all_voices(inst, out_msgs, out_lens, max_out, &count);
        }
        if (inst->sync_mode == SYNC_CLOCK && !keep_progressing) {
            inst->pending_step_triggers = 0;
            inst->delayed_step_triggers = 0;
        }
        if (!inst->latch) {
            clear_active(inst);
            inst->note_set_dirty = 0;
        }
        inst->phrase_running = 0;
        if (!keep_progressing && inst->register_mode == REG_HELD) return count;
    }

    update_phrase_running(inst);

    if (inst->sync_mode == SYNC_CLOCK) {
        if (inst->pending_step_triggers > MAX_PENDING_STEP_TRIGGERS) {
            inst->pending_step_triggers = MAX_PENDING_STEP_TRIGGERS;
        }
        while (inst->pending_step_triggers > 0 && count < max_out) {
            count += run_step(inst, out_msgs + count, out_lens + count, max_out - count);
            inst->pending_step_triggers--;
        }
        return count;
    }

    inst->internal_sample_total += (uint64_t)frames;
    inst->samples_until_step_f -= (double)frames;
    while (inst->samples_until_step_f <= 0.0 && count < max_out) {
        count += run_step(inst, out_msgs + count, out_lens + count, max_out - count);
        inst->samples_until_step_f += next_step_interval(inst);
        if (inst->samples_until_step_f < 1.0) inst->samples_until_step_f = 1.0;
    }
    inst->samples_until_step = (int)(inst->samples_until_step_f + 0.5);
    if (inst->samples_until_step < 1) inst->samples_until_step = 1;
    return count;
}

static midi_fx_api_v1_t g_api = {
    .api_version = MIDI_FX_API_VERSION,
    .create_instance = eucalypso_create_instance,
    .destroy_instance = eucalypso_destroy_instance,
    .process_midi = eucalypso_process_midi,
    .tick = eucalypso_tick,
    .set_param = eucalypso_set_param,
    .get_param = eucalypso_get_param
};

midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
