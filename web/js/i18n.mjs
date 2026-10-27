// The website's words, in English and Spanish. index.html marks each piece
// of text with data-i18n="key" (data-i18n-html when it holds links); app.mjs
// fills them in for the chosen language. {name} placeholders are filled by
// t(key, { name: ... }).

export const STRINGS = {
  en: {
    "page.title": "Pokémon Platinum 3DS: build it from your cartridge",
    "lang.other": "Español",
    "hero.tagline": "Build the 3DS port of Pokémon Platinum from your own cartridge, right in this page.",
    "hero.privacy": "Your ROM is read by this page only. It never leaves your computer.",
    "hero.release": "Version {version}",
    "hero.prerelease": "Version {version}: an early version, with bugs. Keep a backup of your save, and report what you find.",
    "hero.noRelease": "No version has been released yet. Come back soon.",

    "top.idle.title": "Choose your Pokémon Platinum ROM to begin.",
    "top.idle.detail": "A clean dump of the USA cartridge, as a .nds file.",
    "top.checking.title": "Checking your ROM…",
    "top.size.detail": "This file is {size} MB: a Pokémon Platinum ROM is 128 MB.",
    "top.read.detail": "The file couldn't be read ({message}). Choose it again.",
    "top.ok.title": "Ready to build.",
    "top.ok.detail": "{name}",
    "top.bad.title": "This ROM can't be used.",
    "top.bad.detail": "Only clean dumps of Pokémon Platinum (USA), Rev 0 or Rev 1, work: not a hack, a trimmed file or another region's cartridge. Its SHA-1 is {sha1}.",
    "top.building.title": "Building…",
    "top.done.title": "Done.",
    "top.done.detail": "Unzip it and copy its folders to the root of your SD card.",
    "top.done.flat": "The HOME Menu version got the flat banner: the 3D one couldn't be made ({why}).",
    "top.error.title": "The build stopped.",
    "top.error.detail": "{message}",

    "stage.python": "Loading the builder (about 15 MB, once)",
    "stage.release": "Downloading this version's files",
    "stage.rom": "Reading your ROM",
    "stage.pack": "Packing the game's data",
    "stage.banner": "Making the HOME Menu banner",
    "stage.cia": "Making the HOME Menu version",
    "stage.zip": "Putting the ZIP together",

    "bottom.choose": "Choose ROM",
    "bottom.change": "Choose another ROM",
    "bottom.drop": "or drop the .nds file here",
    "bottom.cia": "Also make the HOME Menu version (.cia)",
    "bottom.build": "Build",
    "bottom.download": "Download the ZIP",

    "need.title": "What you need",
    "need.console": "A 3DS with custom firmware (Luma3DS) and the Homebrew Launcher, and FBI for the HOME Menu version. A New 3DS, New 3DS XL or New 2DS XL gets the full-width screens; an Old 3DS shows the DS's two screens in the middle. Setup guide: <a href=\"https://3ds.hacks.guide\">3ds.hacks.guide</a>.",
    "need.rom": "Your own Pokémon Platinum (USA) cartridge, dumped as a .nds file. GodMode9 on your 3DS can dump it: <a href=\"https://3ds.hacks.guide/dumping-titles-and-game-cartridges\">how to dump a cartridge</a>.",
    "need.computer": "A computer with an up-to-date browser and a way to copy files to the SD card.",

    "install.title": "Put it on your 3DS",
    "install.1": "Build above and download the ZIP.",
    "install.2": "Unzip it, and copy its <b>3ds</b> and <b>cia</b> folders to the root of your SD card, merging them with the folders already there.",
    "install.3": "Recommended: open FBI, go to <b>SD → cia → pokeplatinum.cia</b> and choose <b>Install CIA</b>. The game is then on the HOME Menu, and gets all the memory the full-width screens need.",
    "install.4": "Or open it from the Homebrew Launcher: Pokémon Platinum is in the list. Opened from the Rosalina menu, the Homebrew Launcher gives games less memory, and on a New 3DS the game may then show only the DS's two screens. The HOME Menu version doesn't have this problem.",
    "install.save": "The game saves to <code>sd:/3ds/pokeplatinum/platinum.sav</code>, a raw 512 KB save, so you can bring your cartridge's save along.",
    "install.azahar": "On a PC, the <a href=\"https://azahar-emu.org\">Azahar</a> emulator runs it too: copy the same folders into its emulated SD card and open <b>pokeplatinum.3dsx</b>.",

    "news.title": "What's new",
    "news.pre": "Pre-release",
    "news.built": "The version this page builds",
    "news.empty": "No notes for this version.",
    "news.github": "See it on GitHub",

    "update.title": "Updating",
    "update.intro": "Your save stays where it is. Choose the version you have now:",
    "update.pick": "My version",
    "update.same": "Your data pack still works with {version}: copy only the new <b>pokeplatinum.3dsx</b> to <b>sd:/3ds/</b>. If you use the HOME Menu version, build above to get the new .cia.",
    "update.changed": "{version} reads its data differently: build above with your ROM and copy everything again.",
    "update.unknown": "Build above with your ROM and copy everything again.",
    "update.download": "Download pokeplatinum.3dsx",

    "faq.title": "Questions",
    "faq.upload.q": "Is my ROM uploaded anywhere?",
    "faq.upload.a": "No. This page downloads the builder and this version's files, then everything happens in your browser. Your ROM, and the data made from it, stay on your computer.",
    "faq.rejected.q": "Why was my ROM rejected?",
    "faq.rejected.a": "The builder only accepts the two clean dumps of Pokémon Platinum (USA), Rev 0 and Rev 1. A dump of a European or Japanese cartridge, a hacked or trimmed file, or a file that was changed in any way has a different checksum.",
    "faq.cia.q": "Should I use the HOME Menu version?",
    "faq.cia.a": "Yes, on a New 3DS, New 3DS XL or New 2DS XL. It's the same game as the Homebrew Launcher version, but the .cia always gets the memory the full-width screens need, while the Homebrew Launcher can give it less (then you see only the DS's two screens). It also puts the game on the HOME Menu with its own icon and an animated banner made from your cartridge.",
    "faq.share.q": "Can I share the ZIP with a friend?",
    "faq.share.a": "Please don't: it holds the game's data from your cartridge. Your friend can build their own here from their cartridge.",
    "faq.help.q": "Something went wrong. Where do I get help?",
    "faq.help.a": "Ask on <a href=\"https://discord.gg/YjyA5ygpC\">Discord</a> or open an issue on <a href=\"https://github.com/GabeSud0/pokeplatinum-3ds/issues\">GitHub</a> with the message this page showed. On the 3DS, the port writes a log to <code>sd:/3ds/pokeplatinum/log.txt</code>.",

    "foot.source": "Source code",
    "foot.license": "The port's code is free software under the GPL-3.0.",
    "foot.legal": "A fan project, not affiliated with Nintendo, Game Freak, Creatures or The Pokémon Company. It includes none of the game: you need your own cartridge. Pokémon is a trademark of Nintendo, Creatures and Game Freak.",

    "error.network": "Couldn't download {what}. Check your connection and try again.",
    "error.memory": "Your browser ran out of memory. Close other tabs and try again, or use a computer instead of a phone.",
    "error.checksum": "A downloaded file was damaged ({what}). Reload the page and try again.",
  },

  es: {
    "page.title": "Pokémon Platinum 3DS: créalo desde tu cartucho",
    "lang.other": "English",
    "hero.tagline": "Crea el port de Pokémon Platinum para 3DS desde tu propio cartucho, aquí mismo en esta página.",
    "hero.privacy": "Tu ROM solo la lee esta página. Nunca sale de tu computadora.",
    "hero.release": "Versión {version}",
    "hero.prerelease": "Versión {version}: una versión temprana, con errores. Guarda una copia de tu partida y reporta lo que encuentres.",
    "hero.noRelease": "Todavía no hay ninguna versión publicada. Vuelve pronto.",

    "top.idle.title": "Elige tu ROM de Pokémon Platinum para empezar.",
    "top.idle.detail": "Un volcado limpio del cartucho de EE. UU., en un archivo .nds.",
    "top.checking.title": "Revisando tu ROM…",
    "top.size.detail": "Este archivo pesa {size} MB: una ROM de Pokémon Platinum pesa 128 MB.",
    "top.read.detail": "No se pudo leer el archivo ({message}). Vuelve a elegirlo.",
    "top.ok.title": "Listo para crear.",
    "top.ok.detail": "{name}",
    "top.bad.title": "Esta ROM no sirve.",
    "top.bad.detail": "Solo funcionan los volcados limpios de Pokémon Platinum (EE. UU.), Rev 0 o Rev 1: no un hack, un archivo recortado ni el cartucho de otra región. Su SHA-1 es {sha1}.",
    "top.building.title": "Creando…",
    "top.done.title": "Listo.",
    "top.done.detail": "Descomprímelo y copia sus carpetas a la raíz de tu tarjeta SD.",
    "top.done.flat": "La versión del menú HOME lleva el banner plano: no se pudo hacer el 3D ({why}).",
    "top.error.title": "La creación se detuvo.",
    "top.error.detail": "{message}",

    "stage.python": "Cargando el creador (unos 15 MB, una sola vez)",
    "stage.release": "Descargando los archivos de esta versión",
    "stage.rom": "Leyendo tu ROM",
    "stage.pack": "Empaquetando los datos del juego",
    "stage.banner": "Haciendo el banner del menú HOME",
    "stage.cia": "Haciendo la versión del menú HOME",
    "stage.zip": "Armando el ZIP",

    "bottom.choose": "Elegir ROM",
    "bottom.change": "Elegir otra ROM",
    "bottom.drop": "o suelta aquí el archivo .nds",
    "bottom.cia": "Hacer también la versión del menú HOME (.cia)",
    "bottom.build": "Crear",
    "bottom.download": "Descargar el ZIP",

    "need.title": "Qué necesitas",
    "need.console": "Una 3DS con firmware personalizado (Luma3DS) y el Homebrew Launcher, y FBI para la versión del menú HOME. Una New 3DS, New 3DS XL o New 2DS XL usa las pantallas completas; una 3DS anterior muestra las dos pantallas de la DS en el centro. Guía de instalación: <a href=\"https://3ds.hacks.guide\">3ds.hacks.guide</a>.",
    "need.rom": "Tu propio cartucho de Pokémon Platinum (EE. UU.), volcado como archivo .nds. GodMode9 en tu 3DS puede volcarlo: <a href=\"https://3ds.hacks.guide/dumping-titles-and-game-cartridges\">cómo volcar un cartucho</a>.",
    "need.computer": "Una computadora con un navegador actualizado y una forma de copiar archivos a la tarjeta SD.",

    "install.title": "Ponlo en tu 3DS",
    "install.1": "Créalo arriba y descarga el ZIP.",
    "install.2": "Descomprímelo y copia sus carpetas <b>3ds</b> y <b>cia</b> a la raíz de tu tarjeta SD, combinándolas con las carpetas que ya están ahí.",
    "install.3": "Recomendado: abre FBI, ve a <b>SD → cia → pokeplatinum.cia</b> y elige <b>Install CIA</b>. El juego queda en el menú HOME y tiene toda la memoria que necesitan las pantallas completas.",
    "install.4": "O ábrelo desde el Homebrew Launcher: Pokémon Platinum aparece en la lista. Abierto desde el menú Rosalina, el Homebrew Launcher les da menos memoria a los juegos, y en una New 3DS el juego puede mostrar solo las dos pantallas de la DS. La versión del menú HOME no tiene este problema.",
    "install.save": "El juego guarda en <code>sd:/3ds/pokeplatinum/platinum.sav</code>, una partida en bruto de 512 KB, así que puedes traer la partida de tu cartucho.",
    "install.azahar": "En una PC también funciona con el emulador <a href=\"https://azahar-emu.org\">Azahar</a>: copia las mismas carpetas a su tarjeta SD emulada y abre <b>pokeplatinum.3dsx</b>.",

    "news.title": "Novedades",
    "news.pre": "Versión preliminar",
    "news.built": "La versión que crea esta página",
    "news.empty": "Esta versión no tiene notas.",
    "news.github": "Verla en GitHub",

    "update.title": "Actualizar",
    "update.intro": "Tu partida se queda donde está. Elige la versión que tienes ahora:",
    "update.pick": "Mi versión",
    "update.same": "Tu paquete de datos sigue sirviendo con la {version}: copia solo el nuevo <b>pokeplatinum.3dsx</b> a <b>sd:/3ds/</b>. Si usas la versión del menú HOME, créala arriba para tener el nuevo .cia.",
    "update.changed": "La {version} lee sus datos de otra forma: créala arriba con tu ROM y vuelve a copiar todo.",
    "update.unknown": "Créala arriba con tu ROM y vuelve a copiar todo.",
    "update.download": "Descargar pokeplatinum.3dsx",

    "faq.title": "Preguntas",
    "faq.upload.q": "¿Se sube mi ROM a algún lado?",
    "faq.upload.a": "No. Esta página descarga el creador y los archivos de esta versión, y luego todo pasa en tu navegador. Tu ROM, y los datos que salen de ella, se quedan en tu computadora.",
    "faq.rejected.q": "¿Por qué rechazó mi ROM?",
    "faq.rejected.a": "El creador solo acepta los dos volcados limpios de Pokémon Platinum (EE. UU.), Rev 0 y Rev 1. El volcado de un cartucho europeo o japonés, un archivo modificado o recortado, o cualquier archivo cambiado tiene otra suma de verificación.",
    "faq.cia.q": "¿Debo usar la versión del menú HOME?",
    "faq.cia.a": "Sí, en una New 3DS, New 3DS XL o New 2DS XL. Es el mismo juego que la versión del Homebrew Launcher, pero el .cia siempre tiene la memoria que necesitan las pantallas completas, mientras que el Homebrew Launcher puede darle menos (y entonces ves solo las dos pantallas de la DS). Además pone el juego en el menú HOME con su propio ícono y un banner animado hecho desde tu cartucho.",
    "faq.share.q": "¿Puedo compartir el ZIP con un amigo?",
    "faq.share.a": "Mejor no: contiene los datos del juego de tu cartucho. Tu amigo puede crear el suyo aquí desde su propio cartucho.",
    "faq.help.q": "Algo salió mal. ¿Dónde pido ayuda?",
    "faq.help.a": "Pregunta en <a href=\"https://discord.gg/YjyA5ygpC\">Discord</a> o abre un issue en <a href=\"https://github.com/GabeSud0/pokeplatinum-3ds/issues\">GitHub</a> con el mensaje que mostró esta página. En la 3DS, el port escribe un registro en <code>sd:/3ds/pokeplatinum/log.txt</code>.",

    "foot.source": "Código fuente",
    "foot.license": "El código del port es software libre bajo la GPL-3.0.",
    "foot.legal": "Un proyecto de fans, sin relación con Nintendo, Game Freak, Creatures ni The Pokémon Company. No incluye nada del juego: necesitas tu propio cartucho. Pokémon es una marca registrada de Nintendo, Creatures y Game Freak.",

    "error.network": "No se pudo descargar {what}. Revisa tu conexión e inténtalo de nuevo.",
    "error.memory": "Tu navegador se quedó sin memoria. Cierra otras pestañas e inténtalo de nuevo, o usa una computadora en vez de un teléfono.",
    "error.checksum": "Un archivo descargado llegó dañado ({what}). Recarga la página e inténtalo de nuevo.",
  },
};

export function pickLanguage(saved, navigatorLanguages) {
  if (saved && STRINGS[saved]) return saved;
  for (const lang of navigatorLanguages || []) {
    const code = String(lang).slice(0, 2).toLowerCase();
    if (STRINGS[code]) return code;
  }
  return "en";
}

export function translate(lang, key, values = {}) {
  const text = STRINGS[lang]?.[key] ?? STRINGS.en[key] ?? key;
  return text.replace(/\{(\w+)\}/g, (_, name) => (name in values ? String(values[name]) : `{${name}}`));
}
