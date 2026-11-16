import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import (
    ENTITY_CATEGORY_CONFIG,
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_STEP,
    UNIT_CUBIC_METER_PER_HOUR,
    UNIT_METER,
    UNIT_REVOLUTIONS_PER_MINUTE,
)

from . import CONF_GRUNDFOS_ALPHA2GO_ID, GrundfosAlpha2Go, SetpointKind, ns

GrundfosSetpointNumber = ns.class_(
    "GrundfosSetpointNumber", number.Number, cg.Parented.template(GrundfosAlpha2Go)
)

CONF_SETPOINT_HEAD = "setpoint_head"
CONF_SETPOINT_FLOW = "setpoint_flow"
CONF_SETPOINT_SPEED = "setpoint_speed"


def _setpoint(unit, icon, min_value, max_value, step):
    # Limits are defaults; the real limits depend on the pump type and can be overridden
    return number.number_schema(
        GrundfosSetpointNumber,
        unit_of_measurement=unit,
        icon=icon,
        entity_category=ENTITY_CATEGORY_CONFIG,
    ).extend(
        {
            cv.Optional(CONF_MIN_VALUE, default=min_value): cv.float_,
            cv.Optional(CONF_MAX_VALUE, default=max_value): cv.float_,
            cv.Optional(CONF_STEP, default=step): cv.positive_float,
        }
    )


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_GRUNDFOS_ALPHA2GO_ID): cv.use_id(GrundfosAlpha2Go),
        # Pressure control modes (proportional / constant pressure); display only under AutoAdapt
        cv.Optional(CONF_SETPOINT_HEAD): _setpoint(UNIT_METER, "mdi:arrow-collapse-up", 0.5, 6.0, 0.1),
        # Constant flow
        cv.Optional(CONF_SETPOINT_FLOW): _setpoint(UNIT_CUBIC_METER_PER_HOUR, "mdi:waves-arrow-right", 0.1, 3.5, 0.1),
        # Constant speed
        cv.Optional(CONF_SETPOINT_SPEED): _setpoint(UNIT_REVOLUTIONS_PER_MINUTE, "mdi:speedometer", 1000, 5500, 50),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_GRUNDFOS_ALPHA2GO_ID])
    for key, kind, setter in (
        (CONF_SETPOINT_HEAD, SetpointKind.SP_PRESSURE, hub.set_setpoint_head_number),
        (CONF_SETPOINT_FLOW, SetpointKind.SP_FLOW, hub.set_setpoint_flow_number),
        (CONF_SETPOINT_SPEED, SetpointKind.SP_SPEED, hub.set_setpoint_speed_number),
    ):
        if conf := config.get(key):
            num = await number.new_number(
                conf,
                min_value=conf[CONF_MIN_VALUE],
                max_value=conf[CONF_MAX_VALUE],
                step=conf[CONF_STEP],
            )
            await cg.register_parented(num, hub)
            cg.add(num.set_kind(kind))
            cg.add(setter(num))
