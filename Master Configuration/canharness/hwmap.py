"""Hub hardware map and ADS124S08 register recipes (ported from SensorHubV1/config/parse_cfg_to_c.py).

Hub connectors: two 34-pin TE Superseal 1.0. Connector 1 (sensors): 1-20 analog, 21-24 digital, 25 and
30-34 GND, 26-29 tied to pin 20 (shared TC-). Connector 2 (power): 1-17 5V, 18-34 GND.
"""

# connector pin -> AFE pin index (NOT AIN number)
AFE1_PINS = {14: 0, 5: 1, 13: 2, 4: 3, 12: 4, 3: 5, 11: 6, 2: 7, 10: 8, 1: 9}
AFE2_PINS = {9: 0, 18: 1, 19: 2, 20: 3, 17: 4, 8: 5, 16: 6, 7: 7, 15: 8, 6: 9}
DIGITAL_PINS = {21: 0, 22: 1, 23: 2, 24: 3}
# connector 1 (sensors): pin 20 is the shared TC- input, electrically common with pins 26-29
TC_NEG_PINS = (20, 26, 27, 28, 29)
CONN1_GND_PINS = (25, 30, 31, 32, 33, 34)
# connector 2 (power): 1-17 are 5V, 18-34 are GND
CONN2_5V_PINS = tuple(range(1, 18))
CONN2_GND_PINS = tuple(range(18, 35))
AINCOM = 0x0C
TC_REF_PIN = 20
SENSOR_CONNECTOR_PINS = 34
POWER_CONNECTOR_PINS = 34

# conversions per second of each AFE (see gen_datarate)
AFE_SPS = {1: 4000, 2: 100}


def resolve_pin(pin: int) -> tuple[int, int]:
    """Return (afe, afe_pin). afe 1 = AFE A, 2 = AFE B, 3 = digital input."""
    if pin in TC_NEG_PINS:
        pin = TC_REF_PIN
    if pin in AFE1_PINS:
        return 1, AFE1_PINS[pin]
    if pin in AFE2_PINS:
        return 2, AFE2_PINS[pin]
    if pin in DIGITAL_PINS:
        return 3, DIGITAL_PINS[pin]
    raise ValueError(f"connector pin {pin} is not a valid sensor input")


def inpmux(sens_type: str, pos_pin: int, neg_pin: int = AINCOM) -> int:
    if not 0 <= pos_pin < 10:
        raise ValueError("Positive channel out of range")
    if neg_pin != AINCOM and not 0 <= neg_pin < 10:
        raise ValueError("Negative channel out of range")
    if sens_type in ("resistive", "active"):
        if neg_pin != AINCOM:
            raise ValueError(f"{sens_type.capitalize()} sensors must be referenced to board ground (single ended)")
    elif sens_type == "thermocouple":
        if neg_pin != AFE2_PINS[TC_REF_PIN]:
            raise ValueError(f"Thermocouples must be referenced to connector pin {TC_REF_PIN}")
        if pos_pin > 5:
            raise ValueError("Thermocouples may only be placed on inputs 0 through 5")
    else:
        raise ValueError("Invalid Sensor Type")
    return (pos_pin << 4) | neg_pin


def pga(sens_type: str) -> int:
    return {"resistive": 0x08, "active": 0x00, "thermocouple": 0x0F}[sens_type]


def datarate(afe: int, sens_type: str) -> int:
    if afe == 1 and sens_type == "thermocouple":
        raise ValueError("Thermocouples must be placed on AFE B")
    if afe == 1:
        return 0x1D  # 4000 SPS, low-latency filter
    if afe == 2:
        return 0x17  # 100 SPS, low-latency filter
    raise ValueError("Invalid AFE")


def ref_ctrl(sens_type: str) -> int:
    return {"thermocouple": 0x09, "resistive": 0x01, "active": 0x00}[sens_type]


def idacmag(sens_type: str) -> int:
    return {"thermocouple": 0x0F, "active": 0x0F, "resistive": 0x07}[sens_type]


def idacmux(sens_type: str, pin: int) -> int:
    return 0xF0 | pin if sens_type == "resistive" else 0xFF


def vbias(sens_type: str, pin: int) -> int:
    if sens_type == "thermocouple":
        if not 0 <= pin <= 5:
            raise ValueError("VBIAS may only be enabled on AIN 0-5")
        return 1 << pin
    return 0x00
