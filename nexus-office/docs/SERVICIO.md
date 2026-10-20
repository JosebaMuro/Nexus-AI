# Nexus-Office como servicio: arranque automático

Instalado como **servicio**, Nexus-Office arranca solo al encender el equipo, sin ventana, y se reinicia si falla. Es lo
normal en el PC de la oficina: los móviles y los otros equipos lo encuentran siempre en marcha, aunque nadie haya
abierto la aplicación.

- Funciona en Windows, Linux (también Raspberry Pi), macOS, Android con Termux y la app **Nexus-Office autónomo**.
- El servicio apunta a la carpeta donde está instalado Nexus-Office. La carpeta sigue siendo portable, pero si la mueves,
  vuelve a instalar el servicio.
- Con el servicio en marcha, el acceso directo, `start.bat` o `./start.sh` **no arrancan un segundo Nexus-Office**: ven que
  ya responde y solo abren el navegador en `http://127.0.0.1:8775`.

## Cómo se activa

| Desde | Cómo |
|---|---|
| Instalador de Windows (`.exe`) | Página *Opciones* → **Arrancar Nexus-Office como servicio al encender el equipo** y, si quieres, **Accesible desde la red local**. Solo ese paso pide permiso de administrador (UAC). Silencioso: `/S /SERVICIO /RED`. |
| Instalador de Linux (`.run`) o `install.sh` | `--servicio`, y `--red` para la red local. Crea un servicio de usuario. Ejecuta el `.run` sin `sudo` (con `sudo` se instalaría en la carpeta de root) y, si quieres el servicio del sistema, después `sudo <carpeta>/venv/bin/python3 -m nexus servicio instalar`. |
| Paquete `.deb` | `sudo nexus-office servicio instalar [--red]` |
| macOS | `./install.sh --servicio [--red]`, o el comando de la terminal (abajo). |
| `install.ps1` / `install.bat` | `-Servicio`, y `-Red` para la red local. |
| Termux | `bash install-termux.sh --servicio` (siempre accesible desde la red local). |
| Interfaz web | *Sistema → Arranque automático* → **INSTALAR SERVICIO** (modo administrador). Marca *accesible desde la red local* si lo vas a usar desde móviles u otros PC. |
| App Nexus-Office autónomo | *Sistema → Arranque automático* → **ARRANCAR AL ENCENDER**. |

También con la variable de entorno `NEXUS_SERVICIO=1` en `install.sh` e `install.ps1`.

## Comandos

En Windows, `venv\python.exe`; en Linux y macOS, `venv/bin/python3`; con el `.deb`, `nexus-office` en lugar de
`python -m nexus`.

```bash
python -m nexus servicio instalar              # arranque automático, escuchando solo en este equipo
python -m nexus servicio instalar --red        # escucha en la red local (0.0.0.0): móviles y otros PC
python -m nexus servicio instalar --puerto 9000
python -m nexus servicio estado                # tipo, dónde está registrado y si responde
python -m nexus servicio quitar                # lo quita (Nexus-Office sigue instalado)
```

Sin `--red` se usa la dirección de `server.host` de `data/config.yaml` (por defecto `127.0.0.1`), y sin `--puerto`,
`server.port` (8775). `--host <ip>` fija otra dirección de escucha.

