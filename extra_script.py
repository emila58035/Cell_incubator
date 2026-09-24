from pathlib import Path


Import("env")

sdk = Path(env.subst("$PROJECT_PACKAGES_DIR")) / "framework-arduinoespressif32" / "tools" / "sdk" / "esp32s3"
env.Append(LIBPATH=[str(sdk / "lib")])
env.Append(LIBS=["esp_websocket_client"])
