<p align="center">
  <img src="docs/images/logo.png" alt="Pokémon Platinum 3DS" width="560">
</p>

<p align="center">
  <strong>Vuelve a Sinnoh. Las dos pantallas llenas. Un 3D más nítido.</strong><br>
  Un port nativo de Pokémon Platino para Nintendo 3DS, hecho para la New 3DS.
</p>

<p align="center">
  <a href="https://gabesud0.github.io/pokeplatinum-3ds/?lang=es"><img src="https://img.shields.io/badge/Creador_web-Cr%C3%A9alo_en_tu_navegador-9466FF?style=for-the-badge&amp;logo=googlechrome&amp;logoColor=white" alt="Créalo en tu navegador con el creador web"></a>
  <a href="https://discord.gg/YjyA5ygpC"><img src="https://img.shields.io/badge/Discord-%C3%9Anete_al_servidor-5865F2?style=for-the-badge&amp;logo=discord&amp;logoColor=white" alt="Únete al servidor de Discord"></a>
  <a href="https://github.com/GabeSud0/pokeplatinum-3ds/actions/workflows/build.yml"><img src="https://img.shields.io/github/actions/workflow/status/GabeSud0/pokeplatinum-3ds/build.yml?branch=main&amp;style=for-the-badge&amp;label=Build&amp;logo=github" alt="Estado de la compilación"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/Licencia-GPL--3.0-4B5563?style=for-the-badge" alt="Licencia GPL-3.0"></a>
</p>

<p align="center"><a href="README.md">English</a> &nbsp;·&nbsp; <b>Español</b></p>

<p align="center">
  <a href="#capturas">Capturas</a> &nbsp;·&nbsp;
  <a href="#características">Características</a> &nbsp;·&nbsp;
  <a href="#estado">Estado</a> &nbsp;·&nbsp;
  <a href="#cómo-empezar">Cómo empezar</a> &nbsp;·&nbsp;
  <a href="#para-desarrolladores">Para desarrolladores</a> &nbsp;·&nbsp;
  <a href="#comunidad">Comunidad</a>
</p>

---

