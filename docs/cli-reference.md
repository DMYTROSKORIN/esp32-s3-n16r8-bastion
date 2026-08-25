# Терминальная консоль аварийного доступа

## Назначение

SSH-консоль ESP32 должна быть понятной без внешней инструкции. После входа она
сразу показывает dashboard, а команда `help` объясняет доступные действия и
приводит готовые примеры. Пользователю не требуется помнить синтаксис.

Это не Unix shell. Произвольные системные команды, запуск программ и доступ к
файловой системе не поддерживаются. Консоль содержит только безопасный набор
операций для диагностики и восстановления связи.

## Начальный экран

Реализованный dashboard использует ANSI-цвета: индикатор ● в каждой строке
(зелёный — работает, жёлтый — переходное состояние, красный — отказ),
серые второстепенные детали и голубые подсказки команд. Prompt `recovery>`
выделен жирным зелёным.

```text
  ESP32 Recovery Gateway
  ────────────────────────────────────────────────
  Device     ● ONLINE    uptime 0d 00:07:44
  Wi-Fi      ● ONLINE    MyHomeWiFi  -51 dBm
  Internet   ● ONLINE
  WireGuard  ● ONLINE    server-1  10.66.0.6  handshake 10s ago
  Main PC    ● ONLINE    192.168.1.200  ssh :22 open
  WoWLAN     ● STANDBY   AA:BB:CC:DD:EE:FF
  Memory       heap 199 KB  psram 8166 KB  reset: power-on
  ────────────────────────────────────────────────
  help commands   pc ssh how to reach the PC   pc wake wake it up

recovery>
```

Поведение строк:

- `WireGuard` показывает фактическое состояние VPN-задачи
  (`WAITING / CONNECTING / ONLINE / DEGRADED`), активный профиль,
  tunnel IP и возраст последнего handshake.
- Строка `VPN errors` появляется только при последовательных неудачах
  health-check.
- `WoWLAN` показывает `READY` с MAC-адресом, когда ПК офлайн и его можно
  будить, и `STANDBY`, когда ПК уже онлайн.
- `reset:` — причина последней перезагрузки
  (power-on / software / panic / watchdog / brownout).

## Главная справка

Команды `help` и `?` доступны всегда. Фактический вывод `help` в текущей
прошивке:

```text
ESP32 Recovery Gateway - command reference

STATUS
  status                 Show the complete dashboard
  uptime                 Show device uptime
  version                Show firmware and key fingerprint

MAIN PC
  pc status              Check 192.168.1.200:22
  pc wake                Send WoWLAN Magic Packet
  pc ssh                 Show ready-to-use connect commands

NETWORK
  net status             Show Wi-Fi and internet state

VPN
  vpn status             Show tunnel and handshake state
  vpn failover           Switch to the other profile
  vpn retry-primary      Switch to server-1

HELP
  help                   Show this command list
  help <command>         Show details and examples
  help examples          Show common recovery scenarios
```

Также реализованы `exit`, `quit`, `logout`, редактирование строки
(Backspace), `Ctrl+C` (сброс введённой строки) и `Ctrl+D` (выход).

Команды следующих этапов (пока не реализованы): `watch`, `health`,
`pc ping`, `vpn peers`, `vpn reconnect`, `vpn history`, `wifi status`,
`internet check`, `logs`, `logs follow`, `reboot`.

## Подробная справка

`help <command>` обязательно показывает:

- назначение;
- полный синтаксис;
- значения по умолчанию;
- что команда изменяет;
- возможные ошибки;
- один или несколько готовых примеров;
- связанный следующий шаг.

Пример `help pc wake`:

```text
pc wake — wake the main Linux PC over Wi-Fi

Usage:
  pc wake

Target:
  IP         192.168.1.200
  Broadcast  192.168.1.255
  MAC        AA:BB:CC:DD:EE:FF

Action:
  Sends the WoWLAN Magic Packet three times, 250 ms apart.
  Then checks 192.168.1.200:22 for up to 90 seconds.

Example:
  pc wake

Possible results:
  PC is already online
  Wake packet sent; PC became reachable after 18s
  Wake packet sent; timeout waiting for SSH

See also:
  pc status
  pc ssh
```

