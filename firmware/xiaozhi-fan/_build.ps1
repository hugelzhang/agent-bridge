$env:IDF_PATH = "D:\Espressif\frameworks\esp-idf-v5.5.4"
$env:IDF_PYTHON_ENV_PATH = "D:\Espressif\python_env\idf5.5_py3.11_env"
$env:IDF_TOOLS_PATH = "D:\Espressif"
$env:ESP_ROM_ELF_DIR = "D:\Espressif\tools\esp-rom-elfs\20241011"
$env:PATH = "D:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;D:\Espressif\tools\idf-exe\1.0.3;D:\Espressif\tools\ninja\1.12.1;D:\Espressif\tools\cmake\3.30.2\bin;$env:IDF_PYTHON_ENV_PATH\Scripts;$env:PATH"

Set-Location X:\phase7-ai-fan-voice

Write-Host "[1/3] set-target esp32s3"
& D:\Espressif\tools\idf-exe\1.0.3\idf.py.exe set-target esp32s3
if ($LASTEXITCODE -ne 0) { Write-Host "SET-TARGET FAILED"; pause; exit 1 }

Write-Host "[2/3] reconfigure (Ninja response files)"
& D:\Espressif\tools\idf-exe\1.0.3\idf.py.exe -DCMAKE_NINJA_FORCE_RESPONSE_FILE=ON reconfigure
if ($LASTEXITCODE -ne 0) { Write-Host "RECONFIGURE FAILED"; pause; exit 1 }

Write-Host "[3/3] build"
& D:\Espressif\tools\idf-exe\1.0.3\idf.py.exe build
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED"; pause; exit 1 }

Write-Host "=== BUILD SUCCESS ==="
pause
