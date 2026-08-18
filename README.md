# ESPHome Sound Level Meter [![CI](https://github.com/stas-sl/esphome-sound-level-meter/actions/workflows/ci.yaml/badge.svg)](https://github.com/stas-sl/esphome-sound-level-meter/actions/workflows/ci.yaml)

> [!NOTE]
> This component was originally developed a few years ago, back when ESPHome didn’t yet offer official support for I2S audio. Since then, ESPHome has introduced its own [I2S microphone](https://esphome.io/components/microphone/i2s_audio.html) and [sound level](https://esphome.io/components/sensor/sound_level.html) components. These may be sufficient for basic sound level measurements. However, if you need more advanced features - such as A/C-weighting or support for custom IIR filters - you can use my component instead.

This component was made to measure environmental noise levels (Leq, Lmin, Lmax, Lpeak) with different frequency weightings over configured time intervals. It is heavily based on awesome work by Ivan Kostoski: [esp32-i2s-slm](https://github.com/ikostoski/esp32-i2s-slm) (his [hackaday.io project](https://hackaday.io/project/166867-esp32-i2s-slm)).

<img width="488" alt="esphome sound level meter" src="https://github.com/user-attachments/assets/442a9b5d-4607-4d39-945a-9949f19904e0">

Typical weekly traffic noise recorded with a microphone located 50m from a medium traffic road:

<img width="1187" alt="image" src="https://user-images.githubusercontent.com/4602302/224789124-a86224c9-c11d-4972-a564-b042bab97bcb.png">


## Configuration

Add it to your ESPHome config:

```yaml
external_components:
  - source: github://stas-sl/esphome-sound-level-meter@dev  # add @tag if you want to use a specific version (e.g @v1.0.0)
```

For configuration options see [minimal-example-config.yaml](configs/minimal-example-config.yaml) or [advanced-example-config.yaml](configs/advanced-example-config.yaml). I would recommend to start from minimal config, and if it works and produces reasonable values (30-100 dB SPL), then add more filters/sensors.

```yaml
# see official docs https://esphome.io/components/i2s_audio.html
i2s_audio: 
  i2s_lrclk_pin: GPIO18
  i2s_bclk_pin: GPIO23

# see official docs https://esphome.io/components/microphone/i2s_audio
microphone:
  - platform: i2s_audio
    id: mic
    adc_type: external
    i2s_din_pin: GPIO19
    channel: left
    sample_rate: 48000
    bits_per_sample: 32bit
    i2s_mode: primary

sound_level_meter:
  id: sound_level_meter1

  # previously difined microphone instance, or you can even omit it if
  # there is only one mic defined - it will be used by default
  microphone: 
    microphone: mic
    bits_per_sample: 32

  # update_interval specifies over which interval to aggregate audio data
  # you can specify default update_interval on top level, but you can also override
  # it further by specifying it on sensor level
  update_interval: 60s           # default: 60s

  # start sound level meter automatically on boot
  auto_start: true              # default: true

  ring_buffer_size: 100ms       # default: 100ms

  # ignore audio data at startup for this long
  warmup_interval: 500ms        # default: 500ms

  # audio processing runs in a separate task, you can change its settings below.
  # idle task priority is 0,
  # main esphome loop priority is 1,
  # critical system tasks have priorities 18, 19, 20...
  # I set the priority to 2, slightly higher than the main loop,
  # because audio processing is highly sensitive to timing.
  # Large delays from certain components could cause the
  # DMA buffers to overflow, resulting in lost audio data.
  task_stack_size: 4096         # default: 4096
  task_priority: 2              # default: 2
  task_core: 1                  # default: 1

  # see your mic datasheet to find sensitivity and reference SPL.
  # those are used to convert dB FS to db SPL

  # if omitted, the reported values will be in dB FS units
  mic_sensitivity: -26dB        # default: empty
  mic_sensitivity_ref: 94dB     # default: empty
  # additional offset if needed
  offset: 0dB                   # default: empty

  # if you have many filters, you might consider using the IIR filter 
  # implementation from the esp-dsp library. it's written in assembly 
  # and includes several optimized versions, particularly performant on 
  # ESP32 or ESP32-S3, and can provide up to 2x faster processing compared 
  # to my C++ implementation. esp-dsp version uses direct form II, whereas 
  # the C++ version uses the direct form II transposed. the latter may offer 
  # slightly better numerical stability, but in most cases, the difference 
  # is likely negligible.
  use_esp_dsp: false            # default: false

  # under dsp_filters section you can define multiple filters,
  # which can be referenced later by each sensor
  
  # for now only SOS filter type is supported, see math/filter-design.ipynb
  # to learn how to create or convert other filter types to SOS

  # note, that those coefficients are only applicable for
  # specific sample rate (48 kHz), if you will use
  # other value, then you need to update these coefficicients
  dsp_filters:
    - id: f_inmp441             # INMP441 mic eq @ 48kHz
      type: sos
      coeffs:
        #       b0          b1          b2          a1          a2          
        - [ 1.0019784 , -1.9908513, 0.9889158 , -1.9951786, 0.99518436 ]
    - id: f_ics43434            # ICS-43434 mic eq @ 48kHz
      type: sos
      coeffs:                   
        #       b0           b1          b2          a1          a2
        - [ 0.47732642,  0.46294358, 0.11224797, 0.06681948, 0.00111522]
        - [ 1.,         -1.9890593 , 0.98908925, -1.9975533, 0.99755484]    
    - id: f_a                   # A weighting @ 48kHz
      type: sos
      coeffs:
        #        b0            b1            b2            a1            a2            
        - [ 0.16999495  , 0.741029    , 0.52548885  , -0.11321865 , -0.056549273 ]
        - [ 1.          , -2.00027    , 1.0002706   , -0.03433284 , -0.79215795  ]
        - [ 1.          , -0.709303   , -0.29071867 , -1.9822421  , 0.9822986    ]
    - id: f_c                   # C weighting @ 48kHz
      type: sos
      coeffs:
        #        b0             b1             b2             a1             a2             
        - [ -0.49651518  , -0.12296628  , -0.0076134163, -0.37165618  , 0.03453208    ]
        - [ 1.           , 1.3294908    , 0.44188643   , 1.2312505    , 0.37899444    ]
        - [ 1.           , -2.          , 1.           , -1.9946145   , 0.9946217     ]
  sensors:
    # 'eq' type sensor calculates Leq (average) sound level over specified period
    - type: eq
      name: LZeq_1s
      id: LZeq_1s
      # you can override updated_interval specified on top level
      # individually per each sensor
      update_interval: 1s

      # The dsp_filters field is a list of filter IDs defined
      # in the main component's corresponding section.
      # If you're only using a single filter, you can omit the brackets.
      # Alternatively, instead of referencing an existing filter,
      # you can define a filter directly here in the same format
      # as in the main component. However, this filter won't be reusable
      # across other sensors, so it’s best to use this approach only when
      # each sensor requires its own unique filters that don't overlap with others.
      dsp_filters: [f_inmp441]

    # you can have as many sensors of same type, but with different
    # other parameters (e.g. update_interval) as needed
    - type: eq
      name: LZeq_1min
      id: LZeq_1min
      unit_of_measurement: dBZ
      # another syntax for specifying a list
      dsp_filters: 
        - f_inmp441

    # 'max' and 'min' need exactly one detector: either 'window_size' or 'time_weighting'.
    #
    # window_size: splits the update_interval into consecutive rectangular windows and
    # reports the loudest (or quietest) of them. For example, with update_interval 60s and
    # window_size 1s it computes 60 one-second Leq values and reports the max of them.
    #
    # time_weighting: 'fast' (125ms), 'slow' (1s), or an explicit time constant. This is
    # the exponentially time-weighted level defined by IEC 61672-1, i.e. what LAFmax and
    # LASmax actually mean in the standard. Prefer it if you need comparable readings; see
    # "Time weighting" below for how the two differ.
    - type: max
      name: LZmax_1s_1min
      id: LZmax_1s_1min
      window_size: 1s
      unit_of_measurement: dBZ
      # you can omit brackets, if there is only single element
      dsp_filters: f_inmp441

    # same as 'max', but 'min'
    - type: min
      name: LZmin_1s_1min
      id: LZmin_1s_1min
      window_size: 1s
      unit_of_measurement: dBZ
      # it is also possible to define filter right in place, 
      # though it would be considered as a different filter even
      # if it has the same coeffiecients as other defined filters, 
      # so previous calculations could not be reused, there
      dsp_filters:
        - type: sos
          coeffs:
            #       b0          b1          b2          a1          a2          
            - [ 1.0019784 , -1.9908513, 0.9889158 , -1.9951786, 0.99518436 ]

    # max instantaneous level over the whole update_interval, i.e. the largest absolute
    # sample rather than an average. For a sine this reads 3.01 dB above the matching Leq.
    - type: peak
      name: LZpeak_1min
      id: LZpeak_1min
      unit_of_measurement: dBZ

    - type: eq
      name: LAeq_1min
      id: LAeq_1min
      unit_of_measurement: dBA
      dsp_filters: [f_inmp441, f_a]
    - type: max
      name: LAmax_1s_1min
      id: LAmax_1s_1min
      window_size: 1s
      unit_of_measurement: dBA
      dsp_filters: [f_inmp441, f_a]
    - type: min
      name: LAmin_1s_1min
      id: LAmin_1s_1min
      window_size: 1s
      unit_of_measurement: dBA
      dsp_filters: [f_inmp441, f_a]
    # IEC 61672-1 exponentially time-weighted levels, for readings comparable with a
    # real sound level meter
    - type: max
      name: LAFmax_1min
      id: LAFmax_1min
      time_weighting: fast
      unit_of_measurement: dBA
      dsp_filters: [f_inmp441, f_a]
    - type: min
      name: LASmin_1min
      id: LASmin_1min
      time_weighting: slow
      unit_of_measurement: dBA
      dsp_filters: [f_inmp441, f_a]
    - type: peak
      name: LApeak_1min
      id: LApeak_1min
      unit_of_measurement: dBA
      dsp_filters: [f_inmp441, f_a]

    - type: eq
      name: LCeq_1min
      id: LCeq_1min
      unit_of_measurement: dBC
      dsp_filters: [f_inmp441, f_c]
    - type: max
      name: LCmax_1s_1min
      id: LCmax_1s_1min
      window_size: 1s
      unit_of_measurement: dBC
      dsp_filters: [f_inmp441, f_c]
    - type: min
      name: LCmin_1s_1min
      id: LCmin_1s_1min
      window_size: 1s
      unit_of_measurement: dBC
      dsp_filters: [f_inmp441, f_c]
    - type: peak
      name: LCpeak_1min
      id: LCpeak_1min
      unit_of_measurement: dBC
      dsp_filters: [f_inmp441, f_c]

# automation
# available actions:
#   - sound_level_meter.start
#   - sound_level_meter.stop
switch:
  - platform: template
    name: "Sound Level Meter Switch"
    icon: mdi:power
    restore_mode: DISABLED
    lambda: |-
      return id(sound_level_meter1).is_running();
    turn_on_action:
      - sound_level_meter.start
    turn_off_action:
      - sound_level_meter.stop
```

## Time weighting

`max` and `min` sensors offer two detectors, and they do not measure the same thing.

| | `window_size: 1s` | `time_weighting: slow` | `time_weighting: fast` |
|---|---|---|---|
| detector | rectangular, non-overlapping | exponential, τ = 1s | exponential, τ = 125ms |
| IEC 61672-1 | no | yes (LAS) | yes (LAF) |

Measured against a true exponential detector on broadband bursts over a quiet background:

| event | `window_size: 1s` vs LASmax | `window_size: 1s` vs LAFmax |
|---|---|---|
| 20 ms burst | +0.04 dB | −8.7 dB |
| 125 ms burst | +0.15 dB | −7.9 dB |
| 500 ms burst | +0.70 dB | −5.5 dB |
| 2 s burst | +1.7 dB | −1.5 dB |

So `window_size: 1s` tracks **Slow** closely but is a poor stand-in for **Fast**. Rectangular
windows also do not overlap, so a transient landing near a window boundary is split across
two windows: the same 300 ms burst slid across a 1 s boundary varies by up to **2.9 dB**,
where an exponential detector varies by 0.3 dB. Use `time_weighting` if you need readings
that are comparable with a real sound level meter, and `window_size` if you specifically
want short-Leq statistics.

The exponential detector withholds its output for the first 5 τ after start so that it
cannot report a spurious minimum while settling — 0.6 s for Fast, 5 s for Slow. If your
update_interval is shorter than that, the first interval after start publishes `NaN`.

## Occupational noise exposure

`configs/noise-exposure-example-config-ics43434.yaml` is a worked example aimed at
noise-exposure assessment on an ICS-43434, rather than at general sound level metering.
It measures the quantities the exposure standards are defined on and derives the daily
exposure on-device:

| Published value | Standard |
|---|---|
| LAeq (1 s and 1 min), LCeq | IEC 61672-1 |
| LAFmax, LASmax | IEC 61672-1 exponential time weighting |
| LCpeak | IEC 61672-1 (see the range limit below) |
| L_EX,8h, LAeq,Te | ISO 9612, ISO 1999 |
| Sound exposure E_A in Pa²h | IEC 61252 |
| Noise dose, 3 dB exchange, 85 dB(A) criterion | 2003/10/EC |
| Noise dose, 5 dB exchange, 90 dB(A) criterion | OSHA 29 CFR 1910.95 |
| Octave-band levels, 63 Hz .. 8 kHz | ISO 4869-2 (hearing protector selection) |
| LCeq − LAeq | ISO 4869-2 HML indicator |
| Action-value binary sensors at 80 / 85 / 87 dB(A) | 2003/10/EC Article 3 |

The exposure integration runs in ESPHome lambdas; `math/verify_exposure_math.py`
checks it against exposure profiles whose answers follow from the definitions.
The octave-band filters are generated and validated by
`math/design_octave_bands.py --verify` (6th-order Butterworth, unity gain at the
midband frequency, −19.6 dB at the adjacent band centre; measured band-level error
≤ 0.25 dB on flat and pink spectra).

**Range limit.** The ICS-43434's acoustic overload point is 120 dB SPL, so this
hardware *cannot* assess the peak action values of 2003/10/EC (135 / 137 / 140 dB(C))
— the microphone clips long before them. LCpeak is still useful well below that, and
a "Microphone overload" binary sensor fires when the converter approaches full scale
(123 dB SPL peak). This is a monitoring tool, not a type-approved dosimeter.

**Not included.** ISO 532-1 loudness (sone) and DIN 45692 sharpness need 1/3-octave
resolution — 28 bands, roughly 84 biquad sections — which does not fit the CPU budget
alongside the weighting filters and exposure sensors. Octave bands are the correct
input for ISO 4869-2, not for ISO 532-1.

## Measurement caveats

- **Z-weighted readings are not band-limited.** IEC 61672-1 Z-weighting is flat from 10 Hz
  to 20 kHz with a defined roll-off outside; the unweighted path here integrates everything
  down to DC. The microphone equalisation filters have a large low-frequency boost
  (+17 dB at DC for `f_inmp441`), so any residual DC offset or subsonic noise is amplified
  into `LZ*` readings. This is the main reason dBZ sits well above dBA. A/C-weighted
  readings are unaffected, since both weightings reject that region strongly. Add a
  high-pass section to the chain if you need meaningful Z levels.
- **Peak is a sampled peak.** At 48 kHz the largest sample under-reads the true continuous
  peak by up to 0.02 dB at 1 kHz and 0.3 dB at 20 kHz, since the actual peak can fall
  between samples.
- **There is no overload indicator.** If the microphone clips, readings stay plausible but
  are wrong. Keep an eye on `peak` staying below 0 dBFS.

## 10 bands spectrum analyzer

[10-bands-spectrum-analyzer-example-config.yaml](configs/10-bands-spectrum-analyzer-example-config.yaml)

While manually specifying IIR/SOS filters might not be the most user-friendly approach, it offers great flexibility. This method allows you to use any filter you need, provided you know how to customize it to meet your requirements. Originally, my intention wasn’t to go beyond standard weighting functions like A/C, however to showcase the capabilities, I created a 10-band spectrum analyzer using ten 6th-order band-pass filters, each targeting a specific frequency band - simply by writing the appropriate config file, without needing to modify the component's source code.

For real-time visualization, I'm using web server number/slider controls to display the levels of each of the 10 bands. While this might not be the intended use of the sliders and web server - since they may not be designed for such frequent updates and it pushes the ESP32 to its limits, but it works 🤪

With 10 x 6 = 60 SOS filters, the component uses about 60-70% of the CPU, and I assume the web server also consumes some CPU power to send approximately 100 messages per second. So, this is quite a CPU-intensive task. I chose 6th-order filters somewhat arbitrarily; you could experiment with lower-order filters, which might meet your needs while using less CPU power.

https://github.com/user-attachments/assets/6283a8a9-d44d-40e2-992b-8ea2da0ff56e

While this example serves as a stress test, you could also use it to monitor different frequencies over longer time intervals with less frequent updates.

<img width="1193" src="https://github.com/user-attachments/assets/b811edf6-a4dd-4df8-a448-ae9c9b918505">

## Filter design (math)

Check out [filter-design notebook](math/filter-design.ipynb) to learn how those SOS coefficients were calculated.

## Sending data to sensor.community

See [sensor-community-example-config.yaml](configs/sensor-community-example-config.yaml)

## Performance

In Ivan's project SOS filters are implemented using ESP32 assembler, so they are really fast. A quote from him:

> Well, now you can lower the frequency of ESP32 down to 80MHz (i.e. for battery operation) and filtering and summation of I2S data will still take less than 15% of single core processing time. At 240MHz, filtering 1/8sec worth of samples with 2 x 6th-order IIR filters takes less than 5ms.

I'm not so familiar with assembler and it is hard to understand and maintain, so I implemented filtering in regular C++. Looks like the performance is not that bad. At 80MHz filtering and summation takes ~210ms per 1s of audio (48000 samples), which is 21% of single core processing time (vs. 15% if implemented in ASM). At 240MHz same task takes 67ms (vs. 5x8=40ms in ASM).

| CPU Freq | # SOS | Sensors                        | Sample Rate | Buffer size | Time (per 1s audio) |
| -------- | ----- | ------------------------------ | ----------- | ----------- | ------------------- |
| 80MHz    | 0     | 1 Leq                          | 48000       | 1024        | 57 ms               |
| 80MHz    | 6     | 1 Leq                          | 48000       | 1024        | 204 ms              |
| 80MHz    | 6     | 1 Lmax                         | 48000       | 1024        | 211 ms              |
| 80MHz    | 6     | 1 Lpeak                        | 48000       | 1024        | 207 ms              |
| 240MHz   | 0     | 1 Leq                          | 48000       | 1024        | 18 ms               |
| 240MHz   | 6     | 1 Leq                          | 48000       | 1024        | 67 ms               |
| 240MHz   | 6     | 1 Leq, 1 Lpeak, 1 Lmax, 1 Lmin | 48000       | 1024        | 90 ms               |

## Supported platforms

Tested with ESPHome version 2026.2.1 (ESP-IDF v5.5.2)

## Troubleshooting

Setting up I2S microphones, including correct wiring, identifying the right pins, and finding the correct setting values, can be tricky and frustrating, especially if you're doing it for the first time (and even if you're not). With numerous different boards and microphones, each with its own peculiarities, it can be a challenge. Below, I’ll summarize the best advice for troubleshooting if things don't work on the first attempt.

1. Double check your wires. I believe this is the most frequent source of mistakes. Try connecting L/R to GND or to VCC; or alternatively specify left or right channel in microphone configuration. Try different PINS, as some different boards might use some PINS for other purposes.

2. If you are receiving `-inf` values, it indicates that the input data consists entirely of zeros. This typically means you either have incorrect wiring or an incorrect PIN assignment in your i2s_audio/microphone config.

3. I would recommend starting from [minimal-example-config.yaml](configs/minimal-example-config.yaml) and ensuring that it produces values in reasonable range (30-80 dB SPL) and it reacts accordingly if you clap or produce louder noises, before proceeding further.

4. I tested it only on ESP32/ESP32 S3, that have 2 CPU cores, so not sure how it will work on other chips.

5. If you are experiencing performance issues, set logger level to `DEBUG` and the component will print CPU and ring buffer utilization. 

6. If you've set up everything correctly, then in a quite room the lowest LAeq levels should correspond to the noise floor levels from your mic specification. For example for INMP441 it should be ~33 dBA +/- few dBs. Don't confuse this with dBZ levels, which do not have A-weighting applied and can therefore be 5-15 dB higher.

7. Sometimes, even in complete silence, LAeq values may not reach the noise floor level. This can indicate the presence of other noises caused by electromagnetic or RF interference, which may be due to the WiFi module, a poor power supply, long wires, etc. For instance, on one board, I couldn’t achieve lower than 40-45 dBA until I reduced the WiFi power by setting `output_power: 8.5 dB` in config, after which I immediately obtained the expected 33 dBA.

8. Try official [sound_level component](https://esphome.io/components/sensor/sound_level.html). It uses a little bit different scale, so you will see a number < 0. Where 0 dB corresponds to loudest possible sound, and in a quite room you should expect < -80 dB.

9. You can also stream audio to your PC and listen to it. Fortunately, it is now very easy using the microphone's `on_data` handler and [udp component](https://esphome.io/components/udp.html):

```yaml
# streaming audio over UDP

i2s_audio:
  i2s_lrclk_pin: GPIOXX
  i2s_bclk_pin: GPIOYY

microphone:
  - platform: i2s_audio
    id: mic
    adc_type: external
    i2s_din_pin: GPIOZZ
    channel: left
    sample_rate: 48000
    bits_per_sample: 16bit
    i2s_mode: primary
    on_data:
      - udp.write:
          data: !lambda 'return x;'

udp:
  addresses: 192.168.xx.xx # where to stream the audio
  port: 1234
```

On the receiving end, you can use ffplay or mpv to play back and listen to the audio in real time:

```bash
ffplay -fflags nobuffer -flags low_delay -f s16le -ar 48000 -ch_layout mono -probesize 32 -analyzeduration 0 -af volume=30dB udp://0.0.0.0:1234

mpv udp://0.0.0.0:1234 -v --demuxer=rawaudio --demuxer-rawaudio-channels=1 --demuxer-rawaudio-rate=48000 --demuxer-rawaudio-format=s16le --untimed --cache=no -af volume=30dB
```

ffplay even displays by default nice spectrogram of playing audio.

Or to save it to a file, you can use netcat:

```bash
nc -u -l 1234 > mic_data.raw
```

## References

1. [ESP32-I2S-SLM hackaday.io project](https://hackaday.io/project/166867-esp32-i2s-slm)
1. [Measuring Audible Noise in Real-Time hackaday.io project](https://hackaday.io/project/162059-street-sense/log/170825-measuring-audible-noise-in-real-time)
1. [What are LAeq and LAFmax?](https://www.nti-audio.com/en/support/know-how/what-are-laeq-and-lafmax)
1. [Noise measuring @ smartcitizen.me](https://docs.smartcitizen.me/Components/sensors/air/Noise)
1. [EspAudioSensor](https://revspace.nl/EspAudioSensor)
1. [Design of a digital A-weighting filter with arbitrary sample rate (dsp.stackexchange.com)](https://dsp.stackexchange.com/questions/36077/design-of-a-digital-a-weighting-filter-with-arbitrary-sample-rate)
1. [How to compute dBFS? (dsp.stackexchange.com)](https://dsp.stackexchange.com/questions/8785/how-to-compute-dbfs)
1. [Microphone Specification Explained](https://invensense.tdk.com/wp-content/uploads/2015/02/AN-1112-v1.1.pdf)
1. [esp32-i2s-slm source code](https://github.com/ikostoski/esp32-i2s-slm)
1. [DNMS source code](https://github.com/hbitter/DNMS)
1. [NoiseLevel source code](https://github.com/bertrik/NoiseLevel)
