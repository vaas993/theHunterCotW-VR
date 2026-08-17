#include "config.h"

#include "gamesettings.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern HMODULE g_selfModule;

namespace cotwvr {
namespace {

std::wstring SelfDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_selfModule, path, MAX_PATH);
    std::wstring s(path);
    const size_t slash = s.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1);
}

void Assign(Config& c, const char* key, const char* value) {
    // Base 0, so "0x1A" and "26" both work. atoi() stopped at the 'x' and
    // returned 0, which silently zeroed the three shader CRCs the moment Save()
    // started writing them in hex - and a weapon CRC of zero matches nothing, so
    // the whole feature went dead while looking configured. Values have to
    // survive the round trip, not just the key names.
    auto num  = [&] { return (int)strtoul(value, nullptr, 0); };
    auto flag = [&] { return atoi(value) != 0; };

    // Roll, parsed here rather than in the else-if chain below: that chain is
    // already deep enough that VS2019 hits its parser nesting limit (C1061) when
    // more branches are added to it. Early returns cost nothing.
    if (!_stricmp(key, "menu_screen")) { c.menu_screen = flag(); return; }
    if (!_stricmp(key, "menu_screen_auto")) { c.menu_screen_auto = flag(); return; }
    if (!_stricmp(key, "menu_screen_key")) {
        c.menu_screen_key = (int)strtol(value, nullptr, 0);
        return;
    }
    if (!_stricmp(key, "menu_screen_width_m")) {
        c.menu_screen_width_m = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "menu_screen_distance_m")) {
        c.menu_screen_distance_m = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "menu_cursor_hold_ms")) {
        c.menu_cursor_hold_ms = num();
        return;
    }
    if (!_stricmp(key, "menu_screen_hide_world")) {
        c.menu_screen_hide_world = flag();
        return;
    }
    if (!_stricmp(key, "menu_screen_log_signals")) {
        c.menu_screen_log_signals = flag();
        return;
    }
    if (!_stricmp(key, "menu_screen_use_clip")) {
        c.menu_screen_use_clip = flag();
        return;
    }
    // Recentring. Standalone if-blocks, like everything else up here: the
    // else-if chain below is at MSVC's C1061 nesting limit.
    // strtol(value, nullptr, 0) for the hex-written values, NOT atoi - atoi
    // stops at the 'x' and silently zeroes the key.
    if (!_stricmp(key, "recentre_key")) { c.recentre_key = (int)strtol(value, nullptr, 0); return; }
    if (!_stricmp(key, "recentre_pad_enable")) { c.recentre_pad_enable = flag(); return; }
    if (!_stricmp(key, "recentre_pad_chord")) { c.recentre_pad_chord = (int)strtol(value, nullptr, 0); return; }
    if (!_stricmp(key, "recentre_pad_hold_ms")) { c.recentre_pad_hold_ms = num(); return; }
    if (!_stricmp(key, "recentre_pad_swallow")) { c.recentre_pad_swallow = flag(); return; }
    if (!_stricmp(key, "recentre_compensate")) { c.recentre_compensate = flag(); return; }
    if (!_stricmp(key, "recentre_compensate_invert")) { c.recentre_compensate_invert = flag(); return; }
    if (!_stricmp(key, "recentre_settle_ms")) {
        c.recentre_settle_ms = num();
        return;
    }
    if (!_stricmp(key, "recentre_settle_deg_s")) {
        c.recentre_settle_deg_s = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "recentre_settle_max_ms")) {
        c.recentre_settle_max_ms = num();
        return;
    }
    if (!_stricmp(key, "recentre_ramp_frames")) {
        c.recentre_ramp_frames = num();
        return;
    }
    if (!_stricmp(key, "recentre_reset_smoother")) { c.recentre_reset_smoother = flag(); return; }
    if (!_stricmp(key, "recentre_pitch_limit_deg")) { c.recentre_pitch_limit_deg = num(); return; }
    if (!_stricmp(key, "recentre_position")) { c.recentre_position = flag(); return; }
    if (!_stricmp(key, "submit_rendered_pose")) {
        c.submit_rendered_pose = flag();
        return;
    }
    if (!_stricmp(key, "aer_per_eye_pose")) {
        c.aer_per_eye_pose = flag();
        return;
    }
    if (!_stricmp(key, "head_view_basis")) { c.head_view_basis = flag(); return; }
    if (!_stricmp(key, "head_keep_aim_on_reseed")) {
        c.head_keep_aim_on_reseed = flag();
        return;
    }
    if (!_stricmp(key, "profile_slot")) {
        c.profile_slot = (int)strtol(value, nullptr, 0);
        return;
    }
    if (!_stricmp(key, "panel_on_monitor")) {
        c.panel_on_monitor = flag();
        return;
    }
    if (!_stricmp(key, "head_reseed_holdbreath")) {
        c.head_reseed_holdbreath = flag();
        return;
    }
    if (!_stricmp(key, "head_reseed_detour")) {
        c.head_reseed_detour = flag();
        return;
    }
    if (!_stricmp(key, "head_reseed_correct_scale")) {
        c.head_reseed_correct_scale = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "head_block_reseed_yaw")) {
        c.head_block_reseed_yaw = flag();
        return;
    }
    if (!_stricmp(key, "head_bake_undo")) { c.head_bake_undo = flag(); return; }
    if (!_stricmp(key, "head_write_roll")) { c.head_write_roll = flag(); return; }
    if (!_stricmp(key, "head_roll_field")) {
        c.head_roll_field = (int)strtol(value, nullptr, 0);
        return;
    }
    if (!_stricmp(key, "head_roll_invert")) { c.head_roll_invert = flag(); return; }
    if (!_stricmp(key, "head_roll_scale")) {
        c.head_roll_scale = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "weapon_match_index")) {
        c.weapon_match_index = flag();
        return;
    }
    if (!_stricmp(key, "weapon_3d_skip_scene_picture")) {
        c.weapon_3d_skip_scene_picture = flag();
        return;
    }
    if (!_stricmp(key, "tier1_per_eye_crop")) {
        c.tier1_per_eye_crop = flag();
        return;
    }
    if (!_stricmp(key, "tier1_fov")) {
        c.tier1_fov = flag();
        return;
    }
    if (!_stricmp(key, "tier1_fov_k_override")) {
        c.tier1_fov_k_override = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "tier1_scale_viewmodel_fov")) {
        c.tier1_scale_viewmodel_fov = flag();
        return;
    }
    if (!_stricmp(key, "taa_prev_from_last_render")) {
        c.taa_prev_from_last_render = flag();
        return;
    }
    // The TAA resolve probe. Standalone if-blocks for the same reason the HUD
    // probe's are: the else-if chain further down is at MSVC's C1061 limit.
    if (!_stricmp(key, "taa_probe")) { c.taa_probe = flag(); return; }
    if (!_stricmp(key, "taa_probe_seconds")) {
        c.taa_probe_seconds = (int)strtol(value, nullptr, 0);
        return;
    }
    if (!_stricmp(key, "per_eye_temporal_history")) {
        c.per_eye_temporal_history = flag();
        return;
    }
    if (!_stricmp(key, "per_eye_temporal_eye_swap")) {
        c.per_eye_temporal_eye_swap = flag();
        return;
    }
    if (!_stricmp(key, "per_eye_temporal_starve")) {
        c.per_eye_temporal_starve = flag();
        return;
    }
    if (!_stricmp(key, "per_eye_temporal_matrix")) {
        c.per_eye_temporal_matrix = flag();
        return;
    }
    if (!_stricmp(key, "taa_replace_pass")) {
        c.taa_replace_pass = flag();
        return;
    }
    if (!_stricmp(key, "taa_resolve")) { c.taa_resolve = flag(); return; }
    if (!_stricmp(key, "taa_blend")) {
        c.taa_blend = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "taa_use_motion_vectors")) {
        c.taa_use_motion_vectors = flag();
        return;
    }
    if (!_stricmp(key, "taa_mv_invert")) { c.taa_mv_invert = flag(); return; }
    if (!_stricmp(key, "taa_own_motion_vectors")) {
        c.taa_own_motion_vectors = flag();
        return;
    }
    if (!_stricmp(key, "taa_exclude_viewmodel")) {
        c.taa_exclude_viewmodel = flag();
        return;
    }
    if (!_stricmp(key, "taa_sharp_history")) {
        c.taa_sharp_history = flag();
        return;
    }
    if (!_stricmp(key, "taa_camera_relative")) {
        c.taa_camera_relative = flag();
        return;
    }
    if (!_stricmp(key, "taa_clamp")) { c.taa_clamp = flag(); return; }
    if (!_stricmp(key, "taa_clamp_strength")) {
        c.taa_clamp_strength = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "taa_sharpen")) {
        c.taa_sharpen = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "taa_ghosting_fix")) {
        c.taa_ghosting_fix = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "dlss_probe")) { c.dlss_probe = flag(); return; }
    if (!_stricmp(key, "dlss_auto_exposure")) {
        c.dlss_auto_exposure = flag();
        return;
    }
    if (!_stricmp(key, "taa_write_motion_vectors")) {
        c.taa_write_motion_vectors = flag();
        return;
    }
    if (!_stricmp(key, "taa_head_rotation_fix")) {
        c.taa_head_rotation_fix = flag();
        return;
    }
    if (!_stricmp(key, "taa_head_rotation_scale")) {
        c.taa_head_rotation_scale = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "taa_head_rotation_scale_y")) {
        c.taa_head_rotation_scale_y = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "taa_head_rotation_delay")) {
        c.taa_head_rotation_delay = flag();
        return;
    }
    if (!_stricmp(key, "taa_mv_probe")) {
        int v = atoi(value);
        c.taa_mv_probe = (v < 0) ? 0 : (v > 2 ? 2 : v);
        return;
    }
    if (!_stricmp(key, "taa_jitter_inject")) {
        c.taa_jitter_inject = flag();
        return;
    }
    if (!_stricmp(key, "taa_jitter_inject_scale")) {
        c.taa_jitter_inject_scale = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "jitter_hunt")) {
        int v = atoi(value);
        c.jitter_hunt = (v < 0) ? 0 : (v > 2 ? 2 : v);
        return;
    }
    if (!_stricmp(key, "jitter_hunt_offset")) {
        long v = strtol(value, nullptr, 0);       // accepts 0x-prefixed hex
        if (v >= 0 && v <= 0x10000) c.jitter_hunt_offset = (int)v;
        return;
    }
    if (!_stricmp(key, "jitter_take")) { c.jitter_take = flag(); return; }
    if (!_stricmp(key, "jitter_take_scale")) {
        c.jitter_take_scale = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "jitter_mode3")) { c.jitter_mode3 = flag(); return; }
    if (!_stricmp(key, "taa_dejitter")) { c.taa_dejitter = flag(); return; }
    if (!_stricmp(key, "submit_frozen_position")) {
        c.submit_frozen_position = flag();
        return;
    }
    if (!_stricmp(key, "submit_frozen_orientation")) {
        c.submit_frozen_orientation = flag();
        return;
    }
    if (!_stricmp(key, "tier1_crop_latch")) {
        c.tier1_crop_latch = flag();
        return;
    }
    if (!_stricmp(key, "head_yaw_probe")) {
        c.head_yaw_probe = flag();
        return;
    }
    if (!_stricmp(key, "head_tremor_level")) {
        int v = atoi(value);
        c.head_tremor_level = (v < 0) ? 0 : (v > 3 ? 3 : v);
        return;
    }
    if (!_stricmp(key, "head_tremor_release_deg")) {
        c.head_tremor_release_deg = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "head_tremor_tau_ms")) {
        c.head_tremor_tau_ms = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "head_tremor_leash_deg")) {
        c.head_tremor_leash_deg = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "prediction_damp_pct")) {
        int v = atoi(value);
        c.prediction_damp_pct = (v < 0) ? 0 : (v > 100 ? 100 : v);
        return;
    }
    if (!_stricmp(key, "head_latch_per_frame")) {
        c.head_latch_per_frame = flag();
        return;
    }
    if (!_stricmp(key, "taa_double_reproj")) {
        c.taa_double_reproj = flag();
        return;
    }
    if (!_stricmp(key, "taa_dilate_depth")) {
        c.taa_dilate_depth = flag();
        return;
    }
    if (!_stricmp(key, "taa_mv_deadzone_px")) {
        c.taa_mv_deadzone_px = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "taa_camera_lock")) { c.taa_camera_lock = flag(); return; }
    if (!_stricmp(key, "dlss_jitter_exact")) {
        c.dlss_jitter_exact = flag();
        return;
    }
    if (!_stricmp(key, "dlss_enable")) { c.dlss_enable = flag(); return; }
    if (!_stricmp(key, "dlss_jitter_mode")) {
        int v = atoi(value);
        c.dlss_jitter_mode = (v < 0) ? 0 : (v > 2 ? 2 : v);
        return;
    }
    if (!_stricmp(key, "dlss_jitter_scale")) {
        c.dlss_jitter_scale = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "dlss_mv_jittered")) {
        c.dlss_mv_jittered = flag();
        return;
    }
    if (!_stricmp(key, "dlss_depth_inverted")) {
        c.dlss_depth_inverted = flag();
        return;
    }
    if (!_stricmp(key, "dlss_preset")) {
        int v = atoi(value);
        c.dlss_preset = (v < 0) ? 0 : (v > 4 ? 4 : v);
        return;
    }
    if (!_stricmp(key, "apex_ignore_fingerprint")) {
        c.apex_ignore_fingerprint = flag();
        return;
    }
    if (!_stricmp(key, "logging")) {
        c.logging = flag();
        return;
    }
    if (!_stricmp(key, "desktop_present_unlocked")) {
        c.desktop_present_unlocked = flag();
        return;
    }
    if (!_stricmp(key, "welcome_seen")) {
        c.welcome_seen = flag();
        return;
    }
    if (!_stricmp(key, "post_sharpen")) {
        c.post_sharpen = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "post_saturation")) {
        c.post_saturation = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "dlss_upscale_rect_latch")) {
        c.dlss_upscale_rect_latch = flag();
        return;
    }
    if (!_stricmp(key, "taa_single_accumulation")) {
        c.taa_single_accumulation = flag();
        return;
    }
    if (!_stricmp(key, "dlss_upscale_solo")) {
        c.dlss_upscale_solo = flag();
        return;
    }
    if (!_stricmp(key, "dlss_upscale_pct")) {
        int v = atoi(value);
        c.dlss_upscale_pct = (v < 33 || v > 99) ? 100 : v;
        return;
    }
    // *** THESE ARE 'if ... return', NOT MORE 'else if'. ***
    //
    // MSVC caps how deeply else-if chains may nest (C1061), and the chain below
    // is already at that ceiling - adding six more keys to it stopped the build
    // outright. Every new key goes in a returning block like this one instead.
    if (!_stricmp(key, "six_dof"))          { c.six_dof = flag(); return; }
    if (!_stricmp(key, "six_dof_scale"))    { c.six_dof_scale = (float)atof(value); return; }
    if (!_stricmp(key, "six_dof_limit_m"))  { c.six_dof_limit_m = (float)atof(value); return; }
    if (!_stricmp(key, "six_dof_invert_x")) { c.six_dof_invert_x = flag(); return; }
    if (!_stricmp(key, "six_dof_invert_y")) { c.six_dof_invert_y = flag(); return; }
    if (!_stricmp(key, "six_dof_invert_z")) { c.six_dof_invert_z = flag(); return; }
    if (!_stricmp(key, "six_dof_yaw_offset_deg")) {
        int v = atoi(value);
        // Fold anything into -180..270 in quarter turns, so a hand-edited 450
        // becomes 90 rather than something that rotates the world.
        while (v < -180) v += 360;
        while (v > 270) v -= 360;
        c.six_dof_yaw_offset_deg = (v / 90) * 90;
        return;
    }
    if (!_stricmp(key, "six_dof_log")) { c.six_dof_log = flag(); return; }
    if (!_stricmp(key, "full_rate_hook_always")) {
        c.full_rate_hook_always = flag();
        return;
    }
    if (!_stricmp(key, "six_dof_anchor_on_enter")) {
        c.six_dof_anchor_on_enter = flag();
        return;
    }
    if (!_stricmp(key, "six_dof_frame_mode")) {
        int v = atoi(value);
        c.six_dof_frame_mode = (v < 0) ? 0 : (v > 3 ? 3 : v);
        return;
    }
    if (!_stricmp(key, "six_dof_body_frame")) { c.six_dof_body_frame = flag(); return; }
    if (!_stricmp(key, "six_dof_body_frame_invert")) { c.six_dof_body_frame_invert = flag(); return; }
    if (!_stricmp(key, "dlss_quality")) {
        int v = atoi(value);
        c.dlss_quality = (v < 0) ? 0 : (v > 5 ? 5 : v);
        return;
    }
    if (!_stricmp(key, "dlss_jitter_x")) {
        c.dlss_jitter_x = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "dlss_jitter_y")) {
        c.dlss_jitter_y = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "dlss_mv_object_blend")) {
        c.dlss_mv_object_blend = (float)atof(value);
        return;
    }
    // The HUD component probe. Standalone if-blocks, like everything else here:
    // the else-if chain further down is already at MSVC's C1061 nesting limit.
    if (!_stricmp(key, "hud_probe")) { c.hud_probe = flag(); return; }
    if (!_stricmp(key, "scope_mono_ads")) {
        c.scope_mono_ads = flag();
        return;
    }
    if (!_stricmp(key, "scope_mono")) {
        c.scope_mono = flag();
        return;
    }
    if (!_stricmp(key, "scope_ipd_mm")) {
        c.scope_ipd_mm = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "hud_hide_sunflare")) {
        c.hud_hide_sunflare = flag();
        return;
    }
    if (!_stricmp(key, "hud_hide_glare")) {
        c.hud_hide_glare = flag();
        return;
    }
    if (!_stricmp(key, "hud_move_x")) {
        c.hud_move_x = num();
        return;
    }
    if (!_stricmp(key, "hud_move_y")) {
        c.hud_move_y = num();
        return;
    }
    if (!_stricmp(key, "hud_scale")) {
        c.hud_scale = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "hud_isolate_post")) {
        c.hud_isolate_post = flag();
        return;
    }
    if (!_stricmp(key, "hud_isolate")) {
        c.hud_isolate = num();
        return;
    }
    if (!_stricmp(key, "hud_probe_frames")) {
        c.hud_probe_frames = (int)strtol(value, nullptr, 0);
        return;
    }
    if (!_stricmp(key, "tier1_measure_fov")) {
        c.tier1_measure_fov = flag();
        return;
    }
    if (!_stricmp(key, "weapon_3d_mask_follows_glass")) {
        c.weapon_3d_mask_follows_glass = flag();
        return;
    }
    if (!_stricmp(key, "weapon_3d_optic_signature")) {
        c.weapon_3d_optic_signature = flag();
        return;
    }
    if (!_stricmp(key, "weapon_stencil_only")) {
        c.weapon_stencil_only = flag();
        return;
    }
    if (!_stricmp(key, "weapon_match_stencil")) {
        c.weapon_match_stencil = flag();
        return;
    }
    if (!_stricmp(key, "hook_device_creation")) {
        c.hook_device_creation = flag();
        return;
    }
    if (!_stricmp(key, "hook_context_vtable")) {
        c.hook_context_vtable = flag();
        return;
    }
    if (!_stricmp(key, "rebuild_contexts_on_start")) {
        c.rebuild_contexts_on_start = flag();
        return;
    }
    if (!_stricmp(key, "hook_contexts_early")) {
        c.hook_contexts_early = flag();
        return;
    }
    if (!_stricmp(key, "weapon_3d")) { c.weapon_3d = flag(); return; }
    if (!_stricmp(key, "weapon_3d_amount")) {
        c.weapon_3d_amount = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "weapon_3d_slot")) {
        c.weapon_3d_slot = (int)strtol(value, nullptr, 0);
        return;
    }
    if (!_stricmp(key, "weapon_3d_layout")) {
        c.weapon_3d_layout = (int)strtol(value, nullptr, 0);
        return;
    }
    if (!_stricmp(key, "weapon_3d_wvp_offset")) {
        c.weapon_3d_wvp_offset = (int)strtol(value, nullptr, 0);
        return;
    }
    if (!_stricmp(key, "weapon_3d_glass_mask_mesh")) {
        c.weapon_3d_glass_mask_mesh = flag();
        return;
    }
    if (!_stricmp(key, "weapon_3d_glass_via_rasterizer")) {
        c.weapon_3d_glass_via_rasterizer = flag();
        return;
    }
    if (!_stricmp(key, "weapon_3d_glass_via_constants")) {
        c.weapon_3d_glass_via_constants = flag();
        return;
    }
    if (!_stricmp(key, "weapon_3d_glass_follow_body_eye")) {
        c.weapon_3d_glass_follow_body_eye = flag();
        return;
    }
    if (!_stricmp(key, "weapon_view_scale")) {
        c.weapon_view_scale = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "weapon_view_offset_x")) {
        c.weapon_view_offset_x = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "weapon_view_offset_y")) {
        c.weapon_view_offset_y = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "weapon_3d_scoped_separate")) {
        c.weapon_3d_scoped_separate = flag();
        return;
    }
    if (!_stricmp(key, "weapon_3d_amount_scoped")) {
        c.weapon_3d_amount_scoped = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "weapon_match_stencil_write")) {
        c.weapon_match_stencil_write = flag();
        return;
    }
    if (!_stricmp(key, "weapon_all_draw_types")) {
        c.weapon_all_draw_types = flag();
        return;
    }
    // The game's own graphics options, matched here as standalone tests rather
    // than as more links on the else-if chain further down. That chain is at
    // MSVC's nesting limit - adding eight more to it is a hard C1061, not a
    // warning - so anything new goes in this form from now on.
    if (!_stricmp(key, "apply_game_graphics")) {
        c.apply_game_graphics = flag();
        return;
    }
    if (!_stricmp(key, "game_fov_follows_auto")) {
        c.game_fov_follows_auto = flag();
        return;
    }
    if (!_stricmp(key, "frame_latency")) {
        c.frame_latency = num();
        return;
    }
    if (!_stricmp(key, "aim_steady")) {
        c.aim_steady = flag();
        return;
    }
    if (!_stricmp(key, "aim_steady_ms")) {
        c.aim_steady_ms = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "aim_steady_scoped_only")) {
        c.aim_steady_scoped_only = flag();
        return;
    }
    if (!_stricmp(key, "lock_pitch_input")) {
        c.lock_pitch_input = flag();
        return;
    }
    if (!_stricmp(key, "lock_pitch_pad")) {
        c.lock_pitch_pad = flag();
        return;
    }
    if (!_stricmp(key, "lock_pitch_mouse")) {
        c.lock_pitch_mouse = flag();
        return;
    }
    if (!_stricmp(key, "graphics_motion_blur")) {
        c.graphics_motion_blur = num();
        return;
    }
    if (!_stricmp(key, "graphics_depth_of_field")) {
        c.graphics_depth_of_field = num();
        return;
    }
    if (!_stricmp(key, "graphics_vignette")) {
        c.graphics_vignette = num();
        return;
    }
    if (!_stricmp(key, "graphics_ssao")) {
        c.graphics_ssao = num();
        return;
    }
    if (!_stricmp(key, "graphics_ssr")) {
        c.graphics_ssr = num();
        return;
    }
    if (!_stricmp(key, "graphics_contact_shadows")) {
        c.graphics_contact_shadows = num();
        return;
    }
    if (!_stricmp(key, "graphics_aa")) {
        c.graphics_aa = num();
        return;
    }
    if (!_stricmp(key, "weapon_3d_glass_whole_pass")) {
        c.weapon_3d_glass_whole_pass = flag();
        return;
    }
    if (!_stricmp(key, "weapon_3d_glass_blended_only")) {
        c.weapon_3d_glass_blended_only = flag();
        return;
    }
    if (!_stricmp(key, "weapon_3d_glass_scale")) {
        c.weapon_3d_glass_scale = (float)atof(value);
        return;
    }
    if (!_stricmp(key, "weapon_3d_glass")) {
        c.weapon_3d_glass = (int)strtol(value, nullptr, 0);
        return;
    }
    if (!_stricmp(key, "weapon_pass_gap")) {
        c.weapon_pass_gap = (int)strtol(value, nullptr, 0);
        return;
    }
    if (!_stricmp(key, "weapon_max_since_clear")) {
        c.weapon_max_since_clear = (int)strtol(value, nullptr, 0);
        return;
    }
    if (!_stricmp(key, "weapon_min_since_clear")) {
        c.weapon_min_since_clear = (int)strtol(value, nullptr, 0);
        return;
    }
    if (!_stricmp(key, "weapon_pass_tail_pct")) {
        c.weapon_pass_tail_pct = (int)strtol(value, nullptr, 0);
        return;
    }

    if      (!_stricmp(key, "version"))           c.version           = num();
    else if (!_stricmp(key, "enabled"))           c.enabled           = flag();
    else if (!_stricmp(key, "submit_to_headset")) c.submit_to_headset = flag();
    else if (!_stricmp(key, "blank_layer_test"))  c.blank_layer_test  = flag();
    else if (!_stricmp(key, "log_frame_every"))   c.log_frame_every   = num();
    else if (!_stricmp(key, "camera_probe"))      c.camera_probe       = flag();
    else if (!_stricmp(key, "camera_probe_every"))c.camera_probe_every = num();
    else if (!_stricmp(key, "camera_test_axis"))  c.camera_test_axis   = num();
    else if (!_stricmp(key, "camera_test_amount"))c.camera_test_amount = (float)atof(value);
    else if (!_stricmp(key, "render_preset"))     c.render_preset      = num();
    else if (!_stricmp(key, "render_custom_w"))   c.render_custom_w    = num();
    else if (!_stricmp(key, "render_custom_h"))   c.render_custom_h    = num();
    else if (!_stricmp(key, "force_game_fov"))    c.force_game_fov     = num();
    else if (!_stricmp(key, "headset_vfov_deg"))  c.headset_vfov_deg   = (float)atof(value);
    else if (!_stricmp(key, "symmetric_fov"))     c.symmetric_fov      = flag();
    else if (!_stricmp(key, "game_fov_deg"))      c.game_fov_deg       = (float)atof(value);
    else if (!_stricmp(key, "fov_is_horizontal")) c.fov_is_horizontal  = flag();
    else if (!_stricmp(key, "stereo"))            c.stereo             = flag();
    else if (!_stricmp(key, "ipd_mm"))            c.ipd_mm             = (float)atof(value);
    else if (!_stricmp(key, "stereo_basis_from_lever")) c.stereo_basis_from_lever = flag();
    else if (!_stricmp(key, "eye_axis_row"))      c.eye_axis_row       = num();
    else if (!_stricmp(key, "eye_swap"))          c.eye_swap           = flag();
    else if (!_stricmp(key, "stereo_log_every"))  c.stereo_log_every   = num();
    else if (!_stricmp(key, "aer_reuse_swapchain")) c.aer_reuse_swapchain = flag();
    else if (!_stricmp(key, "perf_log_every"))    c.perf_log_every     = num();
    else if (!_stricmp(key, "full_rate_stereo"))  c.full_rate_stereo   = flag();
    else if (!_stricmp(key, "full_rate_warmup_frames")) c.full_rate_warmup_frames = num();
    else if (!_stricmp(key, "full_rate_freeze_clock")) c.full_rate_freeze_clock = flag();
    else if (!_stricmp(key, "full_rate_zero_dt")) c.full_rate_zero_dt = flag();
    else if (!_stricmp(key, "full_rate_freeze_time")) c.full_rate_freeze_time = flag();
    else if (!_stricmp(key, "full_rate_correct_delta")) c.full_rate_correct_delta = flag();
    else if (!_stricmp(key, "full_rate_fix_scaled_delta"))
        c.full_rate_fix_scaled_delta = flag();
    else if (!_stricmp(key, "full_rate_skip_clock_update"))
        c.full_rate_skip_clock_update = flag();
    else if (!_stricmp(key, "full_rate_freeze_qpc")) c.full_rate_freeze_qpc = flag();
    else if (!_stricmp(key, "full_rate_patch_delta_getter"))
        c.full_rate_patch_delta_getter = flag();
    else if (!_stricmp(key, "full_rate_hook_second_getter"))
        c.full_rate_hook_second_getter = flag();
    else if (!_stricmp(key, "full_rate_hook_timer_tick"))
        c.full_rate_hook_timer_tick = flag();
    else if (!_stricmp(key, "full_rate_fragment_threshold"))
        c.full_rate_fragment_threshold = (float)atof(value);
    else if (!_stricmp(key, "full_rate_patch_flag"))
        c.full_rate_patch_flag = num();
    else if (!_stricmp(key, "full_rate_patch_callsite"))
        c.full_rate_patch_callsite = (int)strtol(value, nullptr, 0);
    else if (!_stricmp(key, "full_rate_log_callsites"))
        c.full_rate_log_callsites = flag();
    else if (!_stricmp(key, "full_rate_zero_desc_dt"))
        c.full_rate_zero_desc_dt = flag();
    else if (!_stricmp(key, "full_rate_skip_second_present"))
        c.full_rate_skip_second_present = flag();
    else if (!_stricmp(key, "head_tracking"))     c.head_tracking      = flag();
    else if (!_stricmp(key, "head_invert"))       c.head_invert        = flag();
    else if (!_stricmp(key, "camera_object_probe")) c.camera_object_probe = flag();
    else if (!_stricmp(key, "head_test_axis"))    c.head_test_axis     = num();
    else if (!_stricmp(key, "head_test_degrees")) c.head_test_degrees  = (float)atof(value);
    else if (!_stricmp(key, "head_test_sweep"))   c.head_test_sweep    = flag();
    else if (!_stricmp(key, "head_write_yaw"))    c.head_write_yaw     = flag();
    else if (!_stricmp(key, "head_yaw_field"))    c.head_yaw_field     = (int)strtol(value, nullptr, 0);
    else if (!_stricmp(key, "head_write_pitch"))  c.head_write_pitch   = flag();
    else if (!_stricmp(key, "head_pitch_field"))  c.head_pitch_field   = (int)strtol(value, nullptr, 0);
    else if (!_stricmp(key, "head_pitch_invert")) c.head_pitch_invert  = flag();
    else if (!_stricmp(key, "weapon_site"))       c.weapon_site        = (int)strtol(value, nullptr, 0);
    else if (!_stricmp(key, "weapon_test_sweep")) c.weapon_test_sweep  = flag();
    else if (!_stricmp(key, "weapon_test_amount"))c.weapon_test_amount = (float)atof(value);
    else if (!_stricmp(key, "weapon_scan"))       c.weapon_scan        = flag();
    else if (!_stricmp(key, "weapon_scan_seconds")) c.weapon_scan_seconds = num();
    else if (!_stricmp(key, "weapon_stereo"))     c.weapon_stereo      = flag();
    else if (!_stricmp(key, "weapon_depth"))      c.weapon_depth       = (float)atof(value);
    else if (!_stricmp(key, "weapon_cb_scan"))    c.weapon_cb_scan     = flag();
    else if (!_stricmp(key, "weapon_cb_stereo"))  c.weapon_cb_stereo   = flag();
    else if (!_stricmp(key, "weapon_cb_size"))    c.weapon_cb_size     = num();
    else if (!_stricmp(key, "weapon_cb_offset"))  c.weapon_cb_offset   = (int)strtol(value, nullptr, 0);
    else if (!_stricmp(key, "weapon_cb_axis"))    c.weapon_cb_axis     = num();
    else if (!_stricmp(key, "weapon_cb_depth"))   c.weapon_cb_depth    = (float)atof(value);
    else if (!_stricmp(key, "weapon_cb_test"))    c.weapon_cb_test     = num();
    else if (!_stricmp(key, "shader_hide_index")) c.shader_hide_index  = num();
    else if (!_stricmp(key, "shader_hide_count")) c.shader_hide_count  = num();
    else if (!_stricmp(key, "shader_list"))       c.shader_list        = flag();
    else if (!_stricmp(key, "shader_dump"))       c.shader_dump        = flag();
    else if (!_stricmp(key, "weapon_cb_dump"))    c.weapon_cb_dump     = flag();
    else if (!_stricmp(key, "weapon_screen_stereo")) c.weapon_screen_stereo = flag();
    else if (!_stricmp(key, "weapon_shift_use_vs")) c.weapon_shift_use_vs = flag();
    else if (!_stricmp(key, "weapon_distance_m")) c.weapon_distance_m  = (float)atof(value);
    else if (!_stricmp(key, "weapon_shift_scale")) c.weapon_shift_scale = (float)atof(value);
    else if (!_stricmp(key, "weapon_shift_invert")) c.weapon_shift_invert = flag();
    else if (!_stricmp(key, "weapon_shift_width")) c.weapon_shift_width = num();
    else if (!_stricmp(key, "weapon_pass_live_ps")) c.weapon_pass_live_ps = flag();
    else if (!_stricmp(key, "weapon_pass_depth_rounds")) c.weapon_pass_depth_rounds = flag();
    else if (!_stricmp(key, "weapon_pass_diag"))  c.weapon_pass_diag   = flag();
    else if (!_stricmp(key, "hook_deferred_context")) c.hook_deferred_context = flag();
    else if (!_stricmp(key, "weapon_hide"))       c.weapon_hide        = flag();
    else if (!_stricmp(key, "head_early_write")) c.head_early_write   = flag();
    else if (!_stricmp(key, "freelook_key"))     c.freelook_key       = (int)strtol(value, nullptr, 0);
    else if (!_stricmp(key, "head_reset_watch")) c.head_reset_watch   = flag();
    else if (!_stricmp(key, "head_undo_mode")) c.head_undo_mode      = num();
    else if (!_stricmp(key, "head_clean_saved_aim")) c.head_clean_saved_aim = flag();
    else if (!_stricmp(key, "head_matrix_rotate")) c.head_matrix_rotate = flag();
    else if (!_stricmp(key, "head_matrix_yaw_invert")) c.head_matrix_yaw_invert = flag();
    else if (!_stricmp(key, "head_matrix_pitch_invert")) c.head_matrix_pitch_invert = flag();
    else if (!_stricmp(key, "head_matrix_pitch_axis_row")) c.head_matrix_pitch_axis_row = num();
    else if (!_stricmp(key, "head_suspend_on_stance")) c.head_suspend_on_stance = flag();
    else if (!_stricmp(key, "stance_rate_mps")) c.stance_rate_mps = (float)atof(value);
    else if (!_stricmp(key, "stance_hold_ms")) c.stance_hold_ms = num();
    else if (!_stricmp(key, "head_gate_on_latch")) c.head_gate_on_latch = flag();
    else if (!_stricmp(key, "head_leak_correct")) c.head_leak_correct = flag();
    else if (!_stricmp(key, "head_leak_after_the_fact")) c.head_leak_after_the_fact = flag();
    else if (!_stricmp(key, "cheat_set_hour"))   c.cheat_set_hour     = (float)atof(value);
    else if (!_stricmp(key, "cheat_freeze_time")) c.cheat_freeze_time = flag();
    else if (!_stricmp(key, "cheat_time_scale")) c.cheat_time_scale = (float)atof(value);
    else if (!_stricmp(key, "cheat_weather_speed")) c.cheat_weather_speed = (float)atof(value);
    else if (!_stricmp(key, "head_rotate_lever")) c.head_rotate_lever  = flag();
    else if (!_stricmp(key, "head_rotate_lever_invert")) c.head_rotate_lever_invert = flag();
    else if (!_stricmp(key, "head_rotate_lever_pitch")) c.head_rotate_lever_pitch = flag();
    else if (!_stricmp(key, "head_rotate_lever_yaw")) c.head_rotate_lever_yaw = flag();
    else if (!_stricmp(key, "head_rotate_lever_pitch_invert")) c.head_rotate_lever_pitch_invert = flag();
    else if (!_stricmp(key, "weapon_rot_probe"))  c.weapon_rot_probe   = flag();
    else if (!_stricmp(key, "weapon_rot_field"))  c.weapon_rot_field   = (int)strtol(value, nullptr, 0);
    else if (!_stricmp(key, "weapon_rot_invert")) c.weapon_rot_invert  = flag();
    else if (!_stricmp(key, "weapon_ps_crc0"))    c.weapon_ps_crc0     = (int)strtoul(value, nullptr, 0);
    else if (!_stricmp(key, "weapon_ps_crc1"))    c.weapon_ps_crc1     = (int)strtoul(value, nullptr, 0);
    else if (!_stricmp(key, "weapon_ps_crc2"))    c.weapon_ps_crc2     = (int)strtoul(value, nullptr, 0);
    else if (!_stricmp(key, "weapon_pos_x"))      c.weapon_pos_x       = (float)atof(value);
    else if (!_stricmp(key, "weapon_pos_y"))      c.weapon_pos_y       = (float)atof(value);
    else if (!_stricmp(key, "weapon_pos_z"))      c.weapon_pos_z       = (float)atof(value);
    else if (!_stricmp(key, "sound_enabled"))     c.sound_enabled      = flag();
    else if (!_stricmp(key, "sound_volume"))      c.sound_volume       = (float)atof(value);
    else if (!_stricmp(key, "panel_distance"))    c.panel_distance     = (float)atof(value);
    else if (!_stricmp(key, "panel_size"))        c.panel_size         = (float)atof(value);
    else COTW_LOG("[config] ignoring unknown key '%s'", key);
}

}  // namespace

