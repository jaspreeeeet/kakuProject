
import os
import re

sketch_path = r'f:\kakuProject-main\esp32_sketch\esp32_sketch.ino'
header_path = r'f:\kakuProject-main\esp32_sketch\all_pets.h'

animations = [
    "infant_frames", "child_frames", "adult_frames", "egg_crack_frames",
    "infant_cry_frames", "infant_surprise_frames", "infant_angry_frames",
    "infant_sad_frames", "old_frames", "old_happy", "old_sad", "old_cry",
    "old_surprise", "home_icon_frames", "food_icon_frames", "eating_frames",
    "poop_frames", "toilet_icon", "play_icon_frames", "heart_icon_frames",
    "aid_icon_frames", "injection_frames", "clean_slide_frames", "old_angry",
    "happy_child", "cry_child", "child_sad", "child_angry",
    "child_blinking_idle", "child_surprise", "happy_adult", "adult_sad",
    "angry_adult", "cry_adult", "surprise_adult", "infant_happy"
]

with open(sketch_path, 'r', encoding='utf-8', errors='ignore') as f:
    content = f.read()

unused = []
for anim in animations:
    if anim not in content:
        unused.append(anim)

print("UNUSED ANIMATIONS:")
for anim in unused:
    print(f"- {anim}")
