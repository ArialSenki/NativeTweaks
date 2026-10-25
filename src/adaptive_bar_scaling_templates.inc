// Built-in NativeTweaks template for AdaptiveBarScaling_config.ini
// (Adaptive Bar Scaling 1.5 port). Uses the structs of
// solid_uncapper_templates.inc. Without it bar_style (a word) would be left out
// of the generic editor. Build 4 of the mod applies every value live.

#define NT_BARS "HUD bars", "Barras del HUD"

constexpr BuiltinTemplate kAdaptiveBarScalingTemplates[] = {
    {"Enabled", "bool", nullptr, nullptr, nullptr, nullptr,
        NT_BARS, "Activado",
        "Turns Adaptive Bar Scaling on or off. Off gives the bars their vanilla limits back.",
        "Activa o desactiva Adaptive Bar Scaling. Desactivado, las barras vuelven a sus límites originales."},
    {"bar_style", "enum", nullptr, nullptr, nullptr, "dynamic:Dynamic|static:Static",
        NT_BARS, "Estilo de barra",
        "Dynamic: bars grow with your maximum once it passes the caps below. Static: bars are sized to fit the HUD frame from the start (sizes above 1 count as 1).",
        "Dinámico: las barras crecen con tu máximo cuando supera los topes de abajo. Estático: las barras se ajustan al marco del HUD desde el principio (tamaños mayores que 1 cuentan como 1)."},
    {"hp_bar_size", "float", "0.05", "3", "0.05", nullptr,
        NT_BARS, "Tamaño de la barra de vida",
        "Length of the HP bar. 0.5 is half the default length (the original mod's default).",
        "Longitud de la barra de vida. 0,5 es la mitad de la longitud original (el valor del mod original)."},
    {"fp_bar_size", "float", "0.05", "3", "0.05", nullptr,
        NT_BARS, "Tamaño de la barra de PC",
        "Length of the FP bar. 0.5 is half the default length.",
        "Longitud de la barra de PC. 0,5 es la mitad de la longitud original."},
    {"stamina_bar_size", "float", "0.05", "3", "0.05", nullptr,
        NT_BARS, "Tamaño de la barra de aguante",
        "Length of the stamina bar. 0.5 is half the default length.",
        "Longitud de la barra de aguante. 0,5 es la mitad de la longitud original."},
    {"hp_cap", "int", "0", "10000", "5", nullptr,
        NT_BARS, "Tope de vida",
        "Dynamic style: the HP bar starts growing above this maximum. 0 uses the game's own limit; 3565 is the original mod's value.",
        "Estilo dinámico: la barra de vida empieza a crecer por encima de este máximo. 0 usa el límite del juego; 3565 es el valor del mod original."},
    {"fp_cap", "int", "0", "10000", "5", nullptr,
        NT_BARS, "Tope de PC",
        "Dynamic style: the FP bar starts growing above this maximum. 0 uses the game's own limit; 825 is the original mod's value.",
        "Estilo dinámico: la barra de PC empieza a crecer por encima de este máximo. 0 usa el límite del juego; 825 es el valor del mod original."},
    {"stamina_cap", "int", "0", "10000", "5", nullptr,
        NT_BARS, "Tope de aguante",
        "Dynamic style: the stamina bar starts growing above this maximum. 0 uses the game's own limit; 480 is the original mod's value.",
        "Estilo dinámico: la barra de aguante empieza a crecer por encima de este máximo. 0 usa el límite del juego; 480 es el valor del mod original."},
};

constexpr BuiltinLabel kAdaptiveBarScalingLabelsEn[] = {
    {"Enabled", "Enabled"},
    {"bar_style", "Bar style"},
    {"hp_bar_size", "HP bar size"},
    {"fp_bar_size", "FP bar size"},
    {"stamina_bar_size", "Stamina bar size"},
    {"hp_cap", "HP cap"},
    {"fp_cap", "FP cap"},
    {"stamina_cap", "Stamina cap"},
};
