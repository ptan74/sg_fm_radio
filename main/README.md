# main: device composition root

Initializes NVS and Recovery, loads the UI bundle, prepares the display and
starts GSP, then composes the radio: loads the saved station and volume,
starts `app_radio_player`, creates the `app_radio_ui` controller with a sink
that forwards to the player, and finally starts `app_wifi`. It contains no
input handling, drawing or audio logic and exports no headers.

Wiring:

- UI sink (GSP render task) -> `app_radio_player_tune/set_paused/set_volume`
  (non-blocking, latest-wins).
- Wi-Fi state (event task) -> `app_radio_player_set_online` and
  `app_radio_ui_report`.
- Player status (player tasks) -> `app_radio_ui_report`.

The private composition context retains only owned opaque handles; the
controller handle and current link state are atomics because Wi-Fi and player
tasks read them. Everything lives until reboot. If the speaker cannot be
initialized the dial still works without audio; bundle or display failure
leaves Recovery reachable. Other unrecoverable errors use ESP-IDF's
error-check policy.

Private dependencies: `app_display`, `app_radio_ui`, `app_radio_player`,
`app_wifi`, Iris, Recovery, bundle loading, screen mirroring, NVS and logging.
Central dependency pins/overrides live in `idf_component.yml`.