std::wstring Config::Path() const { return SelfDir() + L"cotwvr.ini"; }

// --- profiles ---------------------------------------------------------------
//
// Deliberately file-copies rather than a second serialiser. Save() already knows
// how to write every key and Load() how to read them, so a profile is the same
// file under another name: no second format, no way for the two to drift apart,
// and a profile from an older build still loads because unknown keys are
// ignored exactly as they are in the main ini.
namespace {
std::wstring ProfilePath(int slot) {
    wchar_t name[64];
    _snwprintf_s(name, 64, _TRUNCATE, L"cotwvr.profile%d.ini", slot);
    return SelfDir() + name;
}
}  // namespace

bool Config::SaveProfile(int slot) const {
    if (slot < 1 || slot > kProfileSlots) return false;
    Save();                                     // flush current state to the ini
    const std::wstring from = SelfDir() + L"cotwvr.ini";
    const bool ok = CopyFileW(from.c_str(), ProfilePath(slot).c_str(), FALSE) != 0;
    Logf(ok ? "[profile] saved slot %d" : "[profile] could not save slot %d (%lu)",
         slot, GetLastError());
    return ok;
}

bool Config::LoadProfile(int slot) {
    if (slot < 1 || slot > kProfileSlots) return false;
    const std::wstring from = ProfilePath(slot);
    if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Logf("[profile] slot %d is empty", slot);
        return false;
    }
    const std::wstring to = SelfDir() + L"cotwvr.ini";
    if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) {
        Logf("[profile] could not load slot %d (%lu)", slot, GetLastError());
        return false;
    }
    const int keepSlot = profile_slot;
    Load();
    profile_slot = keepSlot;                    // do not let a profile move the dial
    Logf("[profile] loaded slot %d", slot);
    return true;
}

