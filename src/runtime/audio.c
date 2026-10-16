#include "p8p/audio.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
#define P8P_STATIC_ASSERT static_assert
#else
#define P8P_STATIC_ASSERT _Static_assert
#endif

#define P8P_AUDIO_CHANNELS 4
/* Output rate.  Voices are synthesized at half of it, close to PICO-8's own
 * 22050 Hz, and each synthesized sample is interpolated to two outputs; this
 * halves the per-channel work. */
#define P8P_AUDIO_RATE 48000u
#define P8P_SYNTH_RATE 24000u
#define P8P_SFX_BASE 0x3200u
#define P8P_MUSIC_BASE 0x3100u
#define P8P_SFX_BYTES 68u

typedef struct p8p_audio_channel {
    int sfx;
    int end_note;
    uint8_t note;
    uint8_t is_music;
    uint8_t can_loop;
    uint8_t waveform;
    uint8_t effect;
    uint8_t key;
    uint8_t previous_key;
    uint8_t volume;
    uint8_t previous_volume;
    uint32_t phase;
    uint32_t noise;
    uint32_t note_samples;
    uint32_t sample_in_note;
    int32_t increment;
    int32_t increment_step;
    int32_t volume_q16;
    int32_t volume_step;
    uint32_t vibrato_phase;
} p8p_audio_channel_t;

/* PCM samples buffered from serial(0x808), about 0.74 s at 5512.5 Hz. */
#define P8P_PCM_CAPACITY 4096u
/* 5512.5 / 24000 as a 32-bit phase step. */
#define P8P_PCM_STEP 986501120u

/* PICO-8 reverb delays are 366 and 732 samples at 22050 Hz. */
#define P8P_REVERB_SHORT 398
#define P8P_REVERB_LONG 797

typedef struct p8p_biquad {
    float x1, x2, y1, y2;
} p8p_biquad_t;

/*
 * Per-channel state added for custom instruments and SFX filters.  It is
 * deliberately kept out of saved states (p8p_audio_state_size() stops before
 * it) so existing state files keep loading; after a load an instrument simply
 * restarts on its parent's next note and filter memory starts silent.
 */
typedef struct p8p_audio_extra {
    p8p_audio_channel_t instrument;  /* SFX 0-7 played as an instrument */
    int seen_sfx;
    int seen_note;
    uint8_t parent_custom;
    uint8_t last_instrument;
    uint8_t last_key;
    uint8_t odd_period;              /* saw buzz alternates whole periods */
    int32_t factor_increment;        /* parent pitch the cached factor is for */
    uint32_t factor_q16;
    uint32_t detune_phase;
    int32_t noise_sample;
    uint32_t reverb_index;
    int16_t reverb_short[P8P_REVERB_SHORT];
    int16_t reverb_long[P8P_REVERB_LONG];
    p8p_biquad_t damp1;
    p8p_biquad_t damp2;
} p8p_audio_extra_t;

struct p8p_audio {
    uint8_t *ram;
    p8p_audio_channel_t channels[P8P_AUDIO_CHANNELS];
    int music_pattern;
    int music_count;
    uint8_t music_mask;
    uint32_t music_samples_remaining;
    int32_t music_volume_q24;
    int32_t music_fade_step;
    /* Not serialized; see p8p_audio_extra_t. */
    p8p_audio_extra_t extra[P8P_AUDIO_CHANNELS];
    /* PCM stream from serial(0x808); also not serialized. */
    uint8_t pcm[P8P_PCM_CAPACITY];
    uint32_t pcm_read;
    uint32_t pcm_count;
    uint32_t pcm_phase;
    /* Output interpolation: the last synthesized sample, and whether the
     * next output frame is the second of its pair.  Not serialized. */
    int32_t held_sample;
    uint8_t second_output;
};

/* 440 * 2^((key - 33) / 12), converted to a 32-bit phase step at 48 kHz;
 * NOTE_INCREMENT doubles it for the 24 kHz synthesis rate. */
static const uint32_t note_increment[64] = {
    5852465u, 6200470u, 6569170u, 6959793u, 7373644u, 7812103u, 8276635u, 8768789u,
    9290209u, 9842633u, 10427907u, 11047982u, 11704930u, 12400941u, 13138339u, 13919586u,
    14747287u, 15624207u, 16553270u, 17537579u, 18580418u, 19685267u, 20855814u, 22095965u,
    23409859u, 24801882u, 26276679u, 27839171u, 29494575u, 31248413u, 33106541u, 35075158u,
    37160835u, 39370534u, 41711627u, 44191930u, 46819719u, 49603764u, 52553357u, 55678342u,
    58989149u, 62496826u, 66213081u, 70150316u, 74321671u, 78741067u, 83423255u, 88383859u,
    93639437u, 99207528u, 105106715u, 111356685u, 117978298u, 124993653u, 132426162u, 140300631u,
    148643341u, 157482134u, 166846509u, 176767719u, 187278874u, 198415056u, 210213429u, 222713370u
};

#define NOTE_INCREMENT(key) ((int32_t)(note_increment[key] << 1))

static uint8_t *sfx_data(p8p_audio_t *audio, int sfx) {
    return audio->ram + P8P_SFX_BASE + (unsigned)sfx * P8P_SFX_BYTES;
}

static uint32_t samples_per_note(uint8_t speed) {
    uint32_t actual_speed = speed ? speed : 1u;
    return (actual_speed * 183u * P8P_SYNTH_RATE + 11025u) / 22050u;
}

