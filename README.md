# HASHCAT wrapper project

Эта папка — локальная оболочка для сборки hashcat из исходников.

`source` хранит обычный upstream checkout hashcat. Корень этой папки хранит локальную сборочную обвязку, патчи и готовые артефакты. Поэтому `source` можно обновлять или заменять новой версией hashcat без потери локальных `BUILD / CLEAN / UPDATE`.

## Структура

```text
HASHCAT\
  scripts\
    BUILD.ps1
    BUILD-DESKTOP.ps1
    CLEAN.ps1
    UPDATE.ps1
  README.md
  patches\
    hashcat-android-ndk.patch
  source\
    upstream hashcat checkout
  build\
    arm64-v8a\
      Android native runtime
    windows-x64\
      Windows portable runtime
    linux-x64\
      Linux portable runtime
    macos-x64\
      macOS Intel portable runtime
    macos-arm64\
      macOS Apple Silicon portable runtime
    macos-universal\
      macOS universal portable runtime
  release\
    hashcat-<version>-<platform>.zip
```

## Основные команды

Единый `BUILD.ps1` собирает выбранные target'ы. По умолчанию он собирает Android `arm64-v8a` и Windows x64 portable runtime:

```powershell
.\scripts\BUILD.ps1
```

Собрать только Android `arm64-v8a`:

```powershell
.\scripts\BUILD.ps1 -Target android-arm64-v8a
```

Собрать только Windows x64 portable runtime:

```powershell
.\scripts\BUILD.ps1 -Target windows-x64
```

Собрать Windows x64 с полной очисткой upstream native-выходов перед сборкой:

```powershell
.\scripts\BUILD.ps1 -Target windows-x64 -Clean
```

Собрать Linux x64 через WSL или на Linux-хосте:

```powershell
.\scripts\BUILD.ps1 -Target linux-x64
```

Собрать macOS на macOS-хосте с PowerShell Core:

```powershell
.\scripts\BUILD.ps1 -Target macos-x64
.\scripts\BUILD.ps1 -Target macos-arm64
.\scripts\BUILD.ps1 -Target macos-universal
```

По умолчанию desktop target'ы также создают zip-пакет в `release`. Это можно отключить:

```powershell
.\scripts\BUILD.ps1 -Target windows-x64 -Package:$false
```

Очистить локальную сборку:

```powershell
.\scripts\CLEAN.ps1
```

Обновить upstream checkout в `source`:

```powershell
.\scripts\UPDATE.ps1
```

Обновить и сразу собрать стандартные target'ы:

```powershell
.\scripts\UPDATE.ps1 -BuildAfterUpdate $true
```
## Android build

`BUILD.ps1 -Target android-arm64-v8a` собирает Android runtime для приложения:

1. Берёт исходники из `source`.
2. Временно применяет локальный patch из `patches\hashcat-android-ndk.patch`.
3. Запускает native-сборку через MSYS2 + Android NDK.
4. Складывает результат в `build\<abi>`.
5. Откатывает временный patch, чтобы `source` оставался обычным upstream checkout.

Результат Android `arm64-v8a` после сборки:

```text
build\arm64-v8a\hashcat
build\arm64-v8a\modules\*.so
build\arm64-v8a\bridges\*.so
build\arm64-v8a\feeds\*.so
build\arm64-v8a\OpenCL\*
build\arm64-v8a\rules\*
build\arm64-v8a\tunings\*
build\arm64-v8a\pcfg\*
build\arm64-v8a\hashcat.hcstat2
```

## Desktop build

BUILD.ps1 -Target <platform> вызывает desktop-сборку и складывает portable runtime в uild\<platform>. BUILD-DESKTOP.ps1 остаётся внутренним helper-скриптом для desktop target'ов.

Поддерживаемые цели:

| Target | Где выполняется | Результат |
|---|---|---|
| `windows-x64` | Windows + MSYS2 MINGW64 | `build\windows-x64` |
| `linux-x64` | Windows + WSL или Linux + PowerShell Core | `build\linux-x64` |
| `macos-x64` | macOS + PowerShell Core | `build\macos-x64` |
| `macos-arm64` | macOS + PowerShell Core | `build\macos-arm64` |
| `macos-universal` | macOS + PowerShell Core | `build\macos-universal` |

