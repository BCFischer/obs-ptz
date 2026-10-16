# Canon XC driver

The **Canon XC PTZ Control** filter drives Canon remote cameras (CR-N100,
CR-N300, CR-N500, CR-N700, CR-X300, and others that speak the XC protocol)
over HTTP, using Canon's own protocol instead of VISCA.

It does this because XC can link pan/tilt speed to the zoom. The camera
handles that itself, using the real lens's angle of view, so the farther
you zoom in, the slower the camera pans.

## Setup

1. Add the filter **Canon XC PTZ Control** to the camera's source.
2. Set **Host** to the camera's IP address. If you leave it blank, the
   driver uses the host of an NDI or browser source. Port is 80.
3. Fill in **Username** and **Password** if the camera has user
   authentication turned on. HTTP Digest is handled automatically.

## Pan/Tilt speed

| Setting | XC mode | What full joystick means |
|---|---|---|
| Linked to zoom: screens per second (default) | `auto1` (`c.1.pan.speed.ratio`) | The picture moves *N* screen-widths per second at **any** zoom |
| Linked to zoom: the camera's own scaling | `auto2` (`c.1.pan.speed`) | The camera's top speed at wide angle, scaled down by the camera as it zooms in |
| Fixed, whatever the zoom | `manual` (`c.1.pan.speed`) | A constant speed, like VISCA |

- **Screens a second at full speed** applies to `auto1`. 1.0 means one
  screen-width per second. Lower it for slower, more cinematic moves.
- **Joystick response curve**: speed grows as deflection^curve. At 2.0, a
  half push gives a quarter of full speed, which leaves fine control at the
  bottom of the stick. 1.0 is linear.
- The standard **Pan/Tilt max speed** slider still caps the deflection
  before any of this is applied.

Speed settings in XC belong to the session, so every move request carries
its own mode and speed. Moves don't change the camera's stored defaults, and
they don't change what an RC-IP100 or the camera's web page uses.

## Presets

Presets use the camera's own 100 slots, with names stored on the camera:
create, save, recall, rename and delete. **Speed to a preset** is saved with
each preset, as a percentage of the camera's fastest speed. If **Presets also
keep exposure…** is off, a preset stores only where the camera points and
its focus. The driver watches `p.status` to report when a recall has
arrived.

## Other

- Zoom position is reported on a logarithmic scale, so 0.5 means halfway
  in magnification.
- The tally lamp lights red for program and green for preview (`f.tally`,
  `f.tally.mode`).
- The driver doesn't open a session, so a camera already under control by
  another controller refuses moves (Livescope status 301). The OBS log says
  when this happens.
- The protocol has no pipelining, so the driver keeps one request in flight
  at a time, at least 30 ms apart. Joystick moves are coalesced so the
  latest speed always goes next.

Parameters come from *Canon XC Control Protocol Specifications* 1.1.
