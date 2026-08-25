# Терминальная консоль аварийного доступа

## Назначение

SSH-консоль ESP32 должна быть понятной без внешней инструкции. После входа она
сразу показывает dashboard, а команда `help` объясняет доступные действия и
приводит готовые примеры. Пользователю не требуется помнить синтаксис.

Это не Unix shell. Произвольные системные команды, запуск программ и доступ к
файловой системе не поддерживаются. Консоль содержит только безопасный набор
операций для диагностики и восстановления связи.

## Начальный экран

```text
ESP32 Recovery Gateway
────────────────────────────────────────────────
Device              ONLINE   uptime 12d 04:18:31
Wi-Fi               ONLINE   -42 dBm
Internet            ONLINE
WireGuard            ONLINE   server-1
Last handshake       8s ago
VPN server-1         ACTIVE   31 ms
VPN server-2         STANDBY  47 ms
Main PC              ONLINE   192.168.1.200
SSH                  OPEN     192.168.1.200:22
WoWLAN               READY
Free heap / PSRAM    181 KB / 7.6 MB
Last reset           power-on
────────────────────────────────────────────────
Type 'help' to see all commands and examples.

recovery>
```

## Главная справка

Команды `help`, `?` и пустая команда после первого входа доступны всегда.
Планируемый вывод `help`:

```text
ESP32 Recovery Gateway — command reference

STATUS
  status                 Show the complete status dashboard
  watch [seconds]        Refresh dashboard until Ctrl+C
  health                 Run all health checks now

MAIN PC
  pc status              Check PC reachability and SSH port
  pc ping [count]        Ping 192.168.1.200
  pc wake                Send the configured WoWLAN Magic Packet
  pc ssh                 Show the exact SSH jump-host command

WIREGUARD
  vpn status             Show active profile and handshake health
  vpn peers              Show both profiles without secret keys
  vpn failover           Switch to the other profile
  vpn retry-primary      Test server-1 and switch only if healthy
  vpn reconnect          Recreate the current tunnel
  vpn history            Show recent tunnel transitions

NETWORK
  net status             Show Wi-Fi, IP, gateway and DNS state
  wifi status            Show SSID, RSSI and reconnect counters
  internet check         Test internet access immediately

DEVICE
  uptime                 Show device uptime
  version                Show firmware and build information
  logs [count]           Show recent events; default: 25
  logs follow            Stream new events until Ctrl+C
  reboot                 Reboot ESP32 after confirmation

HELP
  help                   Show this command list
  help <command>         Show details and examples for one command
  help examples          Show common recovery scenarios

Examples:
  help pc wake
  help vpn failover
  pc status
  logs 50
```

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

Пример `help pc ssh`:

```text
pc ssh — connect to the main PC through this ESP32 bastion

The ESP32 does not store your Linux password or private SSH key.
Exit this console and run the displayed ProxyJump command locally:

  ssh -J recovery@<ESP32_VPN_IP> <LINUX_USER>@192.168.1.200

For verbose troubleshooting:

  ssh -vv -J recovery@<ESP32_VPN_IP> <LINUX_USER>@192.168.1.200

The bastion permits only destination 192.168.1.200:22.
```

## Готовые сценарии

Команда `help examples` показывает последовательности для типичных аварий.

### Основной VPN на ПК не работает

```text
1. status
2. pc status
3. pc ssh
4. Exit the ESP32 console
5. Run the printed ProxyJump command
6. Restart WireGuard on the Linux PC
```

### ПК находится в режиме сна

```text
1. pc status
2. pc wake
3. Wait for the automatic SSH check
4. pc ssh
```

### Первый VPN-сервер недоступен

```text
1. vpn status
2. vpn peers
3. vpn failover
4. vpn status
5. vpn history
```

### Интернет или Wi-Fi нестабилен

```text
1. net status
2. wifi status
3. internet check
4. logs 50
```

## Поведение и безопасность

- В production разрешена только SSH-аутентификация по public key.
- ESP32 никогда не показывает private key, preshared key или пароль Wi-Fi.
- SSH bastion разрешает только `192.168.1.200:22`.
- Неизвестная команда не выполняется и предлагает похожие допустимые команды.
- `Ctrl+C` останавливает `watch` и `logs follow`, но не закрывает SSH-сессию.
- Команды изменения состояния выводят результат проверки после операции.
- `reboot` требует явного текстового подтверждения.
- Все события записываются в кольцевой журнал без секретных данных.

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
