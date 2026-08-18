# pylint: disable=no-name-in-module,invalid-name,unused-argument

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation, core
from esphome.automation import maybe_simple_id
from esphome.components import microphone, ota, sensor
from esphome.components.esp32 import add_idf_component
from esphome.const import (
    CONF_ID,
    CONF_MICROPHONE,
    CONF_OFFSET,
    CONF_SENSORS,
    CONF_TYPE,
    CONF_UPDATE_INTERVAL,
    CONF_WINDOW_SIZE,
    DEVICE_CLASS_SOUND_PRESSURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_DECIBEL,
)
from esphome.core import ID, TimePeriodMilliseconds

CODEOWNERS = ["@stas-sl"]
DEPENDENCIES = ["esp32", "microphone"]
MULTI_CONF = True

sound_level_meter_ns = cg.esphome_ns.namespace("sound_level_meter")
SoundLevelMeter = sound_level_meter_ns.class_("SoundLevelMeter", cg.Component)
SoundLevelMeterSensor = sound_level_meter_ns.class_(
    "SoundLevelMeterSensor", sensor.Sensor
)
SoundLevelMeterSensorEq = sound_level_meter_ns.class_(
    "SoundLevelMeterSensorEq", SoundLevelMeterSensor, sensor.Sensor
)
SoundLevelMeterSensorMax = sound_level_meter_ns.class_(
    "SoundLevelMeterSensorMax", SoundLevelMeterSensor, sensor.Sensor
)
SoundLevelMeterSensorMin = sound_level_meter_ns.class_(
    "SoundLevelMeterSensorMin", SoundLevelMeterSensor, sensor.Sensor
)
SoundLevelMeterSensorPeak = sound_level_meter_ns.class_(
    "SoundLevelMeterSensorPeak", SoundLevelMeterSensor, sensor.Sensor
)
Filter = sound_level_meter_ns.class_("Filter")
SosFilter = sound_level_meter_ns.class_("SosFilter", Filter)
SosCoeffs = sound_level_meter_ns.struct("SosCoeffs")
StartAction = sound_level_meter_ns.class_("StartAction", automation.Action)
StopAction = sound_level_meter_ns.class_("StopAction", automation.Action)

CONF_AUTO_START = "auto_start"
CONF_COEFFS = "coeffs"
CONF_DSP_FILTERS = "dsp_filters"
CONF_EQ = "eq"
CONF_HIGH_FREQ = "high_freq"
CONF_MAX = "max"
CONF_MIC_SENSITIVITY = "mic_sensitivity"
CONF_MIC_SENSITIVITY_REF = "mic_sensitivity_ref"
CONF_MIN = "min"
CONF_PEAK = "peak"
CONF_RING_BUFFER_SIZE = "ring_buffer_size"
CONF_SOS = "sos"
CONF_TASK_CORE = "task_core"
CONF_TASK_PRIORITY = "task_priority"
CONF_TASK_STACK_SIZE = "task_stack_size"
CONF_TIME_WEIGHTING = "time_weighting"
CONF_USE_ESP_DSP = "use_esp_dsp"
CONF_WARMUP_INTERVAL = "warmup_interval"

ICON_WAVEFORM = "mdi:waveform"

# Number of coefficients per second-order section: b0, b1, b2, a1, a2.
SOS_COEFFS_PER_SECTION = 5

# IEC 61672-1 exponential time weightings, as time constants.
TIME_WEIGHTINGS = {"fast": 125, "slow": 1000}


def _validate_time_weighting(value):
    """Accept the IEC names, or an explicit time constant for anything else."""
    if isinstance(value, str) and value.lower() in TIME_WEIGHTINGS:
        return TimePeriodMilliseconds(milliseconds=TIME_WEIGHTINGS[value.lower()])
    return cv.positive_time_period_milliseconds(value)


def AUTO_LOAD(config) -> list[str]:
    """Only pull in the sensor platform when the user actually configured sensors.

    A meter without sensors compiles without the sensor component at all; the C++ side is
    gated behind ``#ifdef USE_SENSOR`` to match.
    """
    loads = ["audio"]
    configs = config if isinstance(config, list) else [config]
    for conf in configs:
        if isinstance(conf, dict) and conf.get(CONF_SENSORS):
            loads.append("sensor")
            break
    return loads