static int last_audible_note(const uint8_t *sfx) {
    int last = 0;
    for (int note = 0; note < 32; ++note)
        if ((sfx[note * 2 + 1] >> 1) & 7)
            last = note + 1;
    return last;
}

static void stop_channel(p8p_audio_channel_t *channel) {
    channel->sfx = -1;
    channel->volume_q16 = 0;
    channel->volume_step = 0;
}

static void configure_note(p8p_audio_t *audio, p8p_audio_channel_t *channel) {
    uint8_t *sfx;
    uint8_t low;
    uint8_t high;
    int32_t target_increment;
    int32_t target_volume;

    if (channel->sfx < 0 || channel->note >= channel->end_note) {
        stop_channel(channel);
        return;
    }
    sfx = sfx_data(audio, channel->sfx);
    low = sfx[channel->note * 2];
    high = sfx[channel->note * 2 + 1];
    channel->key = low & 0x3f;
    channel->waveform = (uint8_t)(((low >> 6) | ((high & 1) << 2)) & 7);
    channel->volume = (high >> 1) & 7;
    channel->effect = (high >> 4) & 7;
    channel->note_samples = samples_per_note(sfx[65]);
    channel->sample_in_note = 0;
    target_increment = NOTE_INCREMENT(channel->key);
    target_volume = (int32_t)channel->volume << 16;
    channel->increment_step = 0;
    channel->volume_step = 0;

    switch (channel->effect) {
    case 1: /* slide from the previous note */
        channel->increment = NOTE_INCREMENT(channel->previous_key);
        channel->increment_step =
            (target_increment - channel->increment) / (int32_t)channel->note_samples;
        channel->volume_q16 = channel->previous_volume ?
            (int32_t)channel->previous_volume << 16 : target_volume;
        channel->volume_step =
            (target_volume - channel->volume_q16) / (int32_t)channel->note_samples;
        break;
    case 3: /* drop */
        channel->increment = target_increment;
        channel->increment_step = -target_increment / (int32_t)channel->note_samples;
        channel->volume_q16 = target_volume;
        break;
    case 4: /* fade in */
        channel->increment = target_increment;
        channel->volume_q16 = 0;
        channel->volume_step = target_volume / (int32_t)channel->note_samples;
        break;
    case 5: /* fade out */
        channel->increment = target_increment;
        channel->volume_q16 = target_volume;
        channel->volume_step = -target_volume / (int32_t)channel->note_samples;
        break;
    default:
        channel->increment = target_increment;
        channel->volume_q16 = target_volume;
        break;
    }
}

static void advance_note(p8p_audio_t *audio, p8p_audio_channel_t *channel) {
    uint8_t *sfx;
    uint8_t loop_start;
    uint8_t loop_end;

    if (channel->sfx < 0)
        return;
    channel->previous_key = channel->key;
    channel->previous_volume = channel->volume;
    ++channel->note;
    sfx = sfx_data(audio, channel->sfx);
    loop_start = sfx[66];
    loop_end = sfx[67];
    if (channel->can_loop && loop_end > loop_start && channel->note >= loop_end)
        channel->note = loop_start;
    if (channel->note >= channel->end_note) {
        stop_channel(channel);
        return;
    }
    configure_note(audio, channel);
}

static void launch_sfx(p8p_audio_t *audio, int sfx_index, int channel_index,
                       int offset, int length, int is_music) {
    p8p_audio_channel_t *channel = &audio->channels[channel_index];
    uint8_t *sfx = sfx_data(audio, sfx_index);
    int end_note = 32;

    if (length > 0 && !is_music && offset + length < end_note)
        end_note = offset + length;
    if (!is_music && sfx[67] <= sfx[66]) {
        int audible_end = last_audible_note(sfx);
        if (sfx[67] == 0 && sfx[66] > 0 && sfx[66] < end_note)
            end_note = sfx[66];
        else if (audible_end < end_note)
            end_note = audible_end;
    }

    memset(channel, 0, sizeof(*channel));
    channel->sfx = sfx_index;
    channel->note = (uint8_t)(offset < 0 ? 0 : offset);
    channel->end_note = end_note;
    channel->is_music = (uint8_t)is_music;
    channel->can_loop = 1;
    channel->previous_key = 24;
    channel->noise = 0x9e3779b9u ^ (uint32_t)(channel_index * 0x10203u);
    configure_note(audio, channel);
}

static uint32_t pattern_duration_samples(p8p_audio_t *audio, int pattern) {
    const uint8_t *song = audio->ram + P8P_MUSIC_BASE + (unsigned)pattern * 4u;
    int duration_looping = -1;
    int duration_nonlooping = -1;

    for (int channel = 0; channel < 4; ++channel) {
        int index = song[channel] & 0x7f;
        uint8_t *sfx;
        int end_note;
        int duration;
        if (index & 0x40)
            continue;
        sfx = sfx_data(audio, index & 0x3f);
        if (sfx[67] > sfx[66]) {
            duration = 32 * (sfx[65] ? sfx[65] : 1);
            if (duration > duration_looping) duration_looping = duration;
        } else {
            end_note = (sfx[67] == 0 && sfx[66] > 0) ? sfx[66] : 32;
            duration_nonlooping = end_note * (sfx[65] ? sfx[65] : 1);
            break;
        }
    }
    int units = duration_nonlooping > 0 ? duration_nonlooping : duration_looping;
    if (units <= 0) units = 32;
    return (uint32_t)(((uint64_t)(unsigned)units * 183u * P8P_SYNTH_RATE + 11025u) /
                      22050u);
}

