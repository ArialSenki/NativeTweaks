from __future__ import annotations

from math import hypot
from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "package" / "menu" / "win"
PREVIEW = ROOT / "ui"
SCALE = 4
SIZE = 80


def point(x: float, y: float) -> tuple[int, int]:
    return round(x * SCALE), round(y * SCALE)


def make_icon(selected: bool) -> Image.Image:
    image = Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE), (0, 0, 0, 0))

    if selected:
        pixels = image.load()
        cx = cy = 40 * SCALE
        radius = 38 * SCALE
        for y in range(image.height):
            for x in range(image.width):
                distance = hypot(x - cx, y - cy) / radius
                if distance < 1:
                    strength = (1 - distance) ** 2
                    alpha = round(58 * strength)
                    pixels[x, y] = (201, 183, 131, alpha)

    draw = ImageDraw.Draw(image)
    if selected:
        draw.ellipse(
            [point(7, 7), point(73, 73)],
            fill=(196, 174, 120, 34),
            outline=(187, 164, 109, 225),
            width=round(1.8 * SCALE),
        )
        outline = (210, 193, 142, 255)
        line_width = round(2.4 * SCALE)
        hexagon = [(40, 19), (58, 29), (58, 50), (40, 61), (22, 50), (22, 29)]
        rules = [(31, 34, 49, 34), (31, 40, 49, 40), (31, 46, 43, 46)]
    else:
        outline = (148, 139, 120, 200)
        line_width = round(2.4 * SCALE)
        hexagon = [(40, 10), (62, 22.5), (62, 47.5), (40, 60), (18, 47.5), (18, 22.5)]
        rules = [(31, 29, 49, 29), (31, 36, 49, 36), (31, 43, 43, 43)]

    draw.line([point(*p) for p in (*hexagon, hexagon[0])], fill=outline, width=line_width, joint="curve")
    for x1, y1, x2, y2 in rules:
        draw.line([point(x1, y1), point(x2, y2)], fill=outline, width=line_width)

    return image.resize((SIZE, SIZE), Image.Resampling.LANCZOS)


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    PREVIEW.mkdir(parents=True, exist_ok=True)
    for selected, stem in ((False, "ERModSettingsTab"), (True, "ERModSettingsTabSelected")):
        icon = make_icon(selected)
        icon.save(OUTPUT / f"{stem}.tga", format="TGA")
        icon.save(PREVIEW / f"{stem}.png", format="PNG")
    print(f"Generated native tab icons in {OUTPUT}")


if __name__ == "__main__":
    main()