bool Config::ProfileExists(int slot) const {
    if (slot < 1 || slot > kProfileSlots) return false;
    return GetFileAttributesW(ProfilePath(slot).c_str()) != INVALID_FILE_ATTRIBUTES;
}

void Config::ResetToDefaults() {
    const int keepSlot = profile_slot;
    *this = Config();                           // every default, from the header
    profile_slot = keepSlot;
    Save();
    Logf("[profile] every setting reset to its default");
}


void Config::Load() {
    FILE* f = _wfopen(Path().c_str(), L"r");
    if (!f) {
        COTW_LOG("[config] no cotwvr.ini, using defaults; writing one");
        Save();
        return;
    }
    char line[512];
    int keys = 0;
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == '\r' || *p == '\n' || *p == '\0') continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = p;
        char* val = eq + 1;
        for (char* e = eq - 1; e >= key && (*e == ' ' || *e == '\t'); --e) *e = '\0';
        while (*val == ' ' || *val == '\t') ++val;
        for (char* e = val + strlen(val) - 1;
             e >= val && (*e == '\r' || *e == '\n' || *e == ' ' || *e == '\t'); --e) *e = '\0';
        Assign(*this, key, val);
        ++keys;
    }
    fclose(f);
    COTW_LOG("[config] loaded %d keys from cotwvr.ini", keys);
}