def _validate_sos_filter(value):
    value = cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SosFilter),
            cv.Required(CONF_COEFFS): [[cv.float_]],
        }
    )(value)

    # An empty table would emit `static const SosCoeffs x[] = {};`, which is not valid C++.
    if not value[CONF_COEFFS]:
        raise cv.Invalid(
            f"{CONF_SOS} filter needs at least one section", [CONF_COEFFS]
        )

    for idx, row in enumerate(value[CONF_COEFFS]):
        if len(row) != SOS_COEFFS_PER_SECTION:
            raise cv.Invalid(
                f"Each SOS coefficient row must contain exactly "
                f"{SOS_COEFFS_PER_SECTION} values (b0, b1, b2, a1, a2); "
                f"row {idx} has {len(row)}",
                [CONF_COEFFS, idx],
            )
    return value


CONFIG_DSP_FILTER_SCHEMA = cv.typed_schema({CONF_SOS: _validate_sos_filter})

CONFIG_SENSOR_DSP_FILTER_SCHEMA = cv.ensure_list(
    cv.Any(cv.use_id(Filter), CONFIG_DSP_FILTER_SCHEMA)
)


def _sensor_schema(class_, extra=None):
    schema = {
        cv.Optional(CONF_UPDATE_INTERVAL): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_DSP_FILTERS, default=[]): CONFIG_SENSOR_DSP_FILTER_SCHEMA,
    }
    if extra:
        schema.update(extra)
    return sensor.sensor_schema(
        class_,
        unit_of_measurement=UNIT_DECIBEL,
        accuracy_decimals=2,
        state_class=STATE_CLASS_MEASUREMENT,
        device_class=DEVICE_CLASS_SOUND_PRESSURE,
        icon=ICON_WAVEFORM,
    ).extend(schema)


# Exactly one of these is required; _validate_detector() enforces that.
_EXTREMUM_SCHEMA = {
    cv.Optional(CONF_WINDOW_SIZE): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_TIME_WEIGHTING): _validate_time_weighting,
}

CONFIG_SENSOR_SCHEMA = cv.typed_schema(
    {
        CONF_EQ: _sensor_schema(SoundLevelMeterSensorEq),
        CONF_MAX: _sensor_schema(SoundLevelMeterSensorMax, _EXTREMUM_SCHEMA),
        CONF_MIN: _sensor_schema(SoundLevelMeterSensorMin, _EXTREMUM_SCHEMA),
        CONF_PEAK: _sensor_schema(SoundLevelMeterSensorPeak),
    }
)


def _validate_effective_sensor_intervals(config):
    # TimePeriod does not compare against plain ints, so work in milliseconds throughout.
    top_update = config[CONF_UPDATE_INTERVAL].total_milliseconds
    # A 0ms effective update_interval makes update_samples_ == 0, which stalls
    # the sensor (it never publishes a real value). Likewise reject a 0ms window_size.
    if top_update <= 0:
        raise cv.Invalid(
            f"Top-level {CONF_UPDATE_INTERVAL} must be greater than 0ms",
            [CONF_UPDATE_INTERVAL],
        )
    for idx, sensor_cfg in enumerate(config[CONF_SENSORS]):
        if (update_interval := sensor_cfg.get(CONF_UPDATE_INTERVAL)) is not None:
            effective_update = update_interval.total_milliseconds
        else:
            effective_update = top_update
        if effective_update <= 0:
            raise cv.Invalid(
                f"Sensor at index {idx} must have an effective "
                f"{CONF_UPDATE_INTERVAL} greater than 0ms",
                [CONF_SENSORS, idx, CONF_UPDATE_INTERVAL],
            )

        if (window_size := sensor_cfg.get(CONF_WINDOW_SIZE)) is not None:
            window_size_ms = window_size.total_milliseconds
            if window_size_ms <= 0:
                raise cv.Invalid(
                    f"Sensor at index {idx} must have a {CONF_WINDOW_SIZE} "
                    f"greater than 0ms",
                    [CONF_SENSORS, idx, CONF_WINDOW_SIZE],
                )
            if window_size_ms > effective_update:
                raise cv.Invalid(
                    f"Sensor at index {idx} has {CONF_WINDOW_SIZE} greater than its "
                    f"effective {CONF_UPDATE_INTERVAL}",
                    [CONF_SENSORS, idx, CONF_WINDOW_SIZE],
                )

        if (time_weighting := sensor_cfg.get(CONF_TIME_WEIGHTING)) is not None:
            if time_weighting.total_milliseconds <= 0:
                raise cv.Invalid(
                    f"Sensor at index {idx} must have a {CONF_TIME_WEIGHTING} "
                    f"greater than 0ms",
                    [CONF_SENSORS, idx, CONF_TIME_WEIGHTING],
                )
    return config