Pokémon Platinum 3DS está hecho a partir de la decompilación
[pret/pokeplatinum](https://github.com/pret/pokeplatinum). No es un emulador ni
un remake: **el código del propio juego corre en la 3DS**, y el port lo dibuja
como lo haría un juego de 3DS. En una 3DS antigua, muestra las dos pantallas de
la DS en el centro de las de la 3DS.

Platino es el **prototipo**. El objetivo a largo plazo es **Pokémon Plata
SoulSilver / Oro HeartGold**, cuando la decompilación
[pret/pokeheartgold](https://github.com/pret/pokeheartgold) esté lo bastante
avanzada. Los dos son juegos de la Generación 4 que comparten casi todo el motor.

## Capturas

<p align="center"><em>Sinnoh, en las dos pantallas de una New 3DS.</em></p>

<table>
  <tr>
    <td align="center"><img src="docs/images/screenshots/jubilife.png" alt="Ciudad Jubileo, con el mapa abajo" width="400"><br><sub>Ciudad Jubileo, con el mapa abajo</sub></td>
    <td align="center"><img src="docs/images/screenshots/route.png" alt="En una ruta" width="400"><br><sub>En una ruta</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/images/screenshots/battle.png" alt="Un combate" width="400"><br><sub>Un combate</sub></td>
    <td align="center"><img src="docs/images/screenshots/cave.png" alt="Bajo tierra, con la mochila" width="400"><br><sub>Bajo tierra, con la mochila</sub></td>
  </tr>
</table>

## Características

En una New 3DS:

| | Qué esperar |
| :--- | :--- |
| **Las dos pantallas llenas** | Las imágenes de la DS se dibujan 1,25 veces más grandes: la pantalla inferior llena sus 320×240, y la superior se reparte a lo largo de sus 400 píxeles de ancho (menús, cuadros de texto y combates incluidos). Nunca se estira ni se refleja nada para llenar el espacio extra. |
| **Un menú en la pantalla inferior** | En el mapa, la pantalla inferior es un menú junto al Pokéreloj: MAP, POKéMON, BAG, TRAINER, POKéDEX, POKéTCH, SAVE y OPTIONS, a un toque (o con X y la cruceta). La mochila, el equipo, los datos del Pokémon, la ficha de entrenador, el guardado, las opciones y el Mapa Pueblo se abren dentro de su panel mientras el mapa sigue en la pantalla superior. Dos casillas guardan tus objetos registrados: toca una para usarlo. |
| **3D más nítido** | El mundo y los combates los dibuja la GPU de la 3DS a la resolución de la propia pantalla, con bordes suavizados (antialiasing), la niebla del juego y una vista más amplia del mundo a los lados. Los contornos de la DS alrededor del 3D vienen apagados; `edges on` en `remaster.txt` los dibuja. |
| **3D estereoscópico** | Con el deslizador 3D. |
| **60 imágenes por segundo** | El juego mueve el mapa a 30; el port dibuja una imagen intermedia entre cada par, y vuelve a 30 donde la GPU no da abasto (nuevo, todavía en ajuste). |
| **Mejoras y trucos** | Opcionales, desde OPTIONS: SPEED hasta 4x (los menús y las decisiones en combate siguen a 1x), Repartir Experiencia para todo el equipo, experiencia al capturar, evoluciones por intercambio al nivel 40, MO sin enseñar los movimientos, correr siempre, tu primer Pokémon siguiéndote (FOLLOWER: los sprites de HeartGold/SoulSilver, que descarga el creador; sale de la Poké Ball en la que lo capturaste y puedes hablarle: reacciona y a veces te encuentra un objeto); y trucos (captura segura, victoria instantánea, huevos rápidos, dinero infinito, frecuencia de Pokémon salvajes, probabilidad de shiny, dar objetos). |
| **Pequeñas comodidades** | Si capturas un Pokémon con el equipo lleno, puedes cambiarlo por uno de tu equipo ahí mismo, en el combate. Una partida nueva guarda encima de la anterior sin tener que borrarla antes. L + R + START + SELECT reinicia el juego desde la intro. |
| **Píxeles nítidos** | Píxeles parejos y nítidos en el 2D, y suavizado xBRZ opcional para las texturas del 3D. |
| **En el menú HOME** | Se instala como `.cia`, con el icono de tu cartucho y un banner 3D animado. |
| **Hecho con tu propio cartucho** | El builder crea los datos del juego en tu propia computadora, a partir de tu propio volcado. No se distribuye nada del juego. |

## Estado

| ✅ Funciona | 🛠️ En progreso | 💭 Lista de deseos |
| :--- | :--- | :--- |
| Jugable en consola real, desde una partida nueva o una guardada: explorar, combates, menús y guardar (todavía tiene algunos errores visuales y de juego) | Pulir la interfaz de la pantalla inferior (la pantalla para aprender un movimiento) | Wi-Fi |
| Una interfaz nueva para la pantalla superior: menús, cuadros de texto y combates a lo largo de todo su ancho | Un Pokémon que te sigue | Mejor iluminación |
| Una interfaz nueva para la pantalla inferior: un menú junto al Pokéreloj, con la mochila, el equipo, los datos del Pokémon, la ficha de entrenador, el guardado, las opciones y el Mapa Pueblo en su panel | 60 FPS y mejor calidad en el 3D | Batallas a 60 FPS |
| Mejoras y trucos desde las opciones (SPEED, Repartir Experiencia para el equipo y más) |Resolución de Pixel Exacta |Resolución de Pixel Exacta en batallas |
| La resolución de la New 3DS XL: las dos pantallas llenas, el 3D dibujado a la resolución de la pantalla | | |
| Animaciones actualizadas para la intro, y animaciones nuevas para parte de la interfaz | | |


> [!IMPORTANT]
> **Este repositorio no contiene nada del juego.** Aquí y en las versiones
> publicadas no se guarda ninguna ROM, gráfico, música, texto ni ningún otro
> dato del juego. Las versiones publicadas traen el programa del port; los
> datos del juego se hacen desde **tu propio cartucho**, en tu computadora o en
> tu navegador, y nunca salen de ahí. No compartas los archivos que hagas:
> contienen los datos de tu cartucho.

## Cómo empezar

### Qué necesitas

- Una **consola de la familia 3DS** con custom firmware (Luma3DS) y el Homebrew
  Launcher (y **FBI** para la versión del menú HOME). Para la pantalla
  remasterizada hace falta una New 3DS, New 3DS XL o New 2DS XL. Guía para
  prepararla: <https://3ds.hacks.guide/es_ES/>
- Tu propio volcado de **Pokémon Platinum (USA)**, Rev 0 o Rev 1.
- Una computadora con un navegador actualizado. Solo para compilarlo tú mismo:
  [devkitPro](https://devkitpro.org/wiki/Getting_Started) con el grupo de
  paquetes `3ds-dev`, Python 3.10 o superior y Git.
- Opcional: el emulador [Azahar](https://azahar-emu.org) para probar en la PC.
  Corre la CPU de la 3DS más lento que una New 3DS real, así que juzga el
  rendimiento en la consola.

### 1. Haz los archivos del juego

**La forma fácil: [el creador web](https://gabesud0.github.io/pokeplatinum-3ds/?lang=es).** Elige tu ROM, presiona
**Crear** y descarga un ZIP con todos los archivos de abajo, listos para la
tarjeta SD. Todo pasa en tu navegador: tu ROM nunca se sube a ningún lado.
También hace la versión del menú HOME, con un banner animado hecho desde tu
cartucho.

**O compílalo tú mismo**, desde el código fuente:

<details>
<summary><strong>Windows</strong></summary>

<br>

Descarga [`tools/build_windows_msys2.sh`](tools/build_windows_msys2.sh) y
ejecútalo en la terminal MSYS2 de devkitPro (menú Inicio → devkitPro → MSYS2).
Hace todo: instala lo que la compilación necesita aparte de devkitPro, clona
este repositorio, compila el juego (las dos versiones), crea tu paquete de
datos y deja los archivos de la tarjeta SD en `%USERPROFILE%\pokeplatinum-sd`
(y en la carpeta SD de Azahar, si la encuentra):

```sh
bash build_windows_msys2.sh 'C:\ruta\a\tu\platinum.nds'
```

</details>

<details>
<summary><strong>Linux</strong> (incluidos Bazzite y sistemas tipo SteamOS)</summary>

<br>

Descarga [`tools/build_linux.sh`](tools/build_linux.sh) en tu carpeta personal
y ejecútalo. Igual que el de Windows, hace todo: instala devkitPro y el resto
de lo que la compilación necesita, clona este repositorio, compila el juego
(las dos versiones), crea tu paquete de datos y deja los archivos de la
tarjeta SD en `~/pokeplatinum-sd` (y en la carpeta SD de Azahar, si la
encuentra):

```sh
bash ~/build_linux.sh ~/ruta/a/tu/platinum.nds
```

En **Bazzite** y otros sistemas inmutables (Fedora Silverblue, Kinoite,
Aurora, Bluefin...), trabaja dentro de un distrobox que crea una sola vez
(`pokeplatinum-3ds`, a partir de la imagen oficial de devkitPro, la misma con
la que compila la CI), así que tu sistema no cambia; deja la ROM dentro de tu
carpeta personal. En **Ubuntu o Debian** corre directamente. Ejecútalo otra vez
para actualizar y volver a compilar.

</details>

<details>
<summary><strong>A mano</strong> (Linux o macOS)</summary>

<br>

1. **Descarga la decompilación y los SDK, y compila el juego:**

   ```sh
   python tools/bootstrap.py
   make game                                   # -> 3ds_port/game/build/3ds/pokeplatinum.3dsx
   make game-cia ROM=/ruta/a/platinum.nds      # -> .../pokeplatinum.cia (versión del menú HOME)
   ```

2. **Crea tu paquete de datos** a partir de tu propio volcado
   ([`builder/`](builder/README.md)):

   ```sh
   python builder/verify_rom.py /ruta/a/platinum.nds    # comprueba que es un volcado compatible
   python builder/extract_rom.py /ruta/a/platinum.nds   # -> data_pack/
   python builder/pack.py data_pack/                    # -> platinum3ds.pak
   ```

</details>

### 2. Copia a la tarjeta SD

El ZIP del creador web los trae en sus carpetas `3ds` y `cia`: copia las dos a
la raíz de la tarjeta SD.

```
sdmc:/3ds/pokeplatinum/platinum3ds.pak
sdmc:/3ds/pokeplatinum/manifest.json      (de data_pack/)
sdmc:/3ds/pokeplatinum.3dsx               (para el Homebrew Launcher)
sdmc:/cia/pokeplatinum.cia                (para el menú HOME, opcional)
```

En Azahar, van en la carpeta de su tarjeta SD emulada.

### 3. Juega

- **Homebrew Launcher:** abre `pokeplatinum.3dsx`, o ábrelo en Azahar.
- **Menú HOME:** en FBI, ve a **SD → cia → pokeplatinum.cia → Install CIA**.
  Después el juego se abre y se cierra desde el menú HOME como cualquier otro.
  Para actualizarlo, instala el `.cia` nuevo de la misma forma; tu partida se
  conserva.
- **Actualizar:** crea la nueva versión con el creador web y copia sus archivos
  encima de los anteriores. Cuando una versión lee sus datos igual que la
  anterior, su página lo dice: entonces basta con copiar el nuevo
  `pokeplatinum.3dsx`.

El juego guarda en `sdmc:/3ds/pokeplatinum/platinum.sav`, un guardado de 512 KB
en bruto como los de los emuladores, así que puedes copiar ahí el guardado de
tu cartucho.

### Mientras juegas

Todos los botones son los del juego; en el mapa, la pantalla inferior es un
menú que puedes tocar. L + R + START + SELECT (el reinicio del propio juego)
reinicia el juego desde la intro. El port escribe un registro en
`sdmc:/3ds/pokeplatinum/log.txt` (envíalo cuando reportes un problema); aparece
solo en pantalla si algo sale mal.

Las opciones del juego, los 60 FPS, el 3D suavizado, y las mejoras y los
trucos están en la página **OPTIONS** de la pantalla inferior; el port guarda
los suyos en `sdmc:/3ds/pokeplatinum/options.txt`, junto a la partida. La
pantalla tiene más ajustes en `sdmc:/3ds/pokeplatinum/remaster.txt`; los
valores por defecto son los que casi todos quieren. Consulta
[`docs/REMASTER.md`](docs/REMASTER.md) (en inglés).

Donde la distribución del port para la pantalla más ancha no se vea bien,
puedes dibujar las franjas tú mismo con el editor **WideArt** (los dibujos
hechos hasta ahora, como el fondo de la evolución, ya vienen en el juego):
consulta
[`docs/WIDE_ART.md`](docs/WIDE_ART.md) (en inglés).

> [!TIP]
> **¿Necesitas ayuda?** Pregunta en [Discord](https://discord.gg/YjyA5ygpC) o
> abre un issue, e incluye tu consola o emulador, la revisión de tu ROM y
> `log.txt`.

## Para desarrolladores

<details>
<summary><strong>Cómo funciona</strong></summary>

<br>

```
tu ROM ──► builder/ ──► platinum3ds.pak (se queda en tu computadora / tarjeta SD)

pret/pokeplatinum @ commit fijo ──► + patches/ ──► + 3ds_port/ ──► .3dsx / .cia
```

- El código C original del juego se compila con GCC para la CPU de la 3DS. La
  decompilación está fijada en [`upstream.lock`](upstream.lock); los cambios
  del port viven como archivos de parche, pequeños y fáciles de aplicar sobre
  una versión más nueva.
- Las bibliotecas de DS de Nintendo (NitroSDK y NitroSystem) también se
  compilan para la 3DS ([`3ds_port/nitro`](3ds_port/nitro),
  [`3ds_port/nitrosystem`](3ds_port/nitrosystem)). El hardware de DS con el
  que hablan lo pone el port: un renderizador 2D, un motor de geometría 3D que
  pasa las escenas a la GPU de la 3DS, un chip de sonido de DS por software y
  los servicios de la segunda CPU de la DS (chip de guardado, reloj, energía,
  pantalla táctil).
- El juego lee sus archivos de tu paquete de datos, por sus ID de archivo
  originales ([`docs/PAK_FORMAT.md`](docs/PAK_FORMAT.md)).
- El trabajo se reparte entre los núcleos de la New 3DS: el juego, el motor de
  geometría y el dibujo corren cada uno en el suyo.

</details>

<details>
<summary><strong>Compilar y probar</strong></summary>

<br>

```sh
python tools/bootstrap.py   # una vez: descarga la decompilación y los SDK, aplica los parches
make test                   # compila para la PC y corre todas las pruebas
make game                   # el juego para la 3DS
make game-cia ROM=...       # lo mismo, como .cia para el menú HOME
```

El mismo código del juego también compila como **`pokeplatinum-pc`**, un
ejecutable para PC sin pantalla, para ver rápido hasta dónde llega el juego y
dónde se detiene ([`3ds_port/game/README.md`](3ds_port/game/README.md), en
inglés, que también explica cómo depurar cuelgues y el audio).

Para compilar la ROM original de DS a partir de la decompilación como
comprobación, sigue el [INSTALL.md](https://github.com/pret/pokeplatinum/blob/main/INSTALL.md)
de pret dentro de `build/upstream/`.

</details>

<details>
<summary><strong>Qué contiene este repositorio</strong></summary>

<br>

- el código propio del port (`3ds_port/`),
- parches que se aplican sobre la decompilación y los SDK (`patches/`),
- herramientas que descargan la decompilación y compilan el port (`tools/`),
- el builder, que convierte **tu propio cartucho, volcado legalmente,** en los
  datos del juego, en tu propia computadora (`builder/`).

La CI compila y prueba todo en cada push, pero solo sube informes de texto y
herramientas sin código del juego (`tools/check_ci_artifacts.py` hace fallar
la compilación si no es así). Nunca subas al repositorio una ROM, un archivo
extraído ni una compilación del juego. El `.gitignore` bloquea los casos
comunes, pero revisa `git status` antes de hacer commit.

```
3ds_port/          código del lado de la 3DS
  game/            el juego: el programa de 3DS, el ejecutable para PC, la pantalla, el sonido, los controles
  nitro/           NitroSDK compilado con GCC, la capa de hardware de DS, pruebas unitarias en PC
  nitrosystem/     las bibliotecas Fnd/G2d/G3d/Gfd/Snd de NitroSystem y sus pruebas
  pak/             el lector del paquete de datos
  common.mk        opciones de compilación compartidas (host, host32 y devkitARM)
builder/           convierte tu propia ROM en el paquete de datos (Python)
docs/              la pantalla remasterizada, el arte ancho, el paquete de datos
patches/           cambios aplicados sobre la decompilación y los SDK
tools/             bootstrap, ayudas de compilación, el editor WideArt (Python)
web/               el sitio del creador web (GitHub Pages; tools/build_site.py)
wideart/           franjas dibujadas a mano para la pantalla más ancha, incluidas en el juego (sin píxeles del juego)
Makefile           `make game`, `make game-cia`, `make test`, ...
LICENSE            GPL-3.0, para el código propio de este proyecto
upstream.lock      el commit exacto de pret/pokeplatinum sobre el que se compila
```

</details>

### Hoja de ruta

- **Hecho:** las herramientas de compilación; el código del juego compilado y
  corriendo en la 3DS; 2D, 3D, sonido, controles, guardado; el juego
  completamente jugable; la interfaz nueva de la pantalla superior; la
  pantalla inferior como menú, con la mochila, el equipo, los datos del
  Pokémon, la ficha de entrenador, el guardado, las opciones y el Mapa
  Pueblo en su panel; mejoras y trucos; la resolución de la New 3DS XL;
  animaciones actualizadas de la intro y la interfaz; la versión del menú
  HOME.
- **En progreso:** 60 imágenes por segundo, mejor calidad en el 3D, pulir la
  interfaz de la pantalla inferior, mejor iluminación, un Pokémon que te sigue.
- **Lista de deseos:** Wi-Fi.
- **Más adelante:** SoulSilver / HeartGold sobre la misma base.

### Contribuir

Consulta [`CONTRIBUTING.md`](CONTRIBUTING.md) para ver cómo se hacen y prueban
los cambios, y [`AI_DISCLOSURE.md`](AI_DISCLOSURE.md) para ver cómo se usa la
ayuda de IA (los dos en inglés).

## Comunidad

Únete al **[servidor de Discord](https://discord.gg/YjyA5ygpC)** para seguir el
desarrollo, compartir capturas y pedir ayuda. ¿Encontraste un error? Abre un
[issue](https://github.com/GabeSud0/pokeplatinum-3ds/issues) con tu `log.txt`.

## Créditos y aspectos legales

El código original de este repositorio tiene licencia
[GNU General Public License v3.0](LICENSE). Eso cubre solo el código propio de
este proyecto, no el juego ni la decompilación.

Hecho sobre el trabajo de los equipos de decompilación de
[pret](https://pret.github.io/), e inspirado por
[pokeemerald-3Ds-dualscreen](https://github.com/ZallaxDev/pokeemerald-3Ds-dualscreen).
Consulta [`NOTICE.md`](NOTICE.md).

Este es un proyecto no oficial hecho por fans, sin relación con Nintendo, Game
Freak, Creatures ni The Pokémon Company, ni respaldado por ellos. Pokémon es una
marca registrada de sus respectivos dueños.

> Esta traducción sigue al [README en inglés](README.md); si algo no coincide,
> manda el inglés.
