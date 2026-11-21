"""Carla host check: Aloschen VST3 records then loops audio with transport running.

Rack chain: internal audiofile (impulses on bar starts, bars 0-2, then silence)
-> Aloschen VST3. Loop1 is flipped at 0.2 s; after the input goes silent the
Aloschen output peak must stay non-silent. Usage: python3 carla_vst3_loop.py INPUT.wav VST3
Exit 0 on pass.
"""

import sys
import time

sys.path.insert(0, "/usr/share/carla")
from carla_backend import (  # noqa: E402  # pyright: ignore[reportMissingImports]
    BINARY_NATIVE,
    ENGINE_OPTION_PROCESS_MODE,
    ENGINE_OPTION_TRANSPORT_MODE,
    ENGINE_PROCESS_MODE_CONTINUOUS_RACK,
    ENGINE_TRANSPORT_MODE_INTERNAL,
    PLUGIN_INTERNAL,
    PLUGIN_VST3,
    CarlaHostDLL,
)

wav, vst3 = sys.argv[1], sys.argv[2]
host = CarlaHostDLL("/usr/lib/carla/libcarla_standalone2.so", False)
drivers = [host.get_engine_driver_name(i) for i in range(host.get_engine_driver_count())]
print("drivers", drivers)
driver = next(d for d in ("Dummy", "PulseAudio", "ALSA") if d in drivers)
host.set_engine_option(ENGINE_OPTION_PROCESS_MODE, ENGINE_PROCESS_MODE_CONTINUOUS_RACK, "")
host.set_engine_option(ENGINE_OPTION_TRANSPORT_MODE, ENGINE_TRANSPORT_MODE_INTERNAL, "")
assert host.engine_init(driver, "aloschen-test"), host.get_last_error()
print("driver", driver, "rate", host.get_sample_rate())
assert host.add_plugin(BINARY_NATIVE, PLUGIN_INTERNAL, "", "", "audiofile", 0, None, 0)
host.set_custom_data(0, "http://kxstudio.sf.net/ns/carla/string", "file", wav)
assert host.add_plugin(BINARY_NATIVE, PLUGIN_VST3, vst3, "", "", 0, None, 0), host.get_last_error()
names = {}
for i in range(host.get_parameter_count(1)):
    names[host.get_parameter_info(1, i)["name"]] = i
host.set_parameter_value(1, names["Click"], 0.0)
# Carla 2.5 forwards this value to VST3 as normalized: 0.0 -> Bars = 1.
host.set_parameter_value(1, names["Bars"], 0.0)
print(
    "Bars readback",
    host.get_current_parameter_value(1, names["Bars"]),
    "Click",
    host.get_current_parameter_value(1, names["Click"]),
)
host.transport_relocate(0)
host.transport_play()
t0 = time.time()
flipped = False
late_in, late_out, max_state = 0.0, 0.0, 0.0
while (t := time.time() - t0) < 12.0:
    host.engine_idle()
    if not flipped and t > 0.2:
        host.set_parameter_value(1, names["Loop1"], 1.0)
        flipped = True
    max_state = max(max_state, host.get_current_parameter_value(1, names["Loop1 State"]))
    if t > 7.0:
        late_in = max(late_in, host.get_output_peak_value(0, True))
        late_out = max(late_out, host.get_output_peak_value(1, True))
    if int(t * 2) != int((t - 0.01) * 2):
        print(
            f"t={t:.1f} loop1={host.get_current_parameter_value(1, names['Loop1']):.0f}",
            f"state={host.get_current_parameter_value(1, names['Loop1 State']):.2f}",
            f"bpm={host.get_current_parameter_value(1, names['Host BPM']):.0f}",
            f"play={host.get_current_parameter_value(1, names['Host Playing']):.0f}",
            f"step={host.get_current_parameter_value(1, names['Bar Step']):.0f}",
            f"in={host.get_output_peak_value(0, True):.3f}",
            f"out={host.get_output_peak_value(1, True):.3f}",
        )
    time.sleep(0.01)
host.transport_pause()
print(
    f"after 7s: input peak {late_in:.3f}, output peak {late_out:.3f}",
    f"(max loop1_state {max_state:.2f})",
)
host.engine_close()
# Carla 2.5 does not read back VST3 output parameters, so state stays 0 here;
# the audio peaks are the evidence.
# Input residue (< 0.1) is 48k->44.1k resampling ringing of the impulses.
ok = late_in < 0.1 and late_out > 0.3
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
