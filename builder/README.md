# Builder

The builder turns the player's **own** ROM dump into the data the 3DS port
needs, on the player's own computer. The ROM is only read; nothing from it is
uploaded or committed.

Today it contains one tool:

- `verify_rom.py`: checks a dump against the supported versions by SHA-1.

Planned (roadmap item 3.1): extract the game's files from the ROM, convert them
into formats the 3DS backend can load quickly, and write a data pack to the SD
card.
