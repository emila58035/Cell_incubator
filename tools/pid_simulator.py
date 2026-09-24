#!/usr/bin/env python3
"""Small thermal plant and PID simulator for the incubator controller.

The thermal plant is a first-order model with an optional pure delay:

    dT/dt = heat_gain * pwm_fraction - (T - ambient) / time_constant

The PID follows the firmware's direct-acting controller and output range.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from dataclasses import dataclass
from pathlib import Path


def clamp(value: float, lower: float, upper: float) -> float:
    return max(lower, min(upper, value))


@dataclass(frozen=True)
class ThermalParameters:
    ambient_c: float = 25.0
    heat_gain_c_per_s: float = 0.025
    time_constant_s: float = 600.0
    delay_s: float = 5.0


@dataclass(frozen=True)
class PIDParameters:
    conservative_kp: float = 10.0
    conservative_ki: float = 0.2
    conservative_kd: float = 0.1
    aggressive_kp: float = 20.0
    aggressive_ki: float = 0.0
    aggressive_kd: float = 0.0
    conservative_band_c: float = 3.0
    output_max: float = 255.0
    reset_integral_on_band_switch: bool = False


@dataclass
class PIDState:
    integral: float = 0.0
    previous_temperature: float | None = None
    aggressive: bool | None = None


def pid_step(
    temperature_c: float,
    setpoint_c: float,
    dt_s: float,
    params: PIDParameters,
    state: PIDState,
) -> float:
    """Calculate one controller output and update the PID state."""
    error = setpoint_c - temperature_c
    aggressive = abs(error) >= params.conservative_band_c

    if state.aggressive is not None and aggressive != state.aggressive:
        if params.reset_integral_on_band_switch:
            state.integral = 0.0
    state.aggressive = aggressive

    if aggressive:
        kp, ki, kd = (
            params.aggressive_kp,
            params.aggressive_ki,
            params.aggressive_kd,
        )
    else:
        kp, ki, kd = (
            params.conservative_kp,
            params.conservative_ki,
            params.conservative_kd,
        )

    state.integral += ki * error * dt_s
    state.integral = clamp(state.integral, 0.0, params.output_max)

    if state.previous_temperature is None:
        d_temperature = 0.0
    else:
        d_temperature = temperature_c - state.previous_temperature
    state.previous_temperature = temperature_c

    output = kp * error + state.integral - kd * d_temperature
    return clamp(output, 0.0, params.output_max)


def simulate(
    duration_s: float,
    dt_s: float,
    initial_temperature_c: float,
    setpoint_c: float,
    thermal: ThermalParameters,
    pid: PIDParameters,
) -> list[dict[str, float]]:
    if duration_s <= 0 or dt_s <= 0:
        raise ValueError("duration and dt must be greater than zero")
    if thermal.time_constant_s <= 0:
        raise ValueError("time constant must be greater than zero")
    if thermal.delay_s < 0:
        raise ValueError("delay cannot be negative")

    temperature = initial_temperature_c
    pid_state = PIDState()
    delay_steps = max(0, round(thermal.delay_s / dt_s))
    pwm_history = [0.0] * (delay_steps + 1)
    records: list[dict[str, float]] = []
    steps = round(duration_s / dt_s)

    for step in range(steps + 1):
        elapsed_s = step * dt_s
        pwm = pid_step(temperature, setpoint_c, dt_s, pid, pid_state)
        delayed_pwm = pwm_history.pop(0)
        pwm_history.append(pwm)

        records.append(
            {
                "time_s": elapsed_s,
                "temperature_c": temperature,
                "setpoint_c": setpoint_c,
                "pwm": pwm,
                "error_c": setpoint_c - temperature,
                "integral": pid_state.integral,
            }
        )

        temperature_rate = (
            thermal.heat_gain_c_per_s * delayed_pwm / pid.output_max
            - (temperature - thermal.ambient_c) / thermal.time_constant_s
        )
        temperature += temperature_rate * dt_s

    return records


def write_csv(path: Path, records: list[dict[str, float]]) -> None:
    fields = ["time_s", "temperature_c", "setpoint_c", "pwm", "error_c", "integral"]
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        writer.writerows(records)


def plot_records(records: list[dict[str, float]]) -> None:
    try:
        import matplotlib.pyplot as plt
    except ImportError as exc:
        raise SystemExit("--plot requires matplotlib: python -m pip install matplotlib") from exc

    time_s = [row["time_s"] for row in records]
    temperature = [row["temperature_c"] for row in records]
    setpoint = [row["setpoint_c"] for row in records]
    pwm = [row["pwm"] for row in records]

    figure, (temperature_axis, pwm_axis) = plt.subplots(
        2, 1, sharex=True, figsize=(10, 6), constrained_layout=True
    )
    temperature_axis.plot(time_s, temperature, label="temperature")
    temperature_axis.plot(time_s, setpoint, "--", label="setpoint")
    temperature_axis.set_ylabel("Temperature (C)")
    temperature_axis.grid(True, alpha=0.3)
    temperature_axis.legend()

    pwm_axis.plot(time_s, pwm, color="darkorange", label="PWM")
    pwm_axis.set_xlabel("Time (s)")
    pwm_axis.set_ylabel("PWM")
    pwm_axis.set_ylim(0, 255)
    pwm_axis.grid(True, alpha=0.3)
    pwm_axis.legend()
    plt.show()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Simulate the incubator thermal PID loop")
    parser.add_argument("--duration", type=float, default=1800.0, help="simulation time in seconds")
    parser.add_argument("--dt", type=float, default=1.0, help="simulation step in seconds")
    parser.add_argument("--initial-temperature", type=float, default=25.0)
    parser.add_argument("--setpoint", type=float, default=37.0)
    parser.add_argument("--ambient", type=float, default=25.0)
    parser.add_argument("--heat-gain", type=float, default=0.025, help="C/s at PWM=255")
    parser.add_argument("--time-constant", type=float, default=600.0, help="thermal time constant in seconds")
    parser.add_argument("--delay", type=float, default=5.0, help="heater-to-sensor delay in seconds")
    parser.add_argument("--conservative-kp", type=float, default=10.0)
    parser.add_argument("--conservative-ki", type=float, default=0.2)
    parser.add_argument("--conservative-kd", type=float, default=0.1)
    parser.add_argument("--aggressive-kp", type=float, default=20.0)
    parser.add_argument("--aggressive-ki", type=float, default=0.0)
    parser.add_argument("--aggressive-kd", type=float, default=0.0)
    parser.add_argument("--band", type=float, default=3.0, help="conservative band in C")
    parser.add_argument(
        "--reset-integral-on-band-switch",
        action="store_true",
        help="clear the integral term when changing tuning bands",
    )
    parser.add_argument("--output", type=Path, default=Path("pid_simulation.csv"))
    parser.add_argument("--plot", action="store_true", help="show temperature and PWM plots")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        records = simulate(
            duration_s=args.duration,
            dt_s=args.dt,
            initial_temperature_c=args.initial_temperature,
            setpoint_c=args.setpoint,
            thermal=ThermalParameters(
                ambient_c=args.ambient,
                heat_gain_c_per_s=args.heat_gain,
                time_constant_s=args.time_constant,
                delay_s=args.delay,
            ),
            pid=PIDParameters(
                conservative_kp=args.conservative_kp,
                conservative_ki=args.conservative_ki,
                conservative_kd=args.conservative_kd,
                aggressive_kp=args.aggressive_kp,
                aggressive_ki=args.aggressive_ki,
                aggressive_kd=args.aggressive_kd,
                conservative_band_c=args.band,
                reset_integral_on_band_switch=args.reset_integral_on_band_switch,
            ),
        )
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    write_csv(args.output, records)
    temperatures = [row["temperature_c"] for row in records]
    print(f"wrote {len(records)} samples to {args.output}")
    print(
        "temperature range: "
        f"{min(temperatures):.2f} .. {max(temperatures):.2f} C; "
        f"final: {temperatures[-1]:.2f} C"
    )
    if args.plot:
        plot_records(records)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
