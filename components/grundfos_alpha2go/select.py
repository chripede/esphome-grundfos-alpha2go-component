import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_GRUNDFOS_ALPHA2GO_ID, GrundfosAlpha2Go, ns

GrundfosControlModeSelect = ns.class_(
    "GrundfosControlModeSelect", select.Select, cg.Parented.template(GrundfosAlpha2Go)
)

CONF_CONTROL_MODE = "control_mode"

# Order must match SELECT_CONTROL_CODES in grundfos_alpha2go.cpp: 1, 0, 13, 14, 8, 2
CONTROL_MODE_OPTIONS = [
    "Proportional pressure",
    "Constant pressure",
    "Proportional pressure (AutoAdapt)",
    "Constant pressure (AutoAdapt)",
    "Constant flow",
    "Constant speed",
]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_GRUNDFOS_ALPHA2GO_ID): cv.use_id(GrundfosAlpha2Go),
        cv.Optional(CONF_CONTROL_MODE): select.select_schema(
            GrundfosControlModeSelect,
            icon="mdi:tune-vertical",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_GRUNDFOS_ALPHA2GO_ID])
    if conf := config.get(CONF_CONTROL_MODE):
        sel = await select.new_select(conf, options=CONTROL_MODE_OPTIONS)
        await cg.register_parented(sel, hub)
        cg.add(hub.set_control_mode_select(sel))
