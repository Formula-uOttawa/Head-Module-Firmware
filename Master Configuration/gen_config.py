import os

import pandas as pd

# 1. Centralized Parameters
SYSTEM_PARAMS = {
    "network": {
        "max_payload_bytes": 64,
        "timestamp_bytes": 4,
        "base_can_id_multiplier": 0x100,  # e.g., Hub 1 starts at 0x100, Hub 2 at 0x200
    },
    "sensor_types": {
        # Maps user-facing string to (C-Enum, Byte Size in CAN frame)
        "resistive": {"c_enum": "SENSOR_TYPE_RESISTIVE", "size": 2},
        "active": {"c_enum": "SENSOR_TYPE_ACTIVE", "size": 2},
        "thermocouple": {"c_enum": "SENSOR_TYPE_THERMOCOUPLE", "size": 2},
        "digital": {"c_enum": "SENSOR_TYPE_DIGITAL", "size": 2},
    },
    "output_dirs": {"hub": "generated/hub_modules", "head": "generated/head_module"},
}


class SystemCompiler:
    def __init__(self, params, csv_path):
        self.params = params
        # Read and clean the master sheet
        self.df = pd.read_csv(csv_path)
        self.df.columns = self.df.columns.str.strip()

        # Ensure output directories exist
        for path in self.params["output_dirs"].values():
            os.makedirs(path, exist_ok=True)

    def _get_sensor_meta(self, sensor_type):
        """Safely fetches parameter definitions for a given sensor type."""
        s_type = str(sensor_type).strip().lower()
        return self.params["sensor_types"].get(
            s_type,
            {"c_enum": "SENSOR_TYPE_UNKNOWN", "size": 2},  # Default fallback
        )

    def build(self):
        """Runs the auto-packer and generates all C files."""
        hub_configs = {}
        head_routes = []

        # Auto-Packing Logic
        for hub_id, hub_group in self.df.groupby("Hub #"):
            hub_configs[hub_id] = []

            for rate, rate_group in hub_group.groupby("Sampling Frequency"):
                # Initialize frame tracking for this specific Hub and Frequency
                current_frame_id = (
                    int(hub_id) * self.params["network"]["base_can_id_multiplier"]
                ) | int(rate)
                current_offset = self.params["network"]["timestamp_bytes"]

                for index, row in rate_group.iterrows():
                    meta = self._get_sensor_meta(row["Sensor Type"])
                    byte_length = meta["size"]

                    # Frame Rollover Check
                    if (
                        current_offset + byte_length
                        > self.params["network"]["max_payload_bytes"]
                    ):
                        current_frame_id += 1
                        current_offset = self.params["network"]["timestamp_bytes"]

                    # Store Hub-level C Struct (Hardware & Physics)
                    hub_configs[hub_id].append(
                        {
                            "id": row["Channel ID"],
                            "frame_id": current_frame_id,
                            "type_enum": meta["c_enum"],
                            "rate": int(rate),
                            "offset": current_offset,
                            "length": byte_length,
                            "pin_pos": row["Signal Pin (+)"],
                        }
                    )

                    # Store Head-level Routing Data (Network & Dash Formatting)
                    head_routes.append(
                        {
                            "channel_id": row["Channel ID"],
                            "short_name": row["Short Name"],
                            "frame_id": current_frame_id,
                            "offset": current_offset,
                            "length": byte_length,
                            "scaling": row["Scaling (V/V)"],
                        }
                    )

                    current_offset += byte_length

        self._export_hub_firmware(hub_configs)
        self._export_head_firmware(head_routes)
        print("End-to-end generation complete.")

    def _export_hub_firmware(self, hub_configs):
        """Generates the hardware-level C arrays for the STM32G4 Hubs."""
        out_dir = self.params["output_dirs"]["hub"]

        for hub_id, sensors in hub_configs.items():
            c_lines = []
            for s in sensors:
                c_lines.append(f"""    {{
        .frame_id = {s["frame_id"]},
        .sensor_type = {s["type_enum"]},
        .sample_rate = {s["rate"]},
        .byte_offset = {s["offset"]},
        .byte_length = {s["length"]},
        .hw_pin = {s["pin_pos"]}
    }}""")

            c_content = f"""// AUTO-GENERATED - HUB {hub_id}
#include "sensor_config.h"

const SensorConfig_t HUB_{hub_id}_CONFIG[{len(sensors)}] = {{
{",".join(c_lines)}
}};
"""
            with open(os.path.join(out_dir, f"hub_{hub_id}_config.c"), "w") as f:
                f.write(c_content)

    def _export_head_firmware(self, head_routes):
        """Generates the network routing tables for the STM32H7 Head Module."""
        out_dir = self.params["output_dirs"]["head"]
        c_lines = []

        for r in head_routes:
            c_lines.append(
                f"    {{ .frame_id = {r['frame_id']}, .offset = {r['offset']}, "
                f".length = {r['length']}, .scaling = {r['scaling']} }} // {r['short_name']}"
            )

        c_content = f"""// AUTO-GENERATED - STM32H7 GATEWAY ROUTING
#include "routing_table.h"

const RouteEntry_t DASH_ROUTES[{len(head_routes)}] = {{
{chr(10).join(c_lines)}
}};
"""
        with open(os.path.join(out_dir, "head_routing.c"), "w") as f:
            f.write(c_content)


if __name__ == "__main__":
    compiler = SystemCompiler(SYSTEM_PARAMS, "master_config.csv")
    compiler.build()