Desde la red, los dispositivos que no son el propio equipo necesitan la **clave de red** (*Red COOP → Conectar
dispositivos*). Ver [Acceso desde otros dispositivos](PLATAFORMAS.md#acceso-desde-otros-dispositivos-móvil-tablet-consola-tv).

## Qué hace en cada sistema

| Sistema | Con permisos de administrador | Sin permisos |
|---|---|---|
| **Windows** | Tarea programada «Nexus-Office» que arranca **con el equipo** (30 s después), con la cuenta SYSTEM. Con `--red`, abre el puerto en el Firewall de Windows (regla «Nexus-Office»). | Tarea programada que arranca **al iniciar sesión** tu usuario. Sin regla de firewall: con `--red`, Windows puede preguntar la primera vez si permite el acceso. |
| **Linux con systemd** | Servicio del sistema `/etc/systemd/system/nexus-office.service`, que corre como el usuario dueño de la carpeta (o el que hizo `sudo`). Arranca con el equipo. | Servicio de usuario `~/.config/systemd/user/nexus-office.service` y `loginctl enable-linger`, para que arranque al encender sin iniciar sesión. Si *linger* no se puede activar, arranca al iniciar sesión. |
| **Linux sin systemd** | Línea `@reboot` en el crontab. | Igual. |
| **macOS** | LaunchDaemon `/Library/LaunchDaemons/ai.nexus.office.server.plist`: arranca con el equipo. | LaunchAgent `~/Library/LaunchAgents/ai.nexus.office.server.plist`: arranca al iniciar sesión. |
| **Android (Termux)** | — | `termux-services` (`$PREFIX/var/service/nexus-office`) y, con la app **Termux:Boot**, arranque al encender el móvil (`~/.termux/boot/nexus-office.sh`, con *wake lock*). |
| **App Nexus-Office autónomo** | — | El servicio en primer plano de la app se lanza al encender el móvil y tras actualizar la app. |

En todos los casos:

- Se reinicia solo si se cae: systemd a los 5 s, launchd a los 10 s, el Programador de tareas cada minuto (hasta 999
  veces).
- No tiene límite de tiempo: en Windows la tarea no se para a las 72 h ni al pasar a batería.
- Arranca con `python -m nexus run --no-browser`, sin abrir el navegador.
- En Windows usa `pythonw.exe`, así que no abre ninguna ventana.

Para que el arranque sea **con el equipo** y no al iniciar sesión, instálalo con permisos de administrador: desde el
instalador de Windows (pide UAC), con `sudo` en Linux y macOS, o desde una consola de administrador en Windows. Si lo
instalas desde la interfaz web, se instala con los permisos de Nexus-Office (normalmente, los de tu usuario).

Con el `.deb`, `sudo nexus-office servicio instalar` crea el servicio del sistema, que corre como tu usuario y usa tus
datos de `~/.local/share/nexus-office`.

## App Nexus-Office autónomo (Android)

En *Sistema → Arranque automático*, pulsa **ARRANCAR AL ENCENDER**. Desde entonces, al encender el móvil (después del
primer desbloqueo) se inicia el servicio en primer plano de Nexus-Office, con su notificación, sin abrir la app. **DESACTIVAR**
lo deja como antes: Nexus-Office arranca al abrir la app.

Algunos fabricantes bloquean el arranque de las apps con el ahorro de batería extremo. Si no arranca solo, quita a
Nexus-Office del ahorro de batería en los ajustes de Android. Si el sistema lo impide, Nexus-Office arranca igualmente al abrir la app.

## Quitar el servicio

- **Interfaz web**: *Sistema → Arranque automático* → **QUITAR** (modo administrador).
- **Terminal**: `python -m nexus servicio quitar`. Si lo instalaste con permisos de administrador, quítalo también
  con ellos: `sudo` en Linux y macOS (`sudo nexus-office servicio quitar` con el `.deb`), o una consola de administrador
  en Windows.
- **Desinstalar**: el desinstalador de Windows y la desinstalación del `.deb` (`sudo apt remove nexus-office`) quitan el
  servicio antes de borrar Nexus-Office. En Windows, si la tarea era del sistema, pide permiso de administrador.
- **App Nexus-Office autónomo**: **DESACTIVAR** en *Sistema → Arranque automático*.

Quitar el servicio no borra nada: Nexus-Office sigue instalado con sus datos y se abre como siempre.

## Registro (logs)

| Sistema | Dónde |
|---|---|
| Windows | `data\logs\servicio.log` (con `pythonw.exe` no hay consola: la salida va a ese fichero, también al abrir Nexus-Office desde el acceso directo). |
| macOS | `data/logs/servicio.log` |
| Linux con systemd | `journalctl -u nexus-office` (del sistema) o `journalctl --user -u nexus-office` (de usuario). |
| Linux con cron | `data/logs/servicio.log` |
| Termux | `data/logs/current` (svlogd) |

Con el `.deb`, `data/` es `~/.local/share/nexus-office`. El registro general de Nexus-Office (`data/logs/`) se ve también en
*Sistema → Registro* (modo administrador).

## Problemas frecuentes

- **`servicio estado` dice «en marcha: no»**: mira el registro. Si Nexus-Office se acaba de instalar, el primer arranque
  tarda más; en Windows, además, la tarea del sistema espera 30 s tras encender.
- **Solo arranca al iniciar sesión**: se instaló sin permisos de administrador. Quítalo y vuelve a instalarlo con
  `sudo` (Linux, macOS) o desde una consola de administrador (Windows). En Linux, el aviso de `loginctl enable-linger`
  indica lo mismo.
- **«el servicio del sistema se quita con sudo»** o **«no se pudo borrar la tarea»**: el servicio se instaló con
  permisos de administrador; quítalo con ellos.
- **Los móviles no conectan**: instálalo con `--red` (o marca *accesible desde la red local*), comprueba que usan la
  clave de red y, en Windows sin permisos de administrador, permite Nexus-Office en el firewall.
- **El puerto 8775 está ocupado**: otro programa (o un Nexus-Office abierto a mano antes de instalar el servicio) lo usa.
  Cierra Nexus-Office desde *Sistema → Apagar Nexus-Office* o instala el servicio con `--puerto`.
- **Has movido la carpeta**: el servicio sigue apuntando a la ruta antigua. `servicio quitar` y `servicio instalar`
  desde la carpeta nueva.
- **Linux sin systemd ni cron** (algunos contenedores): no hay servicio posible; arranca Nexus-Office con
  `./start.sh --no-browser`.
- **Termux dice que falta termux-services**: `pkg install termux-services`, reinicia Termux y `sv-enable nexus-office`.
  Para arrancar al encender el móvil, instala también la app Termux:Boot desde F‑Droid y ábrela una vez.
- **Parar Nexus-Office sin quitar el servicio**: *Sistema → Apagar Nexus-Office* lo para hasta el siguiente arranque. Con el
  servicio de Windows o de Linux, si se cierra con error se vuelve a lanzar solo.
