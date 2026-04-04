#!/usr/bin/env python3

# You want consistency across densities?
# Then stop doing it manually.

import os
from PIL import Image

# Source directory (your "perfect" assets)
SRC_DIR = "res-xxxhdpi/images"

# Target densities and their sizes
DENSITIES = {
    "res-xxhdpi/images": {
        "fastbootd.png": (768, 480),
        "fastbootd_light.png": (768, 480),
        "ic_back_sel.png": (120, 120),
        "ic_back_sel_light.png": (120, 120),
        "logo_image.png": (768, 480),
        "logo_image_light.png": (768, 480),
        "logo_image_switch.png": (768, 480),
        "logo_image_switch_light.png": (768, 480),
    },
    "res-xhdpi/images": {
        "fastbootd.png": (512, 320),
        "fastbootd_light.png": (512, 320),
        "ic_back_sel.png": (80, 80),
        "ic_back_sel_light.png": (80, 80),
        "logo_image.png": (512, 320),
        "logo_image_light.png": (512, 320),
        "logo_image_switch.png": (512, 320),
        "logo_image_switch_light.png": (512, 320),
    },
    "res-hdpi/images": {
        "fastbootd.png": (384, 240),
        "fastbootd_light.png": (384, 240),
        "ic_back_sel.png": (52, 52),
        "ic_back_sel_light.png": (52, 52),
        "logo_image.png": (384, 240),
        "logo_image_light.png": (384, 240),
        "logo_image_switch.png": (384, 240),
        "logo_image_switch_light.png": (384, 240),
    },
    "res-mdpi/images": {
        "fastbootd.png": (256, 160),
        "fastbootd_light.png": (256, 160),
        "ic_back_sel.png": (34, 34),
        "ic_back_sel_light.png": (34, 34),
        "logo_image.png": (256, 160),
        "logo_image_light.png": (256, 160),
        "logo_image_switch.png": (256, 160),
        "logo_image_switch_light.png": (256, 160),
    }
}

def process():
    if not os.path.isdir(SRC_DIR):
        print("Source directory missing. Fix it.")
        return

    for density_dir, files in DENSITIES.items():
        os.makedirs(density_dir, exist_ok=True)

        for filename, size in files.items():
            src_path = os.path.join(SRC_DIR, filename)
            dst_path = os.path.join(density_dir, filename)

            if not os.path.isfile(src_path):
                print(f"[SKIP] Missing source: {src_path}")
                continue

            try:
                img = Image.open(src_path).convert("RGBA")

                # Resize properly. No jagged garbage.
                resized = img.resize(size, Image.LANCZOS)

                resized.save(dst_path, "PNG")
                print(f"[OK] {filename} -> {density_dir} {size}")

            except Exception as e:
                print(f"[FAIL] {filename}: {e}")

    print("\nDone. If something looks wrong, it's your source image. Not the script.")

if __name__ == "__main__":
    process()