void Config::Save() const {
    FILE* f = _wfopen(Path().c_str(), L"w");
    if (!f) return;
    fprintf(f, "# theHunter: Call of the Wild VR\n");
    fprintf(f, "# Unknown keys are ignored, so this file is safe across versions.\n\n");
    fprintf(f, "version = %d\n", version);
    fprintf(f, "\n# Master switch. 0 = do nothing at all (vanilla game).\n");
    fprintf(f, "enabled = %d\n", enabled ? 1 : 0);
    fprintf(f, "\n# 0 = install hooks but never touch OpenXR (flat-screen debugging).\n");
    fprintf(f, "submit_to_headset = %d\n", submit_to_headset ? 1 : 0);
    fprintf(f, "\n# 1 = submit a solid colour instead of the game image.\n");
    fprintf(f, "#     Proves the whole runtime path with no game imagery involved.\n");
    fprintf(f, "blank_layer_test = %d\n", blank_layer_test ? 1 : 0);
    fprintf(f, "\n# Frames between heartbeat lines in the log. 0 = off.\n");
    fprintf(f, "log_frame_every = %d\n", log_frame_every);
    fprintf(f, "\n# Reverse-engineering aid. Hooks the engine's transform builders and\n");
    fprintf(f, "# reports which call site behaves like the player camera. Costs\n");
    fprintf(f, "# framerate - leave off unless hunting addresses.\n");
    fprintf(f, "camera_probe = %d\n", camera_probe ? 1 : 0);
    fprintf(f, "camera_probe_every = %d\n", camera_probe_every);
    fprintf(f, "\n# Axis test: shift the camera along one basis row to find out which\n");
    fprintf(f, "# row is right/up/forward, and whether the position is a real input.\n");
    fprintf(f, "# 0 = off, 1/2/3 = basis row 0/1/2.\n");
    fprintf(f, "camera_test_axis = %d\n", camera_test_axis);
    fprintf(f, "camera_test_amount = %.3f\n", camera_test_amount);

    fprintf(f, "\n# Show both eyes one shared symmetric frustum. Required while both eyes\n");
    fprintf(f, "# see the same rendered image - the runtime's mirrored asymmetric per-eye\n");
    fprintf(f, "# frusta put that image at a different angle in each eye = double vision.\n");
    fprintf(f, "\n# Render resolution written into the GAME's own settings at startup.\n");
    fprintf(f, "# Takes effect on the NEXT launch. The HEADSET entries are the exact shape\n");
    fprintf(f, "# of the headset's field of view - every pixel drawn is one you can see,\n");
    fprintf(f, "# and no black at the edges. Wider shapes draw outside the display.\n");
    // Listed from the one preset table rather than typed out here, so this can
    // never describe a numbering the build does not actually have.
    for (int i = 0; i < RenderPresetCount(); ++i) {
        const RenderPreset& p = GetRenderPreset(i);
        fprintf(f, "#   %2d = %-22s %s\n", i, p.name, p.note);
    }
    fprintf(f, "render_preset = %d\n", render_preset);
    fprintf(f, "# Your own resolution. Non-zero OVERRIDES the preset above - the\n");
    fprintf(f, "# presets are a menu of sensible shapes, not a limit.\n");
    fprintf(f, "render_custom_w = %d\n", render_custom_w);
    fprintf(f, "render_custom_h = %d\n", render_custom_h);
    fprintf(f, "\n# Field of view written alongside it.\n");
    fprintf(f, "#  -1 = calculate the value that exactly fills the headset (recommended)\n");
    fprintf(f, "#   0 = leave the game's own value\n");
    fprintf(f, "force_game_fov = %d\n", force_game_fov);
    fprintf(f, "# The headset's vertical FOV, used for that calculation.\n");
    fprintf(f, "headset_vfov_deg = %.1f\n", headset_vfov_deg);
    fprintf(f, "# 1 = game_fov_deg is reset at every launch to match the FOV given\n");
    fprintf(f, "#     to the game, so the two cannot drift apart.\n");
    fprintf(f, "# 0 = game_fov_deg is left exactly as you set it. Use this if you\n");
    fprintf(f, "#     are hand-tuning how near or far the world sits.\n");
    fprintf(f, "game_fov_follows_auto = %d\n", game_fov_follows_auto ? 1 : 0);
    fprintf(f, "\n# How many frames D3D may queue ahead. 1 keeps the pacing even,\n");
    fprintf(f, "# which is what alternate-eye rendering needs most.\n");
    fprintf(f, "#   0 = leave the game's own setting alone.\n");
    fprintf(f, "frame_latency = %d\n", frame_latency);
    fprintf(f, "\n# Hold the view steadier while aiming - a scope magnifies whatever\n");
    fprintf(f, "# your neck does. Milliseconds for the view to catch up:\n");
    fprintf(f, "#   0 = off, 60 = default (about 3/4 of the tremor gone), 250 = most.\n");
    fprintf(f, "aim_steady = %d\n", aim_steady ? 1 : 0);
    fprintf(f, "aim_steady_ms = %.0f\n", aim_steady_ms);
    fprintf(f, "aim_steady_scoped_only = %d\n", aim_steady_scoped_only ? 1 : 0);
    fprintf(f, "\n# Look up and down with your head only - the stick and the mouse\n");
    fprintf(f, "# keep turning left and right, but stop pitching the view.\n");
    fprintf(f, "lock_pitch_input = %d\n", lock_pitch_input ? 1 : 0);
    fprintf(f, "lock_pitch_pad = %d\n", lock_pitch_pad ? 1 : 0);
    fprintf(f, "lock_pitch_mouse = %d\n", lock_pitch_mouse ? 1 : 0);
    fprintf(f, "\n# THE GAME'S OWN GRAPHICS OPTIONS.\n");
    fprintf(f, "# Set here rather than in the game's settings.json, because the game\n");
    fprintf(f, "# rewrites that file from memory every time it quits - and regenerates\n");
    fprintf(f, "# it outright on the first run after a reinstall. Written from here on\n");
    fprintf(f, "# every launch instead, the same way the resolution above is.\n");
    fprintf(f, "#  -1 = leave the game's own value alone\n");
    fprintf(f, "# The screen-space effects reuse the previous frame, which under\n");
    fprintf(f, "# full-rate stereo was drawn from the OTHER eye - so they flicker.\n");
    fprintf(f, "apply_game_graphics = %d\n", apply_game_graphics ? 1 : 0);
    fprintf(f, "graphics_motion_blur = %d\n", graphics_motion_blur);
    fprintf(f, "graphics_depth_of_field = %d\n", graphics_depth_of_field);
    fprintf(f, "graphics_vignette = %d\n", graphics_vignette);
    fprintf(f, "graphics_ssao = %d\n", graphics_ssao);
    fprintf(f, "graphics_ssr = %d\n", graphics_ssr);
    fprintf(f, "graphics_contact_shadows = %d\n", graphics_contact_shadows);
    fprintf(f, "graphics_aa = %d\n", graphics_aa);
    fprintf(f, "\n# Weapon / viewmodel field of view - separate from the world FOV,\n");
    fprintf(f, "# because the game renders the held weapon through its own projection.\n");
    fprintf(f, "# 0 = leave the game's own value. Applied live, no relaunch needed.\n");

    fprintf(f, "symmetric_fov = %d\n", symmetric_fov ? 1 : 0);
    fprintf(f, "\n# The VERTICAL field of view the GAME renders with, in degrees. Must match\n");
    fprintf(f, "# the game's own setting or near objects become uncomfortable to look at.\n");
    fprintf(f, "# 0 = stretch to fill the headset instead (wrong geometry, no borders).\n");
    fprintf(f, "game_fov_deg = %.1f\n", game_fov_deg);

    fprintf(f, "\n# --- head tracking ---\n");
    fprintf(f, "head_tracking = %d\n", head_tracking ? 1 : 0);
    fprintf(f, "# Flip if turning your head LEFT sends the view right.\n");
    fprintf(f, "head_invert = %d\n", head_invert ? 1 : 0);
    fprintf(f, "# Axis test: 0 = real head pose, 1/2/3 = forced yaw/pitch/roll\n");
    fprintf(f, "head_test_axis = %d\n", head_test_axis);
    fprintf(f, "head_test_degrees = %.1f\n", head_test_degrees);
    fprintf(f, "# Sweep the test angle back and forth so the effect is unmistakable.\n");
    fprintf(f, "head_test_sweep = %d\n", head_test_sweep ? 1 : 0);
    fprintf(f, "# Write the head yaw into the engine's OWN yaw field (found at +0x4C by\n");
    fprintf(f, "# correlation, r = -1.000) instead of rewriting the derived matrix.\n");
    fprintf(f, "head_write_yaw = %d\n", head_write_yaw ? 1 : 0);
    fprintf(f, "head_yaw_field = 0x%X\n", head_yaw_field);
    fprintf(f, "# Pitch - a candidate, not confirmed. +0x50 sits next to the yaw and\n");
    fprintf(f, "# spans about +/-63 degrees, which is what a pitch clamp looks like.\n");
    fprintf(f, "head_write_pitch = %d\n", head_write_pitch ? 1 : 0);
    fprintf(f, "head_pitch_field = 0x%X\n", head_pitch_field);
    fprintf(f, "# Pitch has its OWN sign flag - the two axes are separate fields.\n");
    fprintf(f, "head_pitch_invert = %d\n", head_pitch_invert ? 1 : 0);
    fprintf(f, "# HEAD ROLL - tip your head sideways and the horizon tips\n");
    fprintf(f, "# with it. Roll was always measured from the headset and just\n");
    fprintf(f, "# never sent to the game.\n");
    fprintf(f, "head_write_roll = %d\n", head_write_roll ? 1 : 0);
    fprintf(f, "# THE VIEW FROM A DIFFERENT HOOK. Turns the view by rotating\n");
    fprintf(f, "# the matrix the view commit builds, instead of writing an angle\n");
    fprintf(f, "# the engine stores and reads back. A camera transition samples\n");
    fprintf(f, "# the aim at its start AND its end - two rotations per stance\n");
    fprintf(f, "# change, and aiming counts as a transition too. With nothing\n");
    fprintf(f, "# stored there is nothing to sample. Moves the VIEW only; the\n");
    fprintf(f, "# weapon follows via head_matrix_rotate.\n");
    fprintf(f, "head_view_basis = %d\n", head_view_basis ? 1 : 0);
    fprintf(f, "# THE RATCHET FIX. 0x00638730 re-seeds your aim from the\n");
    fprintf(f, "# current view - correct in a game where only the player can\n");
    fprintf(f, "# move the camera, wrong once a head can. Cheat Engine caught\n");
    fprintf(f, "# it writing the yaw accumulator exactly 3 times for 3\n");
    fprintf(f, "# crouches. This lets it run and puts the aim back after.\n");
    fprintf(f, "head_keep_aim_on_reseed = %d\n", head_keep_aim_on_reseed ? 1 : 0);
    fprintf(f, "# THE CODE CAVE. Corrects the accumulator write instead of\n");
    fprintf(f, "# blocking it - blocking stops the ratchet but causes the swing,\n");
    fprintf(f, "# because the routine writes a consistent pair. Takes precedence\n");
    fprintf(f, "# over head_keep_aim_on_reseed; both target the same 6 bytes.\n");
    fprintf(f, "head_reseed_detour = %d\n", head_reseed_detour ? 1 : 0);
    fprintf(f, "# Hold breath re-seeds through its OWN deferred latch, in a\n");
    fprintf(f, "# different function - the detour above never touched it.\n");
    fprintf(f, "head_reseed_holdbreath = %d\n", head_reseed_holdbreath ? 1 : 0);
    fprintf(f, "profile_slot = %d\n", profile_slot);
    fprintf(f, "# Show the settings panel on the monitor too, so a setting\n");
    fprintf(f, "# can be tried without putting the headset on.\n");
    fprintf(f, "panel_on_monitor = %d\n", panel_on_monitor ? 1 : 0);
    fprintf(f, "# Sign and amount taken out. That field's convention was never\n");
    fprintf(f, "# measured, so try -1 if +1 makes it worse. 0 = cave does nothing.\n");
    fprintf(f, "head_reseed_correct_scale = %.2f\n", head_reseed_correct_scale);
    fprintf(f, "# Also block the re-seed writing the LIVE aim yaw. Without it\n");
    fprintf(f, "# the drift stops ACCUMULATING but the view still swings into\n");
    fprintf(f, "# each crouch and back out again. Riskier - the renderer reads\n");
    fprintf(f, "# that field - so turn this off first if a camera move breaks.\n");
    fprintf(f, "head_block_reseed_yaw = %d\n", head_block_reseed_yaw ? 1 : 0);
    fprintf(f, "# Remove our own rotation where the engine decomposes the view\n");
    fprintf(f, "# matrix back into the stored aim (0x004B720B). That decomposition\n");
    fprintf(f, "# is how a matrix rotation still becomes permanent aim - the road\n");
    fprintf(f, "# left open once the angle write was closed. Needs head_view_basis.\n");
    fprintf(f, "head_bake_undo = %d\n", head_bake_undo ? 1 : 0);
    fprintf(f, "# Which float on the camera is roll. Yaw is 0x4C and pitch is\n");
    fprintf(f, "# 0x50, so 0x54 is the likely third of the triple - but that is\n");
    fprintf(f, "# a GUESS, not a measurement. Change it here if it is wrong.\n");
    fprintf(f, "head_roll_field = 0x%X\n", head_roll_field);
    fprintf(f, "head_roll_invert = %d\n", head_roll_invert ? 1 : 0);
    fprintf(f, "head_roll_scale = %.2f\n", head_roll_scale);
    fprintf(f, "# SMOOTHNESS. Submit the pose the frame was really drawn\n");
    fprintf(f, "# with rather than one located a frame later. SubmitFrame\n");
    fprintf(f, "# runs before HeadTrackTick, so the camera angles for the\n");
    fprintf(f, "# frame going out now came from the PREVIOUS present. Telling\n");
    fprintf(f, "# the runtime otherwise puts its reprojection one frame of\n");
    fprintf(f, "# head motion out - nothing when still, worse the faster you\n");
    fprintf(f, "# turn. That is judder you only see while moving.\n");
    fprintf(f, "submit_rendered_pose = %d\n", submit_rendered_pose ? 1 : 0);
    fprintf(f, "# Submit each eye with the pose ITS OWN pixels were drawn from.\n");
    fprintf(f, "# Alternate-eye rendering redraws one eye per frame, so a single\n");
    fprintf(f, "# shared pose presented the stale eye as though it were fresh -\n");
    fprintf(f, "# and because the eyes alternate, that error swapped sides every\n");
    fprintf(f, "# frame. Only matters with full_rate_stereo off.\n");
    fprintf(f, "aer_per_eye_pose = %d\n", aer_per_eye_pose ? 1 : 0);
    fprintf(f, "# THE FLAT SCREEN. The game draws its menus flat at screen\n");
    fprintf(f, "# scale; stretched over the whole field of view their edges\n");
    fprintf(f, "# fall outside where the eye can comfortably look. This puts\n");
    fprintf(f, "# the game image on a floating panel instead.\n");
    fprintf(f, "menu_screen = %d\n", menu_screen ? 1 : 0);
    fprintf(f, "# Hotkey, a Windows virtual key code. 0x71 = F2.\n");
    fprintf(f, "menu_screen_key = 0x%X\n", menu_screen_key);
    fprintf(f, "# Two signals: no player camera finds the MAIN menu, a visible\n");
    fprintf(f, "# mouse cursor finds the IN-GAME one. The in-game menu leaves\n");
    fprintf(f, "# the world running, so the camera test cannot see it.\n");
    fprintf(f, "menu_screen_auto = %d\n", menu_screen_auto ? 1 : 0);
    fprintf(f, "menu_screen_width_m = %.2f\n", menu_screen_width_m);
    fprintf(f, "menu_screen_distance_m = %.2f\n", menu_screen_distance_m);
    fprintf(f, "# Blank the world behind it - MAIN menu only.\n");
    fprintf(f, "menu_screen_hide_world = %d\n", menu_screen_hide_world ? 1 : 0);
    fprintf(f, "menu_cursor_hold_ms = %d\n", menu_cursor_hold_ms);
    fprintf(f, "# Report every candidate menu signal the moment it changes,\n");
    fprintf(f, "# so which one marks this game menu can be READ off a run\n");
    fprintf(f, "# instead of guessed at. Costs one line per change.\n");
    fprintf(f, "menu_screen_log_signals = %d\n", menu_screen_log_signals ? 1 : 0);
    fprintf(f, "# Count \"the game released the mouse cursor\" as a menu, not\n");
    fprintf(f, "# just \"a cursor is visible\". ESC opens the menu with a cursor,\n");
    fprintf(f, "# START on the pad opens it without one.\n");
    fprintf(f, "menu_screen_use_clip = %d\n", menu_screen_use_clip ? 1 : 0);

    fprintf(f, "\n# --- the held weapon ---\n");
    fprintf(f, "# The weapon rides the camera, so it inherits the stereo eye shift and\n");
    fprintf(f, "# lands on identical pixels in both eyes - flat and seemingly infinitely\n");
    fprintf(f, "# far away. Cancelling that shift at its own transform gives it depth.\n");
    fprintf(f, "# Which call site draws it must be IDENTIFIED first, by sweeping it.\n");
    fprintf(f, "weapon_site = 0x%X\n", weapon_site);
    fprintf(f, "weapon_test_sweep = %d\n", weapon_test_sweep ? 1 : 0);
    fprintf(f, "weapon_test_amount = %.3f\n", weapon_test_amount);
    fprintf(f, "# Sweep EVERY site in turn so one run tests them all. CTRL+ALT+W marks\n");
    fprintf(f, "# the one on screen when the WEAPON moves.\n");
    fprintf(f, "weapon_scan = %d\n", weapon_scan ? 1 : 0);
    fprintf(f, "weapon_scan_seconds = %d\n", weapon_scan_seconds);
    fprintf(f, "weapon_stereo = %d\n", weapon_stereo ? 1 : 0);
    fprintf(f, "weapon_depth = %.2f\n", weapon_depth);

    // THIRD time this file has lost keys that Load() accepts and Save() forgot,
    // and this time it cost a full test cycle: weapon_cb_scan was wiped on exit,
    // so the constant-buffer scan never installed, the patch never ran, and it
    // presented as "no axis works". A missing line here is indistinguishable
    // from a broken feature.
    fprintf(f, "\n# --- weapon depth (the viewmodel's constant buffer) ---\n");
    fprintf(f, "# Confirmed by holstering: the 192-byte buffer at +0x0000 holds the\n");
    fprintf(f, "# weapon's transform - 0.27-0.52 m from the camera, twice per frame,\n");
    fprintf(f, "# and not written at all while the weapon is put away.\n");
    fprintf(f, "weapon_cb_scan = %d\n", weapon_cb_scan ? 1 : 0);
    fprintf(f, "weapon_cb_stereo = %d\n", weapon_cb_stereo ? 1 : 0);
    fprintf(f, "weapon_cb_size = %d\n", weapon_cb_size);
    fprintf(f, "weapon_cb_offset = 0x%X\n", weapon_cb_offset);
    fprintf(f, "# Which axis is sideways in the weapon's own space: 0, 1 or 2.\n");
    fprintf(f, "weapon_cb_axis = %d\n", weapon_cb_axis);
    fprintf(f, "weapon_cb_depth = %.2f\n", weapon_cb_depth);

    // FOURTH time, and the most expensive yet: all five of these were dropped,
    // so the player toggled the feature on in the panel, saved, and the save
    // itself turned it back off. The report read as "no depth, just flicker"
    // when half of it was simply not running.
    fprintf(f, "\n# --- weapon depth by screen shift (needs no matrix) ---\n");
    fprintf(f, "# For an object at a fixed distance, stereo disparity IS a\n");
    fprintf(f, "# horizontal shift, so the weapon's own draws go through a\n");
    fprintf(f, "# shifted viewport and nothing else in the frame is touched.\n");
    fprintf(f, "weapon_screen_stereo = %d\n", weapon_screen_stereo ? 1 : 0);
    fprintf(f, "# The weapon's vertex shader is shared with other geometry, so\n");
    fprintf(f, "# this shifts objects out in the world too. Off unless needed.\n");
    fprintf(f, "weapon_shift_use_vs = %d\n", weapon_shift_use_vs ? 1 : 0);
    fprintf(f, "# The one number that matters: lower = closer = more depth.\n");
    fprintf(f, "weapon_distance_m = %.3f\n", weapon_distance_m);
    fprintf(f, "weapon_shift_scale = %.3f\n", weapon_shift_scale);
    fprintf(f, "weapon_shift_invert = %d\n", weapon_shift_invert ? 1 : 0);
    fprintf(f, "weapon_shift_width = %d\n", weapon_shift_width);

    fprintf(f, "\n# --- the viewmodel pass, as the RenderDoc capture shows it ---\n");
    fprintf(f, "# The visible weapon is a forward pass AFTER post-processing:\n");
    fprintf(f, "# ten meshes, each drawn three times through plain DrawIndexed.\n");
    fprintf(f, "# Ask the driver what is bound instead of our shadow table - a\n");
    fprintf(f, "# game that cycles deferred contexts exhausts it, after which\n");
    fprintf(f, "# every draw looks like nothing is bound.\n");
    fprintf(f, "weapon_pass_live_ps = %d\n", weapon_pass_live_ps ? 1 : 0);
    fprintf(f, "# Shift the two depth-only rounds too, matched by an index count\n");
    fprintf(f, "# the colour round taught us. Off = the gun fights its own depth.\n");
    fprintf(f, "weapon_pass_depth_rounds = %d\n", weapon_pass_depth_rounds ? 1 : 0);
    fprintf(f, "# Does the hook see the pass at all? Read [wdiag] in the log.\n");
    fprintf(f, "weapon_pass_diag = %d\n", weapon_pass_diag ? 1 : 0);

    fprintf(f, "\n# --- the HUD component probe (diagnostic, read-only) ---\n");
    fprintf(f, "# The HUD sits at the EDGES of the screen, which is the worst\n");
    fprintf(f, "# place for it in a headset. To move a component inward it has\n");
    fprintf(f, "# to be identified first, and shader identity cannot do it -\n");
    fprintf(f, "# several components share one shader. So the handle is WHERE\n");
    fprintf(f, "# THE QUAD LANDS, and this prints exactly that: one [hud] line\n");
    fprintf(f, "# per draw that looks like HUD (depth test off, blending on, not\n");
    fprintf(f, "# viewmodel or optic glass), with viewport, scissor, render\n");
    fprintf(f, "# target size, the textures bound - described, not just their\n");
    fprintf(f, "# pointers - the shader crc32s Shader Toggler shows, and the\n");
    fprintf(f, "# first 16 floats of the constant buffer the draw reads.\n");
    fprintf(f, "# Read-only: Get* calls only, everything released, no draw\n");
    fprintf(f, "# changed. Off costs one global read per draw.\n");
    fprintf(f, "# RUN IT TWICE - once with the game's HUD on and once with it\n");
    fprintf(f, "# off (settings.json GameDisplayHUD) - and diff the two logs.\n");
    fprintf(f, "# Everything that disappears is HUD; everything that survives is\n");
    fprintf(f, "# not. That is a measurement instead of a guessed fingerprint.\n");
    fprintf(f, "hud_probe = %d\n", hud_probe ? 1 : 0);
    fprintf(f, "# It stops by itself after this many PRESENTS, then has to be\n");
    fprintf(f, "# re-armed by setting hud_probe back to 0 and to 1 again. One\n");
    fprintf(f, "# Present spans both eyes under full-rate stereo.\n");
    fprintf(f, "hud_probe_frames = %d\n", hud_probe_frames);
    fprintf(f, "hud_isolate = %d\n", hud_isolate);
    fprintf(f, "hud_isolate_post = %d\n", hud_isolate_post ? 1 : 0);
    fprintf(f, "hud_move_x = %d\n", hud_move_x);
    fprintf(f, "hud_move_y = %d\n", hud_move_y);
    fprintf(f, "hud_scale = %.3f\n", hud_scale);
    fprintf(f, "hud_hide_glare = %d\n", hud_hide_glare ? 1 : 0);
    fprintf(f, "# The game records its frame on DEFERRED contexts and replays\n");
    fprintf(f, "hud_hide_sunflare = %d\n", hud_hide_sunflare ? 1 : 0);
    fprintf(f, "# command lists on the immediate one. Without this we see 0.9\n");
    fprintf(f, "# draws a frame out of 718. Turn OFF if a build is unstable.\n");
    fprintf(f, "hook_deferred_context = %d\n", hook_deferred_context ? 1 : 0);
    fprintf(f, "# Remove the viewmodel outright - the fallback a flat gun deserves.\n");
    fprintf(f, "weapon_hide = %d\n", weapon_hide ? 1 : 0);

    fprintf(f, "\n# --- why the weapon does not follow the head ---\n");
    fprintf(f, "# The view is offset at head_yaw_field; the weapon is oriented\n");
    fprintf(f, "# from the player's AIM, which never sees that offset. This finds\n");
    fprintf(f, "# the aim field by correlating every field against the yaw WE\n");
    fprintf(f, "# inject: view-chain fields carry it, aim-chain fields do not.\n");
    fprintf(f, "# Apply the head offset at the aim consumer (0x0063C0A0) rather\n");
    fprintf(f, "# than at the camera transform builder. The builder runs AFTER\n");
    fprintf(f, "# the weapon is placed - which is why the view followed the head\n");
    fprintf(f, "# and the gun did not. The aim consumer runs before both.\n");
    fprintf(f, "head_early_write = %d\n", head_early_write ? 1 : 0);
    fprintf(f, "# Hold to break the coupling: the view follows your head but the\n");
    fprintf(f, "# weapon stays aimed where it was. Virtual-key code, 0xA4 = LAlt.\n");
    fprintf(f, "freelook_key = 0x%X\n", freelook_key);
    fprintf(f, "\n# --- recentring ---\n");
    fprintf(f, "# Takes your current head direction as straight ahead WITHOUT\n");
    fprintf(f, "# moving the picture. Virtual-key code, 0x24 = HOME.\n");
    fprintf(f, "recentre_key = 0x%X\n", recentre_key);
    fprintf(f, "# Or click BOTH sticks and hold. The buttons are hidden from the\n");
    fprintf(f, "# game while both are down, so nothing sprints or holds breath.\n");
    fprintf(f, "recentre_pad_enable = %d\n", recentre_pad_enable ? 1 : 0);
    fprintf(f, "recentre_pad_chord = 0x%X\n", recentre_pad_chord);
    fprintf(f, "recentre_pad_hold_ms = %d\n", recentre_pad_hold_ms);
    fprintf(f, "recentre_pad_swallow = %d\n", recentre_pad_swallow ? 1 : 0);
    fprintf(f, "# THE MASTER SWITCH. 0 restores the old recentre exactly: the view\n");
    fprintf(f, "# snaps to wherever the game was aiming. It also turns the stick\n");
    fprintf(f, "# chord off, because an accidental chord is only harmless while\n");
    fprintf(f, "# the press does not move the picture.\n");
    fprintf(f, "recentre_compensate = %d\n", recentre_compensate ? 1 : 0);
    fprintf(f, "# Turn on if pressing recentre throws the view TWICE as far.\n");
    fprintf(f, "recentre_compensate_invert = %d\n", recentre_compensate_invert ? 1 : 0);
    fprintf(f, "recentre_reset_smoother = %d\n", recentre_reset_smoother ? 1 : 0);
    fprintf(f, "# Frames to spread the hand-over over. 1 = one step, which the\n");
    fprintf(f, "# game's screen-space effects turn into a brief artefact.\n");
    fprintf(f, "recentre_ramp_frames = %d\n", recentre_ramp_frames);
    fprintf(f, "# Wait until the head is still before handing over. 0 = fire at once.\n");
    fprintf(f, "recentre_settle_ms = %d\n", recentre_settle_ms);
    fprintf(f, "recentre_settle_deg_s = %.1f\n", recentre_settle_deg_s);
    fprintf(f, "recentre_settle_max_ms = %d\n", recentre_settle_max_ms);
    fprintf(f, "# The engine hard-clamps its own pitch accumulator, so the credit\n");
    fprintf(f, "# is clamped here too and the reference advances only by the part\n");
    fprintf(f, "# that fitted. If a press still tips the view when you are looking\n");
    fprintf(f, "# far up or down, lower this to 63 (the measured camera range).\n");
    fprintf(f, "recentre_pitch_limit_deg = %d\n", recentre_pitch_limit_deg);
    fprintf(f, "# Re-anchor your standing position too, so the body ends up back\n");
    fprintf(f, "# under your head. Set to 0 if recentring while LEANING slides the\n");
    fprintf(f, "# view sideways - that is this, not the rotation.\n");
    fprintf(f, "recentre_position = %d\n", recentre_position ? 1 : 0);
    fprintf(f, "head_reset_watch = %d\n", head_reset_watch ? 1 : 0);
    fprintf(f, "# When the early head offset is taken back out of the engine's\n");
    fprintf(f, "# field: 0 = never (the weapon follows, crouching drifts),\n");
    fprintf(f, "# 1 = right after the aim consumer (crouch clean, but the weapon\n");
    fprintf(f, "# stops following), 2 = at the camera transform builder, after\n");
    fprintf(f, "# both the weapon and the view have read it.\n");
    fprintf(f, "head_undo_mode = %d\n", head_undo_mode);
    fprintf(f, "# Suspend the head offset while the stance is changing. Crouch,\n");
    fprintf(f, "# stand, prone and jump all move the camera height fast; walking a\n");
    fprintf(f, "# slope is far slower, which is what tells them apart. Raise the\n");
    fprintf(f, "# rate if it triggers on slopes, lower it if a crouch slips past.\n");
    fprintf(f, "head_suspend_on_stance = %d\n", head_suspend_on_stance ? 1 : 0);
    fprintf(f, "# Rotate the view MATRIX instead of writing the engine's aim\n");
    fprintf(f, "# angles. A value we never write cannot be read back, which is\n");
    fprintf(f, "# what every angle-write variant fell to. Flip an invert if a\n");
    fprintf(f, "# direction is backwards; try pitch_axis_row 0 if pitch rolls the\n");
    fprintf(f, "# horizon. When this is on, no angle is written anywhere.\n");
    fprintf(f, "# THE FIX: when the engine saves the aim for a scripted camera\n");
    fprintf(f, "# episode (crouch, stand, prone, jump, stance change on a slope),\n");
    fprintf(f, "# hand it its OWN value - so nothing of ours is restored later as\n");
    fprintf(f, "# your aim, which is what ratcheted the view.\n");
    fprintf(f, "head_clean_saved_aim = %d\n", head_clean_saved_aim ? 1 : 0);
    fprintf(f, "head_matrix_rotate = %d\n", head_matrix_rotate ? 1 : 0);
    fprintf(f, "head_matrix_yaw_invert = %d\n", head_matrix_yaw_invert ? 1 : 0);
    fprintf(f, "head_matrix_pitch_invert = %d\n", head_matrix_pitch_invert ? 1 : 0);
    fprintf(f, "head_matrix_pitch_axis_row = %d\n", head_matrix_pitch_axis_row);
    fprintf(f, "stance_rate_mps = %.3f\n", stance_rate_mps);
    fprintf(f, "stance_hold_ms = %d\n", stance_hold_ms);
    fprintf(f, "# Disproven: this fired on 100%% of ticks, so it only turned head\n");
    fprintf(f, "# tracking off entirely. Kept switchable, but leave it at 0.\n");
    fprintf(f, "head_gate_on_latch = %d\n", head_gate_on_latch ? 1 : 0);
    fprintf(f, "# Superseded guesswork - put back any offset the engine swallows.\n");
    fprintf(f, "head_leak_correct = %d\n", head_leak_correct ? 1 : 0);
    fprintf(f, "# Fallback: correct AFTER the engine takes the offset instead of\n");
    fprintf(f, "# never leaving one to take. Fights your turning; off.\n");
    fprintf(f, "head_leak_after_the_fact = %d\n", head_leak_after_the_fact ? 1 : 0);

    fprintf(f, "\n# --- cheats (time of day, weather) ---\n");
    fprintf(f, "# Applied once then returned to -1, so time carries on afterwards.\n");
    fprintf(f, "cheat_set_hour = %.3f\n", cheat_set_hour);
    fprintf(f, "cheat_freeze_time = %d\n", cheat_freeze_time ? 1 : 0);
    fprintf(f, "# How fast the day runs. 1 = the game speed, 0.25 = a quarter\n");
    fprintf(f, "# speed so dawn lasts, 5 = watch the sun cross the sky. Done by\n");
    fprintf(f, "# measuring the engine own hourly step and re-applying it scaled,\n");
    fprintf(f, "# so no unverified multiplier field is written.\n");
    fprintf(f, "cheat_time_scale = %.2f\n", cheat_time_scale);
    fprintf(f, "# -1 leaves the game's own weather pacing alone.\n");
    fprintf(f, "cheat_weather_speed = %.3f\n", cheat_weather_speed);
    fprintf(f, "# The OTHER half of head tracking: +0x4C turns the view, the\n");
    fprintf(f, "# lever's matrix turns the weapon and body. Both, or they part.\n");
    fprintf(f, "head_rotate_lever = %d\n", head_rotate_lever ? 1 : 0);
    fprintf(f, "# One axis at a time, and a sign for each - the angles come from\n");
    fprintf(f, "# the angle-FIELD conventions, which say nothing about the\n");
    fprintf(f, "# handedness a geometric rotation needs.\n");
    fprintf(f, "head_rotate_lever_yaw = %d\n", head_rotate_lever_yaw ? 1 : 0);
    fprintf(f, "head_rotate_lever_invert = %d\n", head_rotate_lever_invert ? 1 : 0);
    fprintf(f, "head_rotate_lever_pitch = %d\n", head_rotate_lever_pitch ? 1 : 0);
    fprintf(f, "head_rotate_lever_pitch_invert = %d\n",
            head_rotate_lever_pitch_invert ? 1 : 0);
    fprintf(f, "weapon_rot_probe = %d\n", weapon_rot_probe ? 1 : 0);
    fprintf(f, "# A candidate to test, e.g. 0x54. -1 writes nothing.\n");
    fprintf(f, "weapon_rot_field = %d\n", weapon_rot_field);
    fprintf(f, "weapon_rot_invert = %d\n", weapon_rot_invert ? 1 : 0);

    // The remaining keys Assign() accepts. Every one was silently lost on save
    // until now - including the three shader CRCs that identify the weapon, and
    // the position offsets, which is a feature that could never have survived a
    // restart.
    fprintf(f, "\n# --- weapon position offset, in metres, camera-relative ---\n");
    fprintf(f, "weapon_pos_x = %.3f\n", weapon_pos_x);
    fprintf(f, "weapon_pos_y = %.3f\n", weapon_pos_y);
    fprintf(f, "weapon_pos_z = %.3f\n", weapon_pos_z);
    fprintf(f, "\n# --- catching the game's devices at birth ---\n");
    fprintf(f, "# The game creates FIVE D3D11 devices; the mod only ever knew the one\n");
    fprintf(f, "# that presents, which is why 99.6%% of the frame never reached it.\n");
    fprintf(f, "# CRASHES at present: finding the devices is fine, but hooking the\n");
    fprintf(f, "# second context implementation they expose takes the game down.\n");
    fprintf(f, "hook_device_creation = %d\n", hook_device_creation ? 1 : 0);
    fprintf(f, "# 1 = patch the vtable entry (atomic, cannot race a live draw).\n");
    fprintf(f, "# 0 = the old MinHook function-body patch, which crashed at startup.\n");
    fprintf(f, "hook_context_vtable = %d\n", hook_context_vtable ? 1 : 0);
    fprintf(f, "# Nudge the game window 1px once, so the engine rebuilds the contexts\n");
    fprintf(f, "# it records the frame on - the ones it builds now are ones we hook.\n");
    fprintf(f, "# Takes coverage from 0.4%% of the frame to ~194%% (both eyes).\n");
    fprintf(f, "rebuild_contexts_on_start = %d\n", rebuild_contexts_on_start ? 1 : 0);
    fprintf(f, "# Worker thread hooking contexts from process start instead of at\n");
    fprintf(f, "# first Present. Off = the build that produced a clean vanish.\n");
    fprintf(f, "hook_contexts_early = %d\n", hook_contexts_early ? 1 : 0);
    fprintf(f, "\n# --- the gun in 3D ---\n");
    fprintf(f, "# Shifts clip-space X on the viewmodel's WorldViewProjection,\n");
    fprintf(f, "# opposite per eye, which is the disparity a real object at arm's\n");
    fprintf(f, "# length has. Start amount small: 0.02 is visible, 0.1 is a lot.\n");
    fprintf(f, "# layout 1 is the transposed reading - use it if the gun SHEARS\n");
    fprintf(f, "# instead of sliding sideways.\n");
    fprintf(f, "weapon_3d = %d\n", weapon_3d ? 1 : 0);
    fprintf(f, "weapon_3d_amount = %.4f\n", weapon_3d_amount);
    fprintf(f, "weapon_3d_slot = %d\n", weapon_3d_slot);
    fprintf(f, "weapon_3d_layout = %d\n", weapon_3d_layout);
    fprintf(f, "# -1 = find WorldViewProjection per draw. Two block layouts share the\n");
    fprintf(f, "# slot: InstanceConsts has it at 0, LocalConstants (hands) at 0x40.\n");
    fprintf(f, "weapon_3d_wvp_offset = %d\n", weapon_3d_wvp_offset);
    fprintf(f, "# Scope/binocular GLASS is blended and carries no first-person stencil\n");
    fprintf(f, "# tag. 0 = off, nonzero = on: a blended draw is shifted only when its\n");
    fprintf(f, "# matrix is bit-identical to one shifted on a tagged draw this frame.\n");
    fprintf(f, "weapon_3d_glass = %d\n", weapon_3d_glass);
    fprintf(f, "# Trims how far the LENS moves relative to the gun body, so the\n");
    fprintf(f, "# glass sits centred in its shroud. 1.0 = the computed amount.\n");
    fprintf(f, "weapon_3d_glass_scale = %.3f\n", weapon_3d_glass_scale);
    fprintf(f, "# 1 = move only see-through scope parts; 0 = also the solid ones,\n");
    fprintf(f, "# including the invisible disc that cuts out the lens picture.\n");
    fprintf(f, "weapon_3d_glass_blended_only = %d\n",
            weapon_3d_glass_blended_only ? 1 : 0);
    fprintf(f, "# 1 = move EVERY draw within weapon_3d_glass of a first-person\n");
    fprintf(f, "# draw, without identifying it. Catches the scope mask disc that\n");
    fprintf(f, "# escapes every property test. Keep the reach small.\n");
    fprintf(f, "weapon_3d_glass_whole_pass = %d\n",
            weapon_3d_glass_whole_pass ? 1 : 0);
    fprintf(f, "# 1 = match the lens MATERIAL. The glass and the invisible disc\n");
    fprintf(f, "# that cuts the shroud are one shader pair, so matching it moves\n");
    fprintf(f, "# both. 0 = previous behaviour.\n");
    fprintf(f, "# 1 = run the viewmodel test on every kind of draw call, not\n");
    fprintf(f, "# only the indexed ones. 0 = previous behaviour.\n");
    fprintf(f, "# The scope/binocular LENS vertex shader, as the number ReShade\n");
    fprintf(f, "# Shader Toggler shows. The glass and the invisible disc that\n");
    fprintf(f, "# cuts the shroud are both drawn with it. 0 = slot unused.\n");
    fprintf(f, "# 1 = also move the draw that STAMPS the scope lens circle into\n");
    fprintf(f, "# the stencil buffer. Without it the gun moves and the circle it\n");
    fprintf(f, "# is clipped against stays put, eating a crescent out of the\n");
    fprintf(f, "# shroud. 0 = previous behaviour.\n");
    fprintf(f, "weapon_match_stencil_write = %d\n",
            weapon_match_stencil_write ? 1 : 0);
    fprintf(f, "# 1 = use a separate 3D strength while looking through a scope\n");
    fprintf(f, "# or binoculars. Detected from the lens-circle stamp, which only\n");
    fprintf(f, "# exists while an optic is raised. 0 = one strength everywhere.\n");
    fprintf(f, "# 1 = if a weapon piece cannot be moved through its constants,\n");
    fprintf(f, "# move it at the rasterizer instead, so it never stands still for\n");
    fprintf(f, "# a frame while the rest of the weapon moves. 0 = leave it.\n");
    fprintf(f, "# 1 = scope/binocular glass uses the same eye its weapon body was\n");
    fprintf(f, "# shifted for, instead of deciding separately at draw time. Stops\n");
    fprintf(f, "# the lenses jumping out of the optic on odd frames.\n");
    fprintf(f, "# 1 = match a binoculars second lens by the material of the one\n");
    fprintf(f, "# already confirmed. Without it only one lens of a pair moves and\n");
    fprintf(f, "# the other sits displaced. 0 = previous behaviour.\n");
    fprintf(f, "# 1 = move scope/binocular glass through its constants, like the\n");
    fprintf(f, "# gun body, instead of at the rasterizer. The rasterizer moves a\n");
    fprintf(f, "# lens and its screen-space picture together, which a lens cannot\n");
    fprintf(f, "# survive. 0 = rasterizer only, as before.\n");
    fprintf(f, "# 1 = recognise scope glass by the mesh the lens mask is drawn\n");
    fprintf(f, "# from, rather than by which constant buffer it happened to read.\n");
    fprintf(f, "# The buffer rotates, so that route caught the lens on some frames\n");
    fprintf(f, "# and not others - which is the flicker. 0 = previous behaviour.\n");
    fprintf(f, "weapon_3d_glass_mask_mesh = %d\n",
            weapon_3d_glass_mask_mesh ? 1 : 0);
    fprintf(f, "# 1 = move scope glass and the lens mask at the rasterizer, so\n");
    fprintf(f, "# the magnified picture inside the lens moves WITH the lens. The\n");
    fprintf(f, "# constants route moves the glass and leaves the picture, which\n");
    fprintf(f, "# alternates per eye and reads as flicker.\n");
    fprintf(f, "weapon_3d_glass_via_rasterizer = %d\n",
            weapon_3d_glass_via_rasterizer ? 1 : 0);
    fprintf(f, "weapon_3d_glass_via_constants = %d\n",
            weapon_3d_glass_via_constants ? 1 : 0);
    fprintf(f, "weapon_3d_glass_follow_body_eye = %d\n",
            weapon_3d_glass_follow_body_eye ? 1 : 0);
    fprintf(f, "# The viewmodel has its OWN field of view, separate from the\n");
    fprintf(f, "# world. Below 1 the weapon shrinks; above 1 it fills more view.\n");
    fprintf(f, "weapon_view_scale = %.3f\n", weapon_view_scale);
    fprintf(f, "# Where the weapon sits. 1.0 = half the screen.\n");
    fprintf(f, "weapon_view_offset_x = %.3f\n", weapon_view_offset_x);
    fprintf(f, "weapon_view_offset_y = %.3f\n", weapon_view_offset_y);
    fprintf(f, "weapon_3d_scoped_separate = %d\n",
            weapon_3d_scoped_separate ? 1 : 0);
    fprintf(f, "weapon_3d_amount_scoped = %.4f\n", weapon_3d_amount_scoped);
    fprintf(f, "weapon_all_draw_types = %d\n", weapon_all_draw_types ? 1 : 0);
    fprintf(f, "# How far past the viewmodel the learned material is trusted.\n");
    fprintf(f, "# Draws allowed between two draws of the viewmodel pass before the\n");
    fprintf(f, "# mod decides the pass has ended. Too large and the run leaks into\n");
    fprintf(f, "# the world and scenery picks up the weapon's shift.\n");
    fprintf(f, "weapon_pass_gap = %d\n", weapon_pass_gap);
    fprintf(f, "# Index counts are NOT unique across a map - a fence shares 3252 with a\n");
    fprintf(f, "# real weapon mesh. WHERE in the frame a draw happens does separate them.\n");
    fprintf(f, "# Measured: weapon meshes at clear=1003..1033, impostors at 228 and 710.\n");
    fprintf(f, "# How late in the frame, as a %% of the furthest this pass has got from\n");
    fprintf(f, "# a depth clear. Weapon draws sit at ~96%%, the fence sharing their index\n");
    fprintf(f, "# count at 22%%. Relative, so it survives scenes of different density.\n");
    fprintf(f, "weapon_pass_tail_pct = %d\n", weapon_pass_tail_pct);
    fprintf(f, "weapon_min_since_clear = %d\n", weapon_min_since_clear);
    fprintf(f, "weapon_max_since_clear = %d\n", weapon_max_since_clear);
    fprintf(f, "\n# --- how the viewmodel is recognised ---\n");
    fprintf(f, "# The engine tags first-person geometry with stencil bit 6. Measured over\n");
    fprintf(f, "# two captures: every viewmodel draw has it, none of the ~2180 world draws\n");
    fprintf(f, "# per frame do. Works for every weapon, needs no per-gun tuning.\n");
    fprintf(f, "# 1 = move only what the engine tag and the glass rules claim.\n");
    fprintf(f, "# 0 = also trust the older shader/index/neighbour rules.\n");
    fprintf(f, "weapon_3d_skip_scene_picture = %d\n",
            weapon_3d_skip_scene_picture ? 1 : 0);
    fprintf(f, "taa_prev_from_last_render = %d\n",
            taa_prev_from_last_render ? 1 : 0);
    fprintf(f, "# Read-only probe: finds the engine's TAA resolve draw and reports how\n");
    fprintf(f, "# many run per real frame, their eye tags, and whether both eyes read\n");
    fprintf(f, "# the same history texture. Arm it while playing; it stops by itself.\n");
    fprintf(f, "taa_probe = %d\n", taa_probe ? 1 : 0);
    fprintf(f, "taa_probe_seconds = %d\n", taa_probe_seconds);
    fprintf(f, "# Each eye gets its own temporal history instead of the other eye's.\n");
    fprintf(f, "# 0 restores the previous behaviour exactly. The two below are test\n");
    fprintf(f, "# controls: they make the picture WORSE on purpose, so that 'nothing\n");
    fprintf(f, "# changed' can be told apart from 'nothing was reached'.\n");
    fprintf(f, "per_eye_temporal_history = %d\n", per_eye_temporal_history ? 1 : 0);
    fprintf(f, "per_eye_temporal_eye_swap = %d\n", per_eye_temporal_eye_swap ? 1 : 0);
    fprintf(f, "per_eye_temporal_starve = %d\n", per_eye_temporal_starve ? 1 : 0);
    fprintf(f, "# Reproject each eye's history with that eye's own previous-frame\n");
    fprintf(f, "# matrix. Without it the substituted pixels are fetched from the\n");
    fprintf(f, "# wrong place: blur when still, smearing when moving.\n");
    fprintf(f, "per_eye_temporal_matrix = %d\n", per_eye_temporal_matrix ? 1 : 0);
    fprintf(f, "# Stage 1: take the engine's temporal pass over entirely rather than\n");
    fprintf(f, "# correcting it. At this stage it passes the frame through untouched:\n");
    fprintf(f, "# sharp, aliased, no ghosting. It exists to prove the interception.\n");
    fprintf(f, "taa_replace_pass = %d\n", taa_replace_pass ? 1 : 0);
    fprintf(f, "# One dial. 1.00 = the pair tuned in the headset; the two values it\n");
    fprintf(f, "# scales (taa_blend, taa_clamp_strength) are the calibration below.\n");
    fprintf(f, "taa_ghosting_fix = %.2f\n", taa_ghosting_fix);
    fprintf(f, "# Ask NGX once whether DLSS is available here, and log why not.\n");
    fprintf(f, "# Put back the head rotation the camera matrices never carried.\n");
    fprintf(f, "# Measured: none of the block's four cameras turns with the head.\n");
    fprintf(f, "taa_head_rotation_fix = %d\n", taa_head_rotation_fix ? 1 : 0);
    fprintf(f, "# 1.0 = the derived amount per axis. Negative flips THAT axis -\n");
    fprintf(f, "# yaw and pitch have independent sign conventions on screen.\n");
    fprintf(f, "taa_head_rotation_scale = %.3f\n", taa_head_rotation_scale);
    fprintf(f, "taa_head_rotation_scale_y = %.3f\n", taa_head_rotation_scale_y);
    fprintf(f, "# Use the PREVIOUS frame's head delta (engine consumes a frame late?).\n");
    fprintf(f, "taa_head_rotation_delay = %d\n", taa_head_rotation_delay ? 1 : 0);
    fprintf(f, "# Log the motion-vector texture's centre texel vs the head shift.\n");
    fprintf(f, "# 1 = log the centre motion vector, 2 = log the centre DEPTH.\n");
    fprintf(f, "taa_mv_probe = %d\n", taa_mv_probe);
    fprintf(f, "# Add an 8-entry Halton jitter on top of the engine's 2-phase one.\n");
    fprintf(f, "taa_jitter_inject = %d\n", taa_jitter_inject ? 1 : 0);
    fprintf(f, "# Overlay amplitude: 1.0 = ~0.22 px. Big = steadier edges but\n");
    fprintf(f, "# distant-foliage flicker; small = the reverse. Found by eye.\n");
    fprintf(f, "taa_jitter_inject_scale = %.2f\n", taa_jitter_inject_scale);
    fprintf(f, "# Find the CPU homes of the view-projection and who writes them.\n");
    fprintf(f, "# 1 = memory scan (stand still), 2 = watch the known true home.\n");
    fprintf(f, "jitter_hunt = %d\n", jitter_hunt);
    fprintf(f, "# Mode 2 watch offset in the camera-data object (hex ok).\n");
    fprintf(f, "jitter_hunt_offset = 0x%X\n", jitter_hunt_offset);
    fprintf(f, "# Replace the ENGINE's jitter generator with 8-phase Halton -\n");
    fprintf(f, "# the self-consistent fix; everything renders and compensates it.\n");
    fprintf(f, "jitter_take = %d\n", jitter_take ? 1 : 0);
    fprintf(f, "jitter_take_scale = %.2f\n", jitter_take_scale);
    fprintf(f, "# Force the engine's OWN 16-phase jitter table (AA mode 3) -\n");
    fprintf(f, "# reaches the inlined hot path the detour above cannot.\n");
    fprintf(f, "jitter_mode3 = %d\n", jitter_mode3 ? 1 : 0);
    fprintf(f, "# De-jitter the resolve's reprojection (the engine renders a\n");
    fprintf(f, "# 16-phase sub-pixel jitter; fetching history through the raw\n");
    fprintf(f, "# matrices is the universal small object-shake). 0 = old behaviour.\n");
    fprintf(f, "taa_dejitter = %d\n", taa_dejitter ? 1 : 0);
    fprintf(f, "# Promise the compositor a FIXED position - the engine rotates with\n");
    fprintf(f, "# the head but never moves with it, so a live one makes the runtime\n");
    fprintf(f, "# reproject for translation the pixels never had (headset-only shimmer).\n");
    fprintf(f, "submit_frozen_position = %d\n", submit_frozen_position ? 1 : 0);
    fprintf(f, "# DIAGNOSTIC: freeze the submitted orientation too, so the compositor\n");
    fprintf(f, "# stops warping and only the engine turns the view.\n");
    fprintf(f, "submit_frozen_orientation = %d\n", submit_frozen_orientation ? 1 : 0);
    fprintf(f, "# Latch the per-eye crop: recomputing it every frame from the\n");
    fprintf(f, "# runtime's fov moves the rectangle by a pixel or two per frame,\n");
    fprintf(f, "# which the headset shows as vibration and the desktop does not.\n");
    fprintf(f, "tier1_crop_latch = %d\n", tier1_crop_latch ? 1 : 0);
    fprintf(f, "# Log the angles the engine builds the view from vs what we asked\n");
    fprintf(f, "# for - smooth head + stepping engine = the injection quantises.\n");
    fprintf(f, "head_yaw_probe = %d\n", head_yaw_probe ? 1 : 0);
    fprintf(f, "# Heartbeat/tremor DEAD-BAND (a low-pass cannot separate a pulse\n");
    fprintf(f, "# from real looking - same frequency band; amplitude is the only\n");
    fprintf(f, "# difference). 0 off, 1 light, 2 medium, 3 strong.\n");
    fprintf(f, "head_tremor_level = %d\n", head_tremor_level);
    fprintf(f, "head_tremor_release_deg = %.3f\n", head_tremor_release_deg);
    fprintf(f, "head_tremor_tau_ms = %.1f\n", head_tremor_tau_ms);
    fprintf(f, "head_tremor_leash_deg = %.3f\n", head_tremor_leash_deg);
    fprintf(f, "# Ask the runtime for a pose closer to NOW: extrapolation is what\n");
    fprintf(f, "# turns a small tremor into a visible swing. 0 = stock, 100 = none.\n");
    fprintf(f, "prediction_damp_pct = %d\n", prediction_damp_pct);
    fprintf(f, "# Stamp the frame with ONE head pose, served to every consumer.\n");
    fprintf(f, "head_latch_per_frame = %d\n", head_latch_per_frame ? 1 : 0);
    fprintf(f, "# Invert/multiply the reprojection in DOUBLE - the matrices carry\n");
    fprintf(f, "# world-scale terms and float32 cancellation leaves sub-pixel noise.\n");
    fprintf(f, "taa_double_reproj = %d\n", taa_double_reproj ? 1 : 0);
    fprintf(f, "# Closest-depth dilation: stops silhouette pixels flipping between\n");
    fprintf(f, "# surfaces under the engine's jitter (the edge buzz).\n");
    fprintf(f, "taa_dilate_depth = %d\n", taa_dilate_depth ? 1 : 0);
    fprintf(f, "# Below this many pixels the history is fetched from the exact texel.\n");
    fprintf(f, "taa_mv_deadzone_px = %.2f\n", taa_mv_deadzone_px);
    fprintf(f, "# Pick the camera CONTINUOUS with last frame from the pass's own\n");
    fprintf(f, "# candidates, instead of trusting whatever is bound at the resolve.\n");
    fprintf(f, "taa_camera_lock = %d\n", taa_camera_lock ? 1 : 0);
    fprintf(f, "# Tell DLSS the exact jitter from the matrix, not the estimator.\n");
    fprintf(f, "dlss_jitter_exact = %d\n", dlss_jitter_exact ? 1 : 0);
    fprintf(f, "dlss_probe = %d\n", dlss_probe ? 1 : 0);
    fprintf(f, "# Let DLSS derive exposure from the image - this renderer has no\n");
    fprintf(f, "# exposure buffer we have found, and DLSS judges history on luminance.\n");
    fprintf(f, "dlss_auto_exposure = %d\n", dlss_auto_exposure ? 1 : 0);
    fprintf(f, "taa_write_motion_vectors = %d\n", taa_write_motion_vectors ? 1 : 0);
    fprintf(f, "# DLAA at native resolution, one feature per eye.\n");
    fprintf(f, "dlss_enable = %d\n", dlss_enable ? 1 : 0);
    fprintf(f, "# 0 = report zero jitter (wrong, the old control), 1 = auto from the\n");
    fprintf(f, "# captured matrices, 2 = manual (dlss_jitter_x/y verbatim).\n");
    fprintf(f, "dlss_jitter_mode = %d\n", dlss_jitter_mode);
    fprintf(f, "# Multiplies the auto estimate; -1 flips the sign.\n");
    fprintf(f, "dlss_jitter_scale = %.3f\n", dlss_jitter_scale);
    fprintf(f, "dlss_jitter_x = %.4f\n", dlss_jitter_x);
    fprintf(f, "dlss_jitter_y = %.4f\n", dlss_jitter_y);
    fprintf(f, "# Our vectors are built from jittered matrices - tell DLSS so.\n");
    fprintf(f, "dlss_mv_jittered = %d\n", dlss_mv_jittered ? 1 : 0);
    fprintf(f, "# Apex is reversed-Z; DLSS assumes the opposite unless told.\n");
    fprintf(f, "dlss_depth_inverted = %d\n", dlss_depth_inverted ? 1 : 0);
    fprintf(f, "# 0 default (K), 1 J, 2 K, 3 L, 4 M - the transformer presets.\n");
    fprintf(f, "dlss_preset = %d\n", dlss_preset);
    fprintf(f, "# 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance,\n");
    fprintf(f, "# 5 Custom (uses dlss_upscale_pct below).\n");
    fprintf(f, "dlss_quality = %d\n", dlss_quality);
    fprintf(f, "# Upscaling: percent of the OUTPUT each axis is rendered at.\n");
    fprintf(f, "# 100 = off (DLAA). 67 Quality, 58 Balanced, 50 Performance.\n");
    fprintf(f, "# Takes a game restart - it sizes the swapchains.\n");
    fprintf(f, "dlss_upscale_pct = %d\n", dlss_upscale_pct);
    fprintf(f, "# When upscaling, run DLSS ONCE (at the upscale) rather than\n");
    fprintf(f, "# stacking a DLAA pass under it.\n");
    fprintf(f, "dlss_upscale_solo = %d\n", dlss_upscale_solo ? 1 : 0);
    fprintf(f, "# Let DLSS be the ONLY thing averaging over time: our resolve\n");
    fprintf(f, "# passes the current frame through and only writes vectors.\n");
    fprintf(f, "taa_single_accumulation = %d\n", taa_single_accumulation ? 1 : 0);
    fprintf(f, "# Keep the rectangle DLSS reconstructs from fixed, even when the\n");
    fprintf(f, "# per-eye crop that is submitted changes.\n");
    fprintf(f, "dlss_upscale_rect_latch = %d\n", dlss_upscale_rect_latch ? 1 : 0);
    fprintf(f, "# Sharpening and saturation on the FINISHED eye, after the\n");
    fprintf(f, "# upscale. RCAS, so it cannot ring. 0 / 1.00 = off.\n");
    fprintf(f, "# The first-run page has been read. Set to 0 to see it again.\n");
    fprintf(f, "welcome_seen = %d\n", welcome_seen ? 1 : 0);
    fprintf(f, "# The headset's refresh, not the monitor's, paces the VR loop.\n");
    fprintf(f, "desktop_present_unlocked = %d\n", desktop_present_unlocked ? 1 : 0);
    fprintf(f, "# The log at %%LOCALAPPDATA%%\\theHunterCotWVR. About two lines a\n");
    fprintf(f, "# second; off still writes the opening lines a bug report needs.\n");
    fprintf(f, "logging = %d\n", logging ? 1 : 0);
    fprintf(f, "# Run even if the game build is not the one the mod knows.\n");
    fprintf(f, "# May crash. 0 = refuse unknown builds, which is correct.\n");
    fprintf(f, "apex_ignore_fingerprint = %d\n", apex_ignore_fingerprint ? 1 : 0);
    fprintf(f, "post_sharpen = %.2f\n", post_sharpen);
    fprintf(f, "post_saturation = %.2f\n", post_saturation);
    fprintf(f, "# 0 = our camera-only vectors, 1 = the engine's. Foliage needs\n");
    fprintf(f, "# some of the engine's; the engine's reference frame is wrong.\n");
    fprintf(f, "dlss_mv_object_blend = %.2f\n", dlss_mv_object_blend);
    fprintf(f, "# Stage 2: and resolve it ourselves, one history per eye.\n");
    fprintf(f, "taa_resolve = %d\n", taa_resolve ? 1 : 0);
    fprintf(f, "taa_blend = %.3f\n", taa_blend);
    fprintf(f, "taa_use_motion_vectors = %d\n", taa_use_motion_vectors ? 1 : 0);
    fprintf(f, "taa_mv_invert = %d\n", taa_mv_invert ? 1 : 0);
    fprintf(f, "# Reproject from depth and this eye's own matrices instead of the\n");
    fprintf(f, "# engine's motion vectors, which describe the OTHER eye.\n");
    fprintf(f, "taa_own_motion_vectors = %d\n", taa_own_motion_vectors ? 1 : 0);
    fprintf(f, "# Leave the weapon and phone alone (stencil bit 6), and resample the\n");
    fprintf(f, "# history sharply rather than bilinearly.\n");
    fprintf(f, "taa_exclude_viewmodel = %d\n", taa_exclude_viewmodel ? 1 : 0);
    fprintf(f, "taa_sharp_history = %d\n", taa_sharp_history ? 1 : 0);
    fprintf(f, "taa_camera_relative = %d\n", taa_camera_relative ? 1 : 0);
    fprintf(f, "taa_clamp = %d\n", taa_clamp ? 1 : 0);
    fprintf(f, "taa_clamp_strength = %.2f\n", taa_clamp_strength);
    fprintf(f, "taa_sharpen = %.2f\n", taa_sharpen);
    fprintf(f, "tier1_measure_fov = %d\n", tier1_measure_fov ? 1 : 0);
    fprintf(f, "# Stage 1: widen the engine world FOV. k multiplies the TANGENT.\n");
    fprintf(f, "tier1_per_eye_crop = %d\n", tier1_per_eye_crop ? 1 : 0);
    fprintf(f, "tier1_fov = %d\n", tier1_fov ? 1 : 0);
    fprintf(f, "tier1_fov_k_override = %.3f\n", tier1_fov_k_override);
    fprintf(f, "tier1_scale_viewmodel_fov = %d\n",
            tier1_scale_viewmodel_fov ? 1 : 0);
    fprintf(f, "weapon_3d_mask_follows_glass = %d\n",
            weapon_3d_mask_follows_glass ? 1 : 0);
    fprintf(f, "weapon_3d_optic_signature = %d\n",
            weapon_3d_optic_signature ? 1 : 0);
    fprintf(f, "weapon_stencil_only = %d\n", weapon_stencil_only ? 1 : 0);
    fprintf(f, "weapon_match_stencil = %d\n", weapon_match_stencil ? 1 : 0);
    fprintf(f, "# Fallback only. Index counts are NOT unique across a map - a fence draws\n");
    fprintf(f, "# 3252 indices up close and got treated as part of the gun.\n");
    fprintf(f, "weapon_match_index = %d\n", weapon_match_index ? 1 : 0);
    fprintf(f, "\n# --- which shaders draw the weapon (found with Shader Toggler) ---\n");
    fprintf(f, "# Kept, and correct, but they only match shaders the mod watched being\n");
    fprintf(f, "# created - about 101 of the 2,039 a frame uses. See weapon_match_index.\n");
    fprintf(f, "weapon_ps_crc0 = 0x%08X\n", (unsigned)weapon_ps_crc0);
    fprintf(f, "weapon_ps_crc1 = 0x%08X\n", (unsigned)weapon_ps_crc1);
    fprintf(f, "weapon_ps_crc2 = 0x%08X\n", (unsigned)weapon_ps_crc2);
    fprintf(f, "\n# --- diagnostics (off unless something is being hunted) ---\n");
    fprintf(f, "weapon_cb_dump = %d\n", weapon_cb_dump ? 1 : 0);
    fprintf(f, "weapon_cb_test = %d\n", weapon_cb_test ? 1 : 0);
    fprintf(f, "shader_list = %d\n", shader_list ? 1 : 0);
    fprintf(f, "shader_dump = %d\n", shader_dump ? 1 : 0);
    fprintf(f, "shader_hide_index = %d\n", shader_hide_index);
    fprintf(f, "shader_hide_count = %d\n", shader_hide_count);

    fprintf(f, "\n# --- stereo ---\n");
    fprintf(f, "# Alternate-eye: the camera shifts left/right on alternating frames and\n");
    fprintf(f, "# each eye holds its most recent image. Each eye updates at half the\n");
    fprintf(f, "# headset rate; the pair is genuine stereo from the game's own renderer.\n");
    fprintf(f, "stereo = %d\n", stereo ? 1 : 0);
    fprintf(f, "# 6DoF: move the camera with your head, not just aim it.\n");
    fprintf(f, "six_dof = %d\n", six_dof ? 1 : 0);
    fprintf(f, "six_dof_scale = %.2f\n", six_dof_scale);
    fprintf(f, "# How far from the recentre point the camera may travel, in\n");
    fprintf(f, "# metres. Nothing collides - this is what keeps you out of walls.\n");
    fprintf(f, "six_dof_limit_m = %.2f\n", six_dof_limit_m);
    fprintf(f, "six_dof_invert_x = %d\n", six_dof_invert_x ? 1 : 0);
    fprintf(f, "six_dof_invert_y = %d\n", six_dof_invert_y ? 1 : 0);
    fprintf(f, "six_dof_invert_z = %d\n", six_dof_invert_z ? 1 : 0);
    fprintf(f, "# Measure the lean in the BODY's frame, so turning your head\n");
    fprintf(f, "# does not swing you around a point off to one side.\n");
    fprintf(f, "# Quarter turns, for when the room's zero and the chair disagree.\n");
    fprintf(f, "six_dof_yaw_offset_deg = %d\n", six_dof_yaw_offset_deg);
    fprintf(f, "six_dof_log = %d\n", six_dof_log ? 1 : 0);
    fprintf(f, "# Which facing the lean is measured against: 0 recentre,\n");
    fprintf(f, "# 1 head now, 2 head since recentre, 3 none.\n");
    fprintf(f, "six_dof_frame_mode = %d\n", six_dof_frame_mode);
    fprintf(f, "# Anchor the standing position when the world appears, not in a\n");
    fprintf(f, "# menu - otherwise the view sits high until you recentre.\n");
    fprintf(f, "six_dof_anchor_on_enter = %d\n", six_dof_anchor_on_enter ? 1 : 0);
    fprintf(f, "six_dof_body_frame = %d\n", six_dof_body_frame ? 1 : 0);
    fprintf(f, "six_dof_body_frame_invert = %d\n", six_dof_body_frame_invert ? 1 : 0);
    fprintf(f, "ipd_mm = %.1f\n", ipd_mm);
    fprintf(f, "# One optical axis while scoped - both eyes on the bore, which\n");
    fprintf(f, "# is what closing an eye behind a real scope achieves.\n");
    fprintf(f, "scope_mono = %d\n", scope_mono ? 1 : 0);
    fprintf(f, "scope_ipd_mm = %.1f\n", scope_ipd_mm);
    fprintf(f, "scope_mono_ads = %d\n", scope_mono_ads ? 1 : 0);
    fprintf(f, "\n# Which row of the view basis is 'right'. row1 is known to be up, so\n");
    fprintf(f, "# this is 0 or 2 - whichever fuses. If depth looks inside-out, set\n");
    fprintf(f, "# eye_swap instead of changing the row.\n");
    fprintf(f, "eye_axis_row = %d\n", eye_axis_row);
    fprintf(f, "eye_swap = %d\n", eye_swap ? 1 : 0);
    fprintf(f, "stereo_log_every = %d\n", stereo_log_every);
    fprintf(f, "\n# Alternate-eye fast path: only re-upload the eye that was actually\n");
    fprintf(f, "# re-rendered this frame (1 blit instead of 3). 0 = the older route\n");
    fprintf(f, "# through hold textures, if a runtime dislikes an untouched swapchain.\n");
    fprintf(f, "aer_reuse_swapchain = %d\n", aer_reuse_swapchain ? 1 : 0);
    fprintf(f, "\n# Frames between timing reports (wait / our GPU work / submit).\n");
    fprintf(f, "# 0 = off. Set to 600 to see where the frame time actually goes.\n");
    fprintf(f, "perf_log_every = %d\n", perf_log_every);
    fprintf(f, "\n# Render the frame twice per eye so both eyes come from the SAME instant.\n");
    fprintf(f, "# Fixes disparity being corrupted by motion; costs about half the fps.\n");
    fprintf(f, "full_rate_stereo = %d\n", full_rate_stereo ? 1 : 0);
    fprintf(f, "# Install the scene-draw hook even while full-rate is off, so the\n");
    fprintf(f, "# panel can switch INTO it without a relaunch. Off = no detour at\n");
    fprintf(f, "# all in the alternate-eye path, but full-rate then needs a restart.\n");
    fprintf(f, "full_rate_hook_always = %d\n", full_rate_hook_always ? 1 : 0);
    fprintf(f, "# Scene draws to wait through before it engages (avoids startup).\n");
    fprintf(f, "full_rate_warmup_frames = %d\n", full_rate_warmup_frames);
    fprintf(f, "# Restore the engine's frame clock around the second-eye replay, so the\n");
    fprintf(f, "# world advances once per eye PAIR. Without it the game runs in slow motion.\n");
    fprintf(f, "full_rate_freeze_clock = %d\n", full_rate_freeze_clock ? 1 : 0);
    fprintf(f, "# Tell the engine ZERO time passed while the second eye was drawn - the\n");
    fprintf(f, "# truth, and the proper fix for the world advancing twice per eye pair.\n");
    fprintf(f, "full_rate_zero_dt = %d\n", full_rate_zero_dt ? 1 : 0);
    fprintf(f, "# Restore the engine's time object across the replay. Without it one\n");
    fprintf(f, "# frame's elapsed time is split across two ticks, so walking and\n");
    fprintf(f, "# animations run at HALF SPEED and the game's fps counter reads DOUBLE.\n");
    fprintf(f, "full_rate_freeze_time = %d\n", full_rate_freeze_time ? 1 : 0);
    fprintf(f, "# Write the REAL measured frame interval into the engine's delta. This is\n");
    fprintf(f, "# what actually fixes walking and animations running at half speed.\n");
    fprintf(f, "full_rate_correct_delta = %d\n", full_rate_correct_delta ? 1 : 0);
    fprintf(f, "# The engine keeps a raw delta AND a scaled one. The world reads the raw\n");
    fprintf(f, "# one; the player and the UI read the scaled one, which stays halved.\n");
    fprintf(f, "full_rate_fix_scaled_delta = %d\n", full_rate_fix_scaled_delta ? 1 : 0);
    fprintf(f, "# THE PRIMARY CLOCK FIX: the replay does not run the engine's clock\n");
    fprintf(f, "# update at all, so there is nothing downstream left to repair.\n");
    fprintf(f, "full_rate_skip_clock_update = %d\n", full_rate_skip_clock_update ? 1 : 0);

    // Every key Load() accepts must be written back. Anything missing here is
    // silently reset the next time the file is saved - which had already wiped
    // these once, mid-investigation, and would have looked like a setting that
    // "did nothing".
    fprintf(f, "\n# Correct the frame delta where the engine READS it. The engine asks in\n");
    fprintf(f, "# exactly two places; the world and the player follow different ones.\n");
    fprintf(f, "full_rate_patch_delta_getter = %d\n", full_rate_patch_delta_getter ? 1 : 0);
    fprintf(f, "#   0 = both places, 1 = first only, 2 = second only, 3 = neither\n");
    fprintf(f, "full_rate_patch_callsite = %d\n", full_rate_patch_callsite);
    fprintf(f, "full_rate_patch_flag = %d\n", full_rate_patch_flag);
    fprintf(f, "full_rate_fragment_threshold = %.2f\n", full_rate_fragment_threshold);
    fprintf(f, "# Log which code asks for the frame delta, and how often per frame.\n");
    fprintf(f, "full_rate_log_callsites = %d\n", full_rate_log_callsites ? 1 : 0);
    fprintf(f, "# The replay re-uses the engine's description buffer, which carries the\n");
    fprintf(f, "# frame delta - zero it for the replay or the world advances TWICE.\n");
    fprintf(f, "full_rate_zero_desc_dt = %d\n", full_rate_zero_desc_dt ? 1 : 0);
    fprintf(f, "\n# Both of these are OFF for cause, not preference:\n");
    fprintf(f, "#   the second getter's hook crashed the game on startup\n");
    fprintf(f, "#   freezing the performance counter crashed it too - the OpenXR runtime\n");
    fprintf(f, "#   and DXGI run inside the replay and cannot survive a stopped clock\n");
    fprintf(f, "full_rate_hook_second_getter = %d\n", full_rate_hook_second_getter ? 1 : 0);
    fprintf(f, "full_rate_hook_timer_tick = %d\n", full_rate_hook_timer_tick ? 1 : 0);
    fprintf(f, "full_rate_freeze_qpc = %d\n", full_rate_freeze_qpc ? 1 : 0);
    fprintf(f, "# Do not flip the second eye to the monitor - it saves a full present.\n");
    fprintf(f, "full_rate_skip_second_present = %d\n",
            full_rate_skip_second_present ? 1 : 0);

    // Every key Load() understands is written back. Anything missing here was
    // silently reset to its default the next time the file was saved - a
    // setting the player had deliberately changed would just quietly revert.
    fprintf(f, "\n# Take the eye-separation basis from the camera site that TURNS with the\n");
    fprintf(f, "# player, rather than the one measured to be world-fixed.\n");
    fprintf(f, "stereo_basis_from_lever = %d\n", stereo_basis_from_lever ? 1 : 0);
    fprintf(f, "\n# Read-only hunt for the camera object's orientation field.\n");
    fprintf(f, "camera_object_probe = %d\n", camera_object_probe ? 1 : 0);
    fprintf(f, "\n# Is the FOV number above HORIZONTAL rather than vertical?\n");
    fprintf(f, "fov_is_horizontal = %d\n", fov_is_horizontal ? 1 : 0);

    fprintf(f, "\n# --- in-headset panel ---\n");
    fprintf(f, "sound_enabled = %d\n", sound_enabled ? 1 : 0);
    fprintf(f, "sound_volume = %.2f\n", sound_volume);
    fprintf(f, "# Metres in front of the player, and width in metres.\n");
    fprintf(f, "panel_distance = %.2f\n", panel_distance);
    fprintf(f, "panel_size = %.2f\n", panel_size);
    fclose(f);

    // ROUND-TRIP CHECK. Save() silently forgetting a key that Load() accepts has
    // now happened three times, and every time the symptom was a setting that
    // "did nothing" - the feature looked broken when the value had simply been
    // reset on the previous exit.
    //
    // So read our own file back and count. If the file holds fewer keys than
    // Load() understands, some setting is being dropped, and the log says so
    // rather than leaving it to be discovered by a wasted test session.
    VerifyRoundTrip();
}

