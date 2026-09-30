# RNNoise 0.1.1

Supresión de ruido con una red neuronal chica, de Xiph (Jean-Marc Valin). Es la que
usan Mumble y OBS. Acá la usa `CaptureProcessor` ([voice_dsp.cpp](../voice_dsp.cpp))
para la casilla "Cancelacion de ruido" del chat de voz.

- Origen: `https://github.com/xiph/rnnoise/archive/refs/tags/v0.1.1.tar.gz`,
  sha256 `1641712c9ae31f3b364e8b2f2e0ec3ce24330e76055c151f695941b0ea5d3987`.
- Copiados tal cual `src/` (sin `rnn_reader.c`, que carga modelos de archivo y no
  hace falta), `include/rnnoise.h` y `COPYING` (BSD de 3 cláusulas).
- Un solo tipo de cambio: los arreglos de largo variable, que MSVC no compila, son
  arreglos fijos con un assert del tamaño: `xx` en `rnn_autocorr` (`celt_lpc.c`) y
  `x_lp4`, `y_lp4`, `xcorr` y `yy_lookup` en `pitch.c`. Para encontrarlos todos
  (grep se salta algunos): `cc -std=c99 -Werror=vla -fsyntax-only -I. *.c`.

Por qué vendorizada y no por vcpkg: el port de vcpkg compila con autotools, que no
anda en los runners de Windows (ver [docs/voice-chat.md](../../../../docs/voice-chat.md)).
La 0.2 trae un modelo de 5 a 15 MB compilado; la 0.1.1 lleva el suyo adentro y
suma unos 100 KB al ejecutable.
