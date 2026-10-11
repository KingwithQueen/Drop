"""Encode the supplied PNG artwork as multi-size Windows icon resources.

Run manually after changing the two PNGs; normal MSBuild uses the saved ICOs.
Requires Pillow. The source PNGs remain unchanged.
"""
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)

for source, destination in (
    (ROOT / "Dropico.png", ROOT / "drop/drop.ico"),
    (ROOT / "DropServerico.png", ROOT / "dropserver/dropserver.ico"),
):
    with Image.open(source) as original:
        artwork = original.convert("RGBA")
        edge = max(artwork.size)
        canvas = Image.new("RGBA", (edge, edge))
        canvas.paste(artwork, ((edge - artwork.width) // 2, (edge - artwork.height) // 2))
        canvas.save(destination, format="ICO", sizes=[(size, size) for size in SIZES])
    print(f"Encoded {destination.relative_to(ROOT)} from {source.name}")
