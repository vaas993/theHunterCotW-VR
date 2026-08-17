#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

#include <cstdint>

namespace cotwvr {

// In-headset settings panel.
//
// Reaching for function keys while wearing a headset is impractical, and a
// tuning session that needs twenty relaunches is a wasted evening - so the
// settings live on a quad layer in front of the player, driven by the gamepad
// or the keyboard.
//
// Every row shows its value AND its default AND a line explaining what it does:
// in a headset, a name alone does not tell anyone what "Eye axis row" is.
class Overlay {
public:
    bool Init();
    void Shutdown();

    void Toggle();
    bool Visible() const { return m_visible; }

    // Called once per frame from Present. Handles keyboard input and returns
    // the bitmap to display (nullptr if nothing needs redrawing this frame).
    void Tick();

    // Gamepad, offered by the XInput proxy before the game sees it. Returns
    // true when the panel consumed the state, in which case it is blanked so
    // the player does not walk about while adjusting a slider.
    // userIndex matters: this is called once per controller slot the game polls,
    // and each slot needs its own button history or they overwrite each other.
    bool ConsumePad(DWORD userIndex, XINPUT_STATE* state);

    // BGRA, premultiplied. Valid while the overlay lives.
    const uint32_t* Pixels() const { return m_pixels; }
    int Width() const { return m_width; }
    int Height() const { return m_height; }

    // True when the bitmap changed since the last call, so the texture upload
    // can be skipped on the (many) frames where nothing moved.
    bool TakeDirty();

    float DistanceMetres() const;
    float WidthMetres() const;

private:
    void Redraw();
    uint64_t Signature() const;   // hashes EVERY displayed value, see .cpp
    void MoveSelection(int dir);
    void MoveTab(int dir);
    void SyncTabToSelection();
    void Adjust(int dir);
    void Activate();

    bool m_visible = false;
    bool m_dirty = true;
    int  m_sel = 1;
    int  m_scroll = 0;          // first entry drawn WITHIN the tab, not the whole list
    int  m_tab = 0;             // the headings ARE the tabs; LB/RB or PgUp/PgDn switch
    uint64_t m_lastSignature = 0;

    // 1420, not 1280: the tab bar is one line and every heading costs width. At
    // 1024 with eight tabs the last one ran off the edge and could not be
    // reached at all; WEAPON 3D makes nine.
    static constexpr int m_width = 1420;
    // Taller than it was (860). The help text under the list was being cut off
    // with an ellipsis on the longer entries - and the help is where a setting
    // explains itself, so losing its last sentence is losing the point of it.
    // The extra 100 px buys both a deeper help box and two more visible rows.
    static constexpr int m_height = 960;

    HDC m_dc = nullptr;
    HBITMAP m_bitmap = nullptr;
    HFONT m_fontTitle = nullptr;
    HFONT m_fontRow = nullptr;
    HFONT m_fontHelp = nullptr;
    HFONT m_fontSmall = nullptr;
    uint32_t* m_dib = nullptr;      // what GDI draws into (opaque)
    uint32_t* m_pixels = nullptr;   // premultiplied BGRA handed to OpenXR
    uint8_t* m_mask = nullptr;      // panel shape / soft shadow

    DWORD m_lastInput = 0;

    // *** PER CONTROLLER SLOT, and that is the whole point. ***
    //
    // COTWVR_PadPostProcess runs once for EVERY user index the game polls, not
    // once per frame. These were single values, so a second connected pad - and
    // Steam Input's virtual device means there usually is one - reported
    // all-zero buttons and wiped the real pad's previous state between polls.
    // Its held button then looked like a brand new press every time round, which
    // is why one press acted like ten.
    //
    // When the current d-pad direction was first pressed, for auto-repeat.
    DWORD m_dirHeldSince[4] = {};
    WORD m_prevButtons[4] = {};
    // Which way the stick is pushed right now (0 = centred) and since when. The
    // stick used to act on a bare timer with no idea whether it had returned to
    // centre, so one flick scored several steps and the speed depended on how
    // often the game polled us rather than on anything the player did.
    int m_stickDir[4] = {};
    DWORD m_stickSince[4] = {};

public:
    // The buttons held on ANY slot, so a binding can be tested outside the pad
    // callback - including while the panel is shut, which is exactly when the
    // flat-screen hotkey has to work.
    WORD PadButtons() const {
        WORD any = 0;
        for (WORD b : m_padButtons) any = WORD(any | b);
        return any;
    }

private:
    // *** PER SLOT, then OR'd - and that is not a detail. ***
    //
    // This was ONE value written by every slot in turn. The real pad reports
    // Start on slot 0, then Steam Input's virtual pad reports nothing on the
    // next slot and wipes it, so by the time anything outside the callback
    // looked it always read 0x0000. Binding still worked, because capture runs
    // inside the callback on that slot's own state - but a bound button did
    // nothing, because the hotkey reads this. Exactly the same mistake as the
    // shared m_prevButtons that made one press act like ten.
    WORD m_padButtons[4] = {};
};

extern Overlay g_overlay;

// *** A BINDING IS A KEY *OR* A PAD BUTTON. ***
//
// Gamepad buttons are not virtual key codes, so GetAsyncKeyState cannot see
// them and a bind that only scanned VK codes could never capture one. Pad
// buttons are therefore stored in the same int with a marker bit set:
//
//    0                     nothing bound
//    1 .. 0xFE             a Windows virtual key code
//    kPadBind | wButtons   an XInput button, e.g. kPadBind | XINPUT_GAMEPAD_Y
//
// The XInput button mask is 16 bits, so the marker sits well clear of it and
// clear of every VK code. Config files written before this stored plain VK
// codes, which still read correctly.
constexpr int kPadBind = 0x10000;

// Is that binding held right now, whichever kind it is? The pad half reads the
// buttons last seen by ConsumePad, which the proxy feeds on every poll.
bool BindingDown(int code);

// True while a key row in the panel is listening for its new binding.
//
// HeadTrackTick is a SEPARATE call from Overlay::Tick and never sees that
// function's input swallow, so without this, binding the recentre key would
// fire a recentre on the very press that bound it.
bool BindingCaptureActive();

}  // namespace cotwvr
