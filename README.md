# HASHCAT wrapper project

Эта папка — локальная оболочка для сборки hashcat из исходников.

`source` хранит обычный upstream checkout hashcat. Корень этой папки хранит только локальную сборочную обвязку, патчи и готовые артефакты. Поэтому `source` можно обновлять или заменять новой версией hashcat без потери локальных `BUILD / CLEAN / UPDATE`.

## Структура

```text
HASHCAT\
  BUILD.ps1
  CLEAN.ps1
  UPDATE.ps1
  README.md
  patches\
    hashcat-android-ndk.patch
  source\
    upstream hashcat checkout
  build\
    arm64-v8a\
      hashcat
      modules\*.so
      bridges\*.so
      feeds\*.so
      OpenCL\*
      rules\*
      tunings\*
      pcfg\*
      hashcat.hcstat2
```

## Основные команды

Собрать Android `arm64-v8a`:

```powershell
.\BUILD.ps1
```

Очистить локальную сборку:

```powershell
.\CLEAN.ps1
```

Обновить upstream checkout в `source`:

```powershell
.\UPDATE.ps1
```

Обновить и сразу собрать:

```powershell
.\UPDATE.ps1 -BuildAfterUpdate $true
```

## Настройка путей инструментов

Пути к самому проекту вычисляются относительно расположения скрипта через `$PSScriptRoot`. Поэтому папку `HASHCAT` можно переносить.

`BUILD.ps1` ищет MSYS2 и Android NDK так:

1. Явные параметры `-Msys2Bash` и `-AndroidNdk`.
2. Переменные окружения `MSYS2_ROOT`, `ANDROID_NDK_HOME`, `ANDROID_NDK_ROOT`.
3. `ANDROID_HOME` или `ANDROID_SDK_ROOT`, если внутри них есть папка `ndk`.
4. Для MSYS2 дополнительно проверяется `bash.exe` из `PATH` и стандартные `C:\msys64` / `C:\msys32`.

Пример с явными путями:

```powershell
.\BUILD.ps1 -Msys2Bash "C:\msys64\usr\bin\bash.exe" -AndroidNdk "C:\Android\Sdk\ndk\29.0.14206865"
```

## Что делает BUILD

`BUILD.ps1`:

1. Берёт исходники из `source`.
2. Временно применяет локальный patch из `patches\hashcat-android-ndk.patch`.
3. Запускает native-сборку через MSYS2 + Android NDK.
4. Складывает результат в `build\<abi>`.
5. Откатывает временный patch, чтобы `source` оставался чистым upstream checkout.

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

Скрипт не удаляет корневые `BUILD.ps1`, `CLEAN.ps1`, `UPDATE.ps1`, `patches`, `build` и `README.md`, потому что они не являются частью upstream hashcat.

## Важное правило

Этот проект занимается только сборкой hashcat и складывает результат в свою папку `build`. Он не публикует файлы в другие проекты и не знает о проектах-потребителях. Любой внешний проект должен сам забирать готовые файлы из `build\<abi>` своим собственным импорт-скриптом.