Пример `help vpn failover`:

```text
vpn failover — switch to the standby WireGuard profile

Usage:
  vpn failover

Action:
  Closes the active tunnel, activates the other configured profile,
  and verifies its handshake and health endpoint.

The command never prints private or preshared keys.

Example:
  vpn status
  vpn failover
  vpn status

See also:
  vpn peers
  vpn retry-primary
  vpn history
```

Фактический вывод `pc ssh` (адреса подставляются динамически из активного
профиля — после failover команды печатаются уже с адресами резервного
сервера):

```text
pc ssh - connect through this ESP32 bastion

If your device is a VPN client of server-1 (203.0.113.10):
  ssh -J user@10.66.0.6 user@192.168.1.200

Without a VPN client (jump over the VPN server's sshd):
  ssh -J user@203.0.113.10:8326,user@10.66.0.6 user@192.168.1.200

Only destination 192.168.1.200:22 is permitted.
```

ESP32 не хранит пароль Linux и приватный SSH-ключ пользователя: ProxyJump
выполняет сквозную аутентификацию с локальной машины, плата лишь
пробрасывает TCP-поток. Полноценные интерактивные сессии (включая
полноэкранные TUI вроде `btop`) через бастион работают.

## Готовые сценарии

Команда `help examples` показывает последовательности для типичных аварий.
Сценарии из текущей прошивки:

### Основной VPN на ПК не работает

```text
1. status
2. pc status
3. pc ssh
4. Выйти из консоли ESP32
5. Выполнить напечатанную ProxyJump-команду локально
6. Перезапустить WireGuard на Linux-ПК
```

### ПК находится в режиме сна

```text
1. pc status
2. pc wake
3. Подождать выход ПК из сна (десятки секунд)
4. pc status
5. pc ssh
```

### Активный VPN-сервер недоступен

```text
1. vpn status          (по LAN: ssh user@192.168.1.120, если туннель мёртв)
2. vpn failover
3. vpn status
4. Переключить свой VPN-клиент на тот же сервер, что и плата
```

### Интернет или Wi-Fi нестабилен

```text
1. net status
2. status
3. Смотреть серийный лог по USB (115200 бод) — переходы VPN-состояний
```

## Поведение и безопасность

Реализовано:

- Разрешена только SSH-аутентификация по public key (один авторизованный
  ключ пользователя `user`, скомпилирован в прошивку); максимум
  16 auth-сообщений на сессию.
- ESP32 никогда не показывает private key, preshared key или пароль Wi-Fi.
- SSH bastion разрешает только назначение `192.168.1.200:22`; запрос любого
  другого `direct-tcpip`-назначения отклоняется.
- Сервер обслуживает одну сессию за раз. Пока открыт проброшенный туннель к
  ПК, консоль недоступна (и наоборот).
- Защита от зависших клиентов: 30 с на key exchange/аутентификацию
  (плюс TCP keepalive), 60 с от подключения до запуска shell, 10 минут
  idle-timeout консоли и туннеля — после чего сессия закрывается с
  сообщением `Idle timeout. Bye.`, и сервер снова принимает подключения.
- Неизвестная команда не выполняется и отсылает к `help`.
- `Ctrl+C` сбрасывает набранную строку, `Ctrl+D` завершает сессию.
- Команды `vpn failover` / `vpn retry-primary` подтверждают приём запроса и
  предлагают проверить результат через `vpn status`.

Планируется:

- кольцевой журнал всех событий без секретных данных;
- `reboot` с явным текстовым подтверждением;
- подсказка похожих команд при опечатке.

## Требование к реализации

Команды описываются в едином реестре со следующими полями:

- имя и aliases;
- краткое описание;
- подробная справка;
- синтаксис;
- примеры;
- уровень доступа;
- признак подтверждения;
- функция-обработчик.

`help` и `help <command>` генерируются непосредственно из этого реестра. Это
исключает ситуацию, когда прошивка изменилась, а встроенная справка осталась
устаревшей.