static void start_music_pattern(p8p_audio_t *audio, int pattern) {
    const uint8_t *song;

    for (int channel = 0; channel < 4; ++channel)
        if (audio->channels[channel].is_music)
            stop_channel(&audio->channels[channel]);
    if (pattern < 0 || pattern > 63) {
        audio->music_pattern = -1;
        audio->music_samples_remaining = 0;
        return;
    }

    audio->music_pattern = pattern;
    audio->music_samples_remaining = pattern_duration_samples(audio, pattern);
    song = audio->ram + P8P_MUSIC_BASE + (unsigned)pattern * 4u;
    for (int channel = 0; channel < 4; ++channel) {
        int sfx = song[channel] & 0x7f;
        if (!(sfx & 0x40) && !(audio->music_mask & (1u << channel)))
            launch_sfx(audio, sfx & 0x3f, channel, 0, 0, 1);
    }
}

static void advance_music(p8p_audio_t *audio) {
    const uint8_t *song;
    int next;

    if (audio->music_pattern < 0)
        return;
    song = audio->ram + P8P_MUSIC_BASE + (unsigned)audio->music_pattern * 4u;
    if (song[2] & 0x80) {
        start_music_pattern(audio, -1);
        audio->music_count = -1;
        return;
    }
    next = audio->music_pattern + 1;
    if (song[1] & 0x80) {
        while (next > 0 && !(audio->ram[P8P_MUSIC_BASE + (unsigned)(next - 1) * 4u] & 0x80))
            --next;
    }
    ++audio->music_count;
    start_music_pattern(audio, next > 63 ? -1 : next);
}

static int32_t triangle(uint32_t phase) {
    uint32_t position = phase >> 16;
    int32_t ramp = position < 32768u ? (int32_t)position : (int32_t)(65535u - position);
    return (ramp - 16384) >> 1;
}

/* Stateless waveforms 0-5 and 7 (noise needs per-voice state). */
static inline int32_t basic_waveform(int waveform, uint32_t phase) {
    int32_t sample;
    switch (waveform) {
    case 0: /* triangle */
        return triangle(phase);
    case 1: /* tilted saw (integer approximation) */
        sample = (int32_t)(phase >> 16) - 32768;
        return sample < 24576 ? sample >> 2 : (32767 - sample) >> 1;
    case 2: /* saw */
        return (int16_t)(phase >> 16) >> 2;
    case 3: /* square */
        return (phase & 0x80000000u) ? -8192 : 8192;
    case 4: /* pulse */
        return (phase < 0x51000000u) ? 8192 : -8192;
    case 5: /* organ */
        return (triangle(phase) + triangle(phase * 2u) / 2) * 2 / 3;
    case 7: /* phaser */
        return (triangle(phase) + triangle(phase - phase / 110u)) / 2;
    default:
        return 0;
    }
}

/*
 * The "buzz" SFX filter reshapes each waveform.  Formulas from zepto8 via
 * Fake-08 (WTFPL), measured from PICO-8 exports; they run in float on the
 * Pocket's FPU and only for SFX that set the filter.  Each result is scaled
 * to the amplitude of this engine's plain waveform of the same kind.
 */
static int32_t buzz_waveform(int waveform, uint32_t phase, int odd_period) {
    float t = (float)phase * (1.0f / 4294967296.0f);
    float ret;
    switch (waveform) {
    case 0: {
        float a = 0.875f;
        float tilted = t < a ? 2.f * t / a - 1.f : 2.f * (1.f - t) / (1.f - a) - 1.f;
        ret = (1.0f - fabsf(4.f * t - 2.0f)) * 0.75f + tilted * 0.25f;
        return (int32_t)(ret * 0.5f * 16384.f);
    }
    case 1: {
        float a = 0.975f;
        ret = t < a ? 2.f * t / a - 1.f : 2.f * (1.f - t) / (1.f - a) - 1.f;
        return (int32_t)(ret * 0.5f * 16384.f);
    }
    case 2: {
        float advance = t + (odd_period ? 1.f : 0.f);
        ret = t < 0.5f ? t : t - 1.f;
        ret = ret * 0.83f - (fabsf(advance - 1.0f) < 0.5f ? 0.085f : 0.0f);
        return (int32_t)(0.653f * ret * 25090.f);
    }
    case 3:
        return t < 0.4f ? 8192 : -8192;
    case 4:
        return t < 0.255f ? 8192 : -8192;
    case 5:
        ret = t < 0.5f ? 3.f - fabsf(24.f * t - 6.f) : 1.f - fabsf(16.f * t - 12.f);
        ret = t < 0.5f ? ret * 2.0f + 3.0f : ret;
        ret = (t < 0.5f && ret > -1.875f) ? ret * 0.2f - 1.0f : ret + 0.5f;
        return (int32_t)(ret / 9.f * 24576.f);
    case 7: {
        float t2 = fmodf(t * 109.f / 110.f + (odd_period ? 109.f / 110.f : 0.f), 1.f);
        ret = 2.f - fabsf(8.f * t - 4.f);
        ret += 1.f - fabsf(4.f * t2 - 2.f);
        ret += 0.25f - fabsf(fmodf(t * 2.0f + 0.5f, 1.f) - 0.5f);
        ret += 0.125f - fabsf(0.5f * fmodf(t * 4.0f, 1.f) - 0.25f);
        return (int32_t)(ret / 6.f * 16384.f);
    }
    default:
        return basic_waveform(waveform, phase);
    }
}

