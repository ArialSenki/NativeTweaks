// Built-in NativeTweaks template for sv_config.toml (Souls Vision 0.6.x,
// github.com/samjviana/souls_vision). Uses the structs of
// solid_uncapper_templates.inc. The file is TOML, but the subset Souls Vision
// writes (tables, key = value, trailing # comments) reads like an INI.
// Keys are "<table>.<key>": visible/hideText repeat in every bar table.
//
// Souls Vision checks the file every 100 ms and reloads it by itself, and
// draws every value from the reloaded config each frame. Its reload does not
// rebuild the font or the textures, so fontSize and opacity, like delay, only
// apply after restarting the game: they get their own submenu so every other
// row changes live. debug (a console for development) is hidden.
// The page follows this table's order (BuiltinIni::template_order).
// hideBlightMadness is hidden: 0.6.1 reads it from [general] but writes it to
// [appearance] on every start, so the one in the file never takes effect.
// Integers are written without decimals (Souls Vision writes some as 39.0;
// both load). fontSize keeps one decimal so it stays a TOML float.

#define NT_SV_LOOK "Appearance", "Apariencia"
#define NT_SV_PLACE "Position and size", "Posición y tamaño"
#define NT_SV_BARS "Bars shown", "Barras visibles"
#define NT_SV_TEXT "Bar values", "Valores de las barras"
#define NT_SV_PARTS "Icons", "Iconos"
#define NT_SV_LATER "Requires restart", "Requiere reiniciar"
#define NT_SV_RESTART " Applies after restarting the game."
#define NT_SV_RESTART_ES " Se aplica al reiniciar el juego."

#define NT_SV_BAR(table, name_es) \
    {"components." table ".visible", "bool", nullptr, nullptr, nullptr, nullptr, \
        NT_SV_BARS, name_es, \
        "Off hides this bar.", \
        "Desactivado oculta esta barra."}, \
    {"components." table ".hideText", "bool", nullptr, nullptr, nullptr, nullptr, \
        NT_SV_TEXT, "Ocultar números: " name_es, \
        "Hides the numbers on this bar (e.g. 90/219).", \
        "Oculta los números de esta barra (p. ej. 90/219)."}