Portable runtime содержит:

```text
hashcat / hashcat.exe
hashcat.dll или libhashcat.* если upstream Makefile производит shared core
modules\*
bridges\*
feeds\*
OpenCL\*
rules\*
tunings\*
pcfg\*
charsets\*
masks\*
docs\*
hashcat.hcstat2
example*.hash / example*.cmd / example*.sh / example.dict
```

При использовании `-Package` скрипт создаёт zip-файл:

```text
release\hashcat-<version>-<platform>.zip
```

## Настройка путей инструментов

Пути к самому проекту вычисляются относительно расположения скрипта через `$PSScriptRoot`. Поэтому папку `HASHCAT` можно переносить.

`BUILD.ps1` ищет MSYS2 и Android NDK так:

1. Явные параметры `-Msys2Bash` и `-AndroidNdk`.
2. Переменные окружения `MSYS2_ROOT`, `ANDROID_NDK_HOME`, `ANDROID_NDK_ROOT`.
3. `ANDROID_HOME` или `ANDROID_SDK_ROOT`, если внутри них есть папка `ndk`.
4. Для MSYS2 дополнительно проверяется `bash.exe` из `PATH` и стандартные `C:\msys64` / `C:\msys32`.

Пример Android-сборки с явными путями:

```powershell
.\scripts\BUILD.ps1 -Msys2Bash "C:\msys64\usr\bin\bash.exe" -AndroidNdk "C:\Android\Sdk\ndk\29.0.14206865"
```

`BUILD-DESKTOP.ps1` ищет MSYS2 так:

1. Явный параметр `-Msys2Bash`.
2. Явный параметр `-Msys2Root`.
3. Переменная окружения `MSYS2_ROOT`.
4. `bash.exe` из `PATH`.
5. Стандартные `C:\msys64` / `C:\msys32`.

Пример Windows-сборки с явным MSYS2:

```powershell
.\scripts\BUILD-DESKTOP.ps1 -Target windows-x64 -Msys2Bash "C:\msys64\usr\bin\bash.exe" -Package
```

Для Windows target в MSYS2 нужны пакеты:

```bash
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-make make python3
```

Для Linux target через WSL внутри дистрибутива нужны обычные зависимости upstream hashcat, включая `make`, `gcc`, `g++` и Python 3.

## Что делает CLEAN

`CLEAN.ps1` удаляет локальные native-результаты и промежуточные файлы:

```text
build\
source\hashcat
source\modules
source\bridges
source\feeds
source\obj
source\.tmp
```

После очистки `build` остаётся пустой папкой.

## Что делает UPDATE

`UPDATE.ps1` выполняет `git fetch` и `git pull --ff-only` внутри `source`.

Скрипт не удаляет корневые `BUILD.ps1`, `BUILD-DESKTOP.ps1`, `CLEAN.ps1`, `UPDATE.ps1`, `patches`, `build`, `release` и `README.md`, потому что они не являются частью upstream hashcat.

## Важное правило

Этот проект занимается только сборкой hashcat и складывает результат в свою папку `build`. Он не публикует файлы в другие проекты и не знает о проектах-потребителях. Любой внешний проект должен сам забирать готовые файлы из `build\<platform>` своим собственным импорт-скриптом.
## Build scripts

All project scripts live in `scripts\`.

Unified entry point:

```powershell
.\scripts\BUILD.ps1
```

Target-specific wrappers:

```powershell
.\scripts\BUILD-ANDROID.ps1
.\scripts\BUILD-WINDOWS.ps1
.\scripts\BUILD-LINUX.ps1
.\scripts\BUILD-MACOS-X64.ps1
.\scripts\BUILD-MACOS-ARM64.ps1
.\scripts\BUILD-MACOS-UNIVERSAL.ps1
.\scripts\BUILD-ALL.ps1
```

`BUILD.ps1` reads local machine paths from `local.properties`; currently this is used for `android.ndk`.
`local.properties` is intentionally ignored by git.

Windows packaging uses a temporary staging copy under `build\.package-staging` before creating the zip. This avoids PowerShell `Compress-Archive` races where files can disappear while the archive is being built.

Only one HASHCAT build/update/clean process should run at a time. `BUILD.ps1` uses a project mutex to prevent concurrent builds from touching the same `source`, `build`, and `release` directories.