static inline int32_t waveform_sample(p8p_audio_channel_t *channel, int32_t increment) {
    uint32_t phase = channel->phase;
    uint32_t next_phase = phase + (uint32_t)(increment > 0 ? increment : 0);
    channel->phase = next_phase;
    if (channel->waveform == 6) {
        /* Hold a pseudo-random value for one pitch period. */
        if (next_phase < phase) {
            channel->noise ^= channel->noise << 13;
            channel->noise ^= channel->noise >> 17;
            channel->noise ^= channel->noise << 5;
        }
        return (int16_t)(channel->noise >> 16) >> 2;
    }
    return basic_waveform(channel->waveform, phase);
}

/* Detune adds a second oscillator at half level; ratios from Fake-08. */
static uint32_t detune_factor_q16(int waveform, int detune) {
    switch (waveform) {
    case 0: return detune == 1 ? 49152u : 98304u;      /* 3/4, 3/2 */
    case 5: return detune == 1 ? 65865u : 263467u;     /* 200/199, 800/199 */
    case 7: return detune == 1 ? 64225u : 131731u;     /* 49/50, 400/199 */
    default: return detune == 1 ? 65865u : 131731u;    /* 200/199, 400/199 */
    }
}

/* One voice's sample including the buzz, noiz and detune filters. */
static inline int32_t voice_sample(p8p_audio_extra_t *extra,
                            p8p_audio_channel_t *voice, int32_t increment,
                            uint8_t filters) {
    uint32_t phase = voice->phase;
    int buzz = (filters & 4) != 0;
    int noiz = (filters & 2) != 0;
    int detune = (filters / 8) % 3;
    int32_t sample;
    if (buzz && voice->waveform != 6) {
        uint32_t next_phase = phase + (uint32_t)(increment > 0 ? increment : 0);
        voice->phase = next_phase;
        sample = buzz_waveform(voice->waveform, phase, extra->odd_period);
        if (next_phase < phase)
            extra->odd_period ^= 1;
    } else {
        sample = waveform_sample(voice, increment);
        if (noiz && voice->waveform == 6) {
            /* Sounds a bit like a saw tooth: scale by 2*(t or t-1). */
            int32_t saw = (int32_t)(phase >> 16);
            if (saw >= 32768) saw -= 65536;
            sample = (int32_t)(((int64_t)sample * saw) >> 15);
        }
    }
    if (detune && voice->waveform != 6) {
        int second_wave = (detune == 2 && voice->waveform == 5) ? 0 : voice->waveform;
        uint32_t factor = detune_factor_q16(voice->waveform, detune);
        uint32_t step = (uint32_t)(((uint64_t)(uint32_t)(increment > 0 ? increment : 0) *
                                    factor) >> 16);
        sample += basic_waveform(second_wave, extra->detune_phase) / 2;
        extra->detune_phase += step;
    }
    return sample;
}

/* Instantaneous pitch step of a voice, applying vibrato and arpeggios. */
static inline int32_t voice_increment(p8p_audio_t *audio, p8p_audio_channel_t *voice) {
    int32_t increment = voice->increment;
    if (voice->effect == 2) {
        int32_t lfo = triangle(voice->vibrato_phase);
        voice->vibrato_phase += 1342177u; /* 7.5 Hz */
        increment += (int32_t)(((int64_t)increment * lfo) >> 19);
    } else if (voice->effect == 6 || voice->effect == 7) {
        uint8_t *sfx = sfx_data(audio, voice->sfx);
        int rate = (sfx[65] <= 8 ? 2 : 1) * (voice->effect == 6 ? 30 : 15);
        int arp = (int)(voice->sample_in_note /
                        (P8P_SYNTH_RATE / (unsigned)rate)) & 3;
        int note = (voice->note & ~3) | arp;
        uint8_t key = sfx[note * 2] & 0x3f;
        increment = NOTE_INCREMENT(key);
    }
    return increment;
}

static inline void voice_step(p8p_audio_t *audio, p8p_audio_channel_t *voice) {
    voice->increment += voice->increment_step;
    voice->volume_q16 += voice->volume_step;
    if (++voice->sample_in_note >= voice->note_samples)
        advance_note(audio, voice);
}

/*
 * Custom instruments: a note whose custom bit is set plays SFX 0-7 at the
 * instrument's own speed, loops and effects, transposed by the note's pitch
 * relative to C-2.  As in Fake-08 it restarts when the parent's instrument or
 * key changes, when the parent loops or relaunches, or when the instrument
 * has finished.
 */
static void parent_note_started(p8p_audio_t *audio, int index) {
    p8p_audio_channel_t *channel = &audio->channels[index];
    p8p_audio_extra_t *extra = &audio->extra[index];
    uint8_t *sfx = sfx_data(audio, channel->sfx);
    int custom = (sfx[channel->note * 2 + 1] & 0x80) != 0;
    int restart = channel->waveform != extra->last_instrument ||
                  channel->key != extra->last_key ||
                  channel->sfx != extra->seen_sfx ||
                  channel->note <= extra->seen_note ||
                  extra->instrument.sfx < 0;
    extra->last_instrument = channel->waveform;
    extra->last_key = channel->key;
    extra->seen_sfx = channel->sfx;
    extra->seen_note = channel->note;
    extra->parent_custom = (uint8_t)(custom && channel->volume > 0);
    if (extra->parent_custom && restart) {
        p8p_audio_channel_t *voice = &extra->instrument;
        memset(voice, 0, sizeof(*voice));
        voice->sfx = channel->waveform;
        voice->end_note = 32;
        voice->can_loop = 1;
        voice->previous_key = 24;
        voice->noise = 0x6a09e667u ^ (uint32_t)(index * 0x10203u);
        configure_note(audio, voice);
    }
}