constexpr BuiltinTemplate kSoulsVisionTemplates[] = {
    NT_SV_BAR("hp", "Vida"),
    NT_SV_BAR("fp", "PC"),
    NT_SV_BAR("stamina", "Aguante"),
    NT_SV_BAR("stagger", "Tambaleo"),
    NT_SV_BAR("poison", "Veneno"),
    NT_SV_BAR("scarletRot", "Putrefacción escarlata"),
    NT_SV_BAR("hemorrhage", "Hemorragia"),
    NT_SV_BAR("deathBlight", "Plaga mortal"),
    NT_SV_BAR("frostbite", "Congelación"),
    NT_SV_BAR("sleep", "Sueño"),
    NT_SV_BAR("madness", "Locura"),

    {"components.bestEffects", "bool", nullptr, nullptr, nullptr, nullptr,
        NT_SV_PARTS, "Mejores efectos",
        "Shows the status effects that work best against the enemy.",
        "Muestra los efectos de estado que mejor funcionan contra el enemigo."},
    {"components.immuneEffects", "bool", nullptr, nullptr, nullptr, nullptr,
        NT_SV_PARTS, "Inmunidades",
        "Shows the status effects the enemy is immune to.",
        "Muestra los efectos de estado a los que el enemigo es inmune."},
    {"components.dmgTypes", "bool", nullptr, nullptr, nullptr, nullptr,
        NT_SV_PARTS, "Tipos de daño",
        "Shows the damage types the enemy is weak or resistant to.",
        "Muestra los tipos de daño a los que el enemigo es débil o resistente."},
    {"components.neutralDmgTypes", "bool", nullptr, nullptr, nullptr, nullptr,
        NT_SV_PARTS, "Tipos de daño neutros",
        "Also shows the damage types with no weakness or resistance (needs Damage types on).",
        "Muestra también los tipos de daño sin debilidad ni resistencia (necesita Tipos de daño activado)."},

    {"appearance.bestEffects", "int", "0", "7", "1", nullptr,
        NT_SV_LOOK, "Mejores efectos",
        "How many of the best status effects against the enemy to show, easiest to trigger first. Default 7.",
        "Cuántos de los mejores efectos contra el enemigo se muestran, empezando por el más fácil de provocar. Por defecto, 7."},
    {"appearance.maxEffectBars", "int", "0", "7", "1", nullptr,
        NT_SV_LOOK, "Barras de efectos (máx.)",
        "Maximum number of status buildup bars shown at once. Default 7.",
        "Número máximo de barras de acumulación de efectos a la vez. Por defecto, 7."},
    {"appearance.bestEffectIconSize", "int", "10", "100", "1", nullptr,
        NT_SV_LOOK, "Tamaño de iconos de efectos",
        "Size of the best effect icons. Default 39.",
        "Tamaño de los iconos de mejores efectos. Por defecto, 39."},
    {"appearance.dmgTypeIconSize", "int", "10", "100", "1", nullptr,
        NT_SV_LOOK, "Tamaño de iconos de daño",
        "Size of the damage type icons. Default 30.",
        "Tamaño de los iconos de tipo de daño. Por defecto, 30."},
    {"appearance.statBarSpacing", "int", "0", "50", "1", nullptr,
        NT_SV_LOOK, "Separación entre barras",
        "Space between the bars in pixels. Default 0.",
        "Espacio entre las barras, en píxeles. Por defecto, 0."},

    {"statBar.position.x", "int", "0", "7680", "5", nullptr,
        NT_SV_PLACE, "Posición horizontal",
        "Distance of the bars from the left edge, in pixels. For fine placement turn on Drag with the mouse.",
        "Distancia de las barras al borde izquierdo, en píxeles. Para colocarlas con precisión, activa Mover con el ratón."},
    {"statBar.position.y", "int", "0", "4320", "5", nullptr,
        NT_SV_PLACE, "Posición vertical",
        "Distance of the bars from the top edge, in pixels. Default 10.",
        "Distancia de las barras al borde superior, en píxeles. Por defecto, 10."},
    {"statBar.size.width", "int", "100", "2000", "10", nullptr,
        NT_SV_PLACE, "Anchura de las barras",
        "Width of the bars in pixels. Default 600.",
        "Anchura de las barras, en píxeles. Por defecto, 600."},
    {"statBar.size.height", "int", "10", "120", "1", nullptr,
        NT_SV_PLACE, "Altura de las barras",
        "Height of each bar in pixels; the effect icons follow it. Default 40.",
        "Altura de cada barra, en píxeles; los iconos de efectos la siguen. Por defecto, 40."},
    {"general.dragOverlay", "bool", nullptr, nullptr, nullptr, nullptr,
        NT_SV_PLACE, "Mover con el ratón",
        "Lets you drag the overlay with the mouse. Souls Vision saves the new position itself.",
        "Permite arrastrar la superposición con el ratón. Souls Vision guarda la nueva posición."},

    {"general.opacity", "float", "0.10", "1.00", "0.05", nullptr,
        NT_SV_LATER, "Opacidad",
        "Opacity of the overlay, from 0.1 (almost transparent) to 1 (opaque)." NT_SV_RESTART,
        "Opacidad de la superposición, de 0,1 (casi transparente) a 1 (opaca)." NT_SV_RESTART_ES},
    {"general.fontSize", "float", "10.0", "40.0", "1.0", nullptr,
        NT_SV_LATER, "Tamaño de letra",
        "Font size of the numbers on the bars. Default 18." NT_SV_RESTART,
        "Tamaño de letra de los números de las barras. Por defecto, 18." NT_SV_RESTART_ES},
    {"general.delay", "int", "0", "10000", "500", nullptr,
        NT_SV_LATER, "Retraso al iniciar (ms)",
        "Waits this long after loading before starting the overlay. 2000 to 7000 helps if the game crashes at start (e.g. with Convergence)." NT_SV_RESTART,
        "Espera este tiempo tras la carga antes de iniciar la superposición. Entre 2000 y 7000 ayuda si el juego se cierra al arrancar (p. ej. con Convergence)." NT_SV_RESTART_ES},

    {"general.debug", "hidden", nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr},
    {"appearance.hideBlightMadness", "hidden", nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr},
};

#define NT_SV_BAR_LABELS(table, name) \
    {"components." table ".visible", name}, \
    {"components." table ".hideText", "Hide numbers: " name}

constexpr BuiltinLabel kSoulsVisionLabelsEn[] = {
    {"general.dragOverlay", "Drag with the mouse"},
    {"general.opacity", "Opacity"},
    {"general.fontSize", "Font size"},
    {"general.delay", "Start delay (ms)"},
    {"appearance.bestEffects", "Best effects"},
    {"appearance.maxEffectBars", "Effect bars (max)"},
    {"appearance.bestEffectIconSize", "Effect icon size"},
    {"appearance.dmgTypeIconSize", "Damage icon size"},
    {"appearance.statBarSpacing", "Bar spacing"},
    {"statBar.position.x", "Horizontal position"},
    {"statBar.position.y", "Vertical position"},
    {"statBar.size.width", "Bar width"},
    {"statBar.size.height", "Bar height"},
    NT_SV_BAR_LABELS("hp", "HP"),
    NT_SV_BAR_LABELS("fp", "FP"),
    NT_SV_BAR_LABELS("stamina", "Stamina"),
    NT_SV_BAR_LABELS("stagger", "Stagger"),
    NT_SV_BAR_LABELS("poison", "Poison"),
    NT_SV_BAR_LABELS("scarletRot", "Scarlet rot"),
    NT_SV_BAR_LABELS("hemorrhage", "Hemorrhage"),
    NT_SV_BAR_LABELS("deathBlight", "Death blight"),
    NT_SV_BAR_LABELS("frostbite", "Frostbite"),
    NT_SV_BAR_LABELS("sleep", "Sleep"),
    NT_SV_BAR_LABELS("madness", "Madness"),
    {"components.bestEffects", "Best effects"},
    {"components.immuneEffects", "Immunities"},
    {"components.dmgTypes", "Damage types"},
    {"components.neutralDmgTypes", "Neutral damage types"},
};

#undef NT_SV_BAR
#undef NT_SV_BAR_LABELS
