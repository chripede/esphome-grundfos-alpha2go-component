import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_GRUNDFOS_ALPHA2GO_ID, GrundfosAlpha2Go, SwitchKind, ns

GrundfosSwitch = ns.class_(
    "GrundfosSwitch", switch.Switch, cg.Parented.template(GrundfosAlpha2Go)
)

CONF_PUMP = "pump"
CONF_NIGHT_SETBACK = "night_setback"
CONF_AIR_DETECTION = "air_detection"

# State always comes from the pump; nothing is written at boot
CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_GRUNDFOS_ALPHA2GO_ID): cv.use_id(GrundfosAlpha2Go),
        cv.Optional(CONF_PUMP): switch.switch_schema(
            GrundfosSwitch, icon="mdi:pump", default_restore_mode="DISABLED"
        ),
        cv.Optional(CONF_NIGHT_SETBACK): switch.switch_schema(
            GrundfosSwitch,
            icon="mdi:weather-night",
            entity_category=ENTITY_CATEGORY_CONFIG,
            default_restore_mode="DISABLED",
        ),
        cv.Optional(CONF_AIR_DETECTION): switch.switch_schema(
            GrundfosSwitch,
            icon="mdi:chart-bubble",
            entity_category=ENTITY_CATEGORY_CONFIG,
            default_restore_mode="DISABLED",
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_GRUNDFOS_ALPHA2GO_ID])
    for key, kind, setter in (
        (CONF_PUMP, SwitchKind.PUMP, hub.set_pump_switch),
        (CONF_NIGHT_SETBACK, SwitchKind.NIGHT_SETBACK, hub.set_night_setback_switch),
        (CONF_AIR_DETECTION, SwitchKind.AIR_DETECTION, hub.set_air_detection_switch),
    ):
        if conf := config.get(key):
            sw = await switch.new_switch(conf)
            await cg.register_parented(sw, hub)
            cg.add(sw.set_kind(kind))
            cg.add(setter(sw))