/* Dampen filters: high shelves at 2400 Hz/-6 dB and 1000 Hz/-12 dB. */
typedef struct p8p_biquad_coefficients {
    float b0, b1, b2, a1, a2;
} p8p_biquad_coefficients_t;

static p8p_biquad_coefficients_t damp_coefficients[2];
static int damp_ready;

static void high_shelf(p8p_biquad_coefficients_t *c, float frequency, float gain) {
    float w0 = 6.2831853f * frequency / (float)P8P_SYNTH_RATE;
    float cosw = cosf(w0);
    float a = powf(10.0f, gain / 40.0f);
    float alpha = sinf(w0) / 2.0f * sqrtf(2.0f);
    float sqa = 2.0f * sqrtf(a) * alpha;
    float a0 = (a + 1) - (a - 1) * cosw + sqa;
    c->b0 = a * ((a + 1) + (a - 1) * cosw + sqa) / a0;
    c->b1 = -2 * a * ((a - 1) + (a + 1) * cosw) / a0;
    c->b2 = a * ((a + 1) + (a - 1) * cosw - sqa) / a0;
    c->a1 = 2 * ((a - 1) - (a + 1) * cosw) / a0;
    c->a2 = ((a + 1) - (a - 1) * cosw - sqa) / a0;
}

static int32_t run_biquad(p8p_biquad_t *state, const p8p_biquad_coefficients_t *c,
                          int32_t input) {
    float x = (float)input;
    float y = c->b0 * x + c->b1 * state->x1 + c->b2 * state->x2 -
              c->a1 * state->y1 - c->a2 * state->y2;
    state->x2 = state->x1;
    state->x1 = x;
    state->y2 = state->y1;
    state->y1 = y;
    return (int32_t)y;
}

/* Reverb and dampen for one channel's contribution to the mix. */
static int32_t channel_effects(p8p_audio_t *audio, int index, int32_t value,
                               uint8_t filters) {
    p8p_audio_extra_t *extra = &audio->extra[index];
    int reverb = (filters / 24) % 3;
    int dampen = (filters / 72) % 3;
    uint8_t hw_reverb = audio->ram[0x5f41];
    uint8_t hw_lowpass = audio->ram[0x5f43];
    int reverb_short = reverb == 1 || (hw_reverb & (1u << (index + 4)));
    int reverb_long = reverb == 2 || (hw_reverb & (1u << index));
    int damp1 = dampen == 1 || (hw_lowpass & (1u << (index + 4)));
    int damp2 = dampen == 2 || (hw_lowpass & (1u << index));
    uint32_t short_slot = extra->reverb_index % P8P_REVERB_SHORT;
    uint32_t long_slot = extra->reverb_index % P8P_REVERB_LONG;
    if (reverb_short)
        value += extra->reverb_short[short_slot] / 2;
    if (reverb_long)
        value += extra->reverb_long[long_slot] / 2;
    int32_t stored = value > 32767 ? 32767 : value < -32768 ? -32768 : value;
    extra->reverb_short[short_slot] = (int16_t)stored;
    extra->reverb_long[long_slot] = (int16_t)stored;
    ++extra->reverb_index;
    if (damp1 || damp2) {
        if (!damp_ready) {
            high_shelf(&damp_coefficients[0], 2400.0f, -6.0f);
            high_shelf(&damp_coefficients[1], 1000.0f, -12.0f);
            damp_ready = 1;
        }
        if (damp1)
            value = run_biquad(&extra->damp1, &damp_coefficients[0], value);
        if (damp2)
            value = run_biquad(&extra->damp2, &damp_coefficients[1], value);
    }
    return value;
}

static void reset_extra(p8p_audio_t *audio) {
    memset(audio->extra, 0, sizeof(audio->extra));
    audio->pcm_read = audio->pcm_count = audio->pcm_phase = 0;
    audio->held_sample = 0;
    audio->second_output = 0;
    for (int channel = 0; channel < P8P_AUDIO_CHANNELS; ++channel) {
        audio->extra[channel].instrument.sfx = -1;
        audio->extra[channel].seen_sfx = -1;
        audio->extra[channel].seen_note = -1;
        audio->extra[channel].last_instrument = 0xff;
        audio->extra[channel].last_key = 0xff;
    }
}

p8p_audio_t *p8p_audio_create(uint8_t *ram) {
    p8p_audio_t *audio = (p8p_audio_t *)calloc(1, sizeof(*audio));
    if (audio)
        p8p_audio_reset(audio, ram);
    return audio;
}

void p8p_audio_destroy(p8p_audio_t *audio) {
    free(audio);
}

void p8p_audio_reset(p8p_audio_t *audio, uint8_t *ram) {
    if (!audio)
        return;
    memset(audio, 0, sizeof(*audio));
    audio->ram = ram;
    audio->music_pattern = -1;
    audio->music_count = -1;
    audio->music_volume_q24 = 1 << 24;
    for (int channel = 0; channel < 4; ++channel)
        audio->channels[channel].sfx = -1;
    reset_extra(audio);
}

