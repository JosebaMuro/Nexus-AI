# Guía de usuario

Esta guía explica cómo instalar Nexus-Office en cada equipo y cómo usarlo en el día a día. Para la configuración avanzada,
ver la [guía de administrador](ADMINISTRADOR.md); para montar la oficina de tu empresa, [Oficina para pymes](OFICINA.md).

## Instalación

Todos los instaladores son **100 % portables**: llevan dentro Python, todo lo que necesita Nexus-Office y el motor de IA
(llama-server y sd-cli, para CPU y para la tarjeta gráfica). **Se instalan sin internet.** Lo único que se descarga
es el modelo de IA, elegido según tu equipo, y solo si hay conexión (ver [más abajo](#modelo-de-ia-elegido-según-tu-equipo)).

Elige el fichero de tu equipo (versión 1.8.0):

| Equipo | Fichero |
|---|---|
| PC con Windows 10/11 de 64 bits (Intel o AMD) | `windows/Nexus-Office-Setup-1.8.0-windows-x64.exe` |
| PC con Windows 11 en ARM (Snapdragon X) | `windows/Nexus-Office-Setup-1.8.0-windows-arm64.exe` |
| PC con Linux (Ubuntu, Debian, Fedora, Steam Deck…) | `linux/Nexus-Office-1.8.0-linux-x86_64.run`, `.AppImage` o `.flatpak`; en Debian y Ubuntu también `linux/nexus-office_1.8.0_amd64.deb` |
| Raspberry Pi 4/5 y otros Linux ARM de 64 bits | `linux/Nexus-Office-1.8.0-linux-aarch64.run`, `.AppImage`, `.flatpak` o `linux/nexus-office_1.8.0_arm64.deb` |
| Mac con Apple Silicon (M1, M2, M3…) | `macos/Nexus-Office-1.8.0-macos-arm64.tar.gz` |
| Mac con Intel | `macos/Nexus-Office-1.8.0-macos-x86_64.tar.gz` |
| Móvil Android: Nexus-Office dentro del móvil | `android/Nexus-Office-1.8.0-android-autonomo.apk` |
| Móvil Android: usar el Nexus-Office del PC de la oficina | `android/Nexus-Office-1.8.0-android-vpn.apk` |

`SHA256SUMS.txt` trae la huella de cada fichero, para comprobar que se ha copiado bien. Si Nexus-Office ya está instalado
en un PC de la oficina, los demás equipos y móviles pueden descargar su instalador desde él (ver
[Descargar los instaladores desde el móvil](#descargar-los-instaladores-desde-el-móvil)).

### Windows

1. Haz doble clic en `Nexus-Office-Setup-1.8.0-windows-x64.exe` (o `-arm64.exe` en un PC con Snapdragon X).
2. El instalador no está firmado. Si Windows muestra «Windows protegió su PC», pulsa **Más información →
   Ejecutar de todas formas**.
3. Acepta la licencia y elige la carpeta. Por defecto es `%LOCALAPPDATA%\Nexus-Office` y no hace falta ser administrador.
   La carpeta es portable: puedes elegir un disco USB.
4. En la página **Opciones** eliges:
   - **Perfil de oficina**: el tipo de empresa (herrería, calderería, reformas…), o *ninguno* para los agentes
     generales. Se puede cambiar después en *Agentes → Perfil de oficina*.
   - **Elegir y descargar el modelo de IA adecuado a este equipo** (necesita internet).
   - **Arrancar Nexus-Office como servicio al encender el equipo**: solo este paso pide permiso de administrador. Si no lo
     das, Nexus-Office arranca al iniciar sesión. Ver [Arranque automático](#arranque-automático).
   - **Accesible desde la red local**, para entrar desde los móviles y los otros PC de la oficina.
   - **Crear acceso directo en el escritorio**.
5. Al terminar, marca **Abrir Nexus-Office**. Se abre el navegador en `http://127.0.0.1:8775`.

En el menú Inicio quedan *Nexus-Office*, *Nexus-Office (consola)* (la misma aplicación, con una ventana de registro), el
*Manual de usuario* y *Desinstalar Nexus-Office*. Nexus-Office aparece también en *Configuración → Aplicaciones*. Al
desinstalar, se quita el servicio y te pregunta si borras también tus datos (conversaciones, documentos, modelos y
configuración); si dices que no, quedan en la carpeta `data`.

**Instalación desatendida** (por ejemplo, en varios PC):

```bat
Nexus-Office-Setup-1.8.0-windows-x64.exe /S /OFICINA=herreria /SERVICIO /RED /MODELOS=auto /D=C:\Nexus-Office
```

| Opción | Qué hace |
|---|---|
| `/S` | Sin ventanas. |
| `/OFICINA=<perfil>` | Perfil de oficina (lista en [OFICINA.md](OFICINA.md)). |
| `/SERVICIO` | Arranque automático. Pide permiso de administrador salvo que el instalador ya se ejecute como administrador. |
| `/RED` | El servicio escucha en la red local. |
| `/MODELOS=auto\|todos\|no` | `auto` (por defecto): modelo de chat, documentos y voz; `todos`: también el de imagen; `no`: no descarga nada. |
| `/D=<carpeta>` | Carpeta de instalación. Va siempre la última. |

### Linux (PC, Steam Deck, Raspberry Pi)

**Con el instalador `.run`** (cualquier distribución):

```bash
sh Nexus-Office-1.8.0-linux-x86_64.run                   # en Raspberry Pi y ARM: Nexus-Office-1.8.0-linux-aarch64.run
sh Nexus-Office-1.8.0-linux-x86_64.run ~/Nexus-Office --oficina=herreria --servicio --red
```

Se instala en `~/.local/share/nexus-office` si no indicas otra carpeta. Opciones:

- `--oficina=<perfil>`: perfil de oficina.
- `--modelos=auto|todos|no`: igual que en Windows.
- `--servicio`: arranque automático, como servicio de tu usuario que arranca al encender el equipo (si el sistema
  permite `loginctl enable-linger`; si no, al iniciar sesión). Con `sudo` se instala
  para todo el equipo en `/opt/nexus-office` (a nombre de tu usuario) y el servicio es del sistema:
  `sudo sh Nexus-Office-1.8.0-linux-x86_64.run --servicio --red`.
- `--red` (junto con `--servicio`): el servicio escucha en la red local.

Después, abre *Nexus-Office* desde el menú de aplicaciones o ejecuta `start.sh` en la carpeta de instalación. En Steam
Deck, instálalo en modo escritorio y añade `start.sh` como *juego no‑Steam*.

**Con AppImage** (cualquier distribución, sin instalar nada): un único fichero que se ejecuta tal cual. Nexus-Office ya
va instalado dentro; tus datos quedan en `~/.local/share/nexus-office`.

```bash
chmod +x Nexus-Office-1.8.0-linux-x86_64.AppImage      # en Raspberry Pi y ARM: Nexus-Office-1.8.0-linux-aarch64.AppImage
./Nexus-Office-1.8.0-linux-x86_64.AppImage              # arranca Nexus-Office y abre el navegador
./Nexus-Office-1.8.0-linux-x86_64.AppImage oficina herreria
./Nexus-Office-1.8.0-linux-x86_64.AppImage servicio instalar --red   # arranque automático (usa la ruta del AppImage: no lo muevas después)
```

Si tu sistema no tiene FUSE (aparece «libfuse.so.2 not found»), ejecútalo con `--appimage-extract-and-run` delante
de los argumentos, o instala `libfuse2`.

**Con Flatpak** (Fedora, Ubuntu, Linux Mint, Steam Deck…): la primera vez descarga de Flathub el runtime
`org.freedesktop.Platform 24.08` (si no lo tienes ya).

```bash
flatpak install --user Nexus-Office-1.8.0-linux-x86_64.flatpak     # o -aarch64.flatpak
flatpak run ai.nexus.NexusOffice                                   # o desde el menú de aplicaciones
flatpak run ai.nexus.NexusOffice servicio instalar                 # deja lista la unidad; actívala con:
systemctl --user enable --now nexus-office
```

Los datos quedan en `~/.var/app/ai.nexus.NexusOffice/data/nexus-office`.

**Con el paquete `.deb`** (Debian, Ubuntu, Linux Mint, Raspberry Pi OS de 64 bits):

```bash
sudo apt install ./nexus-office_1.8.0_amd64.deb         # en Raspberry Pi y ARM: nexus-office_1.8.0_arm64.deb
nexus-office                                            # arranca Nexus-Office y abre el navegador
sudo nexus-office servicio instalar --red               # opcional: arranque automático accesible desde la red
```

El programa queda en `/opt/nexus-office` y los datos de cada usuario en `~/.local/share/nexus-office`. La primera vez que
lo abres, evalúa el hardware y descarga el modelo de IA si hay internet. Se desinstala con `sudo apt remove nexus-office`,
que quita también el servicio; tus datos se conservan.

**Raspberry Pi**: hace falta una Raspberry Pi 4 o 5 con **sistema de 64 bits** (Raspberry Pi OS 64‑bit o Ubuntu) y
mejor con 4 GB de RAM o más. Los sistemas de 32 bits no están soportados. Guarda los modelos en un disco SSD o USB 3
si puedes: cargan mucho más rápido que desde la tarjeta SD.

### macOS

1. Descomprime `Nexus-Office-1.8.0-macos-arm64.tar.gz` (o `-macos-x86_64` en un Mac con Intel) donde quieras.
2. Abre **Nexus-Office.command**. Como no está firmado por Apple, la primera vez haz **clic derecho → Abrir** y confirma.
3. La primera vez se prepara Nexus-Office (sin internet salvo para el modelo de IA) y después se abre el navegador.

Desde la terminal también puedes elegir las opciones:

```bash
./install.sh --oficina=herreria --servicio && ./start.sh
```

### Android

Hay dos apps, independientes; puedes instalar las dos:

| App | Fichero | Para qué |
|---|---|---|
| **Nexus-Office autónomo** | `Nexus-Office-1.8.0-android-autonomo.apk` | Nexus-Office completo dentro del móvil, sin PC (Android 9 o superior, ARM64). La APK lo lleva todo y solo descarga el modelo de IA adecuado a la RAM del móvil: Qwen3‑VL 2B o 4B Thinking. Detecta la CPU, la GPU y la NPU del móvil; si la GPU acelera el modelo (Adreno, Mali, Immortalis, Xclipse…) lo pone en ella, y si no, en la CPU. Lo ves en *Sistema → Hardware*. Ver [su guía](../android-autonomo/README.md). |
| **Nexus-Office VPN** | `Nexus-Office-1.8.0-android-vpn.apk` | App ligera para usar el Nexus-Office del PC de la oficina (Android 7 o superior), en la Wi‑Fi o desde fuera por VPN cifrada. En el PC, *Red COOP → VPN cifrada* → **GENERAR CÓDIGO VPN**; en la app, *Conectar por VPN* y pega el código. Ver [VPN](VPN.md). |

1. Copia la APK al móvil o descárgala desde el PC de la oficina (*Sistema → Descargas*).
2. Ábrela. Android pedirá permiso para **instalar apps desconocidas** desde esa app (el navegador o el gestor de
   archivos): concédelo.
3. Si tenías la app Nexus-Office 1.3, desinstálala antes: la app VPN actual está firmada con otra clave.

## Modelo de IA elegido según tu equipo

Al instalar, Nexus-Office **evalúa el hardware** (memoria, tarjeta gráfica, procesador) y elige el mejor modelo de cada tipo
que funciona con fluidez en ese equipo:

| Tipo | Para qué | Modelos posibles |
|---|---|---|
| Chat y visión | Conversar, razonar y entender imágenes | Qwen3‑VL 2B Thinking (1,5 GB, desde 3 GB de RAM), 4B (2,8 GB) u 8B (5,5 GB, con GPU de 8 GB o más) |
| Documentos | Buscar y ordenar tu biblioteca y la memoria | Qwen3‑Embedding 0.6B (639 MB) |
| Voz | Leer las respuestas en voz alta | Piper, voz española (61 MB) |
| Imagen | Generar imágenes (solo con la opción *todos*) | SDXS‑512 en CPU; SD‑Turbo o SDXL‑Turbo con más memoria o GPU |

Si hay internet, los descarga al momento. **Sin internet**, el instalador solo muestra la recomendación y Nexus-Office
funciona igualmente con su núcleo integrado (herramientas, memoria, imagen compuesta a partir del prompt, música y la voz del
navegador). Los modelos se descargan después, cuando haya red, desde **Modelos → Recomendados** (el marcado con ★) o
desde la terminal:

```bash
venv/bin/python3 -m nexus modelos                  # muestra el equipo y el modelo elegido de cada tipo
venv/bin/python3 -m nexus modelos --descargar      # y los descarga (chat, documentos y voz)
venv/bin/python3 -m nexus modelos --descargar --todos   # también el de imagen
```

En Windows, `venv\python.exe` en lugar de `venv/bin/python3`; con el `.deb`, `nexus-office modelos --descargar`.

## Arranque automático

Con el **arranque automático**, Nexus-Office se inicia solo al encender el equipo, sin ventana, y se reinicia si falla. Es
lo recomendable en el PC de la oficina, para que los móviles y los demás equipos lo encuentren siempre.

- Se activa al instalar (opción *Arrancar Nexus-Office como servicio* en Windows, `--servicio` en Linux y macOS) o después,
  en **Sistema → Arranque automático** → **INSTALAR SERVICIO** (modo administrador). Marca *accesible desde la red
  local* si lo vas a usar desde otros dispositivos.
- Con el servicio en marcha, el acceso directo solo abre el navegador: no arranca un segundo Nexus-Office.
- En la app **Nexus-Office autónomo**: *Sistema → Arranque automático* → **ARRANCAR AL ENCENDER**.
- Para quitarlo: **QUITAR** en el mismo panel (o **DESACTIVAR** en la app). Nexus-Office sigue instalado.

Detalles por sistema, comandos y problemas frecuentes: [Servicio](SERVICIO.md).

## Descargar los instaladores desde el móvil

El PC de la oficina puede repartir los instaladores a los demás equipos y móviles de la red, sin cables ni memorias
USB:

1. En el PC donde corre Nexus-Office, copia la carpeta de instaladores (`windows/`, `linux/`, `macos/`, `android/`…) dentro
   de la carpeta `descargas` de los datos de Nexus-Office: `data/descargas/` (con el `.deb`, `~/.local/share/nexus-office/descargas/`).
   La ruta exacta aparece en el panel mientras está vacío.
2. Nexus-Office tiene que ser accesible desde la red: servicio con *accesible desde la red local*, o *Red COOP → Conectar
   dispositivos → Permitir acceso desde la red*.
3. Desde el móvil o el otro PC, abre Nexus-Office (en el navegador con la clave de red, o con la app Nexus-Office VPN) y ve a
   **Sistema → Descargas**.

La lista marca con **anfitrión** el instalador del equipo donde corre Nexus-Office y con **este dispositivo** el que
corresponde al móvil o PC desde el que miras. Pulsa **DESCARGAR**. En la app Android, eliges dónde guardar el fichero;
después ábrelo para instalarlo.

## Uso diario (modo sencillo)

El modo **USUARIO** (arriba a la derecha) muestra solo lo necesario. Nada de lo que hagas en este modo puede romper la configuración avanzada.

### Chat
- Escribe y pulsa **Enter** (Shift+Enter = nueva línea). El selector de la izquierda elige agente; *Automático* deja que Nexus-Office decida.
- **📎** adjunta imágenes (también puedes pegarlas): Nexus-Office las describe, lee texto (OCR) y responde preguntas sobre ellas.
- **🎤** habla; **🔊** activa la lectura en voz alta de las respuestas.
- Bajo cada respuesta: **👍 / 👎** enseñan a Nexus-Office qué te gusta (aprende ejemplos, lecciones y la longitud preferida), **🔊** lee, **⧉** copia.
- Las líneas verdes “⟡ aprendido” muestran lo que acaba de memorizar de ti.
- Si un agente necesita una herramienta con riesgo, aparece una tarjeta **APROBACIÓN REQUERIDA**: tú decides.

Prueba: *«Me llamo Ane y me gusta el surf»*, *«¿Qué sabes de mí?»*, *«Genera una imagen de un faro bajo la tormenta»*, *«Compón una música relajante»*, *«Cuánto es 2^16 / 7»*.

### El avatar
A la derecha del chat está el **núcleo cognitivo**: cada punto es algo que Nexus-Office ha aprendido (personas, lugares, gustos, conceptos) y las líneas son relaciones. Cambia de color según su estado: cian (en línea), violeta (pensando), ámbar (herramienta), magenta (hablando, late con la voz), verde (escuchando).

### Salón
Representación gráfica de los agentes reunidos. Mira cómo conversan, cómo se delegan tareas y qué habilidades han demostrado. Pulsa **Convocar reunión** o escribe algo en el salón.

### Núcleo
Explora tu grafo de conocimiento en 3D (arrastra para girar, rueda para acercar), busca recuerdos, borra los que no quieras y enséñale datos nuevos.

### Estudio
Genera imágenes y música, analiza imágenes y revisa la galería.

### Bolsa
Mesa de bolsa **simulada** para practicar sin dinero: cartera ficticia de 10.000 €, cotizaciones que se mueven solo con la bolsa abierta (en su hora local y con sus festivos), órdenes a mercado, limitadas y stop, comisiones, impuestos, cambio de divisa y deslizamiento. Los precios son ficticios y nada de esto es asesoramiento financiero. Con el perfil *Bolsa e inversiones* los agentes de la oficina también usan la mesa. Una cartera **real** solo sirve de seguimiento: ninguna orden se ejecuta sin que escribas `CONFIRMO`, y Nexus-Office no se conecta a ningún bróker. Ver [BOLSA.md](BOLSA.md).

### Modelos
En modo usuario verás los **recomendados para tu equipo** con una etiqueta: *ideal*, *compatible*, *pesado* o *no cabe*. Si el instalador no pudo descargar el modelo elegido (por ejemplo, sin internet), pulsa **Descargar** en el marcado con ★ y después **Activar**. Descarga también el modelo de **embeddings** (Qwen3-Embedding, 639 MB): mejora la búsqueda en tu biblioteca y en la memoria y se activa solo.

### Ajustes
Nombre del asistente, idioma, estilo de respuesta (breve/equilibrado/detallado), agente por defecto, memoria, voz, velocidad de voz, avatar y color de acento.

En modo administrador aparece el **Gestor de conocimiento**: lo que Nexus-Office sabe de ti y es común a todos los agentes, lo que sabe cada agente de su especialidad, importar grafos y leer un ZIP. Ver [MEMORIA.md](MEMORIA.md).

Nexus-Office recuerda las pautas que le pidas («no uses emojis», «respóndeme siempre con ejemplos», «llámame Joseba») y todos los agentes las respetan.

### Usarlo desde el móvil, tablet o consola
Nexus-Office tiene que escuchar en la red local: instala el [arranque automático](#arranque-automático) con *accesible desde la red local*, activa *Red COOP → Conectar dispositivos → Permitir acceso desde la red* o arráncalo con `--host 0.0.0.0`. Después abre `http://IP-DEL-PC:8775` desde el navegador del otro dispositivo e introduce la **clave de red** que aparece en *Conectar dispositivos*. Puedes **instalarlo como app** (PWA) desde el menú del navegador, o usar la **app Nexus-Office VPN**.

La interfaz se adapta sola al dispositivo:

- **Móvil:**
  - Chat, Salón, Núcleo y Estudio están en la barra inferior. El resto de secciones está en **Más** (⋯).
  - El avatar aparece en una franja sobre el chat. Se pliega con el botón de la persona y se recoge solo al abrir el teclado.
  - Intro hace un salto de línea; para enviar, pulsa el botón de enviar.
  - En horizontal, las secciones pasan a una barra lateral.
- **Tablet:** el avatar va arriba en vertical y a la derecha en horizontal.
- **Desde fuera de casa:** en la app Android, *Conectar desde internet* y pega el código que te dé el administrador. Más detalles en [Acceso remoto](ACCESO_REMOTO.md).
- **App Android:**
  - El botón **Atrás** cierra menús y diálogos y vuelve al chat antes de salir.
  - Las barras del sistema toman el color del tema.
  - **Cambiar de servidor** está en **Más**.
- **Móviles modestos:** se desactivan los efectos pesados para que todo vaya fluido.

Si prefieres otro diseño, cámbialo en *Ajustes → Diseño de la interfaz* (Automático, Móvil, Tablet o Escritorio). El cambio solo afecta a ese dispositivo.
