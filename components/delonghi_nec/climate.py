import esphome.codegen as cg
from esphome.components import climate_ir
import esphome.config_validation as cv

CODEOWNERS = ["@idefxH"]
AUTO_LOAD = ["climate_ir"]

CONF_SUPPORTS_HEAT = "supports_heat"

delonghi_nec_ns = cg.esphome_ns.namespace("delonghi_nec")
DelonghiNEC = delonghi_nec_ns.class_("DelonghiNEC", climate_ir.ClimateIR)

# climate_ir_with_receiver_schema wires up an optional remote_receiver so that
# physical remote presses are mirrored back into the tracked state.
CONFIG_SCHEMA = climate_ir.climate_ir_with_receiver_schema(DelonghiNEC).extend(
    {
        # The Pinguino has no heating element; override the climate_ir default.
        cv.Optional(CONF_SUPPORTS_HEAT, default=False): cv.boolean,
    }
)


async def to_code(config):
    await climate_ir.new_climate_ir(config)
