@echo off
rem Double click to open the GrainWatch V2 provisioning tool (provision_device.py).
rem Uses whichever Python this PC has, without a console window. Missing packages are installed on first run.
cd /d "%~dp0"

where pyw >nul 2>nul && (start "" pyw "%~dp0provision_device.py" & exit /b 0)
where uv >nul 2>nul && (start "" /min uv run --no-project --with pyserial==3.5 --with mpy-cross==1.18 pythonw "%~dp0provision_device.py" & exit /b 0)
where pythonw >nul 2>nul && (start "" pythonw "%~dp0provision_device.py" & exit /b 0)

echo Python wasn't found on this PC.
echo Install Python 3.11 or newer from https://www.python.org (tick "Add python.exe to PATH"), then try again.
pause
