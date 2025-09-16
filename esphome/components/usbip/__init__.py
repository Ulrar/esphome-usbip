import esphome.codegen as cg
import esphome.config_validation as cv

from esphome.const import CONF_PORT, CONF_ID

DEPENDENCIES = ["usb_host"]

usbip_ns = cg.esphome_ns.namespace('usbip')
USBIPComponent = usbip_ns.class_('USBIPComponent', cg.Component)

# Import the usb_host component type so users can reference it by id
usb_host_ns = cg.esphome_ns.namespace('usb_host')
USBHost = usb_host_ns.class_('USBHost', cg.Component)
USBClient = usb_host_ns.class_('USBClient', cg.Component)

CONF_USB_HOST = 'usb_host'

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(USBIPComponent),
    cv.Optional(CONF_PORT, default=3240): cv.port,
    cv.Optional('string_wait_ms', default=2000): cv.Any(cv.positive_time_period_milliseconds, cv.positive_int),
    cv.Optional(CONF_USB_HOST): cv.use_id(USBHost),
    cv.Optional('clients'): cv.ensure_list(cv.use_id(USBClient)),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if CONF_PORT in config:
        cg.add(var.set_port(config[CONF_PORT]))

    if CONF_USB_HOST in config:
        host = await cg.get_variable(config[CONF_USB_HOST])
        # Bind the esphome usb_host instance to the component so the C++ side
        # can create a proper adapter.
        cg.add(var.set_esphome_host(host))

    for c in config.get('clients') or ():
        client = await cg.get_variable(c)
        cg.add(var.add_exported_client(client))
    if 'string_wait_ms' in config:
        cg.add(var.set_string_wait_ms(config['string_wait_ms']))
