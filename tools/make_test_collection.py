"""Create a separate OBS scene collection containing the colour test chart with
the obs-colorwheel filter already attached.

Why a separate collection: OBS is running and rewrites its *current* collection
on exit, so editing that file would be clobbered.  A brand-new collection file
is never touched by the running instance, and the user can switch to it from
Scene Collection and back without restarting.

Written as UTF-8 without BOM, via Python, to avoid the PowerShell
Get-Content/Set-Content encoding trap that corrupted the obs-curves sources.
"""
import json
import os
import shutil
import uuid

SCENES = os.path.join(os.environ["APPDATA"], "obs-studio", "basic", "scenes")
CHART_SRC = r"C:\Users\qiusen\.dsh\dsh_wechat\obs-colorwheel\assets\colorwheel_testchart.png"
CHART_DST_DIR = r"C:\ProgramData\obs-studio\plugins\obs-colorwheel\data"
CHART_DST = os.path.join(CHART_DST_DIR, "colorwheel_testchart.png")
NAME = "校色测试"

os.makedirs(CHART_DST_DIR, exist_ok=True)
shutil.copyfile(CHART_SRC, CHART_DST)

PREV_VER = 537001986
src_uuid = str(uuid.uuid4())
flt_uuid = str(uuid.uuid4())
scene_uuid = str(uuid.uuid4())

filter_obj = {
    "prev_ver": PREV_VER,
    "name": "一级校色轮",
    "uuid": flt_uuid,
    "id": "obs_colorwheel_filter",
    "versioned_id": "obs_colorwheel_filter",
    "settings": {
        "pg_liftR": 0.0, "pg_liftG": 0.0, "pg_liftB": 0.0,
        "pg_gammaR": 0.0, "pg_gammaG": 0.0, "pg_gammaB": 0.0,
        "pg_gainR": 1.0, "pg_gainG": 1.0, "pg_gainB": 1.0,
        "pg_offR": 25.0, "pg_offG": 25.0, "pg_offB": 25.0,
        "pg_contrast": 1.0,
        "pg_pivot": 0.435,
        "pg_sat": 50.0,
        "pg_hue": 50.0,
        "pg_lummix": 100.0,
        "pg_boost": 0.0,
        "pg_shadows": 0.0,
        "pg_highlights": 0.0,
    },
    "mixers": 0, "sync": 0, "flags": 0, "volume": 1.0, "balance": 0.5,
    "enabled": True, "muted": False,
    "push-to-mute": False, "push-to-mute-delay": 0,
    "push-to-talk": False, "push-to-talk-delay": 0,
    "hotkeys": {}, "deinterlace_mode": 0, "deinterlace_field_order": 0,
    "monitoring_type": 0, "private_settings": {},
}

image_src = {
    "prev_ver": PREV_VER,
    "name": "校色测试图",
    "uuid": src_uuid,
    "id": "image_source",
    "versioned_id": "image_source",
    "settings": {
        "file": CHART_DST.replace("\\", "/"),
        "unload": False,
        "linear_alpha": False,
    },
    "mixers": 255, "sync": 0, "flags": 0, "volume": 1.0, "balance": 0.5,
    "enabled": True, "muted": False,
    "push-to-mute": False, "push-to-mute-delay": 0,
    "push-to-talk": False, "push-to-talk-delay": 0,
    "hotkeys": {
        "libobs.mute": [], "libobs.unmute": [],
        "libobs.push-to-mute": [], "libobs.push-to-talk": [],
    },
    "deinterlace_mode": 0, "deinterlace_field_order": 0,
    "monitoring_type": 0, "private_settings": {},
    "filters": [filter_obj],
}

scene_obj = {
    "prev_ver": PREV_VER,
    "name": "测试图",
    "uuid": scene_uuid,
    "id": "scene",
    "versioned_id": "scene",
    "settings": {
        "id_counter": 1,
        "custom_size": False,
        "items": [{
            "name": "校色测试图",
            "source_uuid": src_uuid,
            "visible": True,
            "locked": False,
            "rot": 0.0,
            "scale_ref": {"x": 1920.0, "y": 1080.0},
            "align": 5,
            "bounds_type": 0,
            "bounds_align": 0,
            "bounds_crop": False,
            "crop_left": 0, "crop_top": 0, "crop_right": 0, "crop_bottom": 0,
            "id": 1,
            "group_item_backup": False,
            "pos": {"x": 0.0, "y": 0.0},
            "pos_rel": {"x": 0.0, "y": 0.0},
            "scale": {"x": 1.0, "y": 1.0},
            "scale_rel": {"x": 1.0, "y": 1.0},
            "bounds": {"x": 0.0, "y": 0.0},
            "bounds_rel": {"x": 0.0, "y": 0.0},
            "scale_filter": "disable",
            "blend_method": "default",
            "blend_type": "normal",
            "show_transition": {"duration": 300},
            "hide_transition": {"duration": 300},
            "private_settings": {},
        }],
    },
    "mixers": 0, "sync": 0, "flags": 0, "volume": 1.0, "balance": 0.5,
    "enabled": True, "muted": False,
    "push-to-mute": False, "push-to-mute-delay": 0,
    "push-to-talk": False, "push-to-talk-delay": 0,
    "hotkeys": {"OBSBasic.SelectScene": [], "libobs.show_scene_item.1": [],
                "libobs.hide_scene_item.1": []},
    "deinterlace_mode": 0, "deinterlace_field_order": 0,
    "monitoring_type": 0,
    "private_settings": {},
}

collection = {
    "name": NAME,
    "sources": [image_src, scene_obj],
    "groups": [],
    "scene_order": [{"name": "测试图"}],
    "current_scene": "测试图",
    "current_program_scene": "测试图",
    "current_transition": "淡入淡出",
    "transition_duration": 300,
    "transitions": [],
    "quick_transitions": [],
    "saved_projectors": [],
    "preview_locked": False,
    "scaling_enabled": False,
    "scaling_level": -20,
    "scaling_off_x": 0.0,
    "scaling_off_y": 0.0,
    "virtual-camera": {"type2": 3},
    "modules": {},
    "resolution": {"x": 1920, "y": 1080},
    "version": 2,
}

out = os.path.join(SCENES, NAME + ".json")
with open(out, "w", encoding="utf-8", newline="\n") as fh:
    json.dump(collection, fh, ensure_ascii=False, indent=4)

# verify it round-trips
with open(out, "r", encoding="utf-8") as fh:
    back = json.load(fh)
print("written:", out, os.path.getsize(out), "bytes")
print("verify :", back["name"], "| scenes:", [s["name"] for s in back["sources"] if s["id"] == "scene"])
print("chart  :", CHART_DST, os.path.getsize(CHART_DST), "bytes")