// Every key Assign() accepts. Kept next to nothing else so the check is honest
// about what it is: a hand-maintained list, which is still far better than no
// check at all.
void Config::VerifyRoundTrip() const {
    FILE* f = _wfopen(Path().c_str(), L"r");
    if (!f) return;
    int written = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == ';' || *p == '\r' || *p == '\n' || *p == '\0') continue;
        if (strchr(p, '=')) ++written;
    }
    fclose(f);

    // The count Assign() handles. Bump it when a key is added - and if it is
    // forgotten, this fires instead of the feature quietly dying.
    // Was 62 while Save() already wrote 74, so this guard could never fire - it
    // sat there through four separate key-dropping incidents looking like
    // protection. Checked against the source by tools/checkconfig.py at build
    // time, which is the part that actually keeps it honest.
    constexpr int kKeysUnderstood = 310;
    if (written < kKeysUnderstood) {
        COTW_LOG("[config] *** WROTE %d keys but %d are understood - %d SETTING(S) ARE "
                 "BEING DROPPED and will reset on the next launch. A setting that 'does "
                 "nothing' is probably one of them. ***",
                 written, kKeysUnderstood, kKeysUnderstood - written);
    }
}

Config& Cfg() {
    static Config c;
    return c;
}

int DlssRenderPct() {
    const Config& c = Cfg();
    if (!c.dlss_enable) return 100;
    // NVIDIA's published per-axis ratios. Custom falls back to the hand-dialled
    // percentage, and anything outside the sane band means "off" rather than
    // some silently clamped value the player did not ask for.
    switch (c.dlss_quality) {
        case 0: return 100;                       // DLAA
        case 1: return 67;                        // Quality
        case 2: return 58;                        // Balanced
        case 3: return 50;                        // Performance
        case 4: return 33;                        // Ultra Performance
        default: break;                           // Custom
    }
    const int p = c.dlss_upscale_pct;
    return (p < 33 || p > 100) ? 100 : p;
}

}  // namespace cotwvr
