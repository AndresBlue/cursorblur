# cursorblur

Motion-blurred mouse cursor for Windows, with a fading trail. Native C / Win32, no dependencies. Turns itself off in games.

## How it works

- Hides the standard system cursors and draws the cursor itself in a click-through, topmost layered window.
- Every frame draws the cursor smeared over exactly one refresh of motion. The eye and the camera then see one even smear instead of a row of crisp copies (the phantom-array effect).
- Mouse moves come from a low-level mouse hook at the mouse's full rate. On Windows 11, raw input sent to a background window is batched to 125 Hz.
- Each frame is timed to the vblank of the monitor the cursor is on: DWM timing on the primary monitor, `D3DKMTWaitForVerticalBlankEvent` on the others. This works with mixed refresh rates (for example 350 Hz + 120 Hz).
- Shows the path a few ms in the past, smoothed over ±2 ms, so each frame interpolates between real mouse reports.
- Over shell popups in higher z-bands (Start, notifications, quick settings), the real cursor is shown instead.

### Game mode

Game mode turns the effect off, restores the real cursor and removes the hook. It kicks in when the cursor is hidden, the cursor is clipped to a window, or the foreground window covers its whole monitor (fullscreen or borderless). Maximized windows and the desktop don't count.

## Build

```
gcc -O2 -mwindows cursorblur.c -o cursorblur.exe -ldwmapi -lgdi32 -luser32
```

## Use

- Run `cursorblur.exe`. Quit with **Ctrl+Alt+Shift+Q**, which restores the real cursor.
- To start it with Windows, put it (or a shortcut) in `shell:startup`.
- Tune `TRAIL_MS` (trail length), `STRENGTH` (trail opacity) and `SMOOTH_MS` at the top of the source.

## Caveats

- The drawn cursor lags the real one by ~1 frame plus the interpolation delay: ~4 ms for 1000+ Hz mice, more for slower ones. Clicks are unaffected.
- The animated busy cursors and apps' custom cursors stay real, so they don't get the blur.
- If the process is killed (Task Manager), the cursor stays invisible. Run it again and quit with the hotkey, or sign out.