int p8p_audio_sfx(p8p_audio_t *audio, int sfx, int channel,
                  int offset, int length) {
    if (!audio || sfx < -2 || sfx > 63 || channel < -2 || channel > 3 ||
        offset > 31)
        return -1;
    if (channel == -2) {
        for (int i = 0; i < 4; ++i)
            if (audio->channels[i].sfx == sfx)
                stop_channel(&audio->channels[i]);
        return -1;
    }
    if (sfx == -1 || sfx == -2) {
        for (int i = 0; i < 4; ++i) {
            if (channel >= 0 && i != channel)
                continue;
            if (!audio->channels[i].is_music) {
                if (sfx == -1) stop_channel(&audio->channels[i]);
                else audio->channels[i].can_loop = 0;
            }
        }
        return -1;
    }
    if (channel < 0) {
        for (int i = 0; i < 4; ++i)
            if (!(audio->music_mask & (1u << i)) &&
                (audio->channels[i].sfx < 0 || audio->channels[i].sfx == sfx)) {
                channel = i;
                break;
            }
    }
    if (channel < 0)
        for (int i = 0; i < 4; ++i)
            if (!(audio->music_mask & (1u << i))) {
                channel = i;
                break;
            }
    if (channel < 0)
        return -1;
    for (int i = 0; i < 4; ++i)
        if (i != channel && audio->channels[i].sfx == sfx)
            stop_channel(&audio->channels[i]);
    launch_sfx(audio, sfx, channel, offset < 0 ? 0 : offset, length, 0);
    return channel;
}

void p8p_audio_music(p8p_audio_t *audio, int pattern, int fade_ms, int mask) {
    if (!audio || pattern < -1 || pattern > 63)
        return;
    audio->music_mask = (uint8_t)(mask & 15);
    if (pattern < 0 && fade_ms > 0) {
        uint32_t samples = (uint32_t)fade_ms * (P8P_SYNTH_RATE / 1000u);
        audio->music_fade_step = -(audio->music_volume_q24 / (int32_t)samples);
        if (audio->music_fade_step == 0)
            audio->music_fade_step = -1;
        return;
    }
    if (pattern >= 0 && fade_ms > 0) {
        uint32_t samples = (uint32_t)fade_ms * (P8P_SYNTH_RATE / 1000u);
        audio->music_volume_q24 = 0;
        audio->music_fade_step = (1 << 24) / (int32_t)samples;
        if (audio->music_fade_step == 0)
            audio->music_fade_step = 1;
    } else {
        audio->music_fade_step = 0;
        audio->music_volume_q24 = pattern < 0 ? 0 : 1 << 24;
    }
    audio->music_count = pattern < 0 ? -1 : 0;
    start_music_pattern(audio, pattern);
}

int p8p_audio_channel_sfx(const p8p_audio_t *audio, int channel) {
    if (!audio || channel < 0 || channel >= P8P_AUDIO_CHANNELS)
        return -1;
    return audio->channels[channel].sfx;
}

int p8p_audio_channel_note(const p8p_audio_t *audio, int channel) {
    if (!audio || channel < 0 || channel >= P8P_AUDIO_CHANNELS ||
        audio->channels[channel].sfx < 0)
        return -1;
    return audio->channels[channel].note;
}

int p8p_audio_music_pattern(const p8p_audio_t *audio) {
    return audio ? audio->music_pattern : -1;
}

int p8p_audio_music_count(const p8p_audio_t *audio) {
    return audio ? audio->music_count : -1;
}

int p8p_audio_music_ticks(const p8p_audio_t *audio) {
    uint32_t total, played;
    if (!audio || !audio->ram || audio->music_pattern < 0)
        return -1;
    /* Ticks of 183/22050 s since the pattern started, as stat(26). */
    total = pattern_duration_samples((p8p_audio_t *)audio, audio->music_pattern);
    played = total > audio->music_samples_remaining ?
             total - audio->music_samples_remaining : 0;
    return (int)(((uint64_t)played * 22050u) / (183u * P8P_SYNTH_RATE));
}

/* One mono sample at P8P_SYNTH_RATE: all four channels plus PCM. */
/* Music position and fade, once per synthesized sample. */
static void advance_sequencer(p8p_audio_t *audio) {
    if (audio->music_pattern >= 0) {
        if (audio->music_samples_remaining == 0)
            advance_music(audio);
        if (audio->music_samples_remaining)
            --audio->music_samples_remaining;
    }
    if (audio->music_fade_step) {
        audio->music_volume_q24 += audio->music_fade_step;
        if (audio->music_volume_q24 <= 0) {
            audio->music_volume_q24 = 0;
            audio->music_fade_step = 0;
            start_music_pattern(audio, -1);
        } else if (audio->music_volume_q24 >= (1 << 24)) {
            audio->music_volume_q24 = 1 << 24;
            audio->music_fade_step = 0;
        }
    }
}

