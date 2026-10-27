# Release notes

Each version's notes, written before it's tagged: `docs/releases/<tag>.md`
(for example `v0.1.0.md`, or `v0.2.0-beta.1.md` for a pre-release). When the
tag is pushed, the release job (`.github/workflows/build.yml`) puts the file
at the top of the release's notes on GitHub, and the website shows them under
"What's new". Without a file, the notes list the pull requests merged since
the last release.

Write for players: what's new, what was fixed, known issues, and whether the
data pack must be made again (the website works that out by itself too). The
notes can be edited later on the release's GitHub page: the website follows.
