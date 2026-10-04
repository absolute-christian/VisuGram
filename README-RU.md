# VisuGram

[English](README.md) · [Скачать для Windows](https://github.com/absolute-christian/VisuGram/releases) · [Состояние сборки](https://github.com/absolute-christian/VisuGram/actions)

Экспериментальный форк [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop) на основе Telegram Desktop. Визуальные функции добавлены поверх существующего клиента.

- Переключатель «Визуальные звёзды» и отображение бесконечного баланса.
- Публичный каталог подарков с настоящими моделями, узорами и фонами из API Telegram.
- Локальное дарение, включая отправку себе в Избранное, штатную отрисовку и конфетти.
- Подарки в профиле, скрытие, удаление и закрепление до шести коллекционных подарков возле аватарки.
- Редактируемый визуальный номер телефона и несколько визуальных NFT-юзернеймов.
- Русский и английский языки новых элементов интерфейса.

Визуальные подарки и данные видны только в этом клиенте. Реальные владельцы NFT, данные аккаунта Telegram и баланс звёзд не меняются.

## Запуск на Windows

Скачайте архив Windows x64 из Releases этого репозитория, распакуйте в отдельную папку и запустите `VisuGram.exe`. Сохраните папку `TelegramForcePortable` рядом с программой. В ней будут данные аккаунта; не загружайте и не коммитьте их.

Успешно собрана и упакована [предварительная Windows x64 Debug-версия](https://github.com/absolute-christian/VisuGram/releases/tag/visual-preview-5). Вход, подарки, анимации и сохранение настроек ещё требуют ручной проверки. [Инструкция функций](docs/visual-mode.md).

## Облачная сборка

Actions → Windows visual preview → Run workflow, ветка `dev`. GitHub подготовит зависимости, соберёт Windows x64 Debug, сохранит архив и при включённой опции опубликует предварительный релиз. [Подробности](docs/cloud-build.md).

## Основа и лицензия

Существующие функции и авторство AyuGram сохранены. [AyuGram](https://github.com/AyuGram/AyuGramDesktop), [Telegram Desktop](https://github.com/telegramdesktop/tdesktop), [LEGAL](LEGAL), [LICENSE](LICENSE).