def _validate_detectors(config):
    """'max' and 'min' need exactly one detector: a rectangular window or a time weighting."""
    for idx, sensor_cfg in enumerate(config[CONF_SENSORS]):
        if sensor_cfg[CONF_TYPE] not in (CONF_MAX, CONF_MIN):
            continue
        has_window = CONF_WINDOW_SIZE in sensor_cfg
        has_weighting = CONF_TIME_WEIGHTING in sensor_cfg
        if has_window == has_weighting:
            chosen = "both" if has_window else "neither"
            raise cv.Invalid(
                f"'{sensor_cfg[CONF_TYPE]}' sensor at index {idx} needs exactly one of "
                f"{CONF_WINDOW_SIZE} or {CONF_TIME_WEIGHTING} ({chosen} given). "
                f"{CONF_TIME_WEIGHTING} (fast/slow) gives the IEC 61672-1 exponentially "
                f"time-weighted level; {CONF_WINDOW_SIZE} gives the maximum of "
                f"consecutive rectangular-window Leq blocks",
                [CONF_SENSORS, idx],
            )
    return config


def _validate_filter_sharing(config):
    """Reject a filter instance that would see two different input signals.

    Sensors are evaluated with a shared prefix stack, so a filter is run once per block for
    each distinct chain prefix it appears behind. Because a filter owns one delay line,
    appearing behind two different prefixes makes it process two unrelated signals through
    the same state, silently corrupting both. Give each context its own filter instead.
    """

    def describe(prefix):
        return " -> ".join(prefix) if prefix else "(none)"

    prefixes = {}
    for idx, sensor_cfg in enumerate(config[CONF_SENSORS]):
        chain = [
            str(entry) if isinstance(entry, core.ID) else f"<inline #{id(entry):x}>"
            for entry in sensor_cfg[CONF_DSP_FILTERS]
        ]
        for position, name in enumerate(chain):
            if name.startswith("<inline "):
                continue  # an inline filter belongs to exactly one sensor
            prefix = tuple(chain[:position])
            previous = prefixes.setdefault(name, prefix)
            if previous != prefix:
                raise cv.Invalid(
                    f"dsp_filter '{name}' is used behind two different filter chains: "
                    f"{describe(previous)} and {describe(prefix)}. A filter holds one "
                    f"delay line, so "
                    f"it cannot be shared between chains that feed it different signals. "
                    f"Define a second filter with the same coefficients for one of them",
                    [CONF_SENSORS, idx, CONF_DSP_FILTERS, position],
                )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SoundLevelMeter),
            cv.Optional(
                CONF_MICROPHONE, default={}
            ): microphone.microphone_source_schema(
                min_bits_per_sample=16,
                max_bits_per_sample=32,
            ),
            cv.Optional(
                CONF_UPDATE_INTERVAL, default="60s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_AUTO_START, default=True): cv.boolean,
            cv.Optional(CONF_HIGH_FREQ, default=False): cv.boolean,
            cv.Optional(
                CONF_RING_BUFFER_SIZE, default="100ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_WARMUP_INTERVAL, default="0ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_TASK_STACK_SIZE, default=4096): cv.positive_not_null_int,
            cv.Optional(CONF_TASK_PRIORITY, default=2): cv.uint8_t,
            cv.Optional(CONF_TASK_CORE, default=1): cv.int_range(0, 1),
            cv.Optional(CONF_MIC_SENSITIVITY): cv.decibel,
            cv.Optional(CONF_MIC_SENSITIVITY_REF): cv.decibel,
            cv.Optional(CONF_OFFSET): cv.decibel,
            cv.Optional(CONF_DSP_FILTERS, default=[]): [CONFIG_DSP_FILTER_SCHEMA],
            cv.Optional(CONF_SENSORS, default=[]): [CONFIG_SENSOR_SCHEMA],
            cv.Optional(CONF_USE_ESP_DSP, default=False): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _validate_effective_sensor_intervals,
    _validate_detectors,
    _validate_filter_sharing,
)

SOUND_LEVEL_METER_ACTION_SCHEMA = maybe_simple_id(
    {cv.GenerateID(): cv.use_id(SoundLevelMeter)}
)


