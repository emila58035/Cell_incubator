# PID 仿真工具

运行默认仿真 30 分钟：

```powershell
C:\Users\Emila_58035\.platformio\penv\Scripts\python.exe tools\pid_simulator.py --output pid_simulation.csv
```

查看温度和 PWM 曲线，需要安装 `matplotlib`：

```powershell
C:\Users\Emila_58035\.platformio\penv\Scripts\python.exe -m pip install matplotlib
C:\Users\Emila_58035\.platformio\penv\Scripts\python.exe tools\pid_simulator.py --plot
```

比较切换增益区间时是否清除积分：

```powershell
C:\Users\Emila_58035\.platformio\penv\Scripts\python.exe tools\pid_simulator.py --output keep.csv
C:\Users\Emila_58035\.platformio\penv\Scripts\python.exe tools\pid_simulator.py --reset-integral-on-band-switch --output reset.csv
```

热模型参数：

- `--ambient`：环境温度，单位 ℃
- `--heat-gain`：PWM=255 时每秒提供的升温速率，单位 ℃/s
- `--time-constant`：热惯性时间常数，单位 s
- `--delay`：加热器到传感器的延迟，单位 s

PID 参数可通过 `--conservative-kp`、`--conservative-ki`、`--conservative-kd` 和对应的 `--aggressive-*` 参数调整。输出 CSV 包含时间、温度、设定温度、PWM、误差和积分项。
