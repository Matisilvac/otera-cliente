# Cliente de Otera

El cliente para jugar en **[oteraot.com](https://oteraot.com)**, un servidor de
Tibia 8.6 con el mapa real.

Este repositorio no tiene codigo: existe para publicar las descargas.

## Bajarlo

| Sistema | Descarga |
|---|---|
| Windows (64 bits) | [otera-windows.zip](https://github.com/Matisilvac/otera-cliente/releases/latest/download/otera-windows.zip) |
| macOS (Apple Silicon) | [otera-macos.zip](https://github.com/Matisilvac/otera-cliente/releases/latest/download/otera-macos.zip) |
| Linux (x86-64) | [otera-linux.tar.gz](https://github.com/Matisilvac/otera-cliente/releases/latest/download/otera-linux.tar.gz) |

Se descomprime y se abre. No hay instalador. Trae los graficos adentro, el
servidor ya cargado y el mapa entero descubierto en el minimapa.

Se entra con la misma cuenta y contrasena de [oteraot.com](https://oteraot.com).

La primera vez el sistema avisa que el programa viene de internet, porque no
esta firmado:

- **Windows**: Mas informacion -> Ejecutar de todas formas.
- **macOS**: `xattr -dr com.apple.quarantine <la carpeta Otera>` en la Terminal.
  `Otera.app` tiene que quedar junto a `data/`, `modules/` e `init.lua`: el
  cliente busca sus archivos al lado de la app.
- **Linux**: `chmod +x Otera` si hace falta.

## Que es

[OTClient](https://github.com/opentibiabr/otclient) 4.1 sin modificaciones al
binario, con:

- el set de graficos de 8.60 extendido, que el mapa real necesita;
- `minimap.otmm` con el mundo entero ya descubierto;
- el servidor precargado, asi el login solo pide cuenta y contrasena;
- los modulos propios del servidor (market sobre protocolo 8.60, barra de
  aliento de la sala de entrenamiento);
- sin cavebot, sin tienda y sin los paneles que dependen de servicios de
  Tibia 12 que este servidor no tiene.

Desde Otera 1.9.0 los tres binarios se compilan desde el
[tag 4.1](https://github.com/opentibiabr/otclient/releases/tag/4.1) de opentibiabr
con un parche chico de autowalk, [`client/patches/otclient-4.1-autowalk.patch`](client/patches/otclient-4.1-autowalk.patch).
Windows y Linux los compila el workflow
[`client-native.yml`](.github/workflows/client-native.yml) de este repo, que
deja `native-build.json` con el commit de origen, el hash del parche y el SHA256
del ejecutable; el empaquetador del servidor verifica eso antes de aceptar cada
binario. El de macOS se compila localmente desde el mismo tag y parche.

## Licencias

OTClient es MIT (ver el repositorio de upstream). Los archivos de graficos
(`Tibia.spr`, `Tibia.dat`) son propiedad de CipSoft GmbH y no estan cubiertos
por esa licencia: se incluyen para poder jugar, como hace cualquier cliente de
Open Tibia. Otera no tiene relacion con CipSoft.
