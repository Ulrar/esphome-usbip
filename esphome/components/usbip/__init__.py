import esphome.codegen as cg
import esphome.config_validation as cv

from esphome.const import CONF_PORT, CONF_ID

DEPENDENCIES = []

usbip_ns = cg.esphome_ns.namespace('usbip')
USBIPComponent = usbip_ns.class_('USBIPComponent', cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(USBIPComponent),
    cv.Optional(CONF_PORT, default=3240): cv.port,
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if CONF_PORT in config:
        cg.add(var.set_port(config[CONF_PORT]))
