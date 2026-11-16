import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import ButtonKind, CONF_GRUNDFOS_ALPHA2GO_ID, GrundfosAlpha2Go, ns

GrundfosButton = ns.class_(
    "GrundfosButton", button.Button, cg.Parented.template(GrundfosAlpha2Go)
)

CONF_VENTING_START = "venting_start"
CONF_VENTING_STOP = "venting_stop"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_GRUNDFOS_ALPHA2GO_ID): cv.use_id(GrundfosAlpha2Go),
        # Manually start / abort system venting (88/612)
        cv.Optional(CONF_VENTING_START): button.button_schema(
            GrundfosButton, icon="mdi:air-filter", entity_category=ENTITY_CATEGORY_CONFIG
        ),
        cv.Optional(CONF_VENTING_STOP): button.button_schema(
            GrundfosButton, icon="mdi:stop-circle-outline", entity_category=ENTITY_CATEGORY_CONFIG
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_GRUNDFOS_ALPHA2GO_ID])
    for key, kind in (
        (CONF_VENTING_START, ButtonKind.VENTING_START),
        (CONF_VENTING_STOP, ButtonKind.VENTING_STOP),
    ):
        if conf := config.get(key):
            btn = await button.new_button(conf)
            await cg.register_parented(btn, hub)
            cg.add(btn.set_kind(kind))
            cg.add(hub.set_venting_buttons_present())