async def add_dsp_filter(config, parent):
    if config[CONF_TYPE] != CONF_SOS:
        raise ValueError(f"Unknown dsp filter type: {config[CONF_TYPE]}")

    rows = config[CONF_COEFFS]
    # One `static const SosCoeffs[]` in flash instead of one setter call per section.
    table = cg.static_const_array(
        ID(f"{config[CONF_ID]}_coeffs", is_declaration=True, type=SosCoeffs),
        cg.ArrayInitializer(
            *(cg.ArrayInitializer(*row) for row in rows), multiline=True
        ),
    )
    f = cg.new_Pvariable(config[CONF_ID], table, len(rows))
    cg.add(parent.add_dsp_filter(f))
    return f


async def add_sensor(config, parent):
    s = await sensor.new_sensor(config)
    cg.add(s.set_parent(parent))
    if (window_size := config.get(CONF_WINDOW_SIZE)) is not None:
        cg.add(s.set_window_size(window_size))
    if (time_weighting := config.get(CONF_TIME_WEIGHTING)) is not None:
        cg.add(s.set_time_constant(time_weighting))
    if (update_interval := config.get(CONF_UPDATE_INTERVAL)) is not None:
        cg.add(s.set_update_interval(update_interval))

    filter_configs = config[CONF_DSP_FILTERS]
    cg.add(s.init_dsp_filters(len(filter_configs)))
    for fc in filter_configs:
        if isinstance(fc, core.ID):
            f = await cg.get_variable(fc)
        elif isinstance(fc, dict):
            f = await add_dsp_filter(fc, parent)
        else:
            raise ValueError(f"Unexpected dsp filter entry: {fc!r}")
        cg.add(s.add_dsp_filter(f))
    cg.add(parent.add_sensor(s))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    mic_source = await microphone.microphone_source_to_code(
        config[CONF_MICROPHONE], passive=False
    )
    cg.add(var.set_microphone_source(mic_source))
    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))
    cg.add(var.set_ring_buffer_size(config[CONF_RING_BUFFER_SIZE]))
    cg.add(var.set_warmup_interval(config[CONF_WARMUP_INTERVAL]))
    cg.add(var.set_task_stack_size(config[CONF_TASK_STACK_SIZE]))
    cg.add(var.set_task_priority(config[CONF_TASK_PRIORITY]))
    cg.add(var.set_task_core(config[CONF_TASK_CORE]))
    cg.add(var.set_is_high_freq(config[CONF_HIGH_FREQ]))
    cg.add(var.set_is_auto_start(config[CONF_AUTO_START]))
    if (mic_sensitivity := config.get(CONF_MIC_SENSITIVITY)) is not None:
        cg.add(var.set_mic_sensitivity(mic_sensitivity))
    if (mic_sensitivity_ref := config.get(CONF_MIC_SENSITIVITY_REF)) is not None:
        cg.add(var.set_mic_sensitivity_ref(mic_sensitivity_ref))
    if (offset := config.get(CONF_OFFSET)) is not None:
        cg.add(var.set_offset(offset))
    if config[CONF_USE_ESP_DSP]:
        add_idf_component(name="espressif/esp-dsp", ref="1.7.0")
        cg.add_define("USE_ESP_DSP")

    # Size the fixed-capacity containers before anything is registered into them: an
    # add_*() call past the reserved count is rejected and logged, never silently dropped.
    sensor_configs = config[CONF_SENSORS]
    filter_configs = config[CONF_DSP_FILTERS]
    total_filters = len(filter_configs) + sum(
        1
        for sc in sensor_configs
        for fc in sc[CONF_DSP_FILTERS]
        if isinstance(fc, dict)
    )
    cg.add(var.init_dsp_filters(total_filters))
    if sensor_configs:
        cg.add(var.init_sensors(len(sensor_configs)))

    for fc in filter_configs:
        await add_dsp_filter(fc, var)

    for sc in sensor_configs:
        await add_sensor(sc, var)

    ota.request_ota_state_listeners()


@automation.register_action(
    "sound_level_meter.start",
    StartAction,
    SOUND_LEVEL_METER_ACTION_SCHEMA,
    synchronous=True,
)
@automation.register_action(
    "sound_level_meter.stop",
    StopAction,
    SOUND_LEVEL_METER_ACTION_SCHEMA,
    synchronous=True,
)
async def switch_toggle_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)