static int32_t synthesize(p8p_audio_t *audio) {
    int32_t mix = 0;
    advance_sequencer(audio);
    for (int index = 0; index < 4; ++index) {
        p8p_audio_channel_t *channel = &audio->channels[index];
        p8p_audio_extra_t *extra = &audio->extra[index];
        int32_t increment;
        int32_t sample;
        int32_t volume;
        uint8_t filters;
        if (channel->sfx < 0)
            continue;
        if (channel->sample_in_note == 0)
            parent_note_started(audio, index);
        increment = voice_increment(audio, channel);
        if (extra->parent_custom) {
            p8p_audio_channel_t *voice = &extra->instrument;
            if (voice->sfx >= 0) {
                /* Transpose by the parent pitch relative to C-2:
                 * 2^40 / note_increment[24] = 46969 (Q24 reciprocal),
                 * one more shift for the doubled NOTE_INCREMENT. */
                if (increment != extra->factor_increment) {
                    extra->factor_increment = increment;
                    extra->factor_q16 = (uint32_t)(
                        ((uint64_t)(uint32_t)(increment > 0 ? increment : 0) *
                         46969u) >> 25);
                }
                int32_t voice_inc = voice_increment(audio, voice);
                int32_t scaled = (int32_t)(((int64_t)voice_inc *
                                            extra->factor_q16) >> 16);
                filters = sfx_data(audio, voice->sfx)[64];
                sample = voice_sample(extra, voice, scaled, filters);
                /* q16 x q16 / 7 without a division: both volumes are
                 * at most 7 << 16, so the >> 8 product fits 32 bits;
                 * 9363/65536 ~ 1/7. */
                uint32_t product = (uint32_t)(voice->volume_q16 >> 8) *
                                   (uint32_t)(channel->volume_q16 >> 8);
                volume = (int32_t)(((uint64_t)product * 9363u) >> 16);
                voice_step(audio, voice);
            } else {
                filters = 0;  /* instrument finished: silent */
                sample = 0;
                volume = 0;
            }
        } else {
            filters = sfx_data(audio, channel->sfx)[64];
            sample = voice_sample(extra, channel, increment, filters);
            volume = channel->volume_q16;
        }
        if (channel->is_music)
            volume = (volume >> 8) * (audio->music_volume_q24 >> 16);
        int32_t value = sample * ((volume + 4096) >> 13) / 56;
        if (filters >= 24 || audio->ram[0x5f41] || audio->ram[0x5f43])
            value = channel_effects(audio, index, value, filters);
        mix += value;
        voice_step(audio, channel);
    }
    if (audio->pcm_count) {
        /* 8-bit unsigned PCM, held for each 5512.5 Hz sample period. */
        mix += ((int32_t)audio->pcm[audio->pcm_read] - 128) * 64;
        uint32_t next_phase = audio->pcm_phase + P8P_PCM_STEP;
        if (next_phase < audio->pcm_phase) {
            audio->pcm_read = (audio->pcm_read + 1) % P8P_PCM_CAPACITY;
            --audio->pcm_count;
        }
        audio->pcm_phase = next_phase;
    }
    if (mix > 32767) mix = 32767;
    if (mix < -32768) mix = -32768;
    return mix;
}

/*
 * Mute: advance everything with timing -- music position and fades, notes
 * with their slides and volume envelopes, custom instruments and the PCM
 * stream -- exactly as synthesize() does, but make no sound.  Oscillator
 * and noise phases, vibrato, reverb and filters only shape the waveform and
 * are left alone.
 */
static void silent_sample(p8p_audio_t *audio) {
    advance_sequencer(audio);
    for (int index = 0; index < 4; ++index) {
        p8p_audio_channel_t *channel = &audio->channels[index];
        p8p_audio_extra_t *extra = &audio->extra[index];
        if (channel->sfx < 0)
            continue;
        if (channel->sample_in_note == 0)
            parent_note_started(audio, index);
        if (extra->parent_custom && extra->instrument.sfx >= 0)
            voice_step(audio, &extra->instrument);
        voice_step(audio, channel);
    }
    if (audio->pcm_count) {
        uint32_t next_phase = audio->pcm_phase + P8P_PCM_STEP;
        if (next_phase < audio->pcm_phase) {
            audio->pcm_read = (audio->pcm_read + 1) % P8P_PCM_CAPACITY;
            --audio->pcm_count;
        }
        audio->pcm_phase = next_phase;
    }
}

/* count samples of voice_step() that cannot end the note. */
static void bulk_step(p8p_audio_channel_t *voice, uint32_t count) {
    voice->increment = (int32_t)((uint32_t)voice->increment +
                                 (uint32_t)voice->increment_step * count);
    voice->volume_q16 = (int32_t)((uint32_t)voice->volume_q16 +
                                  (uint32_t)voice->volume_step * count);
    voice->sample_in_note += count;
}

/* Samples of a voice that can be bulk-stepped (0: step it singly). */
static uint32_t bulk_room(const p8p_audio_channel_t *voice) {
    if (voice->sample_in_note == 0 ||
        voice->sample_in_note + 1 >= voice->note_samples)
        return 0;
    return voice->note_samples - voice->sample_in_note - 1;
}

/* silent_sample() samples times, in one step wherever no note, pattern,
 * fade or PCM boundary falls in between. */
static void advance_silently(p8p_audio_t *audio, uint32_t samples) {
    while (samples) {
        uint32_t chunk = samples;
        if (audio->music_fade_step)
            chunk = 0;
        if (audio->music_pattern >= 0 &&
            audio->music_samples_remaining < chunk)
            chunk = audio->music_samples_remaining;
        for (int index = 0; index < 4 && chunk; ++index) {
            const p8p_audio_channel_t *channel = &audio->channels[index];
            const p8p_audio_extra_t *extra = &audio->extra[index];
            uint32_t room;
            if (channel->sfx < 0)
                continue;
            room = bulk_room(channel);
            if (room < chunk) chunk = room;
            if (extra->parent_custom && extra->instrument.sfx >= 0) {
                room = bulk_room(&extra->instrument);
                if (room < chunk) chunk = room;
            }
        }
        if (audio->pcm_count && chunk) {
            /* Stop short of the sample whose phase wraps. */
            uint64_t to_wrap = ((1ull << 32) - audio->pcm_phase +
                                P8P_PCM_STEP - 1) / P8P_PCM_STEP;
            if (to_wrap - 1 < chunk)
                chunk = (uint32_t)(to_wrap - 1);
        }
        if (!chunk) {
            silent_sample(audio);
            --samples;
            continue;
        }
        if (audio->music_pattern >= 0)
            audio->music_samples_remaining -= chunk;
        for (int index = 0; index < 4; ++index) {
            p8p_audio_channel_t *channel = &audio->channels[index];
            p8p_audio_extra_t *extra = &audio->extra[index];
            if (channel->sfx < 0)
                continue;
            if (extra->parent_custom && extra->instrument.sfx >= 0)
                bulk_step(&extra->instrument, chunk);
            bulk_step(channel, chunk);
        }
        if (audio->pcm_count)
            audio->pcm_phase += P8P_PCM_STEP * chunk;
        samples -= chunk;
    }
}

