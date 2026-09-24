#!/usr/bin/env python3
"""Interactive Qt front-end for the thermal PID simulator."""

from __future__ import annotations

import sys
from pathlib import Path

from PySide6 import QtCore, QtWidgets
import pyqtgraph as pg

try:
    from .pid_simulator import PIDParameters, ThermalParameters, simulate, write_csv
except ImportError:
    from pid_simulator import PIDParameters, ThermalParameters, simulate, write_csv


class PIDSimulatorWindow(QtWidgets.QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("加热 PID 仿真")
        self.resize(1280, 780)
        self.fields: dict[str, QtWidgets.QDoubleSpinBox | QtWidgets.QSpinBox] = {}
        self.update_timer = QtCore.QTimer(self)
        self.update_timer.setSingleShot(True)
        self.update_timer.timeout.connect(self.recompute)
        self.records: list[dict[str, float]] = []

        pg.setConfigOptions(antialias=True, background="w", foreground="k")
        self._build_ui()
        self.recompute()

    def _build_ui(self) -> None:
        root = QtWidgets.QWidget()
        self.setCentralWidget(root)
        layout = QtWidgets.QHBoxLayout(root)

        controls = QtWidgets.QScrollArea()
        controls.setWidgetResizable(True)
        controls.setMinimumWidth(330)
        controls.setMaximumWidth(390)
        controls.setWidget(self._build_controls())
        layout.addWidget(controls)

        chart_panel = QtWidgets.QWidget()
        chart_layout = QtWidgets.QVBoxLayout(chart_panel)
        self.temperature_plot = pg.PlotWidget(title="温度响应")
        self.temperature_plot.setLabel("left", "温度", units="°C")
        self.temperature_plot.setLabel("bottom", "时间", units="s")
        self.temperature_plot.showGrid(x=True, y=True, alpha=0.25)
        self.temperature_curve = self.temperature_plot.plot(
            pen=pg.mkPen("#d1495b", width=2), name="实际温度"
        )
        self.setpoint_curve = self.temperature_plot.plot(
            pen=pg.mkPen("#277da1", width=2, style=QtCore.Qt.PenStyle.DashLine),
            name="设定温度",
        )
        chart_layout.addWidget(self.temperature_plot, stretch=3)

        self.pwm_plot = pg.PlotWidget(title="加热输出")
        self.pwm_plot.setLabel("left", "PWM")
        self.pwm_plot.setLabel("bottom", "时间", units="s")
        self.pwm_plot.setYRange(0, 255)
        self.pwm_plot.showGrid(x=True, y=True, alpha=0.25)
        self.pwm_curve = self.pwm_plot.plot(pen=pg.mkPen("#f28e2b", width=2))
        chart_layout.addWidget(self.pwm_plot, stretch=2)

        self.metrics = QtWidgets.QLabel()
        self.metrics.setStyleSheet("padding: 6px; color: #333;")
        chart_layout.addWidget(self.metrics)
        layout.addWidget(chart_panel, stretch=1)

    def _build_controls(self) -> QtWidgets.QWidget:
        panel = QtWidgets.QWidget()
        layout = QtWidgets.QVBoxLayout(panel)
        layout.setAlignment(QtCore.Qt.AlignmentFlag.AlignTop)

        layout.addWidget(QtWidgets.QLabel("仿真参数"))
        simulation = QtWidgets.QFormLayout()
        self._add_double(simulation, "duration", "仿真时长 (s)", 60, 86400, 1800, 10)
        self._add_double(simulation, "dt", "步长 (s)", 0.05, 10, 1, 0.05)
        self._add_double(simulation, "initial", "初始温度 (°C)", -40, 85, 25, 0.1)
        self._add_double(simulation, "setpoint", "设定温度 (°C)", 0, 100, 37, 0.1)
        layout.addLayout(simulation)

        layout.addWidget(QtWidgets.QLabel("热模型"))
        thermal = QtWidgets.QFormLayout()
        self._add_double(thermal, "ambient", "环境温度 (°C)", -40, 85, 25, 0.1)
        self._add_double(thermal, "heat_gain", "满 PWM 升温速率 (°C/s)", 0.0001, 1, 0.025, 0.001)
        self._add_double(thermal, "time_constant", "热惯性 (s)", 1, 100000, 600, 10)
        self._add_double(thermal, "delay", "响应延迟 (s)", 0, 1000, 5, 1)
        layout.addLayout(thermal)

        layout.addWidget(QtWidgets.QLabel("PID 参数"))
        pid = QtWidgets.QFormLayout()
        self._add_double(pid, "conservative_kp", "保守 Kp", 0, 1000, 10, 0.1)
        self._add_double(pid, "conservative_ki", "保守 Ki", 0, 100, 0.2, 0.01)
        self._add_double(pid, "conservative_kd", "保守 Kd", 0, 100, 0.1, 0.01)
        self._add_double(pid, "aggressive_kp", "激进 Kp", 0, 1000, 20, 0.1)
        self._add_double(pid, "aggressive_ki", "激进 Ki", 0, 100, 0, 0.01)
        self._add_double(pid, "aggressive_kd", "激进 Kd", 0, 100, 0, 0.01)
        self._add_double(pid, "band", "保守区间 (°C)", 0, 20, 3, 0.1)
        layout.addLayout(pid)

        self.reset_integral = QtWidgets.QCheckBox("切换增益区间时清除积分")
        self.reset_integral.setChecked(False)
        self.reset_integral.stateChanged.connect(self.schedule_recompute)
        layout.addWidget(self.reset_integral)

        buttons = QtWidgets.QHBoxLayout()
        export = QtWidgets.QPushButton("导出 CSV")
        export.clicked.connect(self.export_csv)
        reset = QtWidgets.QPushButton("恢复默认")
        reset.clicked.connect(self.restore_defaults)
        buttons.addWidget(export)
        buttons.addWidget(reset)
        layout.addLayout(buttons)
        return panel

    def _add_double(
        self,
        form: QtWidgets.QFormLayout,
        key: str,
        label: str,
        minimum: float,
        maximum: float,
        value: float,
        step: float,
    ) -> None:
        widget = QtWidgets.QDoubleSpinBox()
        widget.setRange(minimum, maximum)
        widget.setValue(value)
        widget.setSingleStep(step)
        widget.setDecimals(3 if step < 0.01 else 2)
        widget.valueChanged.connect(self.schedule_recompute)
        self.fields[key] = widget
        form.addRow(label, widget)

    def value(self, key: str) -> float:
        return float(self.fields[key].value())

    def schedule_recompute(self) -> None:
        self.update_timer.start(80)

    def recompute(self) -> None:
        thermal = ThermalParameters(
            ambient_c=self.value("ambient"),
            heat_gain_c_per_s=self.value("heat_gain"),
            time_constant_s=self.value("time_constant"),
            delay_s=self.value("delay"),
        )
        pid = PIDParameters(
            conservative_kp=self.value("conservative_kp"),
            conservative_ki=self.value("conservative_ki"),
            conservative_kd=self.value("conservative_kd"),
            aggressive_kp=self.value("aggressive_kp"),
            aggressive_ki=self.value("aggressive_ki"),
            aggressive_kd=self.value("aggressive_kd"),
            conservative_band_c=self.value("band"),
            reset_integral_on_band_switch=self.reset_integral.isChecked(),
        )
        self.records = simulate(
            duration_s=self.value("duration"),
            dt_s=self.value("dt"),
            initial_temperature_c=self.value("initial"),
            setpoint_c=self.value("setpoint"),
            thermal=thermal,
            pid=pid,
        )
        time_s = [row["time_s"] for row in self.records]
        temperature = [row["temperature_c"] for row in self.records]
        setpoint = [row["setpoint_c"] for row in self.records]
        pwm = [row["pwm"] for row in self.records]
        self.temperature_curve.setData(time_s, temperature)
        self.setpoint_curve.setData(time_s, setpoint)
        self.pwm_curve.setData(time_s, pwm)

        maximum = max(temperature)
        final_error = self.records[-1]["error_c"]
        overshoot = max(0.0, maximum - self.value("setpoint"))
        self.metrics.setText(
            f"最高温度：{maximum:.2f} °C　过冲：{overshoot:.2f} °C　"
            f"最终误差：{final_error:+.2f} °C　最终 PWM：{pwm[-1]:.1f}"
        )

    def restore_defaults(self) -> None:
        defaults = {
            "duration": 1800,
            "dt": 1,
            "initial": 25,
            "setpoint": 37,
            "ambient": 25,
            "heat_gain": 0.025,
            "time_constant": 600,
            "delay": 5,
            "conservative_kp": 10,
            "conservative_ki": 0.2,
            "conservative_kd": 0.1,
            "aggressive_kp": 20,
            "aggressive_ki": 0,
            "aggressive_kd": 0,
            "band": 3,
        }
        for key, value in defaults.items():
            self.fields[key].setValue(value)
        self.reset_integral.setChecked(False)

    def export_csv(self) -> None:
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "导出仿真数据", "pid_simulation.csv", "CSV 文件 (*.csv)"
        )
        if path:
            write_csv(Path(path), self.records)


def main() -> int:
    app = QtWidgets.QApplication(sys.argv)
    window = PIDSimulatorWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
