# Nintendo 64 controls

The N64 pad emulates the PC keyboard: each control injects the PC scancode that
`DefKeysDefault95` (SOURCES/INPUT.CPP) binds to the action. This is the only
scheme that reaches the spell / protopack shortcuts, which the engine polls
directly with `CheckKey(DefKeys[32..35])` in PERSO.CPP. See
SOURCES/JOYSTICK_N64.CPP for the implementation.

## Gameplay

| Control      | Action                              | PC key |
|--------------|-------------------------------------|--------|
| D-pad        | Move (turn left/right, walk fwd/back) | arrows |
| A            | Behaviour action (search/talk, jump, punch…) | Space |
| B            | Directional dodge (hold)            | X      |
| L            | Behaviour menu / toggle (hold)      | LCtrl  |
| R            | Throw magic ball / use weapon       | LAlt   |
| Z (trigger)  | Inventory                           | LShift |
| C-up         | Confirm inventory item + recenter camera | Enter |
| C-down       | Behaviour-agnostic interaction      | W      |
| C-right      | Lightning spell (Foudre)            | F      |
| C-left       | Protection spell                    | C      |
| Start        | In-game menu                        | F10    |
| Stick up     | Protopack / Jetpack toggle          | J      |
| Stick left   | Behaviour: Normal                   | F5     |
| Stick right  | Behaviour: Athletic                 | F6     |
| Stick down   | Behaviour: Aggressive               | F7     |

The stick is decoupled from movement (D-pad only) so its four cardinals are the
behaviour / protopack shortcuts.

## Menus

- **Navigate:** D-pad or stick (both work).
- **Confirm:** A or Start.
- **Cancel / close:** B (acts as Esc while a menu is open).
- **Save-name on-screen keyboard:** A select, B backspace, C-up space,
  Start return, C-right escape.

## Deliberately left out

- **Holomap** — reachable from the inventory (Z) as a map item; no dedicated
  button (all controls are allocated).
- **Meca-penguin** and **Discreet behaviour** — situational; use the inventory /
  the behaviour menu (L).
- **Direct weapon shortcuts (1–7)** — impractical without a touch screen; cycle
  weapons through the inventory.
- Stick diagonals are intentionally unmapped (too twitchy during play).

## Notes

- Stick deadzone is `N64_STICK_DEADZONE` (raw N64 units, ~28% of full travel),
  **not** the cfg's `GamepadDeadzone` (an SDL-unit value that would leave the
  N64 stick dead).
- Held shortcut keys don't repeat: spells/protopack edge-latch internally
  (`WaitNoKey32..35` in PERSO.CPP); behaviours are idempotent.
