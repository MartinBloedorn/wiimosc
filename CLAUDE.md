# Project

You working on wiimosc, a cross-platform utility that pairs to Nintendo Wiimotes (wiimote & nunchuck, wiifit scale) and converts their inputs into OSC signals.

## Information

wiiuse is a battle-tested library for connecting to Wiimotes, and is checked out under external/wiiuse. Link against it, or use it as reference implementation whenever needed.

doc/ contains references to other Wiimote-related projects, as well as details on the pairing mechanism of wiimotes.

Read doc/DevelopmentNotes.md first: it collects the technical decisions, findings, wiiuse patches, testing practices and open work.

# Coding

- Keep solutions simple.
- Ask clarification questions if needed.

## Style

- Use the Qt-style nomenclature as much as possible, primarily:
    - lowerCamelCase for variables and functions
    - mMembers for members, and sStaticStuff for static/globals
    - AbstractClass for abstract base classes
    - "is"-prefix for functons like "isEmpty"
- 4 spaces for identation

- Use doxygen-style documentation:
    - prefer "/*! ... */" single- or multiline blocks
    - add a single-line \brief, further details should be added separately below the brief.
    - don't document trivial parameters and return values; add information only if the functionality isn't evident