# HASHCAT

Форк hashcat для Android. Из этого репозитория собирается рантайм, который
упаковывается в приложение [HASHCAT-APP](https://github.com/rokkystudio/HASHCAT-APP):
сам исполняемый файл, модули хеш-режимов, ядра OpenCL, правила и тюнинги.

Отдельного репозитория с Android-исходниками нет: работа над hashcat ведётся
прямо здесь, в `source/`. Скрипты в `scripts/` собирают и упаковывают рантайм
из этого дерева.

**Это форк, а не зеркало.** Синхронизация с upstream отключена, локальные
изменения перечислены в [docs/FORK.md](docs/FORK.md).

## Что собирается

Четыре Android-архива, по одному на ABI: `arm64-v8a`, `armeabi-v7a`, `x86`,
`x86_64`. Версия задаётся `-VersionTag` (сейчас `v7.1.2`), имя архива —
`hashcat-v7.1.2-android-<abi>.zip`, размер 95–100 МБ на ABI.

Каждый архив содержит `hashcat`, `modules/`, `bridges/`, `OpenCL/`, `rules/`,
`tunings/`, `hashcat.hcstat2` и `libc++_shared.so`. В приложении `hashcat`
переименовывается в `libhashcat_exec.so` и запускается как обычный CLI-процесс.

## Требования

* Windows (сборка проверена на Windows 10/11);
* MSYS2 с `bash`, `make`, `gcc`/`clang` — используется как среда сборки;
* Android NDK (по умолчанию берётся из `D:\ANDROID\SDK\ndk\<версия>`, либо
  задаётся параметром `-AndroidNdk`);
* Git и PowerShell 5.1+.

## Сборка

Android arm64:

```powershell
.\scripts\BUILD-ANDROID-ARM64.ps1
```

Один произвольный ABI:

```powershell
.\scripts\BUILD-ANDROID.ps1 -Abi armeabi-v7a
```

Все четыре ABI:

```powershell
.\scripts\BUILD-ANDROID.ps1 -Abi all
```

Все цели, включая десктопные:

```powershell
.\scripts\BUILD-ALL.ps1
```

Полезные параметры: `-AndroidApi` (по умолчанию `26`), `-VersionTag`,
`-Package:$false` (собрать без упаковки в ZIP), `-Clean`, `-MakeJobs`.

Очистка:

```powershell
.\scripts\CLEAN.ps1
```

Результат: каталог сборки `build\<abi>\` и архив `release\hashcat-<версия>-android-<abi>.zip`.

## CLI-контракт для приложения

Помимо обычных ключей hashcat, форк добавляет машиночитаемое описание
backend'ов и явный выбор backend'а:

```powershell
hashcat --backend-info-json        # JSON в stdout, код возврата 0
hashcat --backend-ignore-vulkan    # не инициализировать Vulkan
hashcat --backend-ignore-opencl    # не инициализировать OpenCL
```

Пример вывода:

```json
{
  "schema": 1,
  "hashcat_version": "v7.1.2",
  "backends": [
    { "id": "opencl", "detected": true, "schedulable": true,
      "devices": [ { "id": 1, "name": "QUALCOMM Adreno(TM)", "type": "GPU",
                     "vendor_id": 2147483648, "backend": "opencl" } ] },
    { "id": "vulkan", "detected": false, "schedulable": false, "devices": [] }
  ],
  "modes": {}
}
```

`detected` — интерфейс открылся и найдено хотя бы одно устройство;
`schedulable` — на backend'е можно запускать задачи; `devices[].id` — то, что
принимает `-d`. Полное описание полей и кодов возврата —
[docs/backend-cli-contract.md](docs/backend-cli-contract.md).

## Особенности Android / Adreno

Компилятор OpenCL на Adreno не линкует pure-ядра ряда режимов и возвращает
`CL_OUT_OF_HOST_MEMORY`, хотя optimized-вариант того же режима собирается и
работает. Патч P3 предпочитает optimized-ядро на устройствах Qualcomm:
режимы без optimized-варианта остаются на pure-ядре, ограничение длины пароля
hashcat печатает сам.

Отключить поведение можно переменной окружения:

```powershell
$env:HASHCAT_DISABLE_ADRENO_OPTIMIZED = '1'
```

Подробности и обязательные проверки — [docs/FORK.md](docs/FORK.md).

## Vulkan

В `source/` есть слой Vulkan 1.0: каталог физических устройств, память,
дескрипторы, пайплайны, очереди, fences и SPIR-V-примитивы хешей. С M3 Vulkan
исполняет один настоящий режим: `-m 0` (MD5), `-a 0`, wordlist, без правил.
Кандидаты длиной 1-55 байт (один блок MD5); всё остальное (другие режимы,
`-a 3/6/7`, `-r`, `-O`, `--increment`, brain, benchmark) отклоняется с
объяснением до запуска ядер.

Обычная Android-сборка включает Vulkan в штатный фронтенд: `scripts\BUILD-ANDROID.ps1`
передаёт `ENABLE_VULKAN=1`, поэтому Vulkan линкуется в `hashcat` и попадает в
обычные архивы `release/`, а не только в диагностический каталог. `-NoVulkan`
сохраняет старую OpenCL-only сборку. Диагностический вариант
`-EnableVulkanDiscovery` (только Android, `-Package:$false`) продолжает работать
как раньше.

`--backend-info-json` сообщает `vulkan.schedulable: true` только если
зарегистрирована программа режима и прошёл self-check устройства. Подробности,
ABI, границы режима и команды воспроизведения - `docs/vulkan-backend-md5-straight.md`,
планы и предыдущие этапы - `docs/vulkan-backend-*.md`.

## Регресс OpenCL

Поведение OpenCL сравнивается с эталоном, чтобы любое изменение в нативной части
можно было проверить:

```powershell
.\tools\check-opencl-regression.ps1 -HashcatExecutable build\windows-x64\hashcat.exe -Baseline docs\opencl-baseline.json
```

Эталон (`docs/opencl-baseline.json`) хранит список устройств и скорости трёх
режимов для ПК и для Android. Переснять:

```powershell
.\tools\capture-opencl-baseline.ps1 -HashcatExecutable build\arm64-v8a -Transport adb -AdbSerial <serial> -HostName android -Label 'REDMAGIC 7S Pro, Android 13'
```

Коды возврата: `0` — совпадает, `1` — разошёлся состав устройств, `2` — падение
скорости сверх порога, `3` — ошибка вызова.

## Структура

```text
HASHCAT/
  source/      исходники hashcat, которые мы собираем
  scripts/     сборка и очистка
  tools/       диагностика, эталоны, архив доказательств
  tests/       артефакты прогонов по режимам
  patches/     патчи, применяемые при сборке
  third_party/ сторонние архивы
  docs/        отчёты, контракты, эталоны
  build/       результаты сборки (игнорируется)
  release/     готовые архивы (игнорируется)
```

## Рабочий процесс

1. Правка в `source/` (или в `scripts/`, `tools/`).
2. Сборка нужных ABI и проверка на устройстве.
3. Регресс-гейт OpenCL должен быть зелёным.
4. Коммит в этот репозиторий.
5. Архивы копируются в `HASHCAT-APP/hashcat/binaries`, обновляется
   `SHA256SUMS.txt`, приложение пересобирается и получает новую версию.

```powershell
cd D:\PROJECTS\HASHCAT
git status
git add source scripts docs README.md
git commit -m "..."
git push origin master
```

## Версии

`-VersionTag` управляет именами архивов и версией, которую получает makefile
hashcat. Менять его осознанно: имя архива связано с константой `hashcatVersion`
в `HASHCAT-APP/hashcat/build.gradle.kts`.

## Лицензия

Форк наследует лицензию hashcat (MIT). Авторы и условия — `source/docs/credits.txt`.
