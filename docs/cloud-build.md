# Сборка VisuGram на GitHub

Workflow «Windows visual preview» компилирует приложение на Windows runner GitHub Actions. На компьютере пользователя компиляция не запускается. Репозиторий: https://github.com/absolute-christian/VisuGram.

Откройте Actions → Windows visual preview → Run workflow. Оставьте ветку dev и включённый Publish the portable ZIP as a prerelease. После успешной компиляции ZIP появляется в Artifacts и на странице Releases. Если компиляция завершается ошибкой, релиз не создаётся; причина находится в журнале соответствующего шага.

Используется Windows x64, Visual Studio 2022, v143 14.44, SDK 10.0.26100.0, Python 3.10, CMake 3.31.6, Qt 5.15.19 и NASM 3.01. Приложение собирается в Debug без LTO, с двумя параллельными задачами. Подготовка зависимостей выполняется штатным prepare.py с skip-release; некоторые сторонние инструменты этот скрипт собирает в своей фиксированной конфигурации. Кэш зависимостей позволяет повторно использовать завершённые этапы. Первый запуск может занять несколько часов; шаг ограничен шестью часами. Кэш большого размера может не сохраниться из-за лимитов GitHub.

Для личной предварительной сборки используется публичная пара API из документации AyuGram. При необходимости её заменяют secrets TELEGRAM_API_ID и TELEGRAM_API_HASH. Эти значения должны принадлежать одному приложению. Аккаунтные данные и папка tdata в архив не включаются. Автообновление отключено, чтобы исходный клиент не заменил форк своей версией.

Распакуйте ZIP целиком в отдельную доступную для записи папку и запустите VisuGram.exe. Сохраните рядом TelegramForcePortable: в ней клиент хранит данные входа и визуальные настройки. Предварительная сборка не подписана. Успешная компиляция подтверждает сборку исходников, но ручная проверка входа, подарков, анимаций и сохранения настроек проводится отдельно.

Источники конфигурации: [AyuGram Windows development](https://docs.ayugram.one/desktop/development/windows/), [Windows runner image](https://github.com/actions/runner-images/blob/main/images/windows/Windows2022-Readme.md), штатные Telegram/build/prepare/prepare.py и Telegram/build/qt_version.py.
