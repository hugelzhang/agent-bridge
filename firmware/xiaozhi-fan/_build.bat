@echo off
set IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.5.4
set IDF_PYTHON_ENV_PATH=D:\Espressif\python_env\idf5.5_py3.11_env
set IDF_TOOLS_PATH=D:\Espressif
set ESP_ROM_ELF_DIR=D:\Espressif\tools\esp-rom-elfs\20241011
set PATH=D:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;D:\Espressif\tools\idf-exe\1.0.3;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\cmake\3.30.2\bin;%IDF_PYTHON_ENV_PATH%\Scripts;%PATH%

X:
cd X:\phase7-ai-fan-voice

echo [1/3] set-target esp32s3
D:\Espressif\tools\idf-exe\1.0.3\idf.py.exe set-target esp32s3
if %ERRORLEVEL% NEQ 0 (echo SET-TARGET FAILED & pause & exit /b 1)

echo.
echo [2/3] reconfigure (Ninja response files)
D:\Espressif\tools\idf-exe\1.0.3\idf.py.exe -DCMAKE_NINJA_FORCE_RESPONSE_FILE=ON reconfigure
if %ERRORLEVEL% NEQ 0 (echo RECONFIGURE FAILED & pause & exit /b 1)

echo.
echo [3/3] build
D:\Espressif\tools\idf-exe\1.0.3\idf.py.exe build
if %ERRORLEVEL% NEQ 0 (echo BUILD FAILED & pause & exit /b 1)

echo.
echo === BUILD SUCCESS ===
pause