void p8p_audio_skip(p8p_audio_t *audio, size_t frames) {
    size_t samples;
    if (!audio || !audio->ram)
        return;
    /* Output frames come in pairs per synthesized sample (render()). */
    samples = (frames + (audio->second_output ? 0u : 1u)) / 2u;
    if (frames & 1u)
        audio->second_output ^= 1;
    audio->held_sample = 0;
    while (samples) {
        uint32_t part = samples > 0x40000000u ? 0x40000000u : (uint32_t)samples;
        advance_silently(audio, part);
        samples -= part;
    }
}

void p8p_audio_render(p8p_audio_t *audio, int16_t *stereo, size_t frames) {
    if (!stereo)
        return;
    if (!audio || !audio->ram) {
        memset(stereo, 0, frames * 2 * sizeof(*stereo));
        return;
    }

    if (frames && audio->second_output) {
        stereo[0] = stereo[1] = (int16_t)audio->held_sample;
        stereo += 2;
        --frames;
        audio->second_output = 0;
    }

    /* Each synthesis step supplies a midpoint followed by the held sample. */
    int32_t previous = audio->held_sample;
    for (; frames; frames -= 2, stereo += 4) {
        int32_t sample = synthesize(audio);
        int16_t midpoint = (int16_t)((previous + sample) / 2);
        stereo[0] = stereo[1] = midpoint;
        previous = sample;
        if (frames == 1) {
            audio->second_output = 1;
            break;
        }
        stereo[2] = stereo[3] = (int16_t)sample;
    }
    audio->held_sample = previous;
}

/* Layout of struct p8p_audio in 0.0.32, whose tail state files contain. */
typedef struct p8p_audio_v032 {
    uint8_t *ram;
    p8p_audio_channel_t channels[P8P_AUDIO_CHANNELS];
    int music_pattern;
    int music_count;
    uint8_t music_mask;
    uint32_t music_samples_remaining;
    int32_t music_volume_q24;
    int32_t music_fade_step;
} p8p_audio_v032_t;

#define P8P_AUDIO_V032_STATE_SIZE \
    (sizeof(p8p_audio_v032_t) - offsetof(p8p_audio_v032_t, channels))

P8P_STATIC_ASSERT(offsetof(p8p_audio_t, extra) - offsetof(p8p_audio_t, channels) >=
               P8P_AUDIO_V032_STATE_SIZE,
               "saved audio state must not overlap the unsaved extra state");

int p8p_audio_pcm_push(p8p_audio_t *audio, const uint8_t *samples, int count) {
    int queued = 0;
    if (!audio || !samples)
        return 0;
    while (queued < count && audio->pcm_count < P8P_PCM_CAPACITY) {
        uint32_t slot = (audio->pcm_read + audio->pcm_count) % P8P_PCM_CAPACITY;
        audio->pcm[slot] = samples[queued++];
        ++audio->pcm_count;
    }
    return queued;
}

int p8p_audio_pcm_queued(const p8p_audio_t *audio) {
    return audio ? (int)audio->pcm_count : 0;
}

size_t p8p_audio_state_size(void) {
    /* The channels and music state, exactly as 0.0.32 saved them. */
    return P8P_AUDIO_V032_STATE_SIZE;
}

/*
 * Saved states keep 0.0.32's 48 kHz units, so files stay interchangeable
 * with builds that synthesized at the output rate: sample counts are twice
 * the synthesis-rate values, per-sample steps half (pitch steps a quarter,
 * as the pitch itself is halved).
 */
static void state_to_saved_units(p8p_audio_v032_t *state) {
    for (int i = 0; i < P8P_AUDIO_CHANNELS; ++i) {
        p8p_audio_channel_t *c = &state->channels[i];
        c->note_samples *= 2;
        c->sample_in_note *= 2;
        c->increment /= 2;
        c->increment_step /= 4;
        c->volume_step /= 2;
    }
    state->music_samples_remaining *= 2;
    state->music_fade_step /= 2;
}

static void state_from_saved_units(p8p_audio_v032_t *state) {
    for (int i = 0; i < P8P_AUDIO_CHANNELS; ++i) {
        p8p_audio_channel_t *c = &state->channels[i];
        c->note_samples = (c->note_samples + 1) / 2;
        c->sample_in_note /= 2;
        c->increment *= 2;
        c->increment_step *= 4;
        c->volume_step *= 2;
    }
    state->music_samples_remaining = (state->music_samples_remaining + 1) / 2;
    state->music_fade_step *= 2;
}

int p8p_audio_save_state(const p8p_audio_t *audio, void *destination,
                         size_t size) {
    size_t expected = p8p_audio_state_size();
    p8p_audio_v032_t state;
    if (!audio || !destination || size != expected)
        return -1;
    memcpy(&state.channels, &audio->channels, expected);
    state_to_saved_units(&state);
    memcpy(destination, &state.channels, expected);
    return 0;
}

int p8p_audio_load_state(p8p_audio_t *audio, uint8_t *ram,
                         const void *source, size_t size) {
    size_t expected = p8p_audio_state_size();
    if (!audio || !ram || !source || size != expected)
        return -1;
    p8p_audio_v032_t state;
    memcpy(&state.channels, source, expected);
    state_from_saved_units(&state);
    memcpy(&audio->channels, &state.channels, expected);
    audio->ram = ram;
    reset_extra(audio);
    return 0;
}
