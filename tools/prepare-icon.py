"""Repack the supplied icon as a compact, standard multi-size Windows ICO."""
from pathlib import Path
from PIL import Image, ImageOps

path = Path(__file__).resolve().parents[1] / "assets" / "duck.ico"
image = Image.open(path).convert("RGBA")
canvas = Image.new("RGBA", (256, 256))
image = ImageOps.contain(image, (256, 256), Image.Resampling.LANCZOS)
canvas.alpha_composite(image, ((256 - image.width) // 2, (256 - image.height) // 2))
canvas=canvas.quantize(colors=256,method=Image.Quantize.FASTOCTREE).convert("RGBA")
canvas.save(path, format="ICO", sizes=[(n, n) for n in (16, 24, 32, 48, 64, 128, 256)],optimize=True)
print(f"{path.name}: {path.stat().st_size:,} bytes")
