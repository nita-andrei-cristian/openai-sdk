# OPENAI SDK for C

Deps: OpenSSL, POSIX

## Notes
This is not meant to be used in any production environment.
I made this toy SDK (heavily assisted by AI) to quickly implement AI in my main project.

This works for me, but I do not claim it's robust neither secure.

## Setup

 `1` Add openai.c and openai.h.

 `2` Add OPENAI_API_KEY in the env to make it work.

```bash
export OPENAI_API_KEY="sk-xxx"
```

 `3` Use it. Example compilation with gcc:.

```bash
gcc example.c ai_openai.c -lssl -lcrypto -o example
```

 `4` Dive deeper. See `example.c` for inspiration.

## License
GNU PUBLIC LICENSE
