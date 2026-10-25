# Instalar NativeTweaks con Elden Mod Loader

Guía para instalar NativeTweaks (**r132**) con **Elden Mod Loader** (EML, de techiew) en lugar de Mod Organizer 2 o Mod Engine. Las instrucciones cortas de los cuatro cargadores (MO2, ME3, ME2, EML) están en el `README.txt` del paquete y en la descripción de Nexus.

> ⚠️ **Sin probar.** El autor solo ha probado NativeTweaks con Mod Organizer 2. Estos pasos siguen la configuración propia de Elden Mod Loader; si algo no funciona, envía los logs (ver *Solución de problemas*).

> ⚠️ **Solo sin conexión.** NativeTweaks modifica la memoria del juego. Úsalo siempre con Easy Anti-Cheat (EAC) desactivado y nunca online.

## Requisitos

- **Elden Ring 2.7.2.0, 2.7.1.0 o 2.7.1.1**, PC (Steam). En otras versiones, las opciones cuyos datos no se encuentren aparecen en gris.
- **Elden Mod Loader** (de su página en Nexus Mods), compatible con tu versión del juego.
- **`NativeTweaks-r132.zip`**.

## 1. Instala Elden Mod Loader

Si ya lo tienes funcionando con otros mods, pasa al paso 2.

1. Abre la carpeta del juego: en Steam, clic derecho en *ELDEN RING* → *Administrar* → *Ver archivos locales*, y entra en **`Game`** (donde está `eldenring.exe`).
2. Copia ahí **`dinput8.dll`** y **`mod_loader_config.ini`** de Elden Mod Loader.
3. Crea la carpeta **`mods`** dentro de `Game` si no existe.
4. Desactiva EAC: crea en `Game` un archivo `steam_appid.txt` que contenga solo `1245620` y arranca el juego con `eldenring.exe` (o usa tu método habitual).

## 2. Copia NativeTweaks en `mods`

Descomprime el zip y copia **el contenido** de su carpeta `DLLs` (no la carpeta) en `Game\mods\`:

```
ELDEN RING\Game\
├─ eldenring.exe
├─ dinput8.dll                    ← Elden Mod Loader
├─ mod_loader_config.ini          ← Elden Mod Loader
└─ mods\
   ├─ ERNativeUI.dll
   ├─ ERNativeUI.ini
   ├─ NativeTweaks.dll
   └─ NativeTweaks.ini
```

- **Usa el `ERNativeUI.dll` incluido**, no el oficial: es un fork de ERNativeUI 1.2.1 con la pestaña propia y el menú adaptado en memoria. Los demás mods de ERNativeUI siguen funcionando con él.
- NativeTweaks crea junto a sus DLL la carpeta `NativeTweaks\` (perfiles, lista de enemigos muertos) y sus logs.
- No hay carpeta `menu` ni `locales`: el menú se adapta en memoria y los textos van dentro de las DLL.

## 3. Orden de carga

`ERNativeUI.dll` antes que `NativeTweaks.dll`, y los dos antes que Solid Uncapper si lo usas. En `Game\mod_loader_config.ini` (los números más bajos cargan antes; deja el resto del archivo igual):

```ini
[loadorder]
ERNativeUI.dll=1
NativeTweaks.dll=2
; otros mods DLL después, por ejemplo:
; SolidUncapper.dll=3
```

## 4. Comprueba que funciona

Arranca el juego sin EAC. En el menú **Sistema** debe aparecer **NativeTweaks** después de *Gráficos*, con *Ajustes vanilla* y una página por cada mod DLL de `mods` que tenga su `.ini` al lado.

## Actualizar desde una versión anterior

- Sobrescribe `ERNativeUI.dll` y `NativeTweaks.dll`. **Conserva tu `NativeTweaks.ini`** para mantener tus ajustes (las secciones nuevas se pueden copiar del INI nuevo).
- Puedes borrar restos de versiones antiguas: `menu\win\02_040_optionsetting.gfx`, `02_042_pc_graphicsetting.gfx`, la carpeta `locales` y `NativeTweaks.animations.txt` (si están, se siguen leyendo).

## Solución de problemas

| Problema | Qué probar |
|---|---|
| No aparece NativeTweaks | Confirma el orden de `[loadorder]` y que usas el `ERNativeUI.dll` incluido. Si sigue sin salir, baja `load_delay` en `mod_loader_config.ini` (p. ej. a `0`–`1000`) para que el menú se adapte antes de que el juego lo cargue, y envía los logs. |
| No carga ningún mod | `dinput8.dll` debe estar en `Game` (junto a `eldenring.exe`), no en `mods`, y el juego debe arrancar sin EAC. |
| Alerta en el juego que nombra un `.gfx` | Otro mod cambia demasiado el menú de opciones y no se puede adaptar; el juego lo usa sin cambios. |
| Opciones en gris | Tu versión del juego no es compatible o no se encontraron los datos de esa opción. |

**Para informar de un fallo:** pon `EnableLog = 1` en `mods\NativeTweaks.ini` y en `mods\ERNativeUI.ini`, reproduce el problema y envía `NativeTweaks.log` y `ERNativeUI.log` (se crean en `mods`).

**Desinstalar:** borra de `mods` los archivos del paso 2 (y la carpeta `NativeTweaks\` si no quieres conservar perfiles) y sus líneas en `[loadorder]